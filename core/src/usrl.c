/**
 * @file usrl_api.c
 * @brief Unified Facade Implementation (Production Ready).
 * 
 * Implements the high-level C API for USRL, bridging the Core Ring buffer
 * with user-facing features like Health Monitoring, Rate Limiting, and Logging.
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

/* Accessor for Lag Calculation (from ring_swmr.c / ring_mwmr.c) */
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
    UsrlPublisher core;         /* SWMR core handle */
    UsrlMwmrPublisher core_mw;  /* MWMR core handle */
    PublishQuota quota;
    bool block_on_full;
    bool use_limiter;
    bool is_mwmr;
    char topic[64];
    void *shm_base;
    size_t map_size; 
    
    /* Local stats for API-layer drops (Rate Limiter) */
    uint64_t local_drops; 
};

struct usrl_sub {
    usrl_ctx_t *ctx;
    UsrlSubscriber core;
    char topic[64];
    void *shm_base;
    size_t map_size; 
    
    /* Local stats for API-layer tracking */
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
    if (!ctx || !config || !config->topic) return NULL;

    uint32_t sc = (config->slot_count > 0) ? config->slot_count : 4096;
    uint32_t ss = (config->slot_size > 0) ? config->slot_size : 1024;
    size_t ring_size = (size_t)sc * ss + (1024 * 1024); // +1MB for headers
    char shm_path[128];
    snprintf(shm_path, sizeof(shm_path), "/usrl-%s", config->topic);

    UsrlTopicConfig tcfg = {0};
    strncpy(tcfg.name, config->topic, 63);
    tcfg.slot_count = sc;
    tcfg.slot_size = ss;
    tcfg.type = (config->ring_type == USRL_RING_MWMR) ? USRL_RING_TYPE_MWMR : USRL_RING_TYPE_SWMR;

    /* Init Core if needed (Auto-create SHM) */
    if (usrl_core_init(shm_path, ring_size, &tcfg, 1) != 0) {
        USRL_ERROR("API", "Failed to init core ring (or exists): %s", config->topic);
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
    pub->is_mwmr = (config->ring_type == USRL_RING_MWMR);
    strncpy(pub->topic, config->topic, 63);

    if (config->rate_limit_hz > 0) {
        usrl_quota_init(&pub->quota, config->rate_limit_hz);
        pub->use_limiter = true;
    }

    uint32_t my_id = __sync_fetch_and_add(&g_pub_id_seq, 1);
    
    if (pub->is_mwmr) {
        usrl_mwmr_pub_init(&pub->core_mw, base, config->topic, my_id);
    } else {
        usrl_pub_init(&pub->core, base, config->topic, my_id);
    }

    return pub;
}

int usrl_pub_send(usrl_pub_t *pub, const void *data, uint32_t len)
{
    if (!pub || !data) return -1;

    /* Rate Limiting Logic */
    if (pub->use_limiter) {
        if (usrl_quota_check(&pub->quota)) {
            if (pub->block_on_full) {
                usleep(usrl_backoff_exponential(1));
            } else {
                pub->local_drops++; /* Track Drop */
                return -1; 
            }
        }
    }

    /* Publish (SWMR or MWMR) */
    int res;
    if (pub->is_mwmr) {
        res = usrl_mwmr_pub_publish(&pub->core_mw, data, len);
        
        /* Spin/Block if requested and ring full/busy */
        while ((res == USRL_RING_FULL || res == USRL_RING_TIMEOUT) && pub->block_on_full) {
            usleep(1);
            res = usrl_mwmr_pub_publish(&pub->core_mw, data, len);
        }
    } else {
        res = usrl_pub_publish(&pub->core, data, len);
        
        while (res == USRL_RING_FULL && pub->block_on_full) {
            usleep(1);
            res = usrl_pub_publish(&pub->core, data, len);
        }
    }
    
    /* Map Ring Error Codes to API Error Codes */
    if (res == USRL_RING_OK) return 0;
    
    /* If we failed (and didn't block), record drop */
    if (res != USRL_RING_OK) {
        // Only count as drop if not a usage error? 
        // Ring Full = Drop.
        if (res == USRL_RING_FULL) pub->local_drops++;
        return -1;
    }
    
    return 0;
}

void usrl_pub_get_health(usrl_pub_t *pub, usrl_health_t *out)
{
    if (!pub || !out) return;
    
    RingHealth *rh = usrl_health_get(pub->shm_base, pub->topic);
    
    if (rh) {
        out->operations = rh->pub_health.total_published;
        out->rate_hz    = rh->pub_health.publish_rate_hz;
        
        /* Combine SHM stats (dropped inside ring?) + API stats (rate limited) */
        /* Currently Core doesn't track ring-internal drops, so we rely on local */
        out->errors     = pub->local_drops;
        
        out->lag        = 0;
        out->healthy    = (out->errors == 0);
        usrl_health_free(rh); 
    } else {
        memset(out, 0, sizeof(*out));
        out->errors = pub->local_drops;
    }
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
    if (!ctx || !topic) return NULL;
    char shm_path[128];
    snprintf(shm_path, 128, "/usrl-%s", topic);
    
    /* Default Map Size (Should read from header really, but 32MB covers most cases) */
    size_t map_size = 32 * 1024 * 1024; 
    void *base = usrl_core_map(shm_path, map_size);
    if (!base) return NULL;

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
    
    /* Handle standardized ring codes */
    if (ret == USRL_RING_NO_DATA) {
        return -11; /* EAGAIN - No Message */
    }
    
    if (ret == USRL_RING_TRUNC) {
        sub->local_skips++; /* Truncation is data loss/error */
        return -1; 
    }
    
    if (ret == USRL_RING_ERROR) {
        return -1;
    }
    
    /* Success (ret >= 0, payload size) */
    sub->local_ops++;
    return ret;
}

void usrl_sub_get_health(usrl_sub_t *sub, usrl_health_t *out)
{
    if (!sub || !out) return;

    out->operations = sub->local_ops;
    
    /* Errors = Local Skips (API errors) + Core Skips (Ring jumps) */
    out->errors     = sub->local_skips + sub->core.skipped_count;
    
    out->rate_hz    = 0; 

    /* Calculate Lag */
    if (sub->core.desc) {
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
    
    /* Simple health heuristic */
    out->healthy = (out->lag < 100 && out->errors == 0); 
}

void usrl_sub_destroy(usrl_sub_t *sub)
{
    if (!sub) return;
    if (sub->shm_base && sub->map_size > 0) {
        munmap(sub->shm_base, sub->map_size);
    }
    free(sub);
}
