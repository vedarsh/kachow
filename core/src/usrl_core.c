/* usrl_core.c */
// code to init the shared memory
// it makes the file and maps it

#include "usrl_core.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// defaults if not defined
#ifndef DEFAULT_PAYLOAD_MAX
#define DEFAULT_PAYLOAD_MAX 120U
#endif

#ifndef SLOT_COUNT_VAL
#define SLOT_COUNT_VAL 1024U
#endif

// debug printing
#ifndef DEBUG_PRINT_CORE
#define DEBUG_PRINT_CORE(...) \
    do { printf("[DEBUG][CORE] " __VA_ARGS__); fflush(stdout); } while (0)
#endif

// helper to get next power of 2
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

int usrl_core_init(const char *path, uint64_t size)
{
    DEBUG_PRINT_CORE("INIT start path=%s size=%lu\n", path, size);

    // check if args are bad
    if (!path || size < 4096) {
        DEBUG_PRINT_CORE("INIT ERROR: invalid args\n");
        return -1;
    }

    // delete old shm if it exists
    shm_unlink(path);

    // open the shared memory file
    int fd = shm_open(path, O_CREAT | O_RDWR | O_EXCL, 0666);
    if (fd < 0) {
        DEBUG_PRINT_CORE("shm_open failed: %s\n", strerror(errno));
        return -1;
    }

    // set the size of the file
    if (ftruncate(fd, (off_t)size) < 0) {
        DEBUG_PRINT_CORE("ftruncate failed: %s\n", strerror(errno));
        close(fd);
        return -2;
    }

    // map it into memory
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        DEBUG_PRINT_CORE("mmap failed: %s\n", strerror(errno));
        close(fd);
        return -3;
    }

    DEBUG_PRINT_CORE("Mapped core @ %p\n", base);

    // clear the memory
    memset(base, 0, size);

    // set up the main header
    CoreHeader *hdr = (CoreHeader*)base;
    hdr->magic   = USRL_MAGIC;
    hdr->version = 1;
    hdr->mmap_size = size;

    uint64_t off = usrl_align_up(sizeof(CoreHeader), USRL_ALIGNMENT);
    hdr->topic_table_offset = off;
    hdr->topic_count        = 1;

    DEBUG_PRINT_CORE("Header set up done\n");

    // set up the topic entry
    TopicEntry *t = (TopicEntry*)((uint8_t*)base + hdr->topic_table_offset);
    memset(t, 0, sizeof(TopicEntry));
    strncpy(t->name, "demo", USRL_MAX_TOPIC_NAME - 1);

    DEBUG_PRINT_CORE("Topic name set to demo\n");

    uint64_t ring_desc_off = usrl_align_up(
        hdr->topic_table_offset + sizeof(TopicEntry) * hdr->topic_count,
        USRL_ALIGNMENT);

    t->ring_desc_offset = ring_desc_off;

    uint32_t slot_count  = next_power_of_two_u32(SLOT_COUNT_VAL);
    uint32_t payload_max = DEFAULT_PAYLOAD_MAX;
    uint32_t slot_size   = (uint32_t)usrl_align_up(
                               sizeof(SlotHeader) + payload_max, 8);

    t->slot_count = slot_count;
    t->slot_size  = slot_size;

    // set up the ring descriptor
    RingDesc *r = (RingDesc*)((uint8_t*)base + ring_desc_off);
    r->slot_count = slot_count;
    r->slot_size  = slot_size;

    uint64_t base_off = usrl_align_up(
        ring_desc_off + sizeof(RingDesc), USRL_ALIGNMENT);

    r->base_offset = base_off;
    atomic_store_explicit(&r->w_head, 0, memory_order_relaxed);

    DEBUG_PRINT_CORE("Ring descriptor setup done\n");

    uint64_t slots_bytes = (uint64_t)slot_count * slot_size;

    // check if it fits
    if (r->base_offset + slots_bytes > size) {
        DEBUG_PRINT_CORE("ERROR: ring too big\n");
        munmap(base, size);
        close(fd);
        return -4;
    }

    // zero out the slots
    uint8_t *slots_base = (uint8_t*)base + r->base_offset;
    memset(slots_base, 0, slots_bytes);

    // init all the slot headers
    for (uint32_t i = 0; i < slot_count; ++i) {
        SlotHeader *sh = (SlotHeader*)(slots_base + (uint64_t)i * slot_size);
        atomic_store_explicit(&sh->seq, 0, memory_order_relaxed);
        sh->payload_len  = 0;
        sh->timestamp_ns = 0;
    }

    DEBUG_PRINT_CORE("Slots initialized\n");

    munmap(base, size);
    close(fd);

    DEBUG_PRINT_CORE("INIT done OK\n");
    return 0;
}

void* usrl_core_map(const char *path, uint64_t size)
{
    DEBUG_PRINT_CORE("MAP start path=%s size=%lu\n", path, size);

    // open shared mem
    int fd = shm_open(path, O_RDWR, 0666);
    if (fd < 0) {
        DEBUG_PRINT_CORE("MAP shm_open failed\n");
        return NULL;
    }

    // map it
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        DEBUG_PRINT_CORE("MAP mmap failed\n");
        close(fd);
        return NULL;
    }

    close(fd);
    DEBUG_PRINT_CORE("Mapped address %p\n", base);
    return base;
}

TopicEntry* usrl_get_topic(void *base, const char *name)
{
    if (!base || !name) return NULL;

    CoreHeader *hdr = (CoreHeader*)base;
    if (hdr->magic != USRL_MAGIC) {
        return NULL;
    }

    TopicEntry *t = (TopicEntry*)((uint8_t*)base + hdr->topic_table_offset);

    // loop to find the topic
    for (uint32_t i = 0; i < hdr->topic_count; i++) {
        if (strncmp(t[i].name, name, USRL_MAX_TOPIC_NAME) == 0)
            return &t[i];
    }

    return NULL;
}
