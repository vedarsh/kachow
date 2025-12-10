#include "usrl_health.h"
#include "usrl_ring.h"
#include "usrl_core.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

/* Helper for monotonic time */
static inline uint64_t usrl_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* =============================================================================
 * HEALTH QUERY (Compat with usrl_api.c)
 * ============================================================================= */
RingHealth *usrl_health_get(void *base, const char *topic)
{
    if (!base || !topic) return NULL;

    TopicEntry *t = usrl_get_topic(base, topic);
    if (!t) return NULL;

    RingDesc *d = (RingDesc *)((uint8_t *)base + t->ring_desc_offset);

    RingHealth *health = malloc(sizeof(*health));
    if (!health) return NULL;
    memset(health, 0, sizeof(*health));

    strncpy(health->topic_name, topic, USRL_MAX_TOPIC_NAME - 1);
    health->ring_type = t->type;
    health->last_updated_ns = usrl_now_ns();

    uint64_t head = atomic_load_explicit(&d->w_head, memory_order_acquire);
    health->pub_health.total_published = head;

    if (head > 0) {
        uint32_t idx = (uint32_t)((head - 1) & (d->slot_count - 1));
        uint8_t *slot = (uint8_t *)base + d->base_offset + ((uint64_t)idx * d->slot_size);
        SlotHeader *hdr = (SlotHeader *)slot;
        
        uint64_t ts = hdr->timestamp_ns;
        uint64_t seq = atomic_load_explicit(&hdr->seq, memory_order_acquire);
        
        if (seq == head) {
            health->pub_health.last_publish_ns = ts;
        } else {
            health->pub_health.last_publish_ns = 0;
        }
    }

    health->sub_health.lag_slots = 0; 

    return health;
}

void usrl_health_free(RingHealth *health)
{
    if (health) free(health);
}

/* =============================================================================
 * MISSING HELPERS (Restored for Linker)
 * ============================================================================= */

int usrl_health_check_lag(void *base, const char *topic, uint64_t lag_threshold_slots)
{
    RingHealth *h = usrl_health_get(base, topic);
    if (!h) return -1;

    int res = (h->sub_health.lag_slots > lag_threshold_slots);
    free(h);
    return res;
}

/* Renamed to match linker expectation if needed. 
   Your error says `usrl_health_detect_deadlock`, so we name it that. 
   Ideally "inactivity" is a better name, but we match the caller. */
int usrl_health_detect_deadlock(void *base, const char *topic, uint64_t timeout_ms)
{
    RingHealth *h = usrl_health_get(base, topic);
    if (!h) return -1;

    if (h->pub_health.last_publish_ns == 0) {
        free(h);
        return 0; // Never published, technically not a deadlock yet
    }

    uint64_t now = usrl_now_ns();
    uint64_t delta = now - h->pub_health.last_publish_ns;
    uint64_t timeout_ns = timeout_ms * 1000000ULL;

    int res = (delta > timeout_ns);
    free(h);
    return res;
}

int usrl_health_export_json(void *base, const char *topic, char *buf, uint32_t max_len)
{
    RingHealth *h = usrl_health_get(base, topic);
    if (!h) return -1;

    int written = snprintf(buf, max_len,
        "{\"topic\":\"%s\",\"published\":%lu,\"last_pub_ns\":%lu}",
        h->topic_name,
        h->pub_health.total_published,
        h->pub_health.last_publish_ns);

    free(h);
    return (written > 0 && written < (int)max_len) ? written : -1;
}
