/* ring_swmr.c */
// this file handles the ring buffer logic
// it does the reading and writing

#include "usrl_core.h"
#include "usrl_ring.h"
#include <string.h>
#include <time.h>

void usrl_pub_init(UsrlPublisher *p, void *core_base, const char *topic)
{
    DEBUG_PRINT_RING("pub init start\n");

    // check inputs
    if (!p || !core_base || !topic) return;

    // find the topic
    TopicEntry *t = usrl_get_topic(core_base, topic);
    if (!t) {
        DEBUG_PRINT_RING("topic not found\n");
        return;
    }

    // set up the publisher struct
    p->desc     = (RingDesc*)((uint8_t*)core_base + t->ring_desc_offset);
    p->base_ptr = (uint8_t*)core_base + p->desc->base_offset;
    p->mask     = p->desc->slot_count - 1;

    DEBUG_PRINT_RING("pub init done\n");
}

int usrl_pub_publish(UsrlPublisher *p, const void *data, uint32_t len)
{
    // check inputs
    if (!p || !p->desc || !data) return -1;

    RingDesc *d = p->desc;

    // check if message is too big
    if (len > (d->slot_size - sizeof(SlotHeader))) return -2;

    // increment the head atomically
    uint64_t old_head  = atomic_fetch_add_explicit(&d->w_head, 1, memory_order_acq_rel);
    uint64_t commit_seq = old_head + 1;

    // calculate the index
    uint32_t idx = (uint32_t)((commit_seq - 1) & p->mask);

    DEBUG_PRINT_RING("publishing seq %lu at idx %u\n", commit_seq, idx);

    uint8_t    *slot = p->base_ptr + ((uint64_t)idx * d->slot_size);
    SlotHeader *hdr  = (SlotHeader*)slot;

    // copy the data
    memcpy(slot + sizeof(SlotHeader), data, len);
    hdr->payload_len = len;

    // get the time
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    hdr->timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

    // commit the sequence number
    atomic_store_explicit(&hdr->seq, commit_seq, memory_order_release);

    return 0;
}

void usrl_sub_init(UsrlSubscriber *s, void *core_base, const char *topic)
{
    DEBUG_PRINT_RING("sub init start\n");

    if (!s || !core_base || !topic) return;

    TopicEntry *t = usrl_get_topic(core_base, topic);
    if (!t) return;

    s->desc     = (RingDesc*)((uint8_t*)core_base + t->ring_desc_offset);
    s->base_ptr = (uint8_t*)core_base + s->desc->base_offset;
    s->mask     = s->desc->slot_count - 1;

    // start from 0 so we get all messages
    s->last_seq = 0;

    DEBUG_PRINT_RING("sub init done, starting at 0\n");
}

int usrl_sub_next(UsrlSubscriber *s, uint8_t *out_buf, uint32_t buf_len)
{
    if (!s || !s->desc || !out_buf) return -1;

    RingDesc *d = s->desc;

    // check where the writer is
    uint64_t w_head = atomic_load_explicit(&d->w_head, memory_order_acquire);
    uint64_t next   = s->last_seq + 1;

    // if next is bigger than head, no new data
    if (next > w_head) return 0;

    // check if we are too far behind
    if (w_head - next >= d->slot_count) {
        uint64_t new_start = w_head - d->slot_count + 1;
        s->last_seq = new_start - 1;
        next = new_start;

        // check again
        w_head = atomic_load_explicit(&d->w_head, memory_order_acquire);
        if (next > w_head) return 0;

        DEBUG_PRINT_RING("overflow, jumping to %lu\n", next);
    }

    // calculate index
    uint32_t idx = (uint32_t)((next - 1) & s->mask);
    uint8_t  *slot = s->base_ptr + ((uint64_t)idx * d->slot_size);
    SlotHeader *hdr = (SlotHeader*)slot;

    // read the sequence
    uint64_t seq = atomic_load_explicit(&hdr->seq, memory_order_acquire);

    // if empty or stale, skip
    if (seq == 0 || seq < next) return 0;

    // if sequence is ahead, we missed something
    if (seq > next) {
        s->last_seq = seq - 1;
        return 0;
    }

    // if we got here, seq equals next
    uint32_t payload_len = hdr->payload_len;

    DEBUG_PRINT_RING("got message seq %lu len %u\n", seq, payload_len);

    // check if buffer is big enough
    if (payload_len > buf_len) {
        s->last_seq = next;
        return -3;
    }

    // copy data out
    memcpy(out_buf, slot + sizeof(SlotHeader), payload_len);
    s->last_seq = next;

    return (int)payload_len;
}
