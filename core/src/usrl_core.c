
#include "usrl_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
/**
 * @file usrl_core.c
 * @brief Core routines for creating and mapping the USRL shared memory region.
 *
 * This file contains the implementation of the core allocator/builder that
 * constructs a shared memory layout for topics, ring descriptors and slots,
 * plus utilities to map an existing USRL region and to lookup topics by name.
 *
 * The public API implemented here:
 *  - usrl_core_init() : create and initialize a new USRL SHM region
 *  - usrl_core_map()  : map an existing USRL SHM region
 *  - usrl_get_topic() : find a topic entry by name in a mapped region
 */

/* --------------------------------------------------------------------------
 * Debug Helpers
 * -------------------------------------------------------------------------- */
#ifdef DEBUG
#define DEBUG_PRINT_CORE(...) \
    do { printf("[DEBUG][CORE] " __VA_ARGS__); fflush(stdout); } while (0)
#else
#define DEBUG_PRINT_CORE(...) ((void)0)
#endif

/* --------------------------------------------------------------------------
 * Utility: Next power-of-two (u32)
 * Used to normalize slot counts so rings remain modulus-friendly.
 * -------------------------------------------------------------------------- */
/**
 * @brief Compute the next power-of-two for a 32-bit unsigned integer.
 *
 * This helper normalizes slot counts so that ring sizes are powers of two
 * (which simplifies index masking and modular arithmetic).
 *
 * @param v Input value.
 * @return The smallest power-of-two >= v. If v == 0 returns 1.
 */
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

/* =============================================================================
 * USRL CORE INITIALIZATION
 * =============================================================================
 *
 * This is the main allocator/builder for the entire USRL shared memory region.
 *
 * High-Level Steps:
 *
 *  1) Clean up any existing SHM file with the same path.
 *  2) Create and size a new SHM region.
 *  3) mmap() it so we can lay out our memory manually.
 *  4) Lay out:
 *       - CoreHeader
 *       - Topic Table
 *       - RingDesc array
 *       - All Slots for all rings
 *
 *  5) Return success after constructing a fully zeroed & aligned USRL region.
 *
 * Returns:
 *   0  = OK
 *  -1  = invalid parameters or shm_open failed
 *  -2  = ftruncate failed
 *  -3  = mmap failed
 *  -4  = out of memory during slot allocation
 *
 * =============================================================================
 */
/**
 * @brief Initialize and build a new USRL shared memory region.
 *
 * High-level behavior:
 *  1) Remove any existing SHM object at 'path'.
 *  2) Create a new shared memory object and set its size to 'size'.
 *  3) mmap() the region and lay out:
 *       - CoreHeader
 *       - Topic table
 *       - RingDesc array
 *       - Per-topic slot memory
 *  4) Initialize slot headers and ring descriptors.
 *
 * The caller must ensure 'topics' points to an array of 'count' UsrlTopicConfig
 * entries describing each topic to reserve.
 *
 * @param path Filesystem path (name) for the POSIX shared memory object.
 * @param size Total size, in bytes, to allocate for the USRL region.
 * @param topics Array of topic configuration descriptors.
 * @param count Number of topics in the 'topics' array.
 *
 * @return 0 on success.
 *         -1 on invalid parameters or shm_open failure.
 *         -2 on ftruncate failure.
 *         -3 on mmap failure.
 *         -4 if insufficient shared memory remains to allocate topic slots.
 */
