#ifndef USRL_RING_H
#define USRL_RING_H

/* --------------------------------------------------------------------------
 * USRL Ring API
 * This header defines the publisher/subscriber interfaces for both:
 *  - SWMR (Single Writer, Multi Reader)
 *  - MWMR (Multi Writer, Multi Reader)
 * -------------------------------------------------------------------------- */

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "usrl_core.h"

/* 
 * RING RETURN CODES 
 * Namespaced to avoid conflicts with logging/system macros
 */
#define USRL_RING_OK          0
#define USRL_RING_ERROR      -1
#define USRL_RING_FULL       -2   /* Payload too large for slot */
#define USRL_RING_TRUNC      -3   /* Buffer too small (Reader) */
#define USRL_RING_TIMEOUT    -4   /* Spinlock timeout (MWMR Writer) */
#define USRL_RING_NO_DATA    -11  /* EAGAIN style - Nothing to read */

/* Publisher Handle (SWMR) */
typedef struct {
    RingDesc *desc;
    uint8_t *base_ptr;
    uint32_t mask;
    uint16_t pub_id;
} UsrlPublisher;

/* Subscriber Handle (Shared SWMR/MWMR) */
typedef struct {
    RingDesc *desc;
    uint8_t *base_ptr;
    uint32_t mask;
    uint64_t last_seq;
    uint64_t skipped_count; /* Internal skip tracker */
} UsrlSubscriber;

/* Publisher Handle (MWMR) */
typedef struct {
    RingDesc *desc;
    uint8_t *base_ptr;
    uint32_t mask;
    uint16_t pub_id;
} UsrlMwmrPublisher;

/* --------------------------------------------------------------------------
 * API Prototypes
 * -------------------------------------------------------------------------- */

/* SWMR */
void usrl_pub_init(UsrlPublisher *p, void *core_base, const char *topic, uint16_t pub_id);
int usrl_pub_publish(UsrlPublisher *p, const void *data, uint32_t len);

/* MWMR */
void usrl_mwmr_pub_init(UsrlMwmrPublisher *p, void *core_base, const char *topic, uint16_t pub_id);
int usrl_mwmr_pub_publish(UsrlMwmrPublisher *p, const void *data, uint32_t len);

/* Subscriber (Common) */
void usrl_sub_init(UsrlSubscriber *s, void *core_base, const char *topic);
int usrl_sub_next(UsrlSubscriber *s, uint8_t *out_buf, uint32_t buf_len, uint16_t *out_pub_id);

/* Telemetry Helpers */
uint64_t usrl_swmr_total_published(void *ring_desc);
uint64_t usrl_mwmr_total_published(void *ring_desc);

#endif /* USRL_RING_H */
