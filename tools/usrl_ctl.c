#define _GNU_SOURCE
#include "usrl_core.h"
#include "usrl_ring.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>
#include <stdatomic.h>

#define SHM_PATH "/usrl_core"

/* --------------------------------------------------------------------------
 * UTILS
 * -------------------------------------------------------------------------- */

/* Check if buffer looks like a printable string */
static int is_printable(const uint8_t *buf, uint32_t len) {
    if (len == 0) return 0;
    // Check for null terminator near end if it claims to be C-string
    for (uint32_t i = 0; i < len; i++) {
        if (buf[i] == 0 && i == len - 1) return 1; // Null at end is OK
        if (buf[i] == 0 && i < len - 1) return 0; // Null in middle = binary
        if (!isprint(buf[i]) && buf[i] != '\n' && buf[i] != '\r' && buf[i] != '\t') {
            return 0; // Binary garbage
        }
    }
    return 1;
}

static void print_hexdump(const uint8_t *buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        printf("%02X ", buf[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    if (len % 16 != 0) printf("\n");
}

/* Smart Map: Reads header first to find total size, then maps everything */
static void* map_system(void) {
    /* Note: In real USRL apps, usrl_core_map handles this. 
       We do it manually here for the tool to avoid linking full API if desired,
       or to inspect raw SHM. */
    int fd = shm_open(SHM_PATH, O_RDWR, 0666);
    if (fd < 0) {
        perror("shm_open");
        fprintf(stderr, "Hint: Have you run init_core or demo_app?\n");
        exit(1);
    }

    // 1. Map Header Only
    void *hdr_ptr = mmap(NULL, sizeof(CoreHeader), PROT_READ, MAP_SHARED, fd, 0);
    if (hdr_ptr == MAP_FAILED) {
        perror("mmap header");
        exit(1);
    }

    CoreHeader hdr_copy;
    memcpy(&hdr_copy, hdr_ptr, sizeof(CoreHeader));
    munmap(hdr_ptr, sizeof(CoreHeader));

    if (hdr_copy.magic != USRL_MAGIC) {
        fprintf(stderr, "Error: Invalid magic number in SHM.\n");
        exit(1);
    }

    // 2. Map Full Region
    void *base = mmap(NULL, hdr_copy.mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        perror("mmap full");
        exit(1);
    }

    close(fd);
    return base;
}

/* --------------------------------------------------------------------------
 * COMMANDS
 * -------------------------------------------------------------------------- */

static void do_list(void *base) {
    CoreHeader *hdr = (CoreHeader*)base;
    TopicEntry *topics = (TopicEntry*)((uint8_t*)base + hdr->topic_table_offset);

    printf("\nUSRL System Status\n");
    printf("------------------\n");
    printf("Size: %lu MB\n", hdr->mmap_size / (1024*1024));
    printf("Topics: %u\n\n", hdr->topic_count);

    printf("%-20s | %-5s | %-8s | %-8s | %-12s\n",
           "NAME", "TYPE", "SLOTS", "SIZE", "MESSAGES");
    printf("------------------------------------------------------------------\n");

    for (uint32_t i = 0; i < hdr->topic_count; i++) {
        TopicEntry *t = &topics[i];
        RingDesc *r = (RingDesc*)((uint8_t*)base + t->ring_desc_offset);

        // Correct atomic load
        uint64_t head = atomic_load_explicit(&r->w_head, memory_order_relaxed);

        printf("%-20s | %-5s | %-8u | %-8u | %-12lu\n",
               t->name,
               (t->type == USRL_RING_TYPE_SWMR) ? "SWMR" : "MWMR",
               t->slot_count,
               t->slot_size,
               head);
    }
    printf("\n");
}

static void do_info(void *base, const char *topic_name) {
    TopicEntry *t = usrl_get_topic(base, topic_name);
    if (!t) {
        fprintf(stderr, "Topic '%s' not found.\n", topic_name);
        return;
    }

    RingDesc *r = (RingDesc*)((uint8_t*)base + t->ring_desc_offset);
    uint64_t head = atomic_load_explicit(&r->w_head, memory_order_relaxed);

    printf("\nTopic: %s\n", t->name);
    printf("Type:  %s\n", (t->type == USRL_RING_TYPE_SWMR) ? "SWMR" : "MWMR");
    printf("Head:  %lu\n", head);
    printf("\nConfiguration:\n");
    printf("  Slot Count: %u\n", r->slot_count);
    printf("  Slot Size:  %u bytes\n", r->slot_size);
    printf("  Base Offset: 0x%lx\n", r->base_offset);
    printf("\nMemory:\n");
    printf("  Ring Size:  %.2f MB\n", (double)(r->slot_count * r->slot_size) / (1024.0 * 1024.0));
}

static void do_tail(void *base, const char *topic_name) {
    TopicEntry *t = usrl_get_topic(base, topic_name);
    if (!t) {
        fprintf(stderr, "Topic '%s' not found.\n", topic_name);
        return;
    }

    printf("Tailing topic '%s' (Ctrl+C to stop)...\n", topic_name);

    UsrlSubscriber sub;
    
    // FIX: Use unified init for both SWMR and MWMR
    usrl_sub_init(&sub, base, topic_name);

    // Set last_seq to head so we only see NEW messages
    RingDesc *d = sub.desc;
    sub.last_seq = atomic_load_explicit(&d->w_head, memory_order_acquire);

    uint8_t *buf = malloc(d->slot_size);
    if (!buf) {
        fprintf(stderr, "OOM\n");
        return;
    }
    
    uint16_t pid;

    while (1) {
        int len = usrl_sub_next(&sub, buf, d->slot_size, &pid);
        
        if (len >= 0) { // Success (could be 0 bytes)
            printf("[%u] ", pid);
            if (len == 0) {
                printf("(Empty Message)\n");
            } else if (is_printable(buf, len)) {
                // Ensure null termination for printf if not present
                if (buf[len-1] != 0) {
                    printf("%.*s\n", len, (char*)buf);
                } else {
                    printf("%s\n", (char*)buf);
                }
            } else {
                printf("(%d bytes) ", len);
                print_hexdump(buf, (len > 16) ? 16 : len);
            }
        } else if (len == USRL_RING_NO_DATA) {
            usleep(1000); // 1ms polling if no data
        } else {
            // Error (Truncated, etc)
            fprintf(stderr, "Error reading: %d\n", len);
            usleep(1000);
        }
    }
    
    free(buf);
}

/* --------------------------------------------------------------------------
 * MAIN
 * -------------------------------------------------------------------------- */

void usage() {
    printf("Usage: usrl-ctl <command> [args]\n");
    printf("Commands:\n");
    printf("  list            List all topics\n");
    printf("  info <topic>    Show topic details\n");
    printf("  tail <topic>    Follow topic data\n");
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 2) usage();

    void *base = map_system();

    if (strcmp(argv[1], "list") == 0) {
        do_list(base);
    }
    else if (strcmp(argv[1], "info") == 0) {
        if (argc < 3) usage();
        do_info(base, argv[2]);
    }
    else if (strcmp(argv[1], "tail") == 0) {
        if (argc < 3) usage();
        do_tail(base, argv[2]);
    }
    else {
        usage();
    }

    return 0;
}