int usrl_core_init(
    const char           *path,
    uint64_t              size,
    const UsrlTopicConfig *topics,
    uint32_t              count
) {
    DEBUG_PRINT_CORE("init path=%s size=%lu topics=%u\n", path, size, count);

    /* ----------------------------------------------------------------------
     * Validate inputs
     * ---------------------------------------------------------------------- */
    if (!path || size < 4096 || !topics || count == 0) {
        return -1;
    }

    /* ----------------------------------------------------------------------
     * Remove any existing SHM file
     * ---------------------------------------------------------------------- */
    shm_unlink(path);

    /* ----------------------------------------------------------------------
     * Create fresh shared memory file
     * ---------------------------------------------------------------------- */
    int fd = shm_open(path, O_CREAT | O_RDWR | O_EXCL, 0666);
    if (fd < 0) {
        DEBUG_PRINT_CORE("shm_open failed\n");
        return -1;
    }

    /* ----------------------------------------------------------------------
     * Resize SHM to requested size
     * ---------------------------------------------------------------------- */
    if (ftruncate(fd, (off_t)size) < 0) {
        DEBUG_PRINT_CORE("ftruncate failed\n");
        close(fd);
        return -2;
    }

    /* ----------------------------------------------------------------------
     * Map the SHM region
     * ---------------------------------------------------------------------- */
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        DEBUG_PRINT_CORE("mmap failed\n");
        close(fd);
        return -3;
    }

    /* Clear entire memory region */
    memset(base, 0, size);

    /* ----------------------------------------------------------------------
     * Core Header Setup
     * ---------------------------------------------------------------------- */
    CoreHeader *hdr     = (CoreHeader*)base;
    hdr->magic          = USRL_MAGIC;
    hdr->version        = 1;
    hdr->mmap_size      = size;

    /* Topic table begins right after header */
    uint64_t current_offset =
        usrl_align_up(sizeof(CoreHeader), USRL_ALIGNMENT);

    hdr->topic_table_offset = current_offset;
    hdr->topic_count        = count;

    /* ----------------------------------------------------------------------
     * Calculate offsets for:
     *   - topic table
     *   - ring descriptors
     *   - slot memory region
     * ---------------------------------------------------------------------- */
    uint64_t ring_desc_start = usrl_align_up(
        current_offset + (sizeof(TopicEntry) * count),
        USRL_ALIGNMENT
    );

    uint64_t slots_start = usrl_align_up(
        ring_desc_start + (sizeof(RingDesc) * count),
        USRL_ALIGNMENT
    );

    uint64_t next_free_slot_offset = slots_start;

    /* =========================================================================
     * Build All Topics + Ring Descriptors + Slot Regions
     * ========================================================================= */
    for (uint32_t i = 0; i < count; ++i) {
        DEBUG_PRINT_CORE("configuring topic %s\n", topics[i].name);

        /* --------------------------------------------------------------
         * 1. Fill Topic Entry
         * -------------------------------------------------------------- */
        TopicEntry *t =
            (TopicEntry*)((uint8_t*)base +
                          hdr->topic_table_offset +
                          (i * sizeof(TopicEntry)));

        /* FIX #8: Ensure null-termination */
        strncpy(t->name, topics[i].name, USRL_MAX_TOPIC_NAME - 1);
        t->name[USRL_MAX_TOPIC_NAME - 1] = '\0';

        t->ring_desc_offset = ring_desc_start + (i * sizeof(RingDesc));

        /* --------------------------------------------------------------
         * Compute aligned slot count and size
         * -------------------------------------------------------------- */
        uint32_t slots_pow2 = next_power_of_two_u32(topics[i].slot_count);
        uint32_t slot_sz_aligned =
            (uint32_t) usrl_align_up(
                sizeof(SlotHeader) + topics[i].slot_size,
                8
            );

        t->type       = topics[i].type;
        t->slot_count = slots_pow2;
        t->slot_size  = slot_sz_aligned;

        /* --------------------------------------------------------------
         * 2. Fill Ring Descriptor
         * -------------------------------------------------------------- */
        RingDesc *r =
            (RingDesc*)((uint8_t*)base + t->ring_desc_offset);

        r->slot_count  = slots_pow2;
        r->slot_size   = slot_sz_aligned;
        r->base_offset = next_free_slot_offset;
        atomic_store_explicit(&r->w_head, 0, memory_order_relaxed);

        /* --------------------------------------------------------------
         * 3. Reserve Slot Memory
         * -------------------------------------------------------------- */
        uint64_t total_bytes_for_topic =
            (uint64_t)slots_pow2 * slot_sz_aligned;

        /* Check if we're out of SHM space */
        if (next_free_slot_offset + total_bytes_for_topic > size) {
            DEBUG_PRINT_CORE("OOM! topic %s needs too much memory\n",
                             topics[i].name);
            munmap(base, size);
            close(fd);
            return -4;
        }

        /* Initialize each slot's header sequence */
        uint8_t *slot_ptr = (uint8_t*)base + next_free_slot_offset;
        for (uint32_t k = 0; k < slots_pow2; ++k) {
            SlotHeader *sh =
                (SlotHeader*)(slot_ptr + ((uint64_t)k * slot_sz_aligned));
            atomic_store_explicit(&sh->seq, 0, memory_order_relaxed);
        }

        /* Move allocation pointer forward */
        next_free_slot_offset += total_bytes_for_topic;
        next_free_slot_offset = usrl_align_up(
            next_free_slot_offset,
            USRL_ALIGNMENT
        );
    }

    /* ----------------------------------------------------------------------
     * Print usage stats and unmap.
     * ---------------------------------------------------------------------- */
    DEBUG_PRINT_CORE("used %lu / %lu bytes\n",
                     next_free_slot_offset, size);

    munmap(base, size);
    close(fd);
    return 0;
}

/* =============================================================================
 * USRL CORE MAP
 * =============================================================================
 * Maps an already-created USRL SHM region into the caller's address space.
 * =============================================================================
 */
/**
 * @brief Map an existing USRL shared memory region into the caller's address space.
 *
 * This returns a pointer to the mapped base address which must later be munmap()'ed
 * by the caller when no longer needed.
 *
 * @param path The name/path of the POSIX shared memory object.
 * @param size The size that was originally used to create the SHM region.
 * @return Pointer to mapped base on success, or NULL on failure.
 */
void* usrl_core_map(const char *path, uint64_t size) {
    int fd = shm_open(path, O_RDWR, 0666);
    if (fd < 0) return NULL;

    void *base =
        mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (base == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    close(fd);
    return base;
}

/* =============================================================================
 * USRL GET TOPIC
 * =============================================================================
 * Performs a linear search over the topic table to find a topic by name.
 * Caller must supply the beginning of the mapped USRL region.
 * =============================================================================
 */
/**
 * @brief Look up a topic entry by name in a mapped USRL region.
 *
 * Performs a linear search over the topic table. The caller must supply a valid
 * mapped USRL base pointer. The returned TopicEntry pointer points into the
 * mapped region and must not be dereferenced after the mapping is unmapped.
 *
 * @param base Base address of a mapped USRL region.
 * @param name Null-terminated topic name to search for.
 * @return Pointer to the matching TopicEntry, or NULL if not found or invalid base.
 */
TopicEntry* usrl_get_topic(void *base, const char *name) {
    if (!base || !name)
        return NULL;

    CoreHeader *hdr = (CoreHeader*)base;
    if (hdr->magic != USRL_MAGIC)
        return NULL;

    TopicEntry *t =
        (TopicEntry*)((uint8_t*)base + hdr->topic_table_offset);

    for (uint32_t i = 0; i < hdr->topic_count; i++) {
        if (strncmp(t[i].name, name, USRL_MAX_TOPIC_NAME) == 0)
            return &t[i];
    }

    return NULL;
}
