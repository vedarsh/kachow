/* =============================================================================
 * USRL TCP SERVER BENCHMARK (Silent)
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
        else if (n == 0) return 0;
        else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
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
        else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
        else return -1;
    }
    return total;
}

int main(int argc, char *argv[]) {
    int port = argc > 1 ? atoi(argv[1]) : DEFAULT_PORT;
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    
    /* Clean Start Log */
    printf("[BENCH] TCP Server listening on port %d...\n", port);
    
    usrl_transport_t *server = usrl_trans_create(
        USRL_TRANS_TCP, NULL, port, 0, USRL_SWMR, true);
    
    if (!server) return 1;
    
    usrl_transport_t *client = NULL;
    while (running && usrl_trans_accept(server, &client) != 0) {
        usleep(10000); 
    }
    
    if (!client || !running) {
        usrl_trans_destroy(server);
        return 0;
    }
    
    uint8_t *payload = malloc(PAYLOAD_SIZE);
    memset(payload, 0xBB, PAYLOAD_SIZE);
    
    /* Silent Benchmark Loop */
    while (running) {
        if (recv_complete(client, payload, PAYLOAD_SIZE) != PAYLOAD_SIZE) break;
        if (send_complete(client, payload, PAYLOAD_SIZE) != PAYLOAD_SIZE) break;
    }
    
    printf("[BENCH] TCP Server session ended.\n");
    
    free(payload);
    usrl_trans_destroy(client);
    usrl_trans_destroy(server);
    return 0;
}
