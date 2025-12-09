#ifndef USRL_RING_H
#define USRL_RING_H

/* --------------------------------------------------------------------------
 * USRL Ring API
 *
 * This header defines the publisher/subscriber interfaces for both:
 *   - SWMR (Single Writer, Multi Reader)
 *   - MWMR (Multi Writer, Multi Reader)
 *
 * Each publisher/subscriber is a *handle* that binds to a specific topic's
 * ring inside the SHM region produced by usrl_core_init().
 *
 * Handles store:
 *   - RingDesc*  : ring metadata (slot_count, sizes, w_head)
 *   - base_ptr   : pointer to first slot in the ring
 *   - mask       : slot_count - 1 (for index wrapping)
 *   - pub_id     : meaningful publisher ID (SWMR/MWMR)
 *   - last_seq   : for subscribers; the next sequence expected
 *
 * -------------------------------------------------------------------------- */

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include "usrl_core.h"

/* =============================================================================
 * Publisher Handle (SWMR)
 * =============================================================================
 *
 * Used for Single-Writer Multi-Reader rings. Only one writer is expected to
 * publish into the ring, but the structure still supports pub_id for
 * debugging / telemetry.
 *
 * =============================================================================
 */
typedef struct
{
    RingDesc *desc;    /* ring descriptor in SHM */
    uint8_t *base_ptr; /* pointer to slot region */
    uint32_t mask;     /* (slot_count - 1) for fast index wrap */
    uint16_t pub_id;   /* publisher id */
} UsrlPublisher;

/* =============================================================================
 * Subscriber Handle (Shared by SWMR + MWMR)
 * =============================================================================
 *
 * Subscribers track a ring descriptor and the last sequence read.
 * last_seq = 0 means no data has been consumed yet.
 *
 * =============================================================================
 */
typedef struct
{
    RingDesc *desc;
    uint8_t *base_ptr;
    uint32_t mask;
    uint64_t last_seq; /* the "next" expected sequence */
} UsrlSubscriber;

/* =============================================================================
 * MWMR Publisher Handle
 * =============================================================================
 *
 * Similar to UsrlPublisher but explicitly intended for multi-writer rings.
 * Behaviour differs in publish path (collision protection / slot safety).
 *
 * =============================================================================
 */
typedef struct
{
    RingDesc *desc;
    uint8_t *base_ptr;
    uint32_t mask;
    uint16_t pub_id;
} UsrlMwmrPublisher;

/* =============================================================================
 * MWMR API
 * =============================================================================
 *
 * usrl_mwmr_pub_init()   : Initialize a multi-writer publisher
 * usrl_mwmr_pub_publish(): Multi-writer safe publish w/ slot spin-wait
 * usrl_mwmr_sub_init()   : Subscriber initialization for MWMR topics
 *
 * =============================================================================
 */
void usrl_mwmr_pub_init(UsrlMwmrPublisher *p,
                        void *core_base,
                        const char *topic,
                        uint16_t pub_id);

int usrl_mwmr_pub_publish(UsrlMwmrPublisher *p,
                          const void *data,
                          uint32_t len);

void usrl_mwmr_sub_init(UsrlSubscriber *s,
                        void *core_base,
                        const char *topic);

/* =============================================================================
 * SWMR API
 * =============================================================================
 *
 * usrl_pub_init()    : Initialize a single-writer publisher
 * usrl_pub_publish() : Fast SWMR write (no collision handling needed)
 *
 * =============================================================================
 */
void usrl_pub_init(UsrlPublisher *p,
                   void *core_base,
                   const char *topic,
                   uint16_t pub_id);

int usrl_pub_publish(UsrlPublisher *p,
                     const void *data,
                     uint32_t len);

/* =============================================================================
 * Subscriber API (Shared by SWMR and MWMR)
 * =============================================================================
 *
 * usrl_sub_init()  : Bind subscriber to a ring
 * usrl_sub_next()  : Retrieve next message sequentially.
 *
 * NEW: out_pub_id
 *   Subscribers can now discover which publisher wrote the message.
 *
 *   This enables:
 *     - multi-writer analytics
 *     - per-publisher telemetry
 *     - safety channel routing
 *
 * =============================================================================
 */
void usrl_sub_init(UsrlSubscriber *s,
                   void *core_base,
                   const char *topic);

int usrl_sub_next(UsrlSubscriber *s,
                  uint8_t *out_buf,
                  uint32_t buf_len,
                  uint16_t *out_pub_id);
/* =============================================================================
 * REAL HEALTH TELEMETRY ACCESSORS (RING)
 * ============================================================================= */

/* -------- MWMR RING TELEMETRY -------- */

/* Total number of published messages (write head) */
uint64_t usrl_mwmr_total_published(void *ring_desc);

/* Timestamp (ns) of most recent published slot */
uint64_t usrl_mwmr_last_publish_ns(void *ring_desc);


/* -------- SWMR RING TELEMETRY -------- */

/* Total number of published messages (write head) */
uint64_t usrl_swmr_total_published(void *ring_desc);

/* Timestamp (ns) of most recent published slot */
uint64_t usrl_swmr_last_publish_ns(void *ring_desc);

#endif /* USRL_RING_H */
