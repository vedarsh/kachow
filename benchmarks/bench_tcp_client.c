/* =============================================================================
 * USRL TCP CLIENT BENCHMARK (Clean Output)
 * =============================================================================
 */

#include "usrl_core.h"
#include "usrl_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#define PAYLOAD_SIZE    4096
#define DEFAULT_PORT    8080
#define BATCH_SIZE      1000000

/* Robust wrappers for blocking I/O */
ssize_t send_complete(usrl_transport_t *ctx, const void *buf, size_t len) {
    size_t total = 0;
    const uint8_t *ptr = buf;
    while (total < len) {
        ssize_t n = usrl_trans_send(ctx, ptr + total, len - total);
        if (n > 0) total += n;
        else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
        else return -1;
    }
    return total;
}

ssize_t recv_complete(usrl_transport_t *ctx, void *buf, size_t len) {
    size_t total = 0;
    uint8_t *ptr = buf;
    while (total < len) {
        ssize_t n = usrl_trans_recv(ctx, ptr + total, len - total);
        if (n > 0) total += n;
        else if (n == 0) return 0;
        else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
        else return -1;
    }
    return total;
}

int main(int argc, char *argv[]) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : DEFAULT_PORT;
    
    /* 1. Startup Log (Matches SHM style) */
    printf("[BENCH] TCP Client starting on %s:%d (Payload: %d)...\n", 
           host, port, PAYLOAD_SIZE);
    
    usrl_transport_t *client = usrl_trans_create(
        USRL_TRANS_TCP, host, port, 0, USRL_SWMR, false);
    
    if (!client) {
        fprintf(stderr, "[BENCH] Error: Connection failed\n");
        return 1;
    }
    
    uint8_t *payload = malloc(PAYLOAD_SIZE);
    memset(payload, 0xAA, PAYLOAD_SIZE);
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    long count = 0;
    for (long i = 0; i < BATCH_SIZE; i++) {
        if (send_complete(client, payload, PAYLOAD_SIZE) != PAYLOAD_SIZE) break;
        if (recv_complete(client, payload, PAYLOAD_SIZE) != PAYLOAD_SIZE) break;
        count++;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    double rate = count / 1e6 / elapsed;
    double bw_mbps = (count * PAYLOAD_SIZE * 8.0) / (elapsed * 1e6);
    double avg_ns = count > 0 ? (elapsed * 1e9 / count) : 0;
    
    /* 2. Result Log (Matches SHM style exactly) */
    printf("[BENCH] TCP Result: %.2f M req/sec | %.2f Mbps | Avg Latency: %.2f ns\n", 
           rate, bw_mbps, avg_ns);
    
    free(payload);
    usrl_trans_destroy(client);
    return 0;
}
