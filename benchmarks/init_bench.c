#include "usrl_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONFIG_TOPICS 64
#define CONFIG_FILE "usrl_config_bench.json"

// --- Helper Functions ---

char* skip_ws(char *p) {
    while (*p && (*p <= 32)) p++;
    return p;
}

char* find_key(char *json, const char *key) {
    char search[64];
    sprintf(search, "\"%s\"", key);
    char *loc = strstr(json, search);
    if (!loc) return NULL;

    loc += strlen(search);
    while (*loc && *loc != ':') loc++;
    if (*loc == ':') loc++;
    return skip_ws(loc);
}

void parse_string_val(char *p, char *dest, int max) {
    if (*p != '\"') return;
    p++;  // Skip opening quote
    int i = 0;
    while (*p && *p != '\"' && i < max - 1) {
        dest[i++] = *p++;
    }
    dest[i] = 0;
}

int parse_int_val(char *p) {
    skip_ws(p);  // Skip whitespace
    return atoi(p);
}

// --- Main ---

int main(void) {
    printf("[BENCH_INIT] Reading config from %s\n", CONFIG_FILE);

    FILE *f = fopen(CONFIG_FILE, "rb");
    if (!f) {
        printf("[BENCH_INIT] Error: could not open %s\n", CONFIG_FILE);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buffer = malloc(fsize + 1);
    fread(buffer, 1, fsize, f);
    buffer[fsize] = 0;
    fclose(f);

    UsrlTopicConfig topics[MAX_CONFIG_TOPICS];
    int count = 0;

    // Default 128MB
    uint64_t mem_size = 128 * 1024 * 1024;

    // Parse memory size
    char *mem_p = find_key(buffer, "memory_size_mb");
    if (mem_p) {
        int mb = parse_int_val(mem_p);
        if (mb > 0) mem_size = (uint64_t)mb * 1024 * 1024;
    }

    if (mem_size < 64 * 1024 * 1024) mem_size = 64 * 1024 * 1024;
    printf("[BENCH_INIT] Memory Size: %lu MB\n", mem_size / (1024*1024));

    // **FIXED: Robust topics parsing**
    char *topics_start = strstr(buffer, "\"topics\"");
    if (topics_start) {
        // Skip to array start "["
        topics_start = strchr(topics_start, '[');
        if (topics_start) {
            char *topic_start = topics_start;
            while ((topic_start = strchr(topic_start, '{')) != NULL && count < MAX_CONFIG_TOPICS) {
                char *name_p = find_key(topic_start, "name");
                char *slots_p = find_key(topic_start, "slots");
                char *size_p = find_key(topic_start, "payload_size");
                char *type_p = find_key(topic_start, "type");

                if (name_p && slots_p && size_p) {
                    parse_string_val(name_p, topics[count].name, USRL_MAX_TOPIC_NAME);
                    topics[count].slot_count = parse_int_val(slots_p);
                    topics[count].slot_size = parse_int_val(size_p);
                    topics[count].type = USRL_RING_TYPE_SWMR;  // Default

                    // Parse type properly
                    if (type_p) {
                        char type_str[16] = {0};
                        parse_string_val(type_p, type_str, 16);
                        if (strstr(type_str, "mwmr") || strstr(type_str, "MWMR")) {
                            topics[count].type = USRL_RING_TYPE_MWMR;
                        }
                    }

                    printf("  Loaded: %-20s (Slots: %d, Size: %d, Type: %s)\n",
                           topics[count].name, 
                           topics[count].slot_count, 
                           topics[count].slot_size,
                           topics[count].type == USRL_RING_TYPE_SWMR ? "SWMR" : "MWMR");
                    count++;
                }
                
                // Move to next topic object
                topic_start = strchr(topic_start, '}');
                if (topic_start) topic_start++;
            }
        }
    }

    free(buffer);

    printf("[BENCH_INIT] Loaded %d topics\n", count);

    if (count == 0) {
        printf("[BENCH_INIT] ERROR: No valid topics found in JSON!\n");
        return 1;
    }

    if (usrl_core_init("/usrl_core", mem_size, topics, count) == 0) {
        printf("[BENCH_INIT] Core initialized successfully.\n");
    } else {
        printf("[BENCH_INIT] FAILED to initialize core.\n");
        return 1;
    }

    return 0;
}
