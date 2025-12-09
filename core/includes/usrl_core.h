#ifndef USRL_CORE_H
#define USRL_CORE_H

/* --------------------------------------------------------------------------
 * USRL Core — Shared Memory Layout & Types
 *
 * This header defines the on-disk/SHM layout for the USRL runtime:
 *   - CoreHeader  : top-level region descriptor
 *   - TopicEntry  : per-topic index entry (in the topic table)
 *   - RingDesc    : per-topic ring descriptor (slot layout + head)
 *   - SlotHeader  : metadata prepended to each slot's payload
 *
 * The layout is designed for zero-copy shared-memory messaging with
 * lock-free writers and readers using sequence numbers.
 * -------------------------------------------------------------------------- */

#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>

/* --------------------------------------------------------------------------
 * Constants & Configuration
 * -------------------------------------------------------------------------- */
#define USRL_MAGIC 0x5553524C  /* 'USRL' */
#define USRL_MAX_TOPIC_NAME 64 /* bytes */
#define USRL_ALIGNMENT 64      /* region alignment (cache line) */
#define USRL_RING_TYPE_SWMR 0  /* single-writer, multi-reader */
#define USRL_RING_TYPE_MWMR 1  /* multi-writer, multi-reader */

/* --------------------------------------------------------------------------
 * Compiler Hints for Optimization
 * -------------------------------------------------------------------------- */
#define USRL_LIKELY(x) __builtin_expect(!!(x), 1)
#define USRL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define USRL_PREFETCH_R(x) __builtin_prefetch((x), 0, 3) /* read, high locality */
#define USRL_PREFETCH_W(x) __builtin_prefetch((x), 1, 3) /* write, high locality */

/* --------------------------------------------------------------------------
 * Topic Table Entry
 *
 * Stored in the topic table region. Each entry references a RingDesc via
 * an offset within the shared memory region.
 * -------------------------------------------------------------------------- */
typedef struct
{
    char name[USRL_MAX_TOPIC_NAME]; /* NUL-terminated topic name */
    uint64_t ring_desc_offset;      /* offset from the base of the region */
    uint32_t slot_count;            /* normalized to a power-of-two */
    uint32_t slot_size;             /* size of each slot (including header) */
    uint32_t type;                  /* USRL_RING_TYPE_* */
} TopicEntry;

/* --------------------------------------------------------------------------
 * Utility: Alignment helper
 *
 * Aligns `v` up to a multiple of `a`. Both arguments must be powers-of-two
 * friendly; used to place structures on cache-line / page boundaries.
 * -------------------------------------------------------------------------- */
static inline uint64_t usrl_align_up(uint64_t v, uint64_t a)
{
    return (v + (a - 1)) & ~(a - 1);
}

/* --------------------------------------------------------------------------
 * Configuration struct used by usrl_core_init()
 *
 * This is the user-provided description for each topic when building the
 * shared memory region.
 * -------------------------------------------------------------------------- */
typedef struct
{
    char name[USRL_MAX_TOPIC_NAME];
    uint32_t slot_count; /* requested slots (will be rounded to power-of-two) */
    uint32_t slot_size;  /* user payload size (slot header added automatically) */
    uint32_t type;       /* USRL_RING_TYPE_SWMR or USRL_RING_TYPE_MWMR */
} UsrlTopicConfig;

/* --------------------------------------------------------------------------
 * Core Header (top of the SHM region)
 *
 * Describes the mapped region: magic/version/size and where the topic table
 * lives along with the topic_count.
 * -------------------------------------------------------------------------- */
typedef struct
{
    uint32_t magic;              /* must equal USRL_MAGIC */
    uint32_t version;            /* layout version (starts at 1) */
    uint64_t mmap_size;          /* total size of the mapped region */
    uint64_t topic_table_offset; /* offset to TopicEntry[topic_count] */
    uint32_t topic_count;        /* number of topics in the table */
    uint32_t _pad;               /* reserved / alignment */
} CoreHeader;

/* --------------------------------------------------------------------------
 * Slot Header (prefixed at the start of each slot)
 *
 * Designed to be atomically published by writers:
 *   - seq is written last (memory_order_release) to signal completion.
 *   - readers use seq to detect fully-committed slots.
 *
 * Fields:
 *   seq          : monotonic commit sequence (0 == unused)
 *   timestamp_ns : wall-clock timestamp for the write
 *   payload_len  : number of bytes in the payload
 *   pub_id       : publisher id (new field — who wrote this slot)
 * -------------------------------------------------------------------------- */
typedef struct __attribute__((aligned(64)))
{
    atomic_uint_fast64_t seq; /* commit sequence; 0 == empty/uninitialized */
    uint64_t timestamp_ns;
    uint32_t payload_len;
    uint16_t pub_id; /* publisher identity */
    uint16_t _pad;   /* pad to 8-byte boundary */
} SlotHeader;

#ifndef __cplusplus
_Static_assert(sizeof(SlotHeader) % 8 == 0, "header size alignment wrong");
#endif

/* --------------------------------------------------------------------------
 * Ring Descriptor
 *
 * Per-topic descriptor containing:
 *   - slot_count, slot_size
 *   - base_offset (where the first slot starts)
 *   - w_head (writer head / monotonic sequence counter)
 *
 * Note: tail/reader state is maintained by subscribers locally (not in the
 * RingDesc) to keep the core small and avoid concurrent writes from readers.
 *
 * FIX #12: Align to cache line to prevent false sharing on w_head.
 * -------------------------------------------------------------------------- */
typedef struct __attribute__((aligned(USRL_ALIGNMENT)))
{
    uint32_t slot_count;
    uint32_t slot_size;
    uint64_t base_offset;        /* offset to first slot (from region base) */
    atomic_uint_fast64_t w_head; /* writers atomically increment this */
    uint8_t _pad[32];            /* reserved for future extension */
} RingDesc;

/* --------------------------------------------------------------------------
 * Public API (core)
 *
 * usrl_core_init  : create and initialize a new SHM region from a list of
 *                   topic configs.
 *
 * usrl_core_map   : open and mmap() an existing region for use by a process.
 *
 * usrl_get_topic  : look up a topic by name in a mapped region.
 * -------------------------------------------------------------------------- */
int usrl_core_init(const char *path,
                   uint64_t size,
                   const UsrlTopicConfig *topics,
                   uint32_t count);

void *usrl_core_map(const char *path, uint64_t size);

TopicEntry *usrl_get_topic(void *base, const char *name);

void usrl_core_unmap(void *base, size_t size);

#endif /* USRL_CORE_H */
