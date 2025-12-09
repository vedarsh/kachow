/**
 * @file space_cert_suite.c
 * @brief USRL Spaceflight Certification Suite (Sanity, Integrity, Backpressure, Jitter).
 */

#define _GNU_SOURCE
#include "usrl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

/* --- PORTABLE CPU RELAX --- */
#if defined(__aarch64__) || defined(__arm__)
  #define cpu_relax() __asm__ volatile("yield" ::: "memory")
#elif defined(__x86_64__) || defined(__i386__)
  #define cpu_relax() __asm__ volatile("pause" ::: "memory")
#else
  #define cpu_relax() usleep(0)
#endif

#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_RESET   "\x1b[0m"

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { printf(COLOR_RED "[FAIL] %s\n" COLOR_RESET, msg); return -1; } } while(0)

#define ASSERT_FALSE(cond, msg) \
    do { if (cond) { printf(COLOR_RED "[FAIL] %s\n" COLOR_RESET, msg); return -1; } } while(0)

#define LOG_TEST(name) printf(COLOR_YELLOW "\n[TEST] %s..." COLOR_RESET "\n", name)
#define PASS_TEST() printf(COLOR_GREEN " [PASS]\n" COLOR_RESET)

/* --- TEST 1: API SANITIZATION --- */
int test_api_sanitization(void) {
    LOG_TEST("API Sanitization (Null/Invalid Inputs)");

    usrl_sys_config_t sys_cfg = { .app_name = "Sanity", .log_level = 0 };
    usrl_ctx_t *ctx = usrl_init(&sys_cfg);
    ASSERT_TRUE(ctx != NULL, "Context init failed");

    // Case 1: Create publisher with NULL context
    usrl_pub_config_t pcfg = { .topic = "t1", .slot_count = 1024, .slot_size = 128 };
    usrl_pub_t *p = usrl_pub_create(NULL, &pcfg);
    ASSERT_TRUE(p == NULL, "Should fail with NULL context");

    // Case 2: Create publisher with NULL config
    p = usrl_pub_create(ctx, NULL);
    ASSERT_TRUE(p == NULL, "Should fail with NULL config");

    // Case 3: Create publisher with NULL topic
    pcfg.topic = NULL;
    p = usrl_pub_create(ctx, &pcfg);
    ASSERT_TRUE(p == NULL, "Should fail with NULL topic");

    // Case 4: Send NULL data
    pcfg.topic = "valid_topic";
    p = usrl_pub_create(ctx, &pcfg);
    ASSERT_TRUE(p != NULL, "Valid create failed");
    int res = usrl_pub_send(p, NULL, 10);
    ASSERT_TRUE(res != 0, "Should fail sending NULL data");

    // Case 5: Subscriber invalid init
    usrl_sub_t *s = usrl_sub_create(NULL, "valid_topic");
    ASSERT_TRUE(s == NULL, "Should fail sub with NULL context");

    usrl_pub_destroy(p);
    usrl_shutdown(ctx);
    PASS_TEST();
    return 0;
}

/* --- TEST 2: DATA INTEGRITY (TORN READS) --- */
typedef struct {
    uint64_t signature_head;
    uint8_t  payload[100];
    uint64_t signature_tail;
} integrity_pkt_t;

volatile bool g_integrity_run = true;
usrl_pub_t *g_pub;

void *integrity_writer(void *arg) {
    integrity_pkt_t pkt;
    uint64_t ctr = 0;
    while (g_integrity_run) {
        ctr++;
        pkt.signature_head = ctr;
        pkt.signature_tail = ctr;
        memset(pkt.payload, (int)(ctr & 0xFF), sizeof(pkt.payload));
        usrl_pub_send(g_pub, &pkt, sizeof(pkt));
    }
    return NULL;
}

