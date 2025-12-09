/**
 * @file usrl_logging.c
 * @brief Thread-safe logging implementation
 */

#include "usrl_logging.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h> /* Added for locking if preferred, or rely on single fprintf */

static FILE *log_file = NULL;
static FILE *trace_file = NULL;
static UsrlLogLevel min_log_level = USRL_LOG_INFO;

/* 
 * Use a mutex to prevent lines from different threads getting mixed up.
 * Alternatively, constructing a single string and calling fprintf once 
 * is usually atomic on POSIX systems.
 */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static inline uint64_t usrl_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static const char *level_str(UsrlLogLevel level)
{
    switch (level) {
        case USRL_LOG_ERROR: return "ERROR";
        case USRL_LOG_WARN: return "WARN";
        case USRL_LOG_INFO: return "INFO";
        case USRL_LOG_DEBUG: return "DEBUG";
        case USRL_LOG_TRACE: return "TRACE";
        default: return "UNKNOWN";
    }
}

int usrl_logging_init(const char *log_file_path, UsrlLogLevel min_level)
{
    min_log_level = min_level;
    
    if (log_file_path) {
        log_file = fopen(log_file_path, "a");
        if (!log_file)
            return -1;
        setvbuf(log_file, NULL, _IOLBF, 4096);
    } else {
        log_file = stderr;
    }
    
    return 0;
}

void usrl_log(UsrlLogLevel level, const char *module, uint32_t line,
             const char *fmt, ...)
{
    if (level > min_log_level || !log_file)
        return;

    uint64_t now = usrl_now_ns();
    char buf[1024]; // Temp buffer for the message
    char final_log[2048]; // Buffer for the full line

    /* Format the user message first */
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    /* Construct the complete log line */
    snprintf(final_log, sizeof(final_log), "[%lu.%03lu] [%s] [%s:%u] %s\n",
            now / 1000000000ULL,
            (now % 1000000000ULL) / 1000000,
            level_str(level),
            module,
            line,
            buf);

    /* Atomic write */
    pthread_mutex_lock(&log_lock);
    fputs(final_log, log_file);
    pthread_mutex_unlock(&log_lock);
}

void usrl_log_metric(const char *module, const char *metric_name, int64_t value)
{
    if (!log_file) return;

    uint64_t now = usrl_now_ns();
    
    pthread_mutex_lock(&log_lock);
    fprintf(log_file, "[%lu.%03lu] [METRIC] [%s] %s=%ld\n",
            now / 1000000000ULL,
            (now % 1000000000ULL) / 1000000,
            module,
            metric_name,
            value);
    pthread_mutex_unlock(&log_lock);
}

void usrl_log_lag(const char *topic, uint64_t lag_slots, uint64_t threshold)
{
    /* Use the main log function to ensure locking */
    usrl_log(USRL_LOG_WARN, "backpressure", 0, 
             "Topic %s: lag=%lu slots (threshold=%lu)", topic, lag_slots, threshold);
}

void usrl_log_drop(const char *topic, uint32_t drop_count)
{
    usrl_log(USRL_LOG_ERROR, "ring", 0, 
             "Topic %s: dropped %u messages", topic, drop_count);
}

void usrl_log_flush(void)
{
    if (log_file && log_file != stderr)
        fflush(log_file);
}

void usrl_logging_shutdown(void)
{
    if (log_file && log_file != stderr) {
        fclose(log_file);
        log_file = NULL;
    }
}

/* Tracing remains mostly the same, usually writes are small enough to be atomic,
   but adding a lock is safer if tracing from multiple threads. */
static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;

int usrl_tracing_init(const char *trace_file_path)
{
    trace_file = fopen(trace_file_path, "w");
    if (!trace_file) return -1;

    fprintf(trace_file, "timestamp_ns,duration_ns,event_name,publisher,sequence,payload_size\n");
    setvbuf(trace_file, NULL, _IOLBF, 8192);
    return 0;
}

void usrl_trace_event(const char *event_name, const char *publisher,
                     uint64_t sequence, uint32_t payload_size,
                     uint64_t duration_ns)
{
    if (!trace_file) return;

    uint64_t now = usrl_now_ns();
    
    pthread_mutex_lock(&trace_lock);
    fprintf(trace_file, "%lu,%lu,%s,%s,%lu,%u\n",
            now,
            duration_ns,
            event_name,
            publisher ? publisher : "unknown",
            sequence,
            payload_size);
    pthread_mutex_unlock(&trace_lock);
}

void usrl_trace_summary(void)
{
    if (!trace_file) return;
    fprintf(stderr, "Trace data written to trace file\n");
}

void usrl_tracing_shutdown(void)
{
    if (trace_file) {
        fclose(trace_file);
        trace_file = NULL;
    }
}
