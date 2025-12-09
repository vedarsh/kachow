/**
 * @file ring_mwmr.c
 * @brief Multi-writer / multiple-reader (MWMR) ring buffer publish/subscribe.
 *
 * Implements MWMR publisher and subscriber helpers that bind to rings created
 * by usrl_core_init(). This module contains timing helpers, a simple backoff
 * strategy and the MWMR publish path which ensures safe overwrites and
 * deadlock prevention via timeouts.
 */

/* usrl_platform.h or at the top of ring_mwmr.c */

#if defined(__x86_64__) || defined(__i386__)
#define CPU_RELAX() __asm__ volatile("pause" ::: "memory")
#elif defined(__aarch64__) || defined(__arm__)
#define CPU_RELAX() __asm__ volatile("yield" ::: "memory")
#else
#define CPU_RELAX() \
    do              \
    {               \
    } while (0) /* Fallback for unknown arch */
#endif

#include "usrl_core.h"
#include "usrl_ring.h"

#include <stdio.h>
#include <string.h>
#include <sched.h>
#include <time.h>

/* --------------------------------------------------------------------------
 * Debug Utilities
 * -------------------------------------------------------------------------- */
#ifdef DEBUG
#define DEBUG_PRINT_MWMR(...)                 \
    do                                        \
    {                                         \
        printf("[DEBUG][MWMR] " __VA_ARGS__); \
        fflush(stdout);                       \
    } while (0)
#else
#define DEBUG_PRINT_MWMR(...) ((void)0)
#endif

/**
 * @brief Return a monotonic timestamp in nanoseconds.
 *
 * Uses CLOCK_MONOTONIC to obtain a monotonic time suitable for message
 * timestamps. This avoids issues with system clock adjustments.
 *
 * @return Current monotonic time in nanoseconds.
 */
static inline uint64_t usrl_timestamp_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/**
 * @brief Simple backoff strategy for spins.
 *
 * Implements a hybrid spin/yield strategy:
 *  - Tight CPU relaxation for the first few iterations (low-cost).
 *  - sched_yield() after threshold to avoid starving other threads.
 *
 * @param iter Current spin iteration count (0-based).
 */
static inline void backoff(int iter)
{
    if (iter < 10)
    {
        CPU_RELAX(); /* x86 hint, nop on other archs */
    }
    else
    {
        sched_yield();
    }
}

/* =============================================================================
 * MWMR PUBLISHER INITIALIZATION
 * =============================================================================
 *
 * usrl_mwmr_pub_init()
 *
 * Initializes a multi-writer publisher for a particular topic.
 *
 * Responsibilities:
 *   - Validate topic
 *   - Confirm the topic is MWMR type
 *   - Bind ring descriptor + base pointer
 *   - Store pub_id
 * =============================================================================
 */
void usrl_mwmr_pub_init(
    UsrlMwmrPublisher *p,
    void *core_base,
    const char *topic,
    uint16_t pub_id)
{
    if (!p || !core_base || !topic)
        return;

    /* Lookup topic descriptor */
    TopicEntry *t = usrl_get_topic(core_base, topic);
    if (!t)
        return;

    /* Type check: must be MWMR */
    if (t->type != USRL_RING_TYPE_MWMR)
    {
        printf("[ERROR] Topic '%s' is not MWMR!\n", topic);
        return;
    }

    /* Bind publisher to ring */
    p->desc = (RingDesc *)((uint8_t *)core_base + t->ring_desc_offset);
    p->base_ptr = (uint8_t *)core_base + p->desc->base_offset;
    p->mask = p->desc->slot_count - 1;
    p->pub_id = pub_id;

    DEBUG_PRINT_MWMR("publisher %u ready on MWMR topic '%s'\n", pub_id, topic);
}

/* =============================================================================
 * MWMR PUBLISH
 * =============================================================================
 *
 * usrl_mwmr_pub_publish()
 *
 * This is the multi-writer-safe publish path.
 *
 * Steps:
 *   1) Atomic reservation of a sequence number (unique per writer).
 *   2) Compute ring index.
 *   3) Spin-wait until the slot is safe to overwrite
 *      (to avoid collisions with lagging writers).
 *   4) Write payload + metadata.
 *   5) Insert memory barrier (FIX #2).
 *   6) Commit by publishing sequence number.
 *
 * FIX #1: Correct generation-based slot safety check.
 * FIX #4: Deadlock prevention with timeout.
 *
 * Return Values:
 *    0 = success
 *   -1 = invalid or null arguments
 *   -2 = payload too large
 *   -3 = timeout waiting for safe overwrite (avoid deadlock)
 *
 * =============================================================================
 */