int test_integrity(void) {
    LOG_TEST("Data Integrity (Torn Read Detection under Contention)");
    
    usrl_sys_config_t sys_cfg = { .app_name = "Integrity", .log_level = 0 };
    usrl_ctx_t *ctx = usrl_init(&sys_cfg);
    
    usrl_pub_config_t pcfg = {
        .topic = "integrity",
        .ring_type = USRL_RING_SWMR,
        .slot_count = 1024,
        .slot_size = sizeof(integrity_pkt_t) + 64,
        .block_on_full = false 
    };
    g_pub = usrl_pub_create(ctx, &pcfg);
    usrl_sub_t *sub = usrl_sub_create(ctx, "integrity");

    pthread_t t_id;
    g_integrity_run = true;
    pthread_create(&t_id, NULL, integrity_writer, NULL);

    uint64_t received = 0;
    uint64_t start = time(NULL);
    
    while (time(NULL) - start < 2) { 
        integrity_pkt_t pkt;
        int len = usrl_sub_recv(sub, &pkt, sizeof(pkt));
        if (len > 0) {
            received++;
            if (pkt.signature_head != pkt.signature_tail) {
                g_integrity_run = false;
                pthread_join(t_id, NULL);
                ASSERT_FALSE(true, "TORN READ DETECTED! Integrity violation.");
            }
        }
    }

    g_integrity_run = false;
    pthread_join(t_id, NULL);
    
    printf("    Verified %lu high-velocity packets.\n", received);
    usrl_pub_destroy(g_pub);
    usrl_sub_destroy(sub);
    usrl_shutdown(ctx);
    PASS_TEST();
    return 0;
}

