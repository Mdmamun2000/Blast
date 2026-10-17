// BLAST sender. Usage: ./sender <ip> <port> <file> <record_size> <M> [loss_rate] [dest_name]
// dest_name is the name the receiver stores the file under (default: the file's basename).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <libgen.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "blast.h"
#include "garble.h"

#define HDR_TIMEOUT_MS    1000
#define HDR_MAX_TRIES     20
#define BLAST_TIMEOUT_MS  300     // wait for REC_MISS this long before re-asking
#define QUERY_MAX_TRIES   30
#define READ_CHUNK_MAX    40      // each disk read fetches 1..READ_CHUNK_MAX records (random)

// ---- disk reader thread: file -> buffer B' ----
// B' is a ring of M record slots; record r lives in slot (r-1) % M. The reader runs
// independently of the network: it reads chunks of random size and publishes how far
// it got (avail). It may fill record r only once r <= released + M, i.e. after the
// blast that used that slot has been confirmed by an empty REC_MISS.
static struct {
    pthread_mutex_t mu; pthread_cond_t cv;
    FILE *f;
    uint8_t *ring; long cap;
    int rec_size;
    long total_recs;
    long avail;                    // records 1..avail are in B'
    long released;                 // records 1..released are confirmed; their slots can be reused
    long reads;                    // number of disk reads
} R = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static void *reader(void *arg) {
    (void)arg;
    unsigned seed = (unsigned)time(NULL) ^ (unsigned)getpid();
    long next = 1;
    while (next <= R.total_recs) {
        pthread_mutex_lock(&R.mu);
        while (next > R.released + R.cap) pthread_cond_wait(&R.cv, &R.mu);
        long room = R.released + R.cap - next + 1;
        pthread_mutex_unlock(&R.mu);

        long want = 1 + rand_r(&seed) % READ_CHUNK_MAX;
        if (want > room) want = room;
        if (want > R.total_recs - next + 1) want = R.total_recs - next + 1;
        long slot = (next - 1) % R.cap;
        if (want > R.cap - slot) want = R.cap - slot;          // a read never wraps the ring
        size_t got = fread(R.ring + slot * R.rec_size, 1, (size_t)want * R.rec_size, R.f);
        if (got == 0) { fprintf(stderr, "disk read failed\n"); exit(1); }
        long n = ((long)got + R.rec_size - 1) / R.rec_size;   // only the file's last record is short

        pthread_mutex_lock(&R.mu);
        next += n;
        R.avail = next - 1;
        R.reads++;
        pthread_cond_broadcast(&R.cv);
        pthread_mutex_unlock(&R.mu);
    }
    return NULL;
}

// block until record r has been read from disk; returns how many records are available
static long wait_avail(long r) {
    pthread_mutex_lock(&R.mu);
    while (R.avail < r) pthread_cond_wait(&R.cv, &R.mu);
    long a = R.avail;
    pthread_mutex_unlock(&R.mu);
    return a;
}

static void release_upto(long r) {
    pthread_mutex_lock(&R.mu);
    R.released = r;
    pthread_cond_broadcast(&R.cv);
    pthread_mutex_unlock(&R.mu);
}

// ---- packet builder: packs records (possibly non-contiguous) into DATA packets ----
struct hseg { uint32_t st, end; };
struct builder {
    int sock; struct sockaddr_in *dst;
    int max_recs;
    struct hseg segs[MAX_SEGS]; int nseg, nrec;
    uint8_t data[MAX_DATA_BYTES]; int dlen;
    long pkts_sent, recs_sent, partial_pkts;   // partial = fewer than max_recs records
};

static void flush_pkt(struct builder *b) {
    if (b->nrec == 0) return;
    uint8_t pkt[MAX_PKT];
    int off = 0;
    pkt[off++] = PKT_DATA;
    pkt[off++] = (uint8_t)b->nseg;
    for (int i = 0; i < b->nseg; i++) {
        struct seg s = { htonl(b->segs[i].st), htonl(b->segs[i].end) };
        memcpy(pkt + off, &s, sizeof s); off += sizeof s;
    }
    memcpy(pkt + off, b->data, b->dlen); off += b->dlen;
    for (;;) {
        ssize_t r = g_sendto(b->sock, pkt, off, 0, (struct sockaddr *)b->dst, sizeof *b->dst);
        if (r >= 0) break;
        if (errno == ENOBUFS || errno == EAGAIN) { usleep(200); continue; }
        perror("sendto"); exit(1);
    }
    b->pkts_sent++;
    b->recs_sent += b->nrec;
    if (b->nrec < b->max_recs) b->partial_pkts++;
    b->nseg = b->nrec = b->dlen = 0;
}

