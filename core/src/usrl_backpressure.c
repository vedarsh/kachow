#include "usrl_backpressure.h"
#include <time.h>

static inline uint64_t usrl_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int usrl_quota_check(PublishQuota *quota)
{
    if (!quota)
        return 0;

    uint64_t now = usrl_now_ns();

    if (now - quota->last_window_start_ns > quota->publish_window_ns) {
        quota->last_window_start_ns = now;
        quota->msgs_in_window = 0;
    }

    if (quota->msgs_in_window >= quota->publish_quota) {
        quota->total_throttled++;
        return 1;
    }

    quota->msgs_in_window++;
    return 0;
}

int usrl_backpressure_check_lag(uint64_t lag, uint64_t threshold)
{
    if (lag > threshold)
        return 1;
    return 0;
}

uint64_t usrl_backoff_exponential(uint32_t attempt)
{
    if (attempt > 20) attempt = 20;
    
    uint64_t backoff_ns = 100;
    for (uint32_t i = 0; i < attempt; i++)
        backoff_ns *= 2;
    
    return backoff_ns;
}

uint64_t usrl_backoff_linear(uint64_t lag, uint64_t max_lag)
{
    if (lag >= max_lag)
        return 100000;
    
    return (lag * 100000) / max_lag;
}
