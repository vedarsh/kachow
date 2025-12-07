/* =============================================================================
 * USRL NETWORK COMMON DISPATCHER
 * =============================================================================
 *
 * This file implements the unified public API entry points.
 * It is the ONLY file that should define usrl_trans_* functions.
 *
 * It dispatches calls to specific backends (TCP, UDP, RDMA) based on the
 * transport type stored in the context.
 * =============================================================================
 */

#include "usrl_net.h"
#include "usrl_tcp.h"
#include "usrl_ring.h"
/* #include "usrl_udp.h" */

#include <stddef.h>

/* --------------------------------------------------------------------------
 * Factory Dispatcher
 * -------------------------------------------------------------------------- */
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
        
        /* Future expansion:
        case USRL_TRANS_UDP:
            return usrl_udp_create(...);
        */

        default:
            return NULL;
    }
}

/* --------------------------------------------------------------------------
 * Accept Dispatcher
 * -------------------------------------------------------------------------- */
int usrl_trans_accept(usrl_transport_t *server, usrl_transport_t **client_out) {
    if (!server) return -1;

    /* Safe cast because we know the layout (type is first member) */
    usrl_transport_type_t type = ((struct usrl_transport_ctx*)server)->type;

    switch (type) {
        case USRL_TRANS_TCP:
            return usrl_tcp_accept_impl(server, client_out);
            
        default:
            return -1;
    }
}

/* --------------------------------------------------------------------------
 * Send Dispatcher
 * -------------------------------------------------------------------------- */
ssize_t usrl_trans_send(usrl_transport_t *ctx, const void *data, size_t len) {
    if (!ctx) return -1;

    usrl_transport_type_t type = ((struct usrl_transport_ctx*)ctx)->type;

    switch (type) {
        case USRL_TRANS_TCP:
            return usrl_tcp_send(ctx, data, len);

        /* 
        case USRL_TRANS_UDP:
            return usrl_udp_send(ctx, data, len);
        */

        default:
            return -1;
    }
}

/* --------------------------------------------------------------------------
 * Recv Dispatcher
 * -------------------------------------------------------------------------- */
ssize_t usrl_trans_recv(usrl_transport_t *ctx, void *data, size_t len) {
    if (!ctx) return -1;

    usrl_transport_type_t type = ((struct usrl_transport_ctx*)ctx)->type;

    switch (type) {
        case USRL_TRANS_TCP:
            return usrl_tcp_recv(ctx, data, len);

        default:
            return -1;
    }
}

/* --------------------------------------------------------------------------
 * Destroy Dispatcher
 * -------------------------------------------------------------------------- */
void usrl_trans_destroy(usrl_transport_t *ctx) {
    if (!ctx) return;

    usrl_transport_type_t type = ((struct usrl_transport_ctx*)ctx)->type;

    switch (type) {
        case USRL_TRANS_TCP:
            usrl_tcp_destroy(ctx);
            break;

        default:
            /* Just free the memory if we don't know the type */
            /* (Though this is risky if the struct has other resources) */
            break; 
    }
}
