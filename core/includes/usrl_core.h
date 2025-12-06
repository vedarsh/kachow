#ifndef USRL_CORE_H
#define USRL_CORE_H

#include <stdint.h>
#include <stdatomic.h>

// magic number to verify the file
#define USRL_MAGIC         0x5553524C
#define USRL_MAX_TOPIC_NAME 64
#define USRL_ALIGNMENT     64


// helper to align numbers
static inline uint64_t usrl_align_up(uint64_t v, uint64_t a) {
    return (v + (a - 1)) & ~(a - 1);
}


// this struct helps us pass config from the json parser to the core
typedef struct {
    char name[USRL_MAX_TOPIC_NAME];
    uint32_t slot_count;
    uint32_t slot_size;
} UsrlTopicConfig;


// main file header
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t mmap_size;
    uint64_t topic_table_offset;
    uint32_t topic_count;
    uint32_t _pad;
} CoreHeader;


// entry in the topic table
typedef struct {
    char     name[USRL_MAX_TOPIC_NAME];
    uint64_t ring_desc_offset;
    uint32_t slot_count;
    uint32_t slot_size;
} TopicEntry;


// header for every single message slot
typedef struct {
    atomic_uint_fast64_t seq;
    uint64_t             timestamp_ns;
    uint32_t             payload_len;
    uint32_t             _pad;
} SlotHeader;


#ifndef __cplusplus
_Static_assert(sizeof(SlotHeader) % 8 == 0, "header size alignment wrong");
#endif


// descriptor for the ring buffer
typedef struct {
    uint32_t             slot_count;
    uint32_t             slot_size;
    uint64_t             base_offset;
    atomic_uint_fast64_t w_head;
    uint8_t              _pad[32];
} RingDesc;


// new init function that takes a list of topics
int        usrl_core_init(const char *path, uint64_t size, const UsrlTopicConfig *topics, uint32_t count);
void*      usrl_core_map(const char *path, uint64_t size);
TopicEntry* usrl_get_topic(void *base, const char *name);

#endif
