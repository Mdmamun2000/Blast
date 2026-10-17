// Step 5 receiver. Usage: ./receiver <port> <max_M> [loss_rate]
// Runs forever and serves one transfer after another.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <libgen.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "blast.h"
#include "garble.h"

enum state { IDLE, ACTIVE, LINGER };

// everything we know about the current transfer
static struct {
    enum state st;
    int fd;
    uint32_t file_size, M, total_recs, nrecv;
    uint16_t rec_size;
    uint8_t *got;                  // got[r] = 1 once record r has arrived
    long pkts, dup_recs, miss_sent;
    double linger_until;
    char name[MAX_NAME + 8];
} S = { .st = IDLE, .fd = -1 };

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void end_session(void) {
    if (S.fd >= 0) close(S.fd);
    S.fd = -1;
    free(S.got); S.got = NULL;
    S.st = IDLE;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) { fprintf(stderr, "usage: %s <port> <max_M> [loss_rate]\n", argv[0]); return 1; }
    int max_M = atoi(argv[2]);
    garble_init(argc == 4 ? atof(argv[3]) : 0.0, (unsigned)time(NULL) ^ getpid());
    setvbuf(stdout, NULL, _IOLBF, 0);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    struct timeval tv = {0, 200000};                    // wake up every 200 ms to check the linger timer
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(atoi(argv[1]));
    if (bind(sock, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    printf("receiver listening on UDP port %s (max M = %d, loss=%.0f%%)\n", argv[1], max_M, g_loss * 100);

    uint8_t buf[MAX_PKT];
    for (;;) {
        // linger over? -> forget the old transfer, get ready for a new one
        if (S.st == LINGER && now_sec() >= S.linger_until) {
            end_session();
            printf("linger over, ready for next transfer\n");
        }

        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        ssize_t n = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
        if (n < 1) continue;                            // timeout or error: loop and re-check timer

        switch (buf[0]) {
        case PKT_FILE_HDR: {
            if (n != sizeof(struct file_hdr)) break;
            struct file_hdr *h = (struct file_hdr *)buf;
            struct file_hdr_ack ack = {0};
            ack.type = PKT_FILE_HDR_ACK;
            uint16_t rs = ntohs(h->record_size);
            uint32_t m = ntohl(h->blast_size);
            if (rs != 256 && rs != 512 && rs != 1024) {
                ack.status = 1;
            } else if (S.st == ACTIVE) {
                printf("duplicate FILE_HDR, re-sending ACK\n");     // our earlier ACK was lost
            } else {
                if (S.st == LINGER) end_session();                  // previous transfer is finished
                h->filename[MAX_NAME - 1] = '\0';
                snprintf(S.name, sizeof S.name, "recv_%s", basename(h->filename));
                S.fd = open(S.name, O_CREAT | O_WRONLY | O_TRUNC, 0644);
                if (S.fd < 0) { perror("open"); ack.status = 1; }
                else {
                    S.rec_size = rs;
                    S.file_size = ntohl(h->file_size);
                    S.M = m > (uint32_t)max_M ? (uint32_t)max_M : m;
                    S.total_recs = (S.file_size + S.rec_size - 1) / S.rec_size;
                    S.got = calloc(S.total_recs + 1, 1);
                    S.nrecv = 0; S.pkts = 0; S.dup_recs = 0; S.miss_sent = 0;
                    g_dropped = 0; g_attempted = 0;               // per-transfer garbler stats
                    S.st = ACTIVE;
                    printf("FILE_HDR: '%s' %u bytes, record_size=%u, M=%u, %u records\n",
                           S.name, S.file_size, S.rec_size, S.M, S.total_recs);
                }
            }
            if (ack.status == 0) { ack.record_size = htons(S.rec_size); ack.blast_size = htonl(S.M); }
            g_sendto(sock, &ack, sizeof ack, 0, (struct sockaddr *)&from, flen);
            break;
        }
        case PKT_DATA: {
            if (S.st != ACTIVE || n < (ssize_t)sizeof(struct data_hdr)) break;
            S.pkts++;
            int nseg = buf[1];
            size_t off = sizeof(struct data_hdr) + (size_t)nseg * sizeof(struct seg);
            if (nseg < 1 || nseg > MAX_SEGS || (size_t)n < off) break;
            for (int i = 0; i < nseg; i++) {
                struct seg s;
                memcpy(&s, buf + sizeof(struct data_hdr) + i * sizeof s, sizeof s);
                uint32_t st = ntohl(s.st_rec), end = ntohl(s.end_rec);
                if (st < 1 || end < st || end > S.total_recs) break;
                uint64_t b_start = (uint64_t)(st - 1) * S.rec_size;
                uint64_t b_end = (uint64_t)end * S.rec_size;
                if (b_end > S.file_size) b_end = S.file_size;
                size_t len = b_end - b_start;
                if (off + len > (size_t)n) break;
                pwrite(S.fd, buf + off, len, b_start);
                off += len;
                for (uint32_t r = st; r <= end; r++) {
                    if (S.got[r]) S.dup_recs++;
                    else { S.got[r] = 1; S.nrecv++; }
                }
            }
            break;
        }
        case PKT_IS_BLAST_OVER: {
            // Also answered while lingering, so a retransmitted query still gets a REC_MISS.
            if (S.st == IDLE || n != sizeof(struct is_blast_over)) break;
            struct is_blast_over *q = (struct is_blast_over *)buf;
            uint32_t mst = ntohl(q->mst), mfin = ntohl(q->mfin);
            if (mst < 1 || mst > mfin || mfin > S.total_recs) break;

            uint8_t out[sizeof(struct rec_miss) + MAX_MISS_RANGES * sizeof(struct seg)];
            struct rec_miss *rm = (struct rec_miss *)out;
            int nr = 0;
            uint32_t r = mst;
            while (r <= mfin && nr < MAX_MISS_RANGES) {
                if (S.got[r]) { r++; continue; }
                uint32_t st = r;
                while (r <= mfin && !S.got[r]) r++;
                struct seg s = { htonl(st), htonl(r - 1) };
                memcpy(out + sizeof *rm + nr * sizeof s, &s, sizeof s);
                nr++;
            }
            rm->type = PKT_REC_MISS;
            rm->seq = q->seq;
            rm->mst = q->mst; rm->mfin = q->mfin;
            rm->nranges = htons((uint16_t)nr);
            g_sendto(sock, out, sizeof *rm + nr * sizeof(struct seg), 0, (struct sockaddr *)&from, flen);
            S.miss_sent++;
            break;
        }
        case PKT_DISCONNECT: {
            if (S.st == IDLE) break;
            if (S.st == ACTIVE) {
                if (S.fd >= 0) { close(S.fd); S.fd = -1; }   // keep got[] for the linger period
                printf("DISCONNECT: %u/%u records, %ld data packets received, %ld duplicate records, "
                       "%ld REC_MISS sent, receiver-side garbler dropped %ld of %ld\n",
                       S.nrecv, S.total_recs, S.pkts, S.dup_recs, S.miss_sent, g_dropped, g_attempted);
                if (S.nrecv != S.total_recs) printf("WARNING: file incomplete\n");
                S.st = LINGER;
                S.linger_until = now_sec() + LINGER_SEC;
            }
            struct ctl ackp = { PKT_DISCONNECT_ACK };        // re-ACK if sender retransmits DISCONNECT
            g_sendto(sock, &ackp, sizeof ackp, 0, (struct sockaddr *)&from, flen);
            break;
        }
        default:
            printf("ignoring packet type %d\n", buf[0]);
        }
    }
}
