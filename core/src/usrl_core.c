#include "usrl_core.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEBUG
#ifdef DEBUG
#define DEBUG_PRINT_CORE(...) \
    do { printf("[DEBUG][CORE] " __VA_ARGS__); fflush(stdout); } while (0)
#else
#define DEBUG_PRINT_CORE(...) ((void)0)
#endif


// helper for power of two
static uint32_t next_power_of_two_u32(uint32_t v) {
    if (v == 0) return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return ++v;
}


// this is the big update: we iterate through the config list to build the memory
int usrl_core_init(const char *path, uint64_t size, const UsrlTopicConfig *topics, uint32_t count) {
    DEBUG_PRINT_CORE("init path=%s size=%lu topics=%u\n", path, size, count);

    if (!path || size < 4096 || !topics || count == 0) {
        return -1;
    }

    // cleaning up old file
    shm_unlink(path);

    // making new file
    int fd = shm_open(path, O_CREAT | O_RDWR | O_EXCL, 0666);
    if (fd < 0) {
        DEBUG_PRINT_CORE("shm_open failed\n");
        return -1;
    }

    if (ftruncate(fd, (off_t)size) < 0) {
        DEBUG_PRINT_CORE("ftruncate failed\n");
        close(fd);
        return -2;
    }

    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        DEBUG_PRINT_CORE("mmap failed\n");
        close(fd);
        return -3;
    }

    memset(base, 0, size);

    // setting up global header
    CoreHeader *hdr = (CoreHeader*)base;
    hdr->magic   = USRL_MAGIC;
    hdr->version = 1;
    hdr->mmap_size = size;

    // topic table starts after the header
    uint64_t current_offset = usrl_align_up(sizeof(CoreHeader), USRL_ALIGNMENT);
    hdr->topic_table_offset = current_offset;
    hdr->topic_count        = count;

    // calculating where ring descriptors start (after the whole topic table)
    uint64_t ring_desc_start = usrl_align_up(
        current_offset + (sizeof(TopicEntry) * count),
        USRL_ALIGNMENT
    );

    // calculating where slots start (after the whole ring descriptor array)
    uint64_t slots_start = usrl_align_up(
        ring_desc_start + (sizeof(RingDesc) * count),
        USRL_ALIGNMENT
    );

    uint64_t next_free_slot_offset = slots_start;

    // loop through every topic in the config
    for (uint32_t i = 0; i < count; ++i) {
        DEBUG_PRINT_CORE("configuring topic %s\n", topics[i].name);

        // 1. Fill Topic Entry
        TopicEntry *t = (TopicEntry*)((uint8_t*)base + hdr->topic_table_offset + (i * sizeof(TopicEntry)));
        strncpy(t->name, topics[i].name, USRL_MAX_TOPIC_NAME - 1);

        // calculate where this topic's ring descriptor lives
        t->ring_desc_offset = ring_desc_start + (i * sizeof(RingDesc));

        // math for sizes
        uint32_t slots_pow2 = next_power_of_two_u32(topics[i].slot_count);
        uint32_t slot_sz_aligned = (uint32_t)usrl_align_up(sizeof(SlotHeader) + topics[i].slot_size, 8);

        t->slot_count = slots_pow2;
        t->slot_size  = slot_sz_aligned;

        // 2. Fill Ring Descriptor
        RingDesc *r = (RingDesc*)((uint8_t*)base + t->ring_desc_offset);
        r->slot_count = slots_pow2;
        r->slot_size  = slot_sz_aligned;
        r->base_offset = next_free_slot_offset;
        atomic_store_explicit(&r->w_head, 0, memory_order_relaxed);

        // 3. Reserve Slot Memory
        uint64_t total_bytes_for_topic = (uint64_t)slots_pow2 * slot_sz_aligned;

        // check if we ran out of memory
        if (next_free_slot_offset + total_bytes_for_topic > size) {
            DEBUG_PRINT_CORE("OOM! topic %s needs too much memory\n", topics[i].name);
            munmap(base, size);
            close(fd);
            return -4;
        }

        // init slots
        uint8_t *slot_ptr = (uint8_t*)base + next_free_slot_offset;
        // memory is already zeroed by memset above, but let's be explicitly safe for headers
        for (uint32_t k = 0; k < slots_pow2; ++k) {
             SlotHeader *sh = (SlotHeader*)(slot_ptr + ((uint64_t)k * slot_sz_aligned));
             atomic_store_explicit(&sh->seq, 0, memory_order_relaxed);
        }

        // move the pointer for the next topic
        next_free_slot_offset += total_bytes_for_topic;
        next_free_slot_offset = usrl_align_up(next_free_slot_offset, USRL_ALIGNMENT);
    }

    DEBUG_PRINT_CORE("used %lu / %lu bytes\n", next_free_slot_offset, size);

    munmap(base, size);
    close(fd);
    return 0;
}

void* usrl_core_map(const char *path, uint64_t size) {
    int fd = shm_open(path, O_RDWR, 0666);
    if (fd < 0) return NULL;

    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    close(fd);
    return base;
}

TopicEntry* usrl_get_topic(void *base, const char *name) {
    if (!base || !name) return NULL;
    CoreHeader *hdr = (CoreHeader*)base;
    if (hdr->magic != USRL_MAGIC) return NULL;

    TopicEntry *t = (TopicEntry*)((uint8_t*)base + hdr->topic_table_offset);
    for (uint32_t i = 0; i < hdr->topic_count; i++) {
        if (strncmp(t[i].name, name, USRL_MAX_TOPIC_NAME) == 0)
            return &t[i];
    }
    return NULL;
}
