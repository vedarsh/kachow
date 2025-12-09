/**
 * @file usrl_api.c
 * @brief Unified Facade Implementation (Spaceflight-Certified).
 */

#define _GNU_SOURCE
#include "usrl.h"
#include "usrl_core.h"
#include "usrl_ring.h"
#include "usrl_backpressure.h"
#include "usrl_health.h"
#include "usrl_logging.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>

/* Accessor for Lag Calculation (from ring_swmr.c) */
// Ensure this matches the signature in ring_swmr.c
uint64_t usrl_swmr_total_published(void *ring_desc);

static uint32_t g_pub_id_seq = 1;

/* ============================================================================
 * INTERNAL STRUCTURES
 * ============================================================================ */

struct usrl_ctx {
    char name[64];
};

struct usrl_pub {
    usrl_ctx_t *ctx;
    UsrlPublisher core;
    PublishQuota quota;
    bool block_on_full;
    bool use_limiter;
    char topic[64];
    void *shm_base;
    size_t map_size; 
};

struct usrl_sub {
    usrl_ctx_t *ctx;
    UsrlSubscriber core;
    char topic[64];
    void *shm_base;
    size_t map_size; 
    
    /* Local Stats */
    uint64_t local_ops;
    uint64_t local_skips;
};

/* ============================================================================
 * LIFECYCLE
 * ============================================================================ */

usrl_ctx_t *usrl_init(const usrl_sys_config_t *config)
{
    if (!config) return NULL;
    usrl_logging_init(config->log_file_path, config->log_level);
    
    usrl_ctx_t *ctx = calloc(1, sizeof(usrl_ctx_t));
    if (!ctx) return NULL;
    
    if (config->app_name) strncpy(ctx->name, config->app_name, 63);
    else strcpy(ctx->name, "usrl_app");
    
    USRL_INFO("API", "USRL System Initialized: %s", ctx->name);
    return ctx;
}

void usrl_shutdown(usrl_ctx_t *ctx)
{
    if (!ctx) return;
    USRL_INFO("API", "USRL System Shutdown: %s", ctx->name);
    usrl_logging_shutdown();
    free(ctx);
}

/* ============================================================================
 * PUBLISHER
 * ============================================================================ */

usrl_pub_t *usrl_pub_create(usrl_ctx_t *ctx, const usrl_pub_config_t *config)
{
    // STRICT VALIDATION
    if (!ctx || !config || !config->topic) {
        USRL_ERROR("API", "usrl_pub_create: Invalid arguments");
        return NULL;
    }

    uint32_t sc = (config->slot_count > 0) ? config->slot_count : 4096;
    uint32_t ss = (config->slot_size > 0) ? config->slot_size : 1024;
    size_t ring_size = (size_t)sc * ss + (1024 * 1024);
    char shm_path[128];
    snprintf(shm_path, sizeof(shm_path), "/usrl-%s", config->topic);

    UsrlTopicConfig tcfg = {0};
    strncpy(tcfg.name, config->topic, 63);
    tcfg.slot_count = sc;
    tcfg.slot_size = ss;
    tcfg.type = (config->ring_type == USRL_RING_MWMR) ? USRL_RING_TYPE_MWMR : USRL_RING_TYPE_SWMR;

    if (usrl_core_init(shm_path, ring_size, &tcfg, 1) != 0) {
        USRL_ERROR("API", "Failed to init core ring: %s", config->topic);
    }

    void *base = usrl_core_map(shm_path, ring_size);
    if (!base) {
        USRL_ERROR("API", "Failed map: %s", config->topic);
        return NULL;
    }

    usrl_pub_t *pub = calloc(1, sizeof(usrl_pub_t));
    pub->ctx = ctx;
    pub->shm_base = base;
    pub->map_size = ring_size;
    pub->block_on_full = config->block_on_full;
    strncpy(pub->topic, config->topic, 63);

    if (config->rate_limit_hz > 0) {
        usrl_quota_init(&pub->quota, config->rate_limit_hz);
        pub->use_limiter = true;
    }

    uint32_t my_id = __sync_fetch_and_add(&g_pub_id_seq, 1);
    usrl_pub_init(&pub->core, base, config->topic, my_id);

    USRL_INFO("API", "Pub Ready: %s [ID=%u]", config->topic, my_id);
    return pub;
}

