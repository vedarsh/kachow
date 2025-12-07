/* =============================================================================
 * USRL TCP SERVER BENCHMARK (Persistent)
 * =============================================================================
 */

#include "usrl_core.h"
#include "usrl_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#define PAYLOAD_SIZE    4096
#define DEFAULT_PORT    8080

volatile sig_atomic_t running = 1;
void sighandler(int sig) { (void)sig; running = 0; }

ssize_t recv_complete(usrl_transport_t *ctx, void *buf, size_t len) {
    size_t total = 0;
    uint8_t *ptr = buf;
    while (total < len && running) {
        ssize_t n = usrl_trans_recv(ctx, ptr + total, len - total);
        if (n > 0) total += n;
        else if (n == 0) return 0; // EOF
        else if (errno == EINTR) continue;
        else return -1;
    }
    return total;
}

ssize_t send_complete(usrl_transport_t *ctx, const void *buf, size_t len) {
    size_t total = 0;
    const uint8_t *ptr = buf;
    while (total < len && running) {
        ssize_t n = usrl_trans_send(ctx, ptr + total, len - total);
        if (n > 0) total += n;
        else if (errno == EINTR) continue;
        else return -1;
    }
    return total;
}

int main(int argc, char *argv[]) {
    int port = argc > 1 ? atoi(argv[1]) : DEFAULT_PORT;
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    
    printf("[BENCH] TCP Server listening on port %d...\n", port);
    
    usrl_transport_t *server = usrl_trans_create(
        USRL_TRANS_TCP, NULL, port, 0, USRL_SWMR, true);
    
    if (!server) return 1;
    
    uint8_t *payload = malloc(PAYLOAD_SIZE);
    memset(payload, 0xBB, PAYLOAD_SIZE);

    /* OUTER LOOP: Accept new clients */
    while (running) {
        usrl_transport_t *client = NULL;
        
        // Wait for connection
        while (running) {
            int rc = usrl_trans_accept(server, &client);
            if (rc == 0 && client != NULL) break;
            // timeout/retry
        }

        if (!running) break;

        // printf("[DEBUG] Client connected.\n");

        /* INNER LOOP: Echo until disconnect */
        while (running) {
            ssize_t n = recv_complete(client, payload, PAYLOAD_SIZE);
            if (n != PAYLOAD_SIZE) {
                // Client disconnected (likely nc -z or finished bench)
                break;
            }
            
            n = send_complete(client, payload, PAYLOAD_SIZE);
            if (n != PAYLOAD_SIZE) break;
        }

        usrl_trans_destroy(client);
        // printf("[DEBUG] Client disconnected, waiting for next...\n");
    }
    
    printf("[BENCH] TCP Server shutting down.\n");
    
    free(payload);
    usrl_trans_destroy(server);
    return 0;
}
