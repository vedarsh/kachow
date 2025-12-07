#include "usrl_ring.h"
#include "usrl_core.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

// 20 Million messages for a solid sample size
#define BATCH_SIZE 20000000

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <topic> <payload_size>\n", argv[0]);
        return 1;
    }

    char *topic = argv[1];
    int payload_size = atoi(argv[2]);

    void *core = usrl_core_map("/usrl_core", 128 * 1024 * 1024);
    if (!core) return 1;

    UsrlPublisher pub;
    usrl_pub_init(&pub, core, topic, 1);

    if (!pub.desc) {
        printf("[BENCH] Error: Topic '%s' not found!\n", topic);
        return 1;
    }

    // Allocate buffer dynamically based on requested size
    uint8_t *payload = malloc(payload_size);
    memset(payload, 0xAA, payload_size);

    printf("[BENCH] SWMR Publisher starting on '%s' (Size: %d bytes)...\n", topic, payload_size);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < BATCH_SIZE; i++) {
        // Spin wait if ring is full
        while (usrl_pub_publish(&pub, payload, payload_size) != 0) {
            __asm__ volatile("nop");
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

    double rate_mpps = (BATCH_SIZE / 1e6) / elapsed;
    double bw_mbps   = ((double)BATCH_SIZE * payload_size / 1024.0 / 1024.0) / elapsed;
    double avg_ns    = (elapsed * 1e9) / BATCH_SIZE;

    printf("[BENCH] SWMR Result: %.2f M msg/sec | %.2f MB/s | Avg Latency: %.2f ns\n",
           rate_mpps, bw_mbps, avg_ns);

    free(payload);
    return 0;
}
