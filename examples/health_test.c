/**
 * @file health_test.c
 * @brief Verify Health Telemetry is active and accurate.
 */

#define _GNU_SOURCE
#include "usrl.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

int main(void) {
    printf("=== USRL HEALTH TELEMETRY CHECK ===\n");

    usrl_sys_config_t sys_cfg = { .app_name = "HealthTest", .log_level = 0 };
    usrl_ctx_t *ctx = usrl_init(&sys_cfg);

    // Create Pub/Sub
    usrl_pub_config_t pcfg = {
        .topic = "health_check",
        .ring_type = USRL_RING_SWMR,
        .slot_count = 1024,
        .slot_size = 64,
        .block_on_full = true 
    };
    usrl_pub_t *pub = usrl_pub_create(ctx, &pcfg);
    usrl_sub_t *sub = usrl_sub_create(ctx, "health_check");

    printf("[TEST] Sending 50,000 messages...\n");

    uint64_t payload = 0;
    for (int i = 0; i < 50000; i++) {
        usrl_pub_send(pub, &payload, sizeof(payload));
        usrl_sub_recv(sub, &payload, sizeof(payload)); // Drain immediately
    }

    // Check Stats
    usrl_health_t p_health = {0}, s_health = {0};
    usrl_pub_get_health(pub, &p_health);
    usrl_sub_get_health(sub, &s_health);

    printf("PUB Stats -> Ops: %lu | Errors: %lu\n", p_health.operations, p_health.errors);
    printf("SUB Stats -> Ops: %lu | Errors: %lu | Lag: %lu\n", s_health.operations, s_health.errors, s_health.lag);

    if (p_health.operations == 0 || s_health.operations == 0) {
        printf("\x1b[31m[FAIL] Stats are ZERO. Health system is broken.\x1b[0m\n");
        return 1;
    } else {
        printf("\x1b[32m[PASS] Telemetry Active.\x1b[0m\n");
    }

    usrl_pub_destroy(pub);
    usrl_sub_destroy(sub);
    usrl_shutdown(ctx);
    return 0;
}
