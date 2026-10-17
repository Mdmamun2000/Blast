# Project Plan: BLAST File Transfer over UDP

**CSc 412 Computer Networks** · Prof. K. Ravindran · **Student:** Md Mamun (individual project) · October 17, 2026

## Objective

Implement the BLAST protocol to transfer a 10–15 MB file over UDP with minimal acknowledgment overhead,
test it under controlled packet loss, and measure maximum throughput versus loss rate. Platform: macOS
(approved by the instructor), C with the UNIX socket API and POSIX threads.

## Approach

1. **Packet formats** (`blast.h`): FILE_HDR / FILE_HDR_ACK, DATA with a segment list
   {(st_rec, end_rec), …} of up to 16 records, IS_BLAST_OVER(Mst, Mfin), REC_MISS (missing ranges),
   DISCONNECT / DISCONNECT_ACK.
2. **Phase 1 – setup:** the sender proposes record size (256/512/1024 B), M (200–10000) and the file
   name; the receiver may lower M to its buffer size. FILE_HDR is retransmitted on timeout.
3. **Phase 2 – blasts:** send M records per blast, then IS_BLAST_OVER; resend only the records listed in
   REC_MISS (several segments per packet) until REC_MISS is empty. IS_BLAST_OVER is retransmitted on
   timeout; a sequence number lets the sender ignore stale REC_MISS replies.
4. **Phase 3 – disconnect:** DISCONNECT / DISCONNECT_ACK; the receiver lingers 3 s to answer late
   retransmissions.
5. **Asynchronous disk I/O:** each side has an M-record buffer B'. A reader thread (sender) and a writer
   thread (receiver) move data between disk and B' independently of the network, so packets carry a
   variable number of records, as in Figure 2 of the assignment.
6. **Garbler:** every outgoing packet (data and control, on both sides) is dropped with probability *p*.

## Testing and measurement

- Verify every received file against the original by checksum, at loss rates from 0% to 30%.
- Sweep *p* ∈ {0, 1, 2, 5, 10, 15, 20, 30}%, record size ∈ {256, 512, 1024}, M ∈ {500, 2000, 10000},
  5 runs each (360 transfers), using a script that records time, throughput, packets sent and dropped,
  records resent and timeouts.
- Plot throughput versus loss rate for every setting and the maximum achievable throughput.

## Schedule

| Week | Dates | Work | Status |
|---|---|---|---|
| 1 | Oct 7–13 | Study the specification; basic UDP sender/receiver; packet formats | Done |
| 2 | Oct 14–20 | Phases 1–3 with timeouts; garbler; asynchronous disk I/O with buffers B' | Done |
| 3 | Oct 21–27 | Correctness testing under loss; first full measurement run | First run done (Oct 17) |
| 4 | Oct 28–Nov 3 | Analyze results; explain throughput behaviour (effect of timeouts, M, record size) | Planned |
| 5 | Nov 4–10 | Possible improvements, e.g. adaptive timeout; rerun measurements if code changes | Planned |
| 6 | Nov 11–18 | Final report with graphs and source code; submit by Nov 18 | Planned |

## Deliverables

Source code (`sender.c`, `receiver.c`, `blast.h`, `garble.h`, Makefile, experiment and plotting
scripts), loss and throughput statistics, a graph of throughput versus packet-loss rate, and a final
report. As this is an individual project, all parts are done by Md Mamun.
