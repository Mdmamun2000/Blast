// blast.h - packet formats shared by sender and receiver.
// All multi-byte fields travel in network byte order (htons/htonl).
#ifndef BLAST_H
#define BLAST_H
#include <stdint.h>

#define PKT_FILE_HDR       1
#define PKT_FILE_HDR_ACK   2
#define PKT_DATA           3   // used from Step 3
#define PKT_IS_BLAST_OVER  4   // Step 4
#define PKT_REC_MISS       5   // Step 4
#define PKT_DISCONNECT     6
#define PKT_DISCONNECT_ACK 7

#define MAX_SEGS           16
// macOS caps UDP datagrams at 9216 bytes by default, so keep data <= 8192.
// Records per packet = min(16, MAX_DATA_BYTES / record_size)  (1024B records -> 8)
#define MAX_DATA_BYTES     8192
#define MAX_PKT            (2 + MAX_SEGS * 8 + MAX_DATA_BYTES)
#define MAX_NAME           256
#define MAX_RECS_PER_PKT   16
#define MIN_M              200
#define MAX_M              10000

struct __attribute__((packed)) file_hdr {
    uint8_t  type;                 // PKT_FILE_HDR
    uint32_t file_size;            // bytes
    uint16_t record_size;          // 256, 512 or 1024
    uint32_t blast_size;           // M, records per blast (proposed)
    char     filename[MAX_NAME];   // name to store under at the receiver
};

struct __attribute__((packed)) file_hdr_ack {
    uint8_t  type;                 // PKT_FILE_HDR_ACK
    uint8_t  status;               // 0 = OK, 1 = rejected
    uint16_t record_size;          // agreed
    uint32_t blast_size;           // agreed (receiver may lower it)
};

// DATA packet = data_hdr, then nseg x seg (the segment list), then record bytes
// for segment 1, segment 2, ... in order. Records are numbered from 1.
struct __attribute__((packed)) data_hdr {
    uint8_t type;                  // PKT_DATA
    uint8_t nseg;                  // N, number of segments (1..MAX_SEGS)
};
struct __attribute__((packed)) seg {
    uint32_t st_rec;               // first record of segment
    uint32_t end_rec;              // last record of segment (inclusive)
};

// ---- Step 4: reliability ----
#define MAX_MISS_RANGES    512   // max ranges reported in one REC_MISS

struct __attribute__((packed)) is_blast_over {
    uint8_t  type;                 // PKT_IS_BLAST_OVER
    uint32_t seq;                  // query number; REC_MISS echoes it so stale replies are ignored
    uint32_t mst;                  // first record of the blast
    uint32_t mfin;                 // last record of the blast
};
struct __attribute__((packed)) rec_miss {
    uint8_t  type;                 // PKT_REC_MISS
    uint32_t seq;                  // echo of is_blast_over.seq
    uint32_t mst, mfin;            // echo of the blast range
    uint16_t nranges;              // 0 = nothing missing. Followed by nranges x struct seg.
};

// ---- Step 5: disconnect ----
#define LINGER_SEC         3     // receiver keeps answering this long after DISCONNECT

struct __attribute__((packed)) ctl {      // DISCONNECT and DISCONNECT_ACK carry only a type
    uint8_t type;
};

#endif
