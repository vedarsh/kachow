/* =============================================================================
 * USRL UDP BENCHMARK SERVER
 * =============================================================================
 */

#define _GNU_SOURCE
#include "usrl_core.h"
#include "usrl_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define PAYLOAD_SIZE    4096
#define DEFAULT_PORT    9090

volatile sig_atomic_t running = 1;

void sighandler(int sig) { 
    (void)sig; 
    running = 0; 
}

int main(int argc, char *argv[]) {
    int port = argc > 1 ? atoi(argv[1]) : DEFAULT_PORT;

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    printf("[UDP-BENCH] UDP Server listening on port %d...\n", port);

    usrl_transport_t *server = usrl_trans_create(
        USRL_TRANS_UDP, NULL, port, 0, USRL_SWMR, true);

    if (!server) return 1;

    uint8_t payload[PAYLOAD_SIZE];

    while (running) {
        ssize_t n = usrl_trans_recv(server, payload, PAYLOAD_SIZE);
        if (n > 0) {
            usrl_trans_send(server, payload, n);
        }
    }

    printf("[UDP-BENCH] UDP Server shutting down.\n");
    usrl_trans_destroy(server);
    return 0;
}
