/**
 * @file usrl_tcp.c
 * @brief Robust TCP transport implementation for USRL.
 *
 * This module implements a blocking, robust TCP transport used by USRL for
 * exchanging framed messages between peers. The implementation provides:
 *  - Server and client creation helpers.
 *  - Robust blocking send/recv helpers that handle EINTR and partial I/O.
 *  - Stream framing helpers (length-prefixed exchange).
 *  - Accept helper with short timeout for graceful server loops.
 *
 * The send/recv helpers are careful to:
 *  - Retry on EINTR.
 *  - Avoid SIGPIPE via MSG_NOSIGNAL where supported.
 *  - Return partial counts for EOF scenarios where appropriate.
 *
 * Thread-safety: sockets returned are standard POSIX sockets; callers are
 * responsible for synchronization if shared across threads.
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
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <stdio.h>

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */
/**
 * @brief Enable TCP_NODELAY (disable Nagle) on a socket.
 *
 * Disabling Nagle is useful for low-latency transports where small writes
 * must not be coalesced. This helper is best-effort; errors from setsockopt
 * are ignored by design.
 *
 * @param fd Socket file descriptor.
 */
static void set_tcp_nodelay(int fd) {
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

/* =============================================================================
 * SERVER FACTORY
 * =============================================================================
 */
/**
 * @brief Create a TCP server transport.
 *
 * Creates a listening TCP socket bound to the provided host:port and returns
 * an allocated usrl_transport_t context on success.
 *
 * Behavior details:
 *  - The returned transport is configured blocking.
 *  - SO_REUSEADDR and SO_REUSEPORT are set to ease rapid restarts.
 *  - The listening socket uses a small receive timeout (100ms) so that
 *    accept() returns periodically to allow graceful shutdown checks.
 *
 * @param host IPv4 address string to bind to (NULL or "0.0.0.0" to bind all).
 * @param port TCP port to bind to.
 * @param ring_size Ignored by TCP transport (present for API parity).
 * @param mode Ignored by TCP transport (present for API parity).
 * @return Pointer to an allocated usrl_transport_t on success, or NULL on error.
 */
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

    /* 1. Create blocking socket */
    ctx->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->sockfd == -1) goto err;

    int opt = 1;
    setsockopt(ctx->sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(ctx->sockfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    /* 2. Bind */
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if(inet_pton(AF_INET, host ? host : "0.0.0.0", &addr.sin_addr) != 1) goto err;

    if (bind(ctx->sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) goto err;
    ctx->addr = addr;

    /* 3. Listen */
    if (listen(ctx->sockfd, 128) == -1) goto err;

    /* 4. Set accept timeout (100ms) for graceful shutdown loop */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    setsockopt(ctx->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return (usrl_transport_t*)ctx;

err:
    if (ctx->sockfd != -1) close(ctx->sockfd);
    free(ctx);
    return NULL;
}

/* =============================================================================
 * CLIENT FACTORY
 * =============================================================================
 */
/**
 * @brief Create a TCP client transport and connect to host:port.
 *
 * Creates a TCP socket, optionally sets TCP_NODELAY and performs a blocking
 * connect. On success returns an allocated usrl_transport_t bound to the
 * connected socket.
 *
 * @param host IPv4 address string of the remote host.
 * @param port Remote TCP port.
 * @param ring_size Ignored by TCP transport (API parity).
 * @param mode Ignored by TCP transport (API parity).
 * @return Pointer to an allocated usrl_transport_t on success, or NULL on error.
 */
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

    /* 1. Create blocking socket */
    ctx->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->sockfd == -1) goto err;

    set_tcp_nodelay(ctx->sockfd);

    /* 2. Address */
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) goto err;

    /* 3. Connect (Blocking) */
    if (connect(ctx->sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        goto err;
    }

    ctx->addr = addr;
    return (usrl_transport_t*)ctx;

err:
    if (ctx->sockfd != -1) close(ctx->sockfd);
    free(ctx);
    return NULL;
}

/* =============================================================================
 * ACCEPT
 * =============================================================================
 */
/**
 * @brief Accept a new client connection from a server transport.
 *
 * This wraps accept() and returns a newly-allocated client transport context
 * pointing to the accepted socket. The server socket is configured with a
 * short SO_RCVTIMEO (100ms) so accept() will periodically return on timeout
 * allowing the server loop to perform maintenance or shutdown.
 *
 * @param server Pointer to server usrl_transport_t (listening socket).
 * @param client_out Out parameter receiving allocated client transport pointer.
 * @return 0 on success, -1 on timeout or error. Caller should inspect errno
 *         to distinguish timeout vs other errors.
 */
int usrl_tcp_accept_impl(usrl_transport_t *server, usrl_transport_t **client_out) {
    /* accept() blocks for 100ms (SO_RCVTIMEO) */
    int client_fd = accept(server->sockfd, NULL, NULL);
    
    if (client_fd == -1) {
        return -1; /* Timeout or error (check errno in caller) */
    }

    set_tcp_nodelay(client_fd);

    struct usrl_transport_ctx *client = calloc(1, sizeof(*client));
    if (!client) {
        close(client_fd);
        return -1;
    }

    client->type = USRL_TRANS_TCP;
    client->is_server = false;
    client->sockfd = client_fd;
    
    *client_out = (usrl_transport_t*)client;
    return 0;
}

/* =============================================================================
 * SEND (BLOCKING, ROBUST)
 * =============================================================================
 */
/**
 * @brief Robust blocking send that fully writes the requested buffer.
 *
 * This function attempts to send exactly len bytes, looping on partial writes
 * and EINTR. It also uses MSG_NOSIGNAL to avoid SIGPIPE on peer disconnects.
 *
 * @param ctx Transport context with valid connected socket.
 * @param data Pointer to bytes to send.
 * @param len Number of bytes to send.
 * @return Number of bytes written on success (== len), or -1 on error.
 */
ssize_t usrl_tcp_send(usrl_transport_t *ctx, const void *data, size_t len) {
    if (!ctx) return -1;
    
    size_t total = 0;
    const uint8_t *ptr = data;
    
    while (total < len) {
        // Use MSG_NOSIGNAL to avoid SIGPIPE crash on client disconnect
        ssize_t n = send(ctx->sockfd, ptr + total, len - total, MSG_NOSIGNAL);
        
        if (n > 0) {
            total += n;
        } else {
            if (errno == EINTR) continue; /* Retry on signal interrupt */
            return -1; /* Real error */
        }
    }
    
    return total;
}

/* =============================================================================
 * RECV (BLOCKING, ROBUST)
 * =============================================================================
 */
/**
 * @brief Robust blocking recv that fully reads the requested buffer.
 *
 * This function loops until len bytes have been received or EOF/error occurs.
 * On EOF it returns the number of bytes read so far (0 if no data read).
 *
 * @param ctx Transport context with valid connected socket.
 * @param data Buffer to receive into.
 * @param len Number of bytes to read.
 * @return Number of bytes read (may be < len on EOF), or -1 on error.
 */
ssize_t usrl_tcp_recv(usrl_transport_t *ctx, void *data, size_t len) {
    if (!ctx) return -1;
    
    size_t total = 0;
    uint8_t *ptr = data;
    
    while (total < len) {
        ssize_t n = recv(ctx->sockfd, ptr + total, len - total, 0);
        
        if (n > 0) {
            total += n;
        } else if (n == 0) {
            /* EOF. Return bytes read so far. 
               If total==0, it returns 0 (Clean EOF). 
               If total>0, it returns partial count (Short Read). */
            return total;
        } else {
            if (errno == EINTR) continue; /* Retry on signal interrupt */
            return -1; /* Real error */
        }
    }
    
    return total;
}

/* =============================================================================
 * STREAM RECV (BLOCKING, ROBUST)
 * =============================================================================
 */
/**
 * @brief Send a length-prefixed frame (network-order u32 length then payload).
 *
 * The stream helpers implement a simple framing protocol:
 *  - Sender: send(u32 length in network byte order), send(payload)
 *  - Receiver: recv(u32 length), recv(payload)
 *
 * usrl_tcp_stream_recv is the sender side for framed RPC-style exchanges: it
 * first transmits the frame length then the payload.
 *
 * @param ctx Transport context.
 * @param data Pointer to payload to send.
 * @param len Payload length in bytes (must fit in uint32_t).
 * @return 0 on success, -1/-2 on failure (see implementation).
 */
ssize_t usrl_tcp_stream_recv(usrl_transport_t *ctx, void *data, size_t len) {
    if(ctx == NULL || data == NULL || len == 0) {
        return -1;
    }

    uint32_t netlen = htonl((uint32_t)len);

    if(usrl_tcp_send(ctx, &netlen, sizeof(netlen)) != sizeof(netlen)) {
        return -1;
    }

    if(usrl_tcp_send(ctx, data, len) != (ssize_t)len) {
        return -2;
    }

    return 0;
}

/*=============================================================================
 * STREAM SEND (BLOCKING, ROBUST)
 * =============================================================================
 */
/**
 * @brief Receive a length-prefixed frame (blocking).
 *
 * Reads a network-order u32 length prefix, converts it to host order and then
 * reads that many bytes into the provided buffer. Ensures the provided buffer
 * is large enough for the incoming frame.
 *
 * @param ctx Transport context.
 * @param data Buffer to receive into.
 * @param len Size of provided buffer in bytes.
 * @return Number of bytes received (frame length) on success.
 *         Negative values indicate errors:
 *           -1 framing/header read failed,
 *           -2 frame too large for provided buffer,
 *           -3 payload read failed.
 */
ssize_t usrl_tcp_stream_send(usrl_transport_t *ctx, const void *data, size_t len) {
    if(ctx == NULL || data == NULL || len == 0) {
        return -1;
    }
    
    uint32_t netlen = 0;

    if(usrl_tcp_recv(ctx, &netlen, sizeof(netlen)) != sizeof(netlen)) {
        return -1;
    }

    netlen = ntohl(netlen);
    if(netlen > len) {
        return -2;
    }

    if(usrl_tcp_recv(ctx, data, netlen) != (ssize_t)netlen) {
        return -3;
    }

    return netlen;
}


/* =============================================================================
 * DESTROY
 * =============================================================================
 */
/**
 * @brief Destroy and free a transport context, closing the underlying socket.
 *
 * Safe to call with NULL. After this call the transport pointer must not be
 * used.
 *
 * @param ctx_ Pointer to transport context returned from create_client/server.
 */
void usrl_tcp_destroy(usrl_transport_t *ctx_) {
    struct usrl_transport_ctx *ctx = (struct usrl_transport_ctx*)ctx_;
    if (!ctx) return;
    
    if (ctx->sockfd != -1) {
        
        close(ctx->sockfd);
    }
    free(ctx);
}

