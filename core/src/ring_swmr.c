/**
 * @file ring_swmr.c
 * @brief Single-writer / multiple-reader ring buffer.
 */

#include "usrl_core.h"
#include "usrl_ring.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Debug macros omitted for brevity */

static inline uint64_t usrl_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

void usrl_pub_init(UsrlPublisher *p, void *core_base, const char *topic, uint16_t pub_id) {
    if (!p || !core_base || !topic) return;
    TopicEntry *t = usrl_get_topic(core_base, topic);
    if (!t) return;
    p->desc = (RingDesc *)((uint8_t *)core_base + t->ring_desc_offset);
    p->base_ptr = (uint8_t *)core_base + p->desc->base_offset;
    p->mask = p->desc->slot_count - 1;
    p->pub_id = pub_id;
}

int usrl_pub_publish(UsrlPublisher *p, const void *data, uint32_t len) {
    if (USRL_UNLIKELY(!p || !p->desc || !data)) return USRL_RING_ERROR;
    RingDesc *d = p->desc;

    /* Check size */
    if (USRL_UNLIKELY(len > (d->slot_size - sizeof(SlotHeader)))) return USRL_RING_FULL;

    uint64_t old_head = atomic_fetch_add_explicit(&d->w_head, 1, memory_order_acq_rel);
    uint64_t commit_seq = old_head + 1;

    uint32_t idx = (uint32_t)((commit_seq - 1) & p->mask);
    uint8_t *slot = p->base_ptr + ((uint64_t)idx * d->slot_size);
    SlotHeader *hdr = (SlotHeader *)slot;

    USRL_PREFETCH_W(slot + sizeof(SlotHeader));

    memcpy(slot + sizeof(SlotHeader), data, len);
    hdr->payload_len = len;
    hdr->pub_id = p->pub_id;
    hdr->timestamp_ns = usrl_timestamp_ns();

    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&hdr->seq, commit_seq, memory_order_release);
    
    return USRL_RING_OK;
}

void usrl_sub_init(UsrlSubscriber *s, void *core_base, const char *topic) {
    if (!s || !core_base || !topic) return;
    TopicEntry *t = usrl_get_topic(core_base, topic);
    if (!t) return;
    s->desc = (RingDesc *)((uint8_t *)core_base + t->ring_desc_offset);
    s->base_ptr = (uint8_t *)core_base + s->desc->base_offset;
    s->mask = s->desc->slot_count - 1;
    s->last_seq = 0;
    s->skipped_count = 0;
}

int usrl_sub_next(UsrlSubscriber *s, uint8_t *out_buf, uint32_t buf_len, uint16_t *out_pub_id) {
    if (USRL_UNLIKELY(!s || !s->desc || !out_buf)) return USRL_RING_ERROR;

    RingDesc *d = s->desc;
    uint64_t w_head = atomic_load_explicit(&d->w_head, memory_order_acquire);
    uint64_t next = s->last_seq + 1;

    if (next > w_head) return USRL_RING_NO_DATA; /* Nothing new */

    /* Lag Jump */
    if (w_head - next >= d->slot_count) {
        uint64_t new_start = w_head - d->slot_count + 1;
        s->skipped_count += (new_start - next);
        s->last_seq = new_start - 1;
        next = new_start;
        w_head = atomic_load_explicit(&d->w_head, memory_order_acquire);
        if (next > w_head) return USRL_RING_NO_DATA;
    }

    uint32_t idx = (uint32_t)((next - 1) & s->mask);
    uint8_t *slot = s->base_ptr + ((uint64_t)idx * d->slot_size);
    SlotHeader *hdr = (SlotHeader *)slot;
    USRL_PREFETCH_R(s->base_ptr + ((uint64_t)(next & s->mask) * d->slot_size));

    uint64_t seq = atomic_load_explicit(&hdr->seq, memory_order_acquire);

    if (seq == 0 || seq < next) return USRL_RING_NO_DATA;

    if (seq > next) {
        s->skipped_count += (seq - next);
        s->last_seq = seq - 1;
        return USRL_RING_NO_DATA;
    }

    uint32_t payload_len = hdr->payload_len;
    if (USRL_UNLIKELY(payload_len > buf_len)) {
        s->last_seq = next;
        return USRL_RING_TRUNC; /* Buffer too small */
    }

    memcpy(out_buf, slot + sizeof(SlotHeader), payload_len);
    if (out_pub_id) *out_pub_id = hdr->pub_id;

    atomic_thread_fence(memory_order_acquire);
    uint64_t post_seq = atomic_load_explicit(&hdr->seq, memory_order_relaxed);

    if (USRL_UNLIKELY(post_seq != seq)) {
        s->skipped_count++;
        s->last_seq = w_head;
        return USRL_RING_NO_DATA;
    }

    s->last_seq = next;
    return (int)payload_len; /* Safe to return 0 for empty payload */
}

uint64_t usrl_swmr_total_published(void *ring_desc) {
    if (!ring_desc) return 0;
    RingDesc *d = (RingDesc *)ring_desc;
    return atomic_load_explicit(&d->w_head, memory_order_acquire);
}
