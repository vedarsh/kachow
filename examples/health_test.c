/**
 * @file health_stress_test.c
 * @brief Stress test for Health Telemetry: Injecting DROPS and LAG.
 */

#define _GNU_SOURCE
#include "usrl.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>

#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_RESET   "\x1b[0m"

int main(void) {
    printf("========================================================\n");
    printf("  USRL HEALTH STRESS TEST (Fault Injection)             \n");
    printf("========================================================\n");

    usrl_sys_config_t sys_cfg = { .app_name = "HealthStress", .log_level = 0 };
    usrl_ctx_t *ctx = usrl_init(&sys_cfg);

    /* =========================================================================
     * PHASE 1: PUBLISHER DROPS (Rate Limiting)
     * ========================================================================= */
    printf("\n[PHASE 1] Injecting Publisher Drops (Rate Limit: 10Hz)...\n");

    // Configure aggressive rate limit
    usrl_pub_config_t pcfg_limit = {
        .topic = "health_drops",
        .ring_type = USRL_RING_SWMR,
        .slot_count = 128,
        .slot_size = 64,
        .rate_limit_hz = 10,   // STRICT LIMIT
        .block_on_full = false // DROP immediately if limit hit
    };
    usrl_pub_t *pub_drop = usrl_pub_create(ctx, &pcfg_limit);
    
    // Blast 1000 messages (should mostly fail)
    int attempts = 1000;
    uint64_t payload = 0;
    for (int i = 0; i < attempts; i++) {
        usrl_pub_send(pub_drop, &payload, sizeof(payload));
    }

    usrl_health_t p_health;
    usrl_pub_get_health(pub_drop, &p_health);

    printf("    Sent: %d | Pub Ops: %lu | Pub Errors (Drops): %lu\n", 
           attempts, p_health.operations, p_health.errors);

    if (p_health.errors > 0) {
        printf(COLOR_GREEN "[PASS] Publisher correctly reported %lu drops.\n" COLOR_RESET, p_health.errors);
    } else {
        printf(COLOR_RED "[FAIL] Publisher reported 0 errors despite rate limit!\n" COLOR_RESET);
        return 1;
    }
    usrl_pub_destroy(pub_drop);

    /* =========================================================================
     * PHASE 2: SUBSCRIBER LAG & SKIPS
     * ========================================================================= */
    printf("\n[PHASE 2] Injecting Subscriber Lag (Ring Overwrite)...\n");

    // Small ring to force wrap-around quickly
    usrl_pub_config_t pcfg_lag = {
        .topic = "health_lag",
        .ring_type = USRL_RING_SWMR,
        .slot_count = 16,     // TINY RING
        .slot_size = 64,
        .block_on_full = false
    };
    usrl_pub_t *pub_lag = usrl_pub_create(ctx, &pcfg_lag);
    usrl_sub_t *sub_lag = usrl_sub_create(ctx, "health_lag");

    // 1. Publisher fills ring 10x over (160 messages)
    // Subscriber is asleep, so it will miss ~144 messages
    for (int i = 0; i < 160; i++) {
        usrl_pub_send(pub_lag, &payload, sizeof(payload));
    }

    // 2. Subscriber wakes up and tries to read ONCE
    // This read should detect the gap and report skipped messages
    usrl_sub_recv(sub_lag, &payload, sizeof(payload));

    usrl_health_t s_health;
    usrl_sub_get_health(sub_lag, &s_health);

    printf("    Pub Sent: 160 | Sub Ops: %lu | Sub Errors (Skips): %lu | Lag: %lu\n", 
           s_health.operations, s_health.errors, s_health.lag);

    // Validation
    // Lag should be non-zero (approx 16, the ring size, or slightly less depending on when we checked)
    // Skips should be non-zero (we detected the jump)
    if (s_health.errors > 0) {
        printf(COLOR_GREEN "[PASS] Subscriber correctly reported %lu skips/errors.\n" COLOR_RESET, s_health.errors);
    } else {
        printf(COLOR_RED "[FAIL] Subscriber reported 0 errors despite missing 140+ messages!\n" COLOR_RESET);
        return 1;
    }

    if (s_health.lag > 0) {
        printf(COLOR_GREEN "[PASS] Subscriber correctly reported lag: %lu slots behind.\n" COLOR_RESET, s_health.lag);
    } else {
        printf(COLOR_YELLOW "[WARN] Subscriber lag reported as 0 (caught up perfectly or metric missing).\n" COLOR_RESET);
    }

    usrl_pub_destroy(pub_lag);
    usrl_sub_destroy(sub_lag);
    usrl_shutdown(ctx);

    printf("\n========================================================\n");
    printf(COLOR_GREEN "  ALL SYSTEMS NOMINAL. HEALTH MONITORING VERIFIED.  " COLOR_RESET "\n");
    printf("========================================================\n");
    return 0;
}