int usrl_mwmr_pub_publish(
    UsrlMwmrPublisher *p,
    const void *data,
    uint32_t len)
{
    if (USRL_UNLIKELY(!p || !p->desc || !data))
        return -1;

    RingDesc *d = p->desc;

    /* Payload must fit inside slot */
    if (USRL_UNLIKELY(len > (d->slot_size - sizeof(SlotHeader))))
        return -2;

    /* ----------------------------------------------------------------------
     * 1. Atomic Reservation
     *
     * Multiple writers increment w_head simultaneously, but fetch_add gives
     * each writer a unique old_head value.
     * ---------------------------------------------------------------------- */
    uint64_t old_head =
        atomic_fetch_add_explicit(&d->w_head, 1, memory_order_acq_rel);
    uint64_t commit_seq = old_head + 1;

    /* Compute slot index */
    uint32_t idx = (uint32_t)((commit_seq - 1) & p->mask);
    uint8_t *slot = p->base_ptr + ((uint64_t)idx * d->slot_size);
    SlotHeader *hdr = (SlotHeader *)slot;

    /* ----------------------------------------------------------------------
     * 2. FIX #1: Correct Generation-Based Safety Check
     *
     * We must ensure that:
     *   - The previous writer (from a prior lap) has finished writing.
     *   - We are not overwriting a slot that is still "in progress".
     *
     * Generation Logic:
     *   my_gen      = commit_seq / slot_count
     *   current_gen = current_seq / slot_count
     *
     * Slot is safe if:
     *   current_seq == 0   (never used)
     *   OR current_gen < my_gen (previous generation)
     *
     * This avoids the underflow bug when current_seq > commit_seq due to
     * wrap-around or stale data.
     *
     * FIX #4: Deadlock prevention with iteration limit.
     * ---------------------------------------------------------------------- */
    int iter = 0;
    const int max_iter = 100000; /* Prevent infinite spin */

    while (1)
    {
        uint64_t current_seq =
            atomic_load_explicit(&hdr->seq, memory_order_acquire);

        /* Case 1: Slot never used */
        if (current_seq == 0)
            break;

        /* FIX #1: Use generation-based comparison */
        uint64_t my_gen = commit_seq / d->slot_count;
        uint64_t current_gen = current_seq / d->slot_count;

        /* Case 2: Previous generation's data - safe to overwrite */
        if (current_gen < my_gen)
            break;

        /* Case 3: Same generation but different seq (another writer's slot)
         * This should not happen due to fetch_add uniqueness, but handle it.
         * If current_seq < commit_seq within same generation, prev writer
         * is still writing. Wait. */

        /* Otherwise, slot is still "active" - wait */
        backoff(iter++);

        /* FIX #4: Timeout to prevent deadlock */
        if (USRL_UNLIKELY(iter > max_iter))
        {
            DEBUG_PRINT_MWMR("timeout waiting for slot %u (seq %lu)\n",
                             idx, commit_seq);
            return -3;
        }
    }

    /* FIX #11: Prefetch for writing */
    USRL_PREFETCH_W(slot + sizeof(SlotHeader));

    /* ----------------------------------------------------------------------
     * 3. WRITE PAYLOAD + METADATA
     * ---------------------------------------------------------------------- */
    memcpy(slot + sizeof(SlotHeader), data, len);

    hdr->payload_len = len;
    hdr->pub_id = p->pub_id;

    /* FIX #6: Use monotonic timestamp */
    hdr->timestamp_ns = usrl_timestamp_ns();

    /* ----------------------------------------------------------------------
     * 4. FIX #2: Memory barrier before commit
     *
     * Ensures all payload/header writes are globally visible BEFORE the
     * sequence number is published.
     * ---------------------------------------------------------------------- */
    atomic_thread_fence(memory_order_release);

    /* ----------------------------------------------------------------------
     * 5. COMMIT
     *
     * Publishing seq marks slot as ready-to-consume.
     * ---------------------------------------------------------------------- */
    atomic_store_explicit(&hdr->seq, commit_seq, memory_order_release);

    DEBUG_PRINT_MWMR("pub %u committed seq %lu at slot %u\n",
                     p->pub_id, commit_seq, idx);
    return 0;
}

/* =============================================================================
 * MWMR SUBSCRIBER INITIALIZATION
 * =============================================================================
 *
 * usrl_mwmr_sub_init()
 *
 * Wrapper to initialize a subscriber on an MWMR topic.
 * Uses the same UsrlSubscriber struct and logic as SWMR.
 * =============================================================================
 */
void usrl_mwmr_sub_init(
    UsrlSubscriber *s,
    void *core_base,
    const char *topic)
{
    if (!s || !core_base || !topic)
        return;

    TopicEntry *t = usrl_get_topic(core_base, topic);
    if (!t)
        return;

    /* Type check: warn if not MWMR */
    if (t->type != USRL_RING_TYPE_MWMR)
    {
        printf("[WARN] Topic '%s' is not MWMR, using anyway\n", topic);
    }

    s->desc = (RingDesc *)((uint8_t *)core_base + t->ring_desc_offset);
    s->base_ptr = (uint8_t *)core_base + s->desc->base_offset;
    s->mask = s->desc->slot_count - 1;
    s->last_seq = 0;

    DEBUG_PRINT_MWMR("subscriber ready on '%s'\n", topic);
}

/* =============================================================================
 * REAL HEALTH TELEMETRY ACCESSORS (MWMR)
 * ============================================================================= */

/* Total published messages = current write head */
uint64_t usrl_mwmr_total_published(void *ring_desc)
{
    if (!ring_desc) return 0;
    RingDesc *d = (RingDesc *)ring_desc;

    return atomic_load_explicit(&d->w_head, memory_order_acquire);
}

/* Last publish timestamp = timestamp of most recent committed slot */
uint64_t usrl_mwmr_last_publish_ns(void *ring_desc)
{
    if (!ring_desc) return 0;
    RingDesc *d = (RingDesc *)ring_desc;

    uint64_t w_head =
        atomic_load_explicit(&d->w_head, memory_order_acquire);

    if (w_head == 0)
        return 0;

    uint32_t idx = (uint32_t)((w_head - 1) & (d->slot_count - 1));
    uint8_t *slot =
        (uint8_t *)d + d->base_offset + ((uint64_t)idx * d->slot_size);

    SlotHeader *hdr = (SlotHeader *)slot;
    return hdr->timestamp_ns;
}
