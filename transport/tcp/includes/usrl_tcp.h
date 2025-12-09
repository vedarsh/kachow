#ifndef USRL_TCP_H
#define USRL_TCP_H

/* =============================================================================
 * USRL TCP TRANSPORT — CONCRETE IMPLEMENTATION
 * =============================================================================
 *
 * This header defines the concrete TCP transport context and specific
 * implementation functions that the dispatcher (usrl_net_common.c) calls.
 *
 * Design:
 *   - struct usrl_transport_ctx : Concrete definition visible here for
 *     type-checking in the dispatcher (first member = type for C polymorphism)
 *   - TCP-specific factory functions (server/client creation)
 *   - TCP-specific method implementations (send/recv/accept/destroy)
 *
 * The transport layer uses USRL ring buffers (UsrlPublisher/UsrlSubscriber)
 * for zero-copy buffering between socket I/O and application payloads.
 * =============================================================================
 */

#include "usrl_net.h"     /* defines usrl_ring_mode_t, ssize_t */
#include "usrl_core.h"    /* RingDesc, SlotHeader */
#include "usrl_ring.h"    /* UsrlPublisher, UsrlSubscriber */


#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

/* =============================================================================
 * CONCRETE TRANSPORT CONTEXT (TCP)
 * =============================================================================
 *
 * This struct definition is visible here so that usrl_net_common.c can:
 * 1) Cast the opaque usrl_transport_t* to access 'type' (first member)
 * 2) Safely dispatch to TCP-specific implementations
 *
 * Ring Buffers:
 *   - pub: Publisher handle for SEND path (app -> ring -> socket)
 *   - sub: Subscriber handle for RECV path (socket -> ring -> app)
 * =============================================================================
 */
/* transport/includes/usrl_tcp.h */
struct usrl_transport_ctx {
    usrl_transport_type_t type;           /* Must be first: USRL_TRANS_TCP */
    
    /* USRL Ring Handles (matches your ring_swmr.c / ring_mwmr.c) */
    UsrlPublisher        *pub;            /* SEND path */
    UsrlSubscriber       *sub;            /* RECV path */
    
    /* Socket State */
    int                   sockfd;
    struct sockaddr_in    addr;
    bool                  is_server;
    uint16_t              pub_id;
    
    /* USRL Core (REQUIRED for ring init) */
    void                 *core_base;      /* Mapped SHM region */
    char                 *tcp_topic;      /* "tcp_client_123" topic name */
};


/* =============================================================================
 * TCP FACTORY FUNCTIONS
 * =============================================================================
 *
 * These are called by the dispatcher (usrl_trans_create) based on transport type.
 * =============================================================================
 */

/**
 * usrl_tcp_create_server()
 *
 * Creates a TCP listener with an associated USRL ring buffer for buffering
 * incoming connections and data.
 *
 * Steps:
 * 1) socket(), setsockopt(SO_REUSEADDR), bind(), listen()
 * 2) fcntl(O_NONBLOCK) for non-blocking accept/send/recv
 * 3) Initialize USRL Publisher/Subscriber on a TCP-specific topic
 * 4) Return concrete context
 *
 * @param host       Bind address (NULL = INADDR_ANY)
 * @param port       Listen port
 * @param ring_size  Total ring buffer size (slots * slot_size)
 * @param mode       USRL_SWMR or USRL_MWMR (server typically SWMR)
 * @return Allocated context or NULL on failure
 */
usrl_transport_t *usrl_tcp_create_server(
    const char      *host,
    int              port,
    size_t           ring_size,
    usrl_ring_mode_t mode
);

/**
 * usrl_tcp_create_client()
 *
 * Creates a TCP client connection with ring buffering.
 *
 * Steps:
 * 1) socket(), connect() (non-blocking with retry logic)
 * 2) setsockopt(TCP_NODELAY) for low latency
 * 3) fcntl(O_NONBLOCK)
 * 4) Initialize ring buffers
 *
 * @param host       Remote host
 * @param port       Remote port
 * @param ring_size  Ring buffer size
 * @param mode       Ring mode
 * @return Allocated context or NULL on failure
 */
usrl_transport_t *usrl_tcp_create_client(
    const char      *host,
    int              port,
    size_t           ring_size,
    usrl_ring_mode_t mode
);

/* =============================================================================
 * TCP METHOD IMPLEMENTATIONS
 * =============================================================================
 *
 * Called by usrl_net_common.c dispatcher after type-checking.
 * =============================================================================
 */

/**
 * usrl_tcp_accept_impl()
 *
 * Non-blocking accept() wrapper. Creates a new client context with fresh
 * ring buffers for the accepted connection.
 *
 * @param server     Server context (listener)
 * @param client_out Receives new client context
 * @return 0 success, -1 failure
 */
int usrl_tcp_accept_impl(usrl_transport_t *server, usrl_transport_t **client_out);

/**
 * usrl_tcp_send()
 *
 * SEND path: App -> Ring(Publisher) -> Socket (non-blocking send)
 *
 * 1) Publish to ring buffer (usrl_pub_publish)
 * 2) Drain ring to socket via non-blocking send()
 *
 * @return Bytes sent (>0) or -1 error
 */
ssize_t usrl_tcp_send(usrl_transport_t *ctx, const void *data, size_t len);

/**
 * usrl_tcp_recv()
 *
 * RECV path: Socket (non-blocking recv) -> Ring(Subscriber) -> App
 *
 * 1) recv() into temp buffer
 * 2) Publish received data to ring
 * 3) App reads via usrl_sub_next()
 *
 * @return Bytes received or -1 error
 */
ssize_t usrl_tcp_recv(usrl_transport_t *ctx, void *data, size_t len);

/**
 * usrl_tcp_stream_recv()
 * 
 * RECV path for stream-oriented reading (e.g., file transfer).
 * This function attempts to read exactly 'len' bytes, blocking as needed.
 * 
 * @param ctx  Transport context
 * @param data Destination buffer
 * @param len  Number of bytes to read
 * @return Bytes received or -1 error 
 */
ssize_t usrl_tcp_stream_recv(usrl_transport_t *ctx, void *data, size_t len);

/**
 * usrl_tcp_stream_send()
 * 
 * SEND path for stream-oriented writing (e.g., file transfer).
 * This function attempts to send exactly 'len' bytes, blocking as needed.
 * 
 * @param ctx  Transport context
 * @param data Source buffer
 * @param len  Number of bytes to send
 * @return Bytes sent or -1 error
 */
ssize_t usrl_tcp_stream_send(usrl_transport_t *ctx, const void *data, size_t len);


/**
 * usrl_tcp_destroy()
 *
 * Cleanup: close socket, destroy ring handles, free context
 */
void usrl_tcp_destroy(usrl_transport_t *ctx);

#endif /* USRL_TCP_H */
