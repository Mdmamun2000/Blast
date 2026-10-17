// garble.h - minimal "garbler": drops outgoing packets at a given rate.
// Both sender and receiver send through g_sendto(), so loss hits data AND control packets.
#ifndef GARBLE_H
#define GARBLE_H
#include <stdlib.h>
#include <sys/socket.h>

static double g_loss = 0.0;        // probability of dropping each outgoing packet
static long g_attempted = 0, g_dropped = 0;

static inline void garble_init(double rate, unsigned seed) {
    g_loss = rate;
    srand48(seed);
}

static inline ssize_t g_sendto(int sock, const void *buf, size_t len, int flags,
                               const struct sockaddr *to, socklen_t tolen) {
    g_attempted++;
    if (g_loss > 0 && drand48() < g_loss) { g_dropped++; return (ssize_t)len; } // "sent" but lost
    return sendto(sock, buf, len, flags, to, tolen);
}
#endif
