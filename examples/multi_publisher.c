#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#include "usrl_core.h"
#include "usrl_backpressure.h"
#include "usrl_logging.h"
#include "usrl_health.h"
#include "usrl_ring.h"

/* =========================
 * ORDER MESSAGE STRUCT
 * ========================= */
typedef struct __attribute__((packed)) {
    uint64_t order_id;
    uint32_t user_id;
    double price;
    uint32_t quantity;
    uint8_t side;
} Order;

/* =========================
 * PUBLISHER THREAD ARGS
 * ========================= */
typedef struct {
    int publisher_id;
    const char *topic;
    void *base;
    int order_count;
} PublisherArgs;

/* =========================
 * HEALTH MONITOR THREAD ARGS
 * ========================= */
typedef struct {
    void *base;
    const char *topic;
    int runtime_sec;
} HealthMonitorArgs;

/* =========================
 * PUBLISHER THREAD
 * ========================= */
void *publisher_thread(void *arg)
{
    PublisherArgs *args = (PublisherArgs *)arg;

    UsrlPublisher pub;
    usrl_pub_init(&pub, args->base, args->topic, args->publisher_id);

    /* ✅ 25k messages/sec per publisher (100k total / 4 pubs) */
    PublishQuota quota;
    usrl_quota_init(&quota, 25000);

    uint32_t backoff_attempt = 0;

    USRL_INFO("order_pub", "Publisher %d started", args->publisher_id);

    for (int i = 0; i < args->order_count; i++) {

        /* ✅ BACKPRESSURE + EXPONENTIAL BACKOFF */
        if (usrl_quota_check(&quota)) {
            uint64_t backoff_ns = usrl_backoff_exponential(backoff_attempt++);
            usleep(backoff_ns / 1000);   // ns → us
            continue;
        }

        backoff_attempt = 0;  // reset on success

        Order order = {
            .order_id = ((uint64_t)args->publisher_id << 32) | i,
            .user_id  = args->publisher_id * 100 + (i % 100),
            .price    = 100.0 + (i % 50),
            .quantity = 10 + (i % 1000),
            .side     = i % 2,
        };

        if (usrl_pub_publish(&pub, (uint8_t *)&order, sizeof(order)) != 0) {
            USRL_ERROR("order_pub", "Publisher %d: publish failed",
                       args->publisher_id);
            break;
        }

        if ((i % 10000) == 0 && i != 0) {
            USRL_INFO("order_pub", "Publisher %d: sent %d orders",
                      args->publisher_id, i);
        }
    }

    USRL_INFO("order_pub", "Publisher %d finished: sent %d orders",
              args->publisher_id, args->order_count);

    return NULL;
}

/* =========================
 * HEALTH MONITOR THREAD
 * ========================= */
void *health_monitor_thread(void *arg)
{
    HealthMonitorArgs *h = (HealthMonitorArgs *)arg;
    time_t start = time(NULL);

    while (time(NULL) - start < h->runtime_sec) {

        RingHealth *health = usrl_health_get(h->base, h->topic);
        if (health) {
            USRL_INFO("health",
                "Topic=%s pub=%lu lag=%lu max_lag=%lu",
                health->topic_name,
                health->pub_health.total_published,
                health->sub_health.lag_slots,
                health->sub_health.max_lag_observed
            );
            usrl_health_free(health);
        }

        if (usrl_health_check_lag(h->base, h->topic, 100)) {
            USRL_WARN("health", "⚠️ LAG threshold exceeded!");
        }

        if (usrl_health_detect_deadlock(h->base, h->topic, 500)) {
            USRL_ERROR("health", "🔥 DEADLOCK detected!");
        }

        char json[256];
        if (usrl_health_export_json(h->base, h->topic, json, sizeof(json)) > 0) {
            USRL_DEBUG("health_json", "%s", json);
        }

        sleep(1);
    }

    USRL_INFO("health", "Health monitor exiting");
    return NULL;
}

/* =========================
 * MAIN
 * ========================= */
int main(void)
{
    printf("=== Multi-Publisher Order Processing + Health Monitor ===\n\n");

    usrl_logging_init(NULL, USRL_LOG_INFO);

    UsrlTopicConfig topics[] = {
        {"orders", 1024, 512, USRL_RING_TYPE_MWMR},
    };

    int ret = usrl_core_init("/usrl-orders", 100 * 1024 * 1024, topics, 1);
    if (ret != 0) {
        USRL_ERROR("order_processor", "Failed to init USRL");
        return 1;
    }

    void *base = usrl_core_map("/usrl-orders", 100 * 1024 * 1024);
    if (!base) {
        USRL_ERROR("order_processor", "Failed to map USRL region");
        return 1;
    }

    /* =========================
     * START HEALTH MONITOR
     * ========================= */
    HealthMonitorArgs health_args = {
        .base = base,
        .topic = "orders",
        .runtime_sec = 5
    };

    pthread_t health_thread;
    pthread_create(&health_thread, NULL,
                   health_monitor_thread, &health_args);

    /* =========================
     * START PUBLISHERS
     * ========================= */
    int num_publishers = 4;
    pthread_t threads[num_publishers];
    PublisherArgs args[num_publishers];

    USRL_INFO("order_processor", "Starting %d publishers", num_publishers);

    for (int i = 0; i < num_publishers; i++) {
        args[i].publisher_id = i + 1;
        args[i].topic = "orders";
        args[i].base = base;
        args[i].order_count = 50000;

        if (pthread_create(&threads[i], NULL,
                           publisher_thread, &args[i]) != 0) {
            USRL_ERROR("order_processor", "Failed to create thread %d", i);
            return 1;
        }
    }

    /* =========================
     * JOIN PUBLISHERS
     * ========================= */
    for (int i = 0; i < num_publishers; i++) {
        pthread_join(threads[i], NULL);
    }

    /* =========================
     * JOIN HEALTH MONITOR
     * ========================= */
    pthread_join(health_thread, NULL);

    printf("\n✅ All publishers finished\n");
    printf("   Total orders: %d\n", num_publishers * 50000);
    printf("   Publishers: %d\n", num_publishers);

    usrl_logging_shutdown();
    return 0;
}
