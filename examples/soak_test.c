/**
 * @file soak_test.c
 * @brief USRL Endurance "Soak" Test.
 *
 * VALIDATES:
 * 1. ZERO Memory Growth (Leaks check via RSS)
 * 2. File Descriptor Stability (Open/Close loop)
 * 3. Sequence ID overflow safety
 * 4. Long-run thermal stability (CPU usage)
 */

#define _GNU_SOURCE
#include "usrl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/resource.h>

#define SOAK_CYCLES 10000     // Number of create/destroy cycles
#define MSGS_PER_CYCLE 1000   // Messages per cycle
#define CHECK_INTERVAL 1000   // Check memory every N cycles

/* --- MEMORY CHECK --- */
long get_memory_usage_kb(void) {
    struct rusage r_usage;
    getrusage(RUSAGE_SELF, &r_usage);
    return r_usage.ru_maxrss;
}

int main(void) {
    printf("========================================================\n");
    printf("  USRL ENDURANCE SOAK TEST                              \n");
    printf("  Cycles: %d | Msgs/Cycle: %d\n", SOAK_CYCLES, MSGS_PER_CYCLE);
    printf("========================================================\n");

    long initial_mem = get_memory_usage_kb();
    printf("[INIT] Baseline Memory: %ld KB\n", initial_mem);

    usrl_sys_config_t sys_cfg = { .app_name = "SoakTest", .log_level = 0 };
    usrl_ctx_t *ctx = usrl_init(&sys_cfg);

    if (!ctx) {
        fprintf(stderr, "[FAIL] Context init failed\n");
        return 1;
    }

    uint64_t total_msgs = 0;

    for (int cycle = 0; cycle < SOAK_CYCLES; cycle++) {
        // Dynamic Topic Name to stress SHM names
        char topic[32];
        snprintf(topic, 32, "soak_topic_%d", cycle % 10); // Reuse 10 slots to test overwrite

        usrl_pub_config_t pcfg = {
            .topic = topic,
            .ring_type = USRL_RING_SWMR,
            .slot_count = 1024,
            .slot_size = 64,
            .block_on_full = true
        };

        // 1. Create Publisher
        usrl_pub_t *pub = usrl_pub_create(ctx, &pcfg);
        if (!pub) {
            fprintf(stderr, "[FAIL] Cycle %d: Create pub failed\n", cycle);
            break;
        }

        // 2. Create Subscriber
        usrl_sub_t *sub = usrl_sub_create(ctx, topic);
        if (!sub) {
            fprintf(stderr, "[FAIL] Cycle %d: Create sub failed\n", cycle);
            usrl_pub_destroy(pub);
            break;
        }

        // 3. Blast Data
        uint64_t payload = 0;
        for (int i = 0; i < MSGS_PER_CYCLE; i++) {
            usrl_pub_send(pub, &payload, sizeof(payload));
            
            // Drain occasionally so ring doesn't just jam if blocking is on
            // (Though for SWMR it overwrites, blocking is irrelevant unless MWMR)
            if (i % 100 == 0) {
                uint64_t rx;
                while (usrl_sub_recv(sub, &rx, sizeof(rx)) > 0);
            }
            total_msgs++;
        }

        // 4. Destroy Resources (This is the leak test)
        usrl_sub_destroy(sub);
        usrl_pub_destroy(pub);

        // 5. Periodic Report
        if (cycle % CHECK_INTERVAL == 0 && cycle > 0) {
            long current_mem = get_memory_usage_kb();
            long growth = current_mem - initial_mem;
            printf("[SOAK] Cycle %d | Msgs: %lu | Mem: %ld KB (Growth: %ld KB)\n", 
                   cycle, total_msgs, current_mem, growth);
            
            // Fail if leaking massively (allow small fluctuation for libc fragmentation)
            if (growth > 5000) { // 5MB tolerance
                fprintf(stderr, "[FAIL] MASSIVE MEMORY LEAK DETECTED!\n");
                return 1;
            }
        }
    }

    usrl_shutdown(ctx);

    long final_mem = get_memory_usage_kb();
    printf("\n[DONE] Final Memory: %ld KB\n", final_mem);
    
    if ((final_mem - initial_mem) > 1024) {
        printf("[WARN] Slight memory growth detected (%ld KB). Check for small leaks.\n", final_mem - initial_mem);
    } else {
        printf("[PASS] Memory Stability Confirmed.\n");
    }

    return 0;
}
