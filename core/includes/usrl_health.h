/**
 * @file usrl_health.h
 * @brief Ring health monitoring and diagnostics API
 */

#ifndef USRL_HEALTH_H
#define USRL_HEALTH_H

#include "usrl_core.h"
#include <stdint.h>
#include <time.h>

typedef struct {
    uint64_t total_published;
    uint64_t total_dropped;
    uint64_t publish_rate_hz;
    uint64_t last_publish_ns;
    uint32_t pending_publishers;
} PublisherHealth;

typedef struct {
    uint64_t total_read;
    uint64_t total_skipped;
    uint64_t subscribe_rate_hz;
    uint64_t last_read_ns;
    uint64_t lag_slots;
    uint64_t max_lag_observed;
} SubscriberHealth;

typedef struct {
    char topic_name[USRL_MAX_TOPIC_NAME];
    PublisherHealth pub_health;
    SubscriberHealth sub_health;
    uint64_t last_updated_ns;
    uint32_t ring_type;
} RingHealth;

RingHealth *usrl_health_get(void *base, const char *topic);
int usrl_health_check_lag(void *base, const char *topic, uint64_t lag_threshold_slots);
int usrl_health_detect_deadlock(void *base, const char *topic, uint64_t timeout_ms);
int usrl_health_export_json(void *base, const char *topic, char *buf, uint32_t max_len);
void usrl_health_free(RingHealth *health);

#endif /* USRL_HEALTH_H */
