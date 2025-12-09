/**
 * @file usrl_net_common.c
 * @brief Transport dispatcher: unified public API for transport backends.
 *
 * This module exposes the public usrl_trans_* API and dispatches calls to
 * concrete transport implementations (TCP, UDP, RDMA, ...). It is the sole
 * place defining the public transport entry points so backend add-ons only
 * need to implement their specific functions.
 *
 * Responsibilities:
 *  - Create client/server transports via usrl_trans_create()
 *  - Accept new connections via usrl_trans_accept()
 *  - Send/receive raw bytes via usrl_trans_send() / usrl_trans_recv()
 *  - Send/receive framed streams via usrl_trans_stream_send() /
 *    usrl_trans_stream_recv()
 *  - Destroy transport contexts via usrl_trans_destroy()
 *
 * Notes:
 *  - The dispatcher performs a type switch on the first field of the
 *    usrl_transport_ctx, so transport implementations must set the 'type'
 *    member when allocating their context.
 *  - Functions return transport-specific error codes as documented on each
 *    function below; callers should consult the backend for precise semantics.
 */

#include "usrl_net.h"
#include "usrl_tcp.h"
#include "usrl_ring.h"
#include "usrl_udp.h"

#include <stddef.h>

/* --------------------------------------------------------------------------
 * Factory Dispatcher
 * -------------------------------------------------------------------------- */
/**
 * @brief Create a transport (client or server) for a specified backend.
 *
 * Factory that dispatches to the selected transport backend.
 *
 * @param type Transport backend type (USRL_TRANS_TCP, USRL_TRANS_UDP, ...).
 * @param host IPv4 address or hostname (backend-specific).
 * @param port TCP/UDP port number or backend-specific port.
 * @param ring_size Requested ring size (transport may ignore).
 * @param mode Ring mode (SWMR/MWMR) (transport may ignore).
 * @param is_server true to create a server/listener, false to create a client.
 * @return Allocated usrl_transport_t* on success, or NULL on error.
 */
usrl_transport_t *usrl_trans_create(
    usrl_transport_type_t type, 
    const char           *host, 
    int                   port, 
    size_t                ring_size, 
    usrl_ring_mode_t      mode, 
    bool                  is_server
) {
    switch (type) {
        case USRL_TRANS_TCP:
            if (is_server) {
                return usrl_tcp_create_server(host, port, ring_size, mode);
            } else {
                return usrl_tcp_create_client(host, port, ring_size, mode);
            }
        
        case USRL_TRANS_UDP:
            if (is_server) {
                return usrl_udp_create_server(host, port, ring_size, mode);
            } else {
                return usrl_udp_create_client(host, port, ring_size, mode);
            }

        default:
            return NULL;
    }
}

/* --------------------------------------------------------------------------
 * Accept Dispatcher
 * -------------------------------------------------------------------------- */
/**
 * @brief Accept an incoming connection from a server transport.
 *
 * The dispatcher forwards to the backend accept implementation. For TCP the
 * returned client_out is a newly-allocated transport context bound to the
 * accepted socket.
 *
 * @param server Pointer to server transport context (must be non-NULL).
 * @param client_out Out parameter to receive newly-allocated client transport.
 * @return 0 on success; -1 on timeout or error (caller may inspect errno).
 */
int usrl_trans_accept(usrl_transport_t *server, usrl_transport_t **client_out) {
    if (!server) return -1;

    /* Safe cast because we know the layout (type is first member) */
    usrl_transport_type_t type = ((struct usrl_transport_ctx*)server)->type;

    switch (type) {
        case USRL_TRANS_TCP:
            return usrl_tcp_accept_impl(server, client_out);

        case USRL_TRANS_UDP:
            /* UDP is connectionless; no accept */
            return 0;
            
        default:
            return -1;
    }
}

/* --------------------------------------------------------------------------
 * Send Dispatcher
 * -------------------------------------------------------------------------- */
/**
 * @brief Send len bytes over the transport (blocking, robust).
 *
 * Dispatches to the backend send implementation. Behaviors such as retrying
 * on EINTR, partial writes, or avoiding SIGPIPE are backend-defined.
 *
 * @param ctx Transport context returned by usrl_trans_create or accept.
 * @param data Pointer to bytes to send.
 * @param len Number of bytes to send.
 * @return Number of bytes sent (typically == len) on success, or negative on error.
 */