/* --- TEST 3: BACKPRESSURE (HARDENED) --- */
int test_backpressure(void) {
    LOG_TEST("Backpressure & Flow Control");

    usrl_sys_config_t sys_cfg = { .app_name = "Backpressure", .log_level = 0 };
    usrl_ctx_t *ctx = usrl_init(&sys_cfg);

    usrl_pub_config_t pcfg = {
        .topic = "limited",
        .slot_count = 128,
        .slot_size = 64,
        .rate_limit_hz = 10, // 10 Hz
        .block_on_full = false
    };
    usrl_pub_t *pub = usrl_pub_create(ctx, &pcfg);

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    int sent = 0;
    int drops = 0;
    int attempts = 10000; 

    for (int i = 0; i < attempts; i++) {
        int res = usrl_pub_send(pub, "data", 4);
        if (res == 0) sent++;
        else drops++;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed = (ts_end.tv_sec - ts_start.tv_sec) + 
                     (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    printf("    Attempts: %d, Sent: %d, Dropped: %d, Time: %.4fs\n", 
           attempts, sent, drops, elapsed);

    ASSERT_TRUE(drops > (attempts * 0.90), "Rate limiter failed to shed >90% of load");
    
    usrl_pub_destroy(pub);
    usrl_shutdown(ctx);
    PASS_TEST();
    return 0;
}

/* --- TEST 4: RING WRAP-AROUND & LAG RECOVERY --- */
int test_wrap_around(void) {
    LOG_TEST("Ring Wrap-Around & Lag Recovery");

    usrl_sys_config_t sys_cfg = { .app_name = "Wrap", .log_level = 0 };
    usrl_ctx_t *ctx = usrl_init(&sys_cfg);

    usrl_pub_config_t pcfg = {
        .topic = "small_ring",
        .slot_count = 16, 
        .slot_size = 64,
        .block_on_full = false
    };
    usrl_pub_t *pub = usrl_pub_create(ctx, &pcfg);
    usrl_sub_t *sub = usrl_sub_create(ctx, "small_ring");

    for (int i = 0; i < 32; i++) {
        uint64_t val = i;
        usrl_pub_send(pub, &val, sizeof(val));
    }

    uint64_t received_val;
    int rx_count = 0;
    while (usrl_sub_recv(sub, &received_val, sizeof(received_val)) > 0) {
        rx_count++;
        if (rx_count == 1) {
            ASSERT_TRUE(received_val > 0, "Subscriber read stale overwritten data (Seq 0)");
            printf("    Recovered at seq %lu\n", received_val);
        }
    }

    usrl_pub_destroy(pub);
    usrl_sub_destroy(sub);
    usrl_shutdown(ctx);
    PASS_TEST();
    return 0;
}

/* --- TEST 5: JITTER & STABILITY --- */
#define JITTER_SAMPLES 100000
#define HARD_DEADLINE_NS 50000 // 50 us
#define WARMUP 1000

int test_jitter_stability(void) {
    LOG_TEST("Jitter & Latency Stability Characterization");

    usrl_sys_config_t sys_cfg = { .app_name = "Jitter", .log_level = 0 };
    usrl_ctx_t *ctx = usrl_init(&sys_cfg);

    usrl_pub_config_t pcfg = {
        .topic = "jitter_test",
        .ring_type = USRL_RING_SWMR,
        .slot_count = 8192,
        .slot_size = 64,
        .block_on_full = true 
    };
    usrl_pub_t *pub = usrl_pub_create(ctx, &pcfg);
    usrl_sub_t *sub = usrl_sub_create(ctx, "jitter_test");

    uint64_t max_lat = 0;
    uint64_t min_lat = 999999999;
    uint64_t sum_lat = 0;
    uint64_t spikes = 0;

    uint64_t payload = 0xDEADBEEF;

    for (int i = 0; i < JITTER_SAMPLES + WARMUP; i++) {
        struct timespec ts_start, ts_now;
        
        clock_gettime(CLOCK_MONOTONIC, &ts_start);
        usrl_pub_send(pub, &payload, sizeof(payload));

        uint64_t rx_val;
        while (usrl_sub_recv(sub, &rx_val, sizeof(rx_val)) <= 0) {
            cpu_relax(); /* FIX: Use portable relax */
        }

        clock_gettime(CLOCK_MONOTONIC, &ts_now);

        if (i >= WARMUP) {
            uint64_t lat_ns = (ts_now.tv_sec - ts_start.tv_sec) * 1000000000ULL + 
                              (ts_now.tv_nsec - ts_start.tv_nsec);
            
            sum_lat += lat_ns;
            if (lat_ns > max_lat) max_lat = lat_ns;
            if (lat_ns < min_lat) min_lat = lat_ns;

            if (lat_ns > HARD_DEADLINE_NS) {
                spikes++;
            }
        }
    }

    double avg = (double)sum_lat / JITTER_SAMPLES;
    
    printf("    Samples: %d\n", JITTER_SAMPLES);
    printf("    Min: %lu ns\n", min_lat);
    printf("    Avg: %.2f ns\n", avg);
    printf("    Max: %lu ns\n", max_lat);
    printf("    Spikes (>%u ns): %lu\n", HARD_DEADLINE_NS, spikes);

    if (spikes > 0) {
        printf(COLOR_YELLOW "    [WARN] Jitter anomalies detected.\n" COLOR_RESET);
        if (max_lat > 1000000) ASSERT_FALSE(true, "Catastrophic Latency (>1ms)");
    }

    usrl_pub_destroy(pub);
    usrl_sub_destroy(sub);
    usrl_shutdown(ctx);
    PASS_TEST();
    return 0;
}

/* --- MAIN RUNNER --- */
int main(void) {
    printf("========================================================\n");
    printf("  USRL SPACEFLIGHT CERTIFICATION SUITE                  \n");
    printf("  Target: Unified API (usrl.h)                          \n");
    printf("========================================================\n");

    if (test_api_sanitization() != 0) return 1;
    if (test_integrity() != 0) return 1;
    if (test_backpressure() != 0) return 1;
    if (test_wrap_around() != 0) return 1;
    if (test_jitter_stability() != 0) return 1;

    printf("\n========================================================\n");
    printf(COLOR_GREEN "  ALL SYSTEMS NOMINAL. CERTIFICATION PASSED.    " COLOR_RESET "\n");
    printf("========================================================\n");
    return 0;
}
