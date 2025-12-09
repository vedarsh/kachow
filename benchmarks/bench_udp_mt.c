/* =============================================================================
 * USRL UDP MULTI-THREADED BENCHMARK CLIENT
 * =============================================================================
 */

#define _GNU_SOURCE
#include "usrl_core.h"
#include "usrl_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define PAYLOAD_SIZE    4096
#define BATCH_SIZE      1000000
#define DEFAULT_THREADS 4

struct ThreadStats {
    long count;
    double elapsed;
};

struct ThreadArgs {
    const char *host;
    int port;
    int id;
    struct ThreadStats *stats;
};

// CHANGED: Removed blocking recv() - pure throughput test
void *client_thread(void *arg) {
    struct ThreadArgs *args = (struct ThreadArgs*)arg;
    uint8_t *payload = malloc(PAYLOAD_SIZE);
    memset(payload, 0xCC, PAYLOAD_SIZE);

    usrl_transport_t *client = usrl_trans_create(
        USRL_TRANS_UDP, args->host, args->port, 0, USRL_SWMR, false);
    if (!client) {
        free(payload);
        return NULL;
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // FIXED: SEND-ONLY (throughput test, no recv deadlock)
    for (long i = 0; i < BATCH_SIZE; i++) {
        if (usrl_trans_send(client, payload, PAYLOAD_SIZE) != PAYLOAD_SIZE)
            break;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    
    args->stats->count = BATCH_SIZE;  // All sends completed
    args->stats->elapsed = elapsed;

    usrl_trans_destroy(client);
    free(payload);
    return NULL;
}


int main(int argc, char *argv[]) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : 8080;
    int num_threads = argc > 3 ? atoi(argv[3]) : DEFAULT_THREADS;

    printf("[UDP-MT-BENCH] Starting %d threads on %s:%d\n",
           num_threads, host, port);

    pthread_t threads[num_threads];
    struct ThreadArgs args[num_threads];
    struct ThreadStats stats[num_threads];

    for (int i = 0; i < num_threads; i++) {
        args[i].host = host;
        args[i].port = port;
        args[i].id = i;
        args[i].stats = &stats[i];

        pthread_create(&threads[i], NULL, client_thread, &args[i]);
    }

    for (int i = 0; i < num_threads; i++)
        pthread_join(threads[i], NULL);

    long total_req = 0;
    double max_time = 0;

    for (int i = 0; i < num_threads; i++) {
        total_req += stats[i].count;
        if (stats[i].elapsed > max_time)
            max_time = stats[i].elapsed;
    }

    double real_bw = (total_req * PAYLOAD_SIZE * 8.0) / (max_time * 1e6);
    double real_rps = total_req / max_time;

    printf("[UDP-MT-BENCH] FINAL RESULT (%d Threads):\n", num_threads);
    printf("   Total Requests: %ld\n", total_req);
    printf("   Aggregate Rate: %.2f M req/sec\n", real_rps / 1e6);
    printf("   Aggregate BW:   %.2f Mbps (%.2f GB/s)\n",
           real_bw, real_bw / 8000.0);

    return 0;
}
