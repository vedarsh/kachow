#ifndef USRL_BACKPRESSURE_H
#define USRL_BACKPRESSURE_H

#include <stdint.h>
#include <time.h>
#include <stdbool.h>

typedef struct {
    uint64_t publish_quota;
    uint64_t publish_window_ns;
    uint64_t last_window_start_ns;
    uint64_t msgs_in_window;
    uint64_t total_throttled;
} PublishQuota;

typedef enum {
    USRL_BP_NONE = 0,
    USRL_BP_DROP,
    USRL_BP_BLOCK,
    USRL_BP_THROTTLE,
} UsrlBackpressureMode;

typedef struct {
    uint64_t subscriber_pos;
    uint64_t writer_pos;
    uint64_t lag_slots;
    uint64_t lag_threshold;
    bool is_lagging;
} UsrlLagTracker;

static inline void usrl_quota_init(PublishQuota *quota, uint64_t msgs_per_sec)
{
    if (!quota || msgs_per_sec == 0)
        return;
    
    quota->publish_quota = msgs_per_sec / 1000;
    quota->publish_window_ns = 1000000;
    quota->last_window_start_ns = 0;
    quota->msgs_in_window = 0;
    quota->total_throttled = 0;
}

int usrl_quota_check(PublishQuota *quota);
int usrl_backpressure_check_lag(uint64_t lag, uint64_t threshold);
uint64_t usrl_backoff_exponential(uint32_t attempt);
uint64_t usrl_backoff_linear(uint64_t lag, uint64_t max_lag);

#endif /* USRL_BACKPRESSURE_H */
