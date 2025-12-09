#ifndef USRL_LOGGING_H
#define USRL_LOGGING_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>

typedef enum {
    USRL_LOG_ERROR = 0,
    USRL_LOG_WARN,
    USRL_LOG_INFO,
    USRL_LOG_DEBUG,
    USRL_LOG_TRACE,
} UsrlLogLevel;

typedef struct {
    uint64_t timestamp_ns;
    UsrlLogLevel level;
    const char *module;
    const char *message;
    uint32_t line;
    int64_t value1;
    int64_t value2;
    const char *context;
} UsrlLogEntry;

typedef struct {
    uint64_t timestamp_ns;
    uint64_t duration_ns;
    const char *event_name;
    const char *publisher;
    uint64_t sequence;
    uint32_t payload_size;
} UsrlTraceEvent;

int usrl_logging_init(const char *log_file, UsrlLogLevel min_level);
void usrl_log(UsrlLogLevel level, const char *module, uint32_t line,
             const char *fmt, ...);
void usrl_log_metric(const char *module, const char *metric_name, int64_t value);
void usrl_log_lag(const char *topic, uint64_t lag_slots, uint64_t threshold);
void usrl_log_drop(const char *topic, uint32_t drop_count);
void usrl_log_flush(void);
void usrl_logging_shutdown(void);

int usrl_tracing_init(const char *trace_file);
void usrl_trace_event(const char *event_name, const char *publisher,
                     uint64_t sequence, uint32_t payload_size,
                     uint64_t duration_ns);
void usrl_trace_summary(void);
void usrl_tracing_shutdown(void);

#define USRL_ERROR(mod, fmt, ...) usrl_log(USRL_LOG_ERROR, mod, __LINE__, fmt, ##__VA_ARGS__)
#define USRL_WARN(mod, fmt, ...) usrl_log(USRL_LOG_WARN, mod, __LINE__, fmt, ##__VA_ARGS__)
#define USRL_INFO(mod, fmt, ...) usrl_log(USRL_LOG_INFO, mod, __LINE__, fmt, ##__VA_ARGS__)
#define USRL_DEBUG(mod, fmt, ...) usrl_log(USRL_LOG_DEBUG, mod, __LINE__, fmt, ##__VA_ARGS__)

#endif /* USRL_LOGGING_H */
