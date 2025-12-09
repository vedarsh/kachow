#define _GNU_SOURCE
#include "usrl_core.h"
#include "usrl_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#define PAYLOAD_SIZE    4096
#define DEFAULT_PORT    9090  // ← FIXED: Match bash script
#define STAT_INTERVAL   100000

volatile sig_atomic_t running = 1;
uint64_t total_reqs = 0;

void sighandler(int sig) { (void)sig; running = 0; }

int main(int argc, char *argv[]) {
    int port = argc > 1 ? atoi(argv[1]) : DEFAULT_PORT;
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    printf("[UDP-SERVER] UDP Server listening on port %d...\n", port);

    usrl_transport_t *server = usrl_trans_create(
        USRL_TRANS_UDP, NULL, port, 0, USRL_SWMR, true);
    if (!server) return 1;

    uint8_t payload[PAYLOAD_SIZE];
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (running) {
        ssize_t n = usrl_trans_recv(server, payload, PAYLOAD_SIZE);
        if (n > 0) {
            total_reqs++;
            usrl_trans_send(server, payload, n);  // Echo back

            // Print stats
            if (total_reqs % STAT_INTERVAL == 0) {
                clock_gettime(CLOCK_MONOTONIC, &now);
                double elapsed = (now.tv_sec - start.tv_sec) + 
                               (now.tv_nsec - start.tv_nsec) / 1e9;
                printf("[UDP-SERVER] %lu reqs (%.2f M/sec)\n", 
                       total_reqs, total_reqs / 1e6 / elapsed);
            }
        }
    }

    printf("[UDP-SERVER] Total: %lu requests. Shutting down.\n", total_reqs);
    usrl_trans_destroy(server);
    return 0;  // ← Clean exit
}
