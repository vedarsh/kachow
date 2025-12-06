#include "usrl_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_CONFIG_TOPICS 64
#define CONFIG_FILE "../usrl_config.json"

// simple helper to skip whitespace
char* skip_ws(char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

// naive json string finder: looks for "key": "value" or "key": number
// this is not a full parser, just enough to work
char* find_key(char *json, const char *key) {
    char search[64];
    sprintf(search, "\"%s\"", key);
    char *loc = strstr(json, search);
    if (!loc) return NULL;

    // move past the key
    loc += strlen(search);
    // move past colon
    while (*loc && *loc != ':') loc++;
    if (*loc == ':') loc++;

    return skip_ws(loc);
}

// extracts a string value "value"
void parse_string_val(char *p, char *dest, int max) {
    if (*p != '\"') return;
    p++; // skip quote
    int i = 0;
    while (*p && *p != '\"' && i < max - 1) {
        dest[i++] = *p++;
    }
    dest[i] = 0;
}

// extracts a number value
int parse_int_val(char *p) {
    return atoi(p);
}

int main(void) {
    printf("reading config from %s\n", CONFIG_FILE);

    // open the file
    FILE *f = fopen(CONFIG_FILE, "rb");
    if (!f) {
        printf("could not open config file\n");
        return 1;
    }

    // get file size
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    // read into buffer
    char *buffer = malloc(fsize + 1);
    fread(buffer, 1, fsize, f);
    buffer[fsize] = 0;
    fclose(f);

    // simple parsing logic
    UsrlTopicConfig topics[MAX_CONFIG_TOPICS];
    int count = 0;

    // check memory size override
    char *mem_p = find_key(buffer, "memory_size_mb");
    uint64_t mem_size = 4 * 1024 * 1024; // default 4MB
    if (mem_p) {
        int mb = parse_int_val(mem_p);
        if (mb > 0) mem_size = (uint64_t)mb * 1024 * 1024;
    }
    printf("memory size: %lu bytes\n", mem_size);

    // find start of topics array
    char *p = strstr(buffer, "\"topics\"");
    if (!p) {
        printf("no topics found in json\n");
        return 1;
    }

    // simplistic loop to find objects { ... }
    while ((p = strchr(p, '{')) != NULL) {
        if (count >= MAX_CONFIG_TOPICS) break;

        // inside an object, find name
        char *name_p = find_key(p, "name");
        char *slots_p = find_key(p, "slots");
        char *size_p = find_key(p, "payload_size");

        if (name_p && slots_p && size_p) {
            parse_string_val(name_p, topics[count].name, USRL_MAX_TOPIC_NAME);
            topics[count].slot_count = parse_int_val(slots_p);
            topics[count].slot_size  = parse_int_val(size_p);

            printf("found topic: %s (slots=%d, size=%d)\n",
                   topics[count].name, topics[count].slot_count, topics[count].slot_size);
            count++;
        }

        // move p forward to avoid finding same object
        p++;
    }

    free(buffer);

    // run init
    if (usrl_core_init("/usrl_core", mem_size, topics, count) == 0) {
        printf("core initialized successfully!\n");
    } else {
        printf("init failed!\n");
    }

    return 0;
}
