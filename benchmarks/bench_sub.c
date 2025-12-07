#include "usrl_ring.h"
#include "usrl_core.h"
#include <stdio.h>
#include <time.h>

// Print stats every 100k messages
#define STAT_INTERVAL 100000

int main(int argc, char **argv) {
    if (argc < 2) {
        // Print to stderr so it shows up in logs if redirected
        fprintf(stderr, "Usage: %s <topic>\n", argv[0]);
        return 1;
    }

    void *core = usrl_core_map("/usrl_core", 128 * 1024 * 1024);
    if (!core) return 1;

    UsrlSubscriber sub;
    usrl_sub_init(&sub, core, argv[1]);

    // We won't use this buffer for benchmarks to save "printing to screen" time,
    // but we need it for the API.
    uint8_t buf[8192];
    uint16_t pid;
    long count = 0;

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // fprintf(stderr, "[BENCH] Subscriber listening on '%s'...\n", argv[1]);

    while (1) {
        int n = usrl_sub_next(&sub, buf, sizeof(buf), &pid);

        if (n > 0) {
            count++;
            if (count % STAT_INTERVAL == 0) {
                clock_gettime(CLOCK_MONOTONIC, &now);
                double elapsed = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;

                // IMPORTANT: Use stderr and flush immediately to ensure it hits the log file
                fprintf(stderr, "[SUB] Rate: %.2f M msg/s | Last ID: %d\n",
                       (count / 1e6) / elapsed, pid);
                fflush(stderr);
            }
            continue;
        }

        if (n == 0) {
             // Busy wait is better for benchmarking max throughput than sleeping
             __asm__ volatile("nop");
        }
    }
    return 0;
}