ssize_t usrl_trans_send(usrl_transport_t *ctx, const void *data, size_t len) {
    if (!ctx) return -1;

    usrl_transport_type_t type = ((struct usrl_transport_ctx*)ctx)->type;

    switch (type) {
        case USRL_TRANS_TCP:
            return usrl_tcp_send(ctx, data, len);

        case USRL_TRANS_UDP:
            return usrl_udp_send(ctx, data, len);

        default:
            return -1;
    }
}
/* --------------------------------------------------------------------------
 * Stream Send Dispatcher
 * -------------------------------------------------------------------------- */
/**
 * @brief Send/receive a framed stream using the backend's stream helper.
 *
 * This dispatcher forwards to the backend stream-send helper which typically
 * implements a length-prefixed exchange.
 *
 * @param ctx Transport context.
 * @param data Pointer to buffer for send/receive as defined by backend.
 * @param len Buffer length or frame size (backend-specific semantics).
 * @return Backend-specific result: non-negative success value or negative error.
 */
ssize_t usrl_trans_stream_send(usrl_transport_t *ctx, const void *data, size_t len) {
    if (!ctx) return -1;

    usrl_transport_type_t type = ((struct usrl_transport_ctx*)ctx)->type;

    switch (type)
    {
    case USRL_TRANS_TCP:
        return usrl_tcp_stream_send(ctx, data, len);

    case USRL_TRANS_UDP:
        return usrl_udp_stream_send(ctx, data, len);
    
    default:
        return -1;
    }
}

/* --------------------------------------------------------------------------
 * Recv Dispatcher
 * -------------------------------------------------------------------------- */
/**
 * @brief Receive len bytes from the transport (blocking, robust).
 *
 * Dispatches to the backend receive implementation. On some backends EOF may
 * return a short read (0 or < len). Callers must handle partial reads.
 *
 * @param ctx Transport context.
 * @param data Buffer to receive into.
 * @param len Number of bytes to read.
 * @return Number of bytes read (may be < len on EOF) or negative on error.
 */
ssize_t usrl_trans_recv(usrl_transport_t *ctx, void *data, size_t len) {
    if (!ctx) return -1;

    usrl_transport_type_t type = ((struct usrl_transport_ctx*)ctx)->type;

    switch (type) {
        case USRL_TRANS_TCP:
            return usrl_tcp_recv(ctx, data, len);
        case USRL_TRANS_UDP:
            return usrl_udp_recv(ctx, data, len);
                
        default:
            return -1;
    }
}

/* --------------------------------------------------------------------------
 * Stream Recv Dispatcher
 * -------------------------------------------------------------------------- */
/**
 * @brief Receive a framed stream using the backend's stream helper.
 *
 * Forwarding dispatcher for backend stream-receive implementations.
 *
 * @param ctx Transport context.
 * @param data Buffer to receive into.
 * @param len Size of provided buffer in bytes.
 * @return Backend-specific result: number of bytes received on success or
 *         negative error code on failure.
 */
ssize_t usrl_trans_stream_recv(usrl_transport_t *ctx, void *data, size_t len) {
    if (!ctx) return -1;

    usrl_transport_type_t type = ((struct usrl_transport_ctx*)ctx)->type;

    switch (type)
    {
    case USRL_TRANS_TCP:
        return usrl_tcp_stream_recv(ctx, data, len);
    case USRL_TRANS_UDP:
        return usrl_udp_stream_recv(ctx, data, len);

    default:
        return -1;
    }
}
        

/* --------------------------------------------------------------------------
 * Destroy Dispatcher
 * -------------------------------------------------------------------------- */
/**
 * @brief Destroy and free a transport context.
 *
 * Dispatches to the backend-specific destroy function. If the backend is
 * unknown the function performs no action (caller should free resources
 * appropriately).
 *
 * @param ctx Transport context to destroy (may be NULL).
 */
void usrl_trans_destroy(usrl_transport_t *ctx) {
    if (!ctx) return;

    usrl_transport_type_t type = ((struct usrl_transport_ctx*)ctx)->type;

    switch (type) {
        case USRL_TRANS_TCP:
            usrl_tcp_destroy(ctx);
            break;
        case USRL_TRANS_UDP:
            usrl_udp_destroy(ctx);
            break;

        default:
            /* Just free the memory if we don't know the type */
            /* (Though this is risky if the struct has other resources) */
            break; 
    }
}
