#ifndef USRL_NET_H
#define USRL_NET_H

/* =============================================================================
 * USRL NETWORK TRANSPORT API
 * =============================================================================
 *
 * Unified interface for TCP/UDP/RDMA transports with zero-copy ring buffering.
 * =============================================================================
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#include "usrl_core.h"

/* --------------------------------------------------------------------------
 * Ring Mode Compatibility Layer
 * 
 * Maps to USRL_RING_TYPE_SWMR/MWMR constants from core.
 * Defined here so transport layer is self-contained.
 * -------------------------------------------------------------------------- */
typedef enum {
    USRL_SWMR = 0,  /* Single Writer, Multi Reader */
    USRL_MWMR = 1   /* Multi Writer, Multi Reader */
} usrl_ring_mode_t;

/* --------------------------------------------------------------------------
 * Transport Protocol Types
 * -------------------------------------------------------------------------- */
typedef enum {
    USRL_TRANS_TCP  = 1,
    USRL_TRANS_UDP  = 2,
    USRL_TRANS_RDMA = 3
} usrl_transport_type_t;

/* --------------------------------------------------------------------------
 * Opaque Transport Handle
 * 
 * Concrete definition is in transport-specific headers (usrl_tcp.h).
 * Dispatcher casts to access 'type' field (C polymorphism pattern).
 * -------------------------------------------------------------------------- */
typedef struct usrl_transport_ctx usrl_transport_t;

/* =============================================================================
 * PUBLIC API (Unified across all transports)
 * =============================================================================
 */
usrl_transport_t *usrl_trans_create(
    usrl_transport_type_t type,
    const char           *host,
    int                   port,
    size_t                ring_size,
    usrl_ring_mode_t      mode,
    bool                  is_server
);

int usrl_trans_accept(usrl_transport_t *server, usrl_transport_t **client_out);
ssize_t usrl_trans_send(usrl_transport_t *ctx, const void *data, size_t len);
ssize_t usrl_trans_recv(usrl_transport_t *ctx, void *data, size_t len);
void usrl_trans_destroy(usrl_transport_t *ctx);

#endif /* USRL_NET_H */
