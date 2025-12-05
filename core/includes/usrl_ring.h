/* usrl_ring.h */
// header for the ring buffer stuff
// pub and sub structs are here

#ifndef USRL_RING_H
#define USRL_RING_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include "usrl_core.h"

// macro for printing debug stuff
// change the define to turn on or off
#define DEBUG_PRINT_RING(...) \
    do { printf("[DEBUG][RING] " __VA_ARGS__); fflush(stdout); } while (0)

// structure for the publisher
typedef struct {
    RingDesc *desc;
    uint8_t  *base_ptr;
    uint32_t  mask;
} UsrlPublisher;

// structure for the subscriber
typedef struct {
    RingDesc *desc;
    uint8_t  *base_ptr;
    uint32_t  mask;
    uint64_t  last_seq;
} UsrlSubscriber;

// function declarations
void usrl_pub_init(UsrlPublisher *p, void *core_base, const char *topic);
int  usrl_pub_publish(UsrlPublisher *p, const void *data, uint32_t len);

void usrl_sub_init(UsrlSubscriber *s, void *core_base, const char *topic);
int  usrl_sub_next(UsrlSubscriber *s, uint8_t *out_buf, uint32_t buf_len);

#endif
