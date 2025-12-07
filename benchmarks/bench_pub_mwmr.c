#include "usrl_ring.h"
#include "usrl_core.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

// Increase to 5M per writer for longer duration
#define MSGS_PER_WRITER 5000000
#define DEFAULT_WRITERS 4

void run_writer(int id, const char *topic, int payload_size) {
    void *core = usrl_core_map("/usrl_core", 128 * 1024 * 1024);
    if (!core) exit(1);

    UsrlMwmrPublisher pub;
    usrl_mwmr_pub_init(&pub, core, topic, (uint16_t)id);

    uint8_t *payload = malloc(payload_size);
    memset(payload, id, payload_size);

    for (int i = 0; i < MSGS_PER_WRITER; i++) {
        // Spin lock
        while (usrl_mwmr_pub_publish(&pub, payload, payload_size) != 0) {
             __asm__ volatile("nop");
        }
    }

    free(payload);
    exit(0);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <topic> [num_writers] [payload_size]\n", argv[0]);
        return 1;
    }

    const char *topic = argv[1];
    int writers = (argc >= 3) ? atoi(argv[2]) : DEFAULT_WRITERS;
    int payload_size = (argc >= 4) ? atoi(argv[3]) : 64; // Default to 64 bytes

    printf("[BENCH] MWMR: Spawning %d writers on '%s' (Size: %d bytes)...\n",
           writers, topic, payload_size);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < writers; i++) {
        if (fork() == 0) {
            run_writer(i + 1, topic, payload_size);
        }
    }

    // Wait for all writers to finish
    while (wait(NULL) > 0);

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

    long total_msgs = (long)writers * MSGS_PER_WRITER;

    double rate_mpps = (total_msgs / 1e6) / elapsed;
    double bw_mbps   = ((double)total_msgs * payload_size / 1024.0 / 1024.0) / elapsed;
    double avg_ns    = (elapsed * 1e9) / total_msgs; // Average time per message (aggregate)

    printf("[BENCH] MWMR Result: %.2f M msg/sec | %.2f MB/s | Avg Latency: %.2f ns\n",
           rate_mpps, bw_mbps, avg_ns);

    return 0;
}
