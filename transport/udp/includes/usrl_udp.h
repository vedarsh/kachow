#ifndef USRL_UDP_H
#define USRL_UDP_H

#include "usrl_net.h"   /* defines usrl_transport_t */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* =============================================================================
 * UDP FACTORY FUNCTIONS
 * =============================================================================
 */

usrl_transport_t *usrl_udp_create_server(
    const char      *host,
    int              port,
    size_t           ring_size,
    usrl_ring_mode_t mode
);

usrl_transport_t *usrl_udp_create_client(
    const char      *host,
    int              port,
    size_t           ring_size,
    usrl_ring_mode_t mode
);

/* =============================================================================
 * UDP METHOD IMPLEMENTATIONS
 * =============================================================================
 */

ssize_t usrl_udp_send(usrl_transport_t *ctx, const void *data, size_t len);
ssize_t usrl_udp_recv(usrl_transport_t *ctx, void *data, size_t len);

ssize_t usrl_udp_stream_send(usrl_transport_t *ctx, const void *data, size_t len);
ssize_t usrl_udp_stream_recv(usrl_transport_t *ctx, void *data, size_t len);

void usrl_udp_destroy(usrl_transport_t *ctx);

#endif /* USRL_UDP_H */