static void add_record(struct builder *b, uint32_t rec, const uint8_t *p, int len) {
    if (b->nrec == b->max_recs) flush_pkt(b);
    if (b->nseg > 0 && b->segs[b->nseg - 1].end + 1 == rec) {
        b->segs[b->nseg - 1].end = rec;
    } else {
        if (b->nseg == MAX_SEGS) flush_pkt(b);
        b->segs[b->nseg].st = b->segs[b->nseg].end = rec;
        b->nseg++;
    }
    memcpy(b->data + b->dlen, p, len);
    b->dlen += len; b->nrec++;
}

static off_t g_file_size;

static const uint8_t *rec_ptr(long r) { return R.ring + ((r - 1) % R.cap) * R.rec_size; }
static int rec_len(long r) {
    return r == R.total_recs ? (int)(g_file_size - (off_t)(r - 1) * R.rec_size) : R.rec_size;
}

static void set_timeout(int sock, int ms) {
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

int main(int argc, char **argv) {
    if (argc < 6 || argc > 8) {
        fprintf(stderr, "usage: %s <ip> <port> <file> <record_size> <M> [loss_rate] [dest_name]\n", argv[0]);
        return 1;
    }
    const char *path = argv[3];
    int rec_size = atoi(argv[4]);
    int M = atoi(argv[5]);
    garble_init(argc >= 7 ? atof(argv[6]) : 0.0, (unsigned)time(NULL) ^ getpid());
    if (rec_size != 256 && rec_size != 512 && rec_size != 1024) {
        fprintf(stderr, "record_size must be 256, 512 or 1024\n"); return 1;
    }
    if (M < MIN_M || M > MAX_M) { fprintf(stderr, "M must be in %d..%d\n", MIN_M, MAX_M); return 1; }
    struct stat st;
    if (stat(path, &st) < 0 || st.st_size == 0) { fprintf(stderr, "cannot read file (or empty)\n"); return 1; }
    g_file_size = st.st_size;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    set_timeout(sock, HDR_TIMEOUT_MS);

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &dst.sin_addr) != 1) { fprintf(stderr, "bad ip\n"); return 1; }

    // ---- Phase 1: connection setup ----
    struct file_hdr hdr = {0};
    hdr.type = PKT_FILE_HDR;
    hdr.file_size = htonl((uint32_t)st.st_size);
    hdr.record_size = htons((uint16_t)rec_size);
    hdr.blast_size = htonl((uint32_t)M);
    if (argc == 8) {
        strncpy(hdr.filename, argv[7], MAX_NAME - 1);
    } else {
        char *tmp = strdup(path);
        strncpy(hdr.filename, basename(tmp), MAX_NAME - 1);
        free(tmp);
    }

    int connected = 0;
    struct file_hdr_ack ack;
    for (int attempt = 1; attempt <= HDR_MAX_TRIES && !connected; attempt++) {
        g_sendto(sock, &hdr, sizeof hdr, 0, (struct sockaddr *)&dst, sizeof dst);
        for (;;) {
            ssize_t n = recvfrom(sock, &ack, sizeof ack, 0, NULL, NULL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                perror("recvfrom"); return 1;
            }
            if (n == sizeof ack && ack.type == PKT_FILE_HDR_ACK) { connected = 1; break; }
        }
    }
    if (!connected) { fprintf(stderr, "no FILE_HDR_ACK, giving up\n"); return 1; }
    if (ack.status != 0) { fprintf(stderr, "receiver rejected the transfer\n"); return 1; }

    rec_size = ntohs(ack.record_size);
    M = (int)ntohl(ack.blast_size);
    long total_recs = (st.st_size + rec_size - 1) / rec_size;
    printf("connected: record_size=%d, M=%d, file=%lld bytes -> '%s', %ld records, loss=%.0f%%\n",
           rec_size, M, (long long)st.st_size, hdr.filename, total_recs, g_loss * 100);

    // ---- Phase 2: blasts ----
    R.f = fopen(path, "rb");
    if (!R.f) { perror("fopen"); return 1; }
    R.rec_size = rec_size;
    R.cap = M;
    R.total_recs = total_recs;
    R.ring = malloc((size_t)M * rec_size);
    if (!R.ring) { perror("malloc"); return 1; }

    struct builder b = {0};
    b.sock = sock; b.dst = &dst;
    b.max_recs = MAX_DATA_BYTES / rec_size < MAX_RECS_PER_PKT ? MAX_DATA_BYTES / rec_size
                                                               : MAX_RECS_PER_PKT;
    set_timeout(sock, BLAST_TIMEOUT_MS);

    long resent_recs = 0, queries = 0, timeouts = 0, miss_rcvd = 0;
    uint32_t seq = 0;
    uint8_t rbuf[MAX_PKT];
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);

    pthread_t rd;
    if (pthread_create(&rd, NULL, reader, NULL) != 0) { fprintf(stderr, "pthread_create failed\n"); return 1; }

    for (long first = 1; first <= total_recs; first += M) {
        long last = first + M - 1 < total_recs ? first + M - 1 : total_recs;

        // Send records as soon as the disk reader makes them available. Whatever is
        // ready goes out right away, so a packet carries anywhere from 1 to max_recs records.
        long r = first;
        while (r <= last) {
            long a = wait_avail(r);
            if (a > last) a = last;
            for (; r <= a; r++) add_record(&b, (uint32_t)r, rec_ptr(r), rec_len(r));
            flush_pkt(&b);
        }

        // Ask "is the blast over?" until the receiver reports nothing missing.
        int rounds = 0;
        for (;;) {
            seq++;
            struct is_blast_over q = { PKT_IS_BLAST_OVER, htonl(seq), htonl((uint32_t)first), htonl((uint32_t)last) };
            struct rec_miss *rm = NULL;
            ssize_t n = 0;
            for (int tries = 0; tries < QUERY_MAX_TRIES && !rm; tries++) {
                g_sendto(sock, &q, sizeof q, 0, (struct sockaddr *)&dst, sizeof dst);
                queries++;
                for (;;) {
                    n = recvfrom(sock, rbuf, sizeof rbuf, 0, NULL, NULL);
                    if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) { timeouts++; break; }
                        perror("recvfrom"); return 1;
                    }
                    struct rec_miss *c = (struct rec_miss *)rbuf;
                    if (n >= (ssize_t)sizeof *c && c->type == PKT_REC_MISS && ntohl(c->seq) == seq) { rm = c; break; }
                }
            }
            if (!rm) { fprintf(stderr, "receiver not responding, giving up\n"); return 1; }
            miss_rcvd++;
            int nr = ntohs(rm->nranges);
            if (n < (ssize_t)(sizeof *rm + nr * sizeof(struct seg))) continue;   // malformed: re-ask
            if (nr == 0) break;                                                  // blast complete
            rounds++;
            for (int i = 0; i < nr; i++) {
                struct seg s;
                memcpy(&s, rbuf + sizeof *rm + i * sizeof s, sizeof s);
                long st_r = ntohl(s.st_rec), en_r = ntohl(s.end_rec);
                if (st_r < first) st_r = first;
                if (en_r > last) en_r = last;
                for (long rr = st_r; rr <= en_r; rr++) {
                    add_record(&b, (uint32_t)rr, rec_ptr(rr), rec_len(rr));
                    resent_recs++;
                }
            }
            flush_pkt(&b);
        }
        release_upto(last);            // B' slots of this blast may now be refilled from disk
        printf("blast %ld-%ld done (%d retransmit rounds)\n", first, last, rounds);
    }
    gettimeofday(&t1, NULL);
    pthread_join(rd, NULL);
    fclose(R.f);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;

    // ---- Phase 3: disconnect ----
    // Every record is already confirmed, so the transfer succeeded even if the
    // DISCONNECT_ACK never arrives; we just retry a few times and then leave.
    struct ctl bye = { PKT_DISCONNECT };
    int closed = 0, bye_sent = 0;
    for (int tries = 0; tries < 10 && !closed; tries++) {
        g_sendto(sock, &bye, sizeof bye, 0, (struct sockaddr *)&dst, sizeof dst);
        bye_sent++;
        for (;;) {
            ssize_t n = recvfrom(sock, rbuf, sizeof rbuf, 0, NULL, NULL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                perror("recvfrom"); return 1;
            }
            if (n == sizeof(struct ctl) && rbuf[0] == PKT_DISCONNECT_ACK) { closed = 1; break; }
        }
    }
    if (closed) printf("disconnected cleanly (%d DISCONNECT sent)\n", bye_sent);
    else printf("no DISCONNECT_ACK after %d tries; all data was already confirmed, exiting\n", bye_sent);
    printf("---- stats ----\n");
    printf("data packets sent: %ld, records resent: %ld\n", b.pkts_sent, resent_recs);
    printf("records per packet: avg %.2f (max %d), partial packets: %ld, disk reads: %ld\n",
           (double)b.recs_sent / b.pkts_sent, b.max_recs, b.partial_pkts, R.reads);
    printf("IS_BLAST_OVER sent: %ld, REC_MISS received: %ld, timeouts: %ld\n", queries, miss_rcvd, timeouts);
    printf("packets dropped by garbler (sender side): %ld of %ld\n", g_dropped, g_attempted);
    printf("time %.3f s, throughput %.2f Mbit/s\n", secs, st.st_size * 8.0 / secs / 1e6);
    return 0;
}
