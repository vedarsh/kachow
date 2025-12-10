/**
 * @file ring_mwmr.c
 * @brief Multi-writer / multiple-reader (MWMR) ring buffer.
 */

#if defined(__x86_64__) || defined(__i386__)
#define CPU_RELAX() __asm__ volatile("pause" ::: "memory")
#elif defined(__aarch64__) || defined(__arm__)
#define CPU_RELAX() __asm__ volatile("yield" ::: "memory")
#else
#define CPU_RELAX() do {} while (0)
#endif

#include "usrl_core.h"
#include "usrl_ring.h"
#include <stdio.h>
#include <string.h>
#include <sched.h>
#include <time.h>

static inline uint64_t usrl_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static inline void backoff(int iter) {
    if (iter < 10) CPU_RELAX();
    else sched_yield();
}

void usrl_mwmr_pub_init(UsrlMwmrPublisher *p, void *core_base, const char *topic, uint16_t pub_id) {
    if (!p || !core_base || !topic) return;
    TopicEntry *t = usrl_get_topic(core_base, topic);
    if (!t) return;
    if (t->type != USRL_RING_TYPE_MWMR) return;

    p->desc = (RingDesc *)((uint8_t *)core_base + t->ring_desc_offset);
    p->base_ptr = (uint8_t *)core_base + p->desc->base_offset;
    p->mask = p->desc->slot_count - 1;
    p->pub_id = pub_id;
}

int usrl_mwmr_pub_publish(UsrlMwmrPublisher *p, const void *data, uint32_t len) {
    if (USRL_UNLIKELY(!p || !p->desc || !data)) return USRL_RING_ERROR;
    RingDesc *d = p->desc;

    if (USRL_UNLIKELY(len > (d->slot_size - sizeof(SlotHeader)))) return USRL_RING_FULL;

    uint64_t old_head = atomic_fetch_add_explicit(&d->w_head, 1, memory_order_acq_rel);
    uint64_t commit_seq = old_head + 1;

    uint32_t idx = (uint32_t)((commit_seq - 1) & p->mask);
    uint8_t *slot = p->base_ptr + ((uint64_t)idx * d->slot_size);
    SlotHeader *hdr = (SlotHeader *)slot;

    int iter = 0;
    const int max_iter = 100000;

    while (1) {
        uint64_t current_seq = atomic_load_explicit(&hdr->seq, memory_order_acquire);

        if (current_seq == 0) break;

        uint64_t my_gen = commit_seq / d->slot_count;
        uint64_t current_gen = current_seq / d->slot_count;

        if (current_gen < my_gen) break;

        backoff(iter++);
        if (USRL_UNLIKELY(iter > max_iter)) {
            return USRL_RING_TIMEOUT; /* Standardized Timeout */
        }
    }

    USRL_PREFETCH_W(slot + sizeof(SlotHeader));

    memcpy(slot + sizeof(SlotHeader), data, len);
    hdr->payload_len = len;
    hdr->pub_id = p->pub_id;
    hdr->timestamp_ns = usrl_timestamp_ns();

    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&hdr->seq, commit_seq, memory_order_release);

    return USRL_RING_OK;
}

/* MWMR subscribers share UsrlSubscriber with SWMR, so they use usrl_sub_init/next in ring_swmr.c */
/* We just need the initialization wrapper if strictly needed, but SWMR init works fine for generic subs */

void usrl_mwmr_sub_init(UsrlSubscriber *s, void *core_base, const char *topic) {
    usrl_sub_init(s, core_base, topic); // Reuse SWMR init logic
}

uint64_t usrl_mwmr_total_published(void *ring_desc) {
    if (!ring_desc) return 0;
    RingDesc *d = (RingDesc *)ring_desc;
    return atomic_load_explicit(&d->w_head, memory_order_acquire);
}
