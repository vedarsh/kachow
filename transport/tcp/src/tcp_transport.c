/* =============================================================================
 * USRL TCP TRANSPORT IMPLEMENTATION (BLOCKING I/O)
 * =============================================================================
 */

#define _GNU_SOURCE

#include "usrl_tcp.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <stdio.h>

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */
static int set_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void set_tcp_nodelay(int fd) {
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

/* =============================================================================
 * SERVER FACTORY
 * ============================================================================= */
usrl_transport_t *usrl_tcp_create_server(
    const char      *host, 
    int              port, 
    size_t           ring_size, 
    usrl_ring_mode_t mode
) {
    (void)ring_size; (void)mode;
    
    struct usrl_transport_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->type = USRL_TRANS_TCP;
    ctx->is_server = true;

    /* 1. Create listener (NON-BLOCKING for accept loop) */
    ctx->sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (ctx->sockfd == -1) goto err;

    int opt = 1;
    setsockopt(ctx->sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* 2. Bind */
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host ? host : "0.0.0.0", &addr.sin_addr);

    if (bind(ctx->sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) goto err;
    ctx->addr = addr;

    /* 3. Listen */
    if (listen(ctx->sockfd, 128) == -1) goto err;

    return (usrl_transport_t*)ctx;

err:
    if (ctx->sockfd != -1) close(ctx->sockfd);
    free(ctx);
    return NULL;
}

/* =============================================================================
 * CLIENT FACTORY
 * ============================================================================= */
usrl_transport_t *usrl_tcp_create_client(
    const char      *host, 
    int              port, 
    size_t           ring_size, 
    usrl_ring_mode_t mode
) {
    (void)ring_size; (void)mode;

    struct usrl_transport_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->type = USRL_TRANS_TCP;
    ctx->is_server = false;

    /* 1. Create socket (NON-BLOCKING for connect) */
    ctx->sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (ctx->sockfd == -1) goto err;

    set_tcp_nodelay(ctx->sockfd);

    /* 2. Address */
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) goto err;

    /* 3. Connect (Non-blocking) */
    if (connect(ctx->sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        if (errno != EINPROGRESS) goto err;
    }

    /* 4. Wait for connect */
    struct pollfd pfd = { .fd = ctx->sockfd, .events = POLLOUT };
    if (poll(&pfd, 1, 5000) <= 0) goto err;

    /* 5. Check error */
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(ctx->sockfd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) goto err;

    /* 6. SWITCH TO BLOCKING FOR DATA TRANSFER */
    set_blocking(ctx->sockfd);

    ctx->addr = addr;
    return (usrl_transport_t*)ctx;

err:
    if (ctx->sockfd != -1) close(ctx->sockfd);
    free(ctx);
    return NULL;
}

/* =============================================================================
 * ACCEPT
 * ============================================================================= */
int usrl_tcp_accept_impl(usrl_transport_t *server, usrl_transport_t **client_out) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    /* Accept (Non-blocking because server socket is non-blocking) */
    int client_fd = accept(server->sockfd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd == -1) return -1;

    /* SWITCH CLIENT TO BLOCKING MODE */
    set_blocking(client_fd);
    set_tcp_nodelay(client_fd);

    struct usrl_transport_ctx *client = calloc(1, sizeof(*client));
    if (!client) {
        close(client_fd);
        return -1;
    }

    client->type = USRL_TRANS_TCP;
    client->is_server = false;
    client->sockfd = client_fd;
    client->addr = client_addr;
    
    *client_out = (usrl_transport_t*)client;
    return 0;
}

/* =============================================================================
 * SEND (BLOCKING)
 * ============================================================================= */
ssize_t usrl_tcp_send(usrl_transport_t *ctx, const void *data, size_t len) {
    if (!ctx) return -1;
    
    size_t total = 0;
    const uint8_t *ptr = data;
    
    while (total < len) {
        /* No MSG_DONTWAIT -> Blocking */
        ssize_t n = send(ctx->sockfd, ptr + total, len - total, MSG_NOSIGNAL);
        
        if (n <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += n;
    }
    return total;
}

/* =============================================================================
 * RECV (BLOCKING)
 * ============================================================================= */
ssize_t usrl_tcp_recv(usrl_transport_t *ctx, void *data, size_t len) {
    if (!ctx) return -1;
    
    size_t total = 0;
    uint8_t *ptr = data;
    
    while (total < len) {
        /* No MSG_DONTWAIT -> Blocking */
        ssize_t n = recv(ctx->sockfd, ptr + total, len - total, 0);
        
        if (n > 0) {
            total += n;
        } else if (n == 0) {
            return 0; /* EOF */
        } else {
            if (errno == EINTR) continue;
            return -1;
        }
    }
    return total;
}

/* =============================================================================
 * DESTROY
 * ============================================================================= */
void usrl_tcp_destroy(usrl_transport_t *ctx_) {
    struct usrl_transport_ctx *ctx = (struct usrl_transport_ctx*)ctx_;
    if (!ctx) return;
    
    if (ctx->sockfd != -1) {
        shutdown(ctx->sockfd, SHUT_RDWR);
        close(ctx->sockfd);
    }
    free(ctx);
}
