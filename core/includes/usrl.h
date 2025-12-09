/**
 * @file usrl_api.h
 * @brief USRL Unified Facade.
 *
 * Centralizes access to Core, Ring, Health, Backpressure, and Logging.
 * No hidden defaults. Total control via configuration structs.
 */

#ifndef USRL_API_H
#define USRL_API_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "usrl_logging.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 1. UNIFIED TYPES
 * ============================================================================ */

typedef struct usrl_ctx usrl_ctx_t;
typedef struct usrl_pub usrl_pub_t;
typedef struct usrl_sub usrl_sub_t;

typedef enum {
    USRL_RING_SWMR = 0, /* Single-Writer / Multi-Reader (Lowest Latency) */
    USRL_RING_MWMR = 1  /* Multi-Writer / Multi-Reader (Thread-Safe) */
} usrl_ring_type_t;

// typedef enum {
//     USRL_LOG_NONE = 0,
//     USRL_LOG_ERROR,
//     USRL_LOG_WARN,
//     USRL_LOG_INFO,
//     USRL_LOG_DEBUG
// } usrl_log_level_t;

/* ============================================================================
 * 2. CONFIGURATION STRUCTS (The "Control Panel")
 * ============================================================================ */

/**
 * @brief Global System Configuration
 */
typedef struct {
    const char *app_name;
    UsrlLogLevel log_level;
    const char *log_file_path; // NULL for stderr
} usrl_sys_config_t;

/**
 * @brief Publisher Configuration (Exposes ALL features)
 */
typedef struct {
    const char *topic;
    
    /* Memory / Topology */
    usrl_ring_type_t ring_type;
    uint32_t slot_count;    // e.g. 4096
    uint32_t slot_size;     // e.g. 1024
    
    /* Flow Control (Backpressure) */
    uint64_t rate_limit_hz; // 0 = Unlimited
    bool block_on_full;     // true = Spin wait, false = Drop immediately
    
    /* Schema (Optional) */
    const char *schema_name;
    // (In a full implementation, you'd pass schema definition fields here)
} usrl_pub_config_t;

/**
 * @brief Health Snapshot
 */
typedef struct {
    uint64_t operations;    // Published or Read
    uint64_t errors;        // Dropped or Skipped
    uint64_t rate_hz;       // Throughput
    uint64_t lag;           // Subscriber lag (0 for pubs)
    bool healthy;           // Based on internal thresholds
} usrl_health_t;

/* ============================================================================
 * 3. SYSTEM LIFECYCLE
 * ============================================================================ */

/**
 * @brief Initialize the unified context.
 * Sets up logging and shared memory basics.
 */
usrl_ctx_t *usrl_init(const usrl_sys_config_t *config);

/**
 * @brief Shutdown the system and release all resources.
 */
void usrl_shutdown(usrl_ctx_t *ctx);

/* ============================================================================
 * 4. PUBLISHER API
 * ============================================================================ */

/**
 * @brief Create a publisher using the full config struct.
 * Handles: Core init, SHM mapping, ID generation, Backpressure init.
 */
usrl_pub_t *usrl_pub_create(usrl_ctx_t *ctx, const usrl_pub_config_t *config);

/**
 * @brief Publish data.
 * Handles: Backpressure checks, spinning (if blocked), and core write.
 */
int usrl_pub_send(usrl_pub_t *pub, const void *data, uint32_t len);

/**
 * @brief Get health metrics for this specific publisher.
 * Wraps usrl_health_get().
 */
void usrl_pub_get_health(usrl_pub_t *pub, usrl_health_t *out);

/**
 * @brief Destroy publisher.
 */
void usrl_pub_destroy(usrl_pub_t *pub);

/* ============================================================================
 * 5. SUBSCRIBER API
 * ============================================================================ */

/**
 * @brief Create a subscriber.
 */
usrl_sub_t *usrl_sub_create(usrl_ctx_t *ctx, const char *topic);

/**
 * @brief Receive data.
 */
int usrl_sub_recv(usrl_sub_t *sub, void *buffer, uint32_t max_len);

/**
 * @brief Get health metrics for this specific subscriber (Lag, throughput).
 */
void usrl_sub_get_health(usrl_sub_t *sub, usrl_health_t *out);

void usrl_sub_destroy(usrl_sub_t *sub);

#ifdef __cplusplus
}
#endif
#endif /* USRL_API_H */
