/**
 * @file usrl_udp.c
 * @brief Robust UDP transport implementation for USRL.
 *
 * This module implements a blocking UDP transport used by USRL for exchanging
 * framed or raw datagrams between peers. The implementation provides:
 *  - Server and client creation helpers.
 *  - Blocking send/recv helpers using sendto()/recvfrom().
 *  - Optional length-prefixed framing helpers (for API parity with TCP).
 *
 * UDP is message-oriented by nature:
 *  - Each recv corresponds to one datagram.
 *  - Partial reads only occur if buffer is too small.
 *
 * Thread-safety: sockets returned are standard POSIX sockets; callers are
 * responsible for synchronization if shared across threads.
 */

#define _GNU_SOURCE

#include "usrl_udp.h"
#include "usrl_tcp.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>

/* =============================================================================
 * SERVER FACTORY
 * =============================================================================
 */
/**
 * @brief Create a UDP server transport.
 *
 * Creates a UDP socket bound to host:port.
 *
 * @param host IPv4 address string to bind to (NULL or "0.0.0.0" to bind all).
 * @param port UDP port to bind to.
 * @param ring_size Ignored by UDP transport.
 * @param mode Ignored by UDP transport.
 * @return Pointer to allocated usrl_transport_t on success, or NULL on error.
 */
usrl_transport_t *usrl_udp_create_server(
    const char      *host,
    int              port,
    size_t           ring_size,
    usrl_ring_mode_t mode
) {
    (void)ring_size; (void)mode;

    struct usrl_transport_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->type = USRL_TRANS_UDP;
    ctx->is_server = true;

    ctx->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->sockfd == -1) goto err;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host ? host : "0.0.0.0", &addr.sin_addr) != 1) goto err;

    if (bind(ctx->sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) goto err;

    ctx->addr = addr;
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
 * @brief Create a UDP client transport.
 *
 * Creates a UDP socket and stores the peer address for sendto().
 *
 * @param host IPv4 address string of the remote host.
 * @param port Remote UDP port.
 * @param ring_size Ignored by UDP transport.
 * @param mode Ignored by UDP transport.
 * @return Pointer to allocated usrl_transport_t on success, or NULL on error.
 */
usrl_transport_t *usrl_udp_create_client(
    const char      *host,
    int              port,
    size_t           ring_size,
    usrl_ring_mode_t mode
) {
    (void)ring_size; (void)mode;

    struct usrl_transport_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->type = USRL_TRANS_UDP;
    ctx->is_server = false;

    ctx->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->sockfd == -1) goto err;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) goto err;

    ctx->addr = addr;
    return (usrl_transport_t*)ctx;

err:
    if (ctx->sockfd != -1) close(ctx->sockfd);
    free(ctx);
    return NULL;
}

/* =============================================================================
 * SEND (BLOCKING, ROBUST)
 * =============================================================================
 */
/**
 * @brief Blocking UDP send.
 *
 * Sends exactly one datagram to the stored peer address.
 *
 * @param ctx Transport context.
 * @param data Pointer to payload.
 * @param len Length of payload.
 * @return Number of bytes sent on success, or -1 on failure.
 */
ssize_t usrl_udp_send(usrl_transport_t *ctx, const void *data, size_t len) {
    if (!ctx || !data || len == 0) return -1;

    ssize_t n = sendto(ctx->sockfd, data, len, 0,
                       (struct sockaddr*)&ctx->addr,
                       sizeof(ctx->addr));

    return (n == (ssize_t)len) ? n : -1;
}

/* =============================================================================
 * RECV (BLOCKING, ROBUST)
 * =============================================================================
 */
/**
 * @brief Blocking UDP receive.
 *
 * Receives exactly one datagram.
 *
 * @param ctx Transport context.
 * @param data Buffer to receive into.
 * @param len Size of buffer.
 * @return Number of bytes received, or -1 on error.
 */
ssize_t usrl_udp_recv(usrl_transport_t *ctx, void *data, size_t len) {
    if (!ctx || !data || len == 0) return -1;

    socklen_t addrlen = sizeof(ctx->addr);
    ssize_t n = recvfrom(ctx->sockfd, data, len, 0,
                         (struct sockaddr*)&ctx->addr,
                         &addrlen);

    return n;
}

/* =============================================================================
 * STREAM SEND (FRAMED OVER UDP)
 * =============================================================================
 */
/**
 * @brief Send a length-prefixed UDP frame.
 *
 * Layout:
 *   [ u32 payload length | payload bytes ]
 */
ssize_t usrl_udp_stream_send(usrl_transport_t *ctx, const void *data, size_t len) {
    if (!ctx || !data || len == 0) {
        return -1;
    }

    if (len > UINT32_MAX) {
        return -2;
    }

    uint8_t frame[sizeof(uint32_t) + len];
    uint32_t netlen = htonl((uint32_t)len);

    memcpy(frame, &netlen, sizeof(netlen));
    memcpy(frame + sizeof(netlen), data, len);

    return usrl_udp_send(ctx, frame, sizeof(frame));
}

/* =============================================================================
 * STREAM RECV (FRAMED OVER UDP)
 * =============================================================================
 */
/**
 * @brief Receive a length-prefixed UDP frame.
 *
 * Expects:
 *   [ u32 payload length | payload bytes ]
 */
ssize_t usrl_udp_stream_recv(usrl_transport_t *ctx, void *data, size_t len) {
    if (!ctx || !data || len == 0) {
        return -1;
    }

    uint8_t frame[65536];
    ssize_t n = usrl_udp_recv(ctx, frame, sizeof(frame));
    if (n < (ssize_t)sizeof(uint32_t)) {
        return -1;
    }

    uint32_t netlen;
    memcpy(&netlen, frame, sizeof(netlen));
    uint32_t payload_len = ntohl(netlen);

    if (payload_len > len) {
        return -2;
    }

    if ((ssize_t)(sizeof(uint32_t) + payload_len) != n) {
        return -3;
    }

    memcpy(data, frame + sizeof(uint32_t), payload_len);
    return payload_len;
}

/* =============================================================================
 * DESTROY
 * =============================================================================
 */
/**
 * @brief Destroy and free a UDP transport context.
 *
 * @param ctx_ Pointer to transport context.
 */
void usrl_udp_destroy(usrl_transport_t *ctx_) {
    struct usrl_transport_ctx *ctx = (struct usrl_transport_ctx*)ctx_;
    if (!ctx) return;

    if (ctx->sockfd != -1) {
        close(ctx->sockfd);
    }

    free(ctx);
}