int usrl_pub_send(usrl_pub_t *pub, const void *data, uint32_t len)
{
    if (!pub || !data) return -1;

    if (pub->use_limiter) {
        if (usrl_quota_check(&pub->quota)) {
            if (pub->block_on_full) {
                usleep(usrl_backoff_exponential(1));
            } else {
                return -1; // Drop due to rate limit
            }
        }
    }

    int res = usrl_pub_publish(&pub->core, data, len);
    while (res != 0 && pub->block_on_full) {
        usleep(1);
        res = usrl_pub_publish(&pub->core, data, len);
    }
    return res;
}

void usrl_pub_get_health(usrl_pub_t *pub, usrl_health_t *out)
{
    if (!pub || !out) return;
    RingHealth *rh = usrl_health_get(pub->shm_base, pub->topic);
    if (rh) {
        out->operations = rh->pub_health.total_published;
        out->errors     = rh->pub_health.total_dropped;
        out->rate_hz    = rh->pub_health.publish_rate_hz;
        out->lag        = 0;
        out->healthy    = (out->errors == 0);
        free(rh);
    } else memset(out, 0, sizeof(*out));
}

void usrl_pub_destroy(usrl_pub_t *pub)
{
    if (!pub) return;
    if (pub->shm_base && pub->map_size > 0) {
        munmap(pub->shm_base, pub->map_size);
    }
    free(pub);
}

/* ============================================================================
 * SUBSCRIBER
 * ============================================================================ */

usrl_sub_t *usrl_sub_create(usrl_ctx_t *ctx, const char *topic)
{
    if (!ctx || !topic) {
        USRL_ERROR("API", "usrl_sub_create: Invalid args");
        return NULL;
    }
    char shm_path[128];
    snprintf(shm_path, 128, "/usrl-%s", topic);
    
    // Use a fixed safe map size. In prod, read header first to get exact size.
    size_t map_size = 32 * 1024 * 1024;
    void *base = usrl_core_map(shm_path, map_size);
    if (!base) {
        USRL_ERROR("API", "Sub map fail: %s", topic);
        return NULL;
    }

    usrl_sub_t *sub = calloc(1, sizeof(usrl_sub_t));
    sub->ctx = ctx;
    sub->shm_base = base;
    sub->map_size = map_size;
    strncpy(sub->topic, topic, 63);
    
    usrl_sub_init(&sub->core, base, topic);
    return sub;
}

int usrl_sub_recv(usrl_sub_t *sub, void *buffer, uint32_t max_len)
{
    if (!sub || !buffer) return -1;
    
    int ret = usrl_sub_next(&sub->core, buffer, max_len, NULL);
    
    // Local stats tracking
    if (ret > 0) {
        sub->local_ops++;
    } else if (ret < 0) {
        sub->local_skips++;
    }
    
    return ret;
}

void usrl_sub_get_health(usrl_sub_t *sub, usrl_health_t *out)
{
    if (!sub || !out) return;

    out->operations = sub->local_ops;
    out->errors     = sub->local_skips;
    out->rate_hz    = 0; 

    // Lag Calculation
    if (sub->core.desc) {
        // We need to fetch the writer's head position from shared memory
        uint64_t w_head = usrl_swmr_total_published(sub->core.desc);
        uint64_t my_seq = sub->core.last_seq;
        
        if (w_head > my_seq) {
            out->lag = w_head - my_seq;
        } else {
            out->lag = 0;
        }
    } else {
        out->lag = 0;
    }
    
    out->healthy = (out->lag < 4096); 
}

void usrl_sub_destroy(usrl_sub_t *sub)
{
    if (!sub) return;
    if (sub->shm_base && sub->map_size > 0) {
        munmap(sub->shm_base, sub->map_size);
    }
    free(sub);
}
