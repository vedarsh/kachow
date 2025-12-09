#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "usrl_core.h"
#include "usrl_health.h"
#include "usrl_schema.h"
#include "usrl_logging.h"
#include "usrl_ring.h"

typedef struct __attribute__((packed)) {
    uint64_t timestamp;
    uint32_t ticker_crc;
    double bid_price;
    double ask_price;
    uint64_t volume;
} PriceQuote;

int main(void)
{
    printf("=== Market Data Publisher Example ===\n\n");

    usrl_logging_init(NULL, USRL_LOG_INFO);

    UsrlTopicConfig topics[] = {
        {"prices", 512, 256, USRL_RING_TYPE_SWMR},
    };

    int ret = usrl_core_init("/usrl-market", 50*1024*1024, topics, 1);
    if (ret != 0) {
        USRL_ERROR("market_pub", "Failed to init USRL");
        return 1;
    }

    void *base = usrl_core_map("/usrl-market", 50*1024*1024);
    if (!base) {
        USRL_ERROR("market_pub", "Failed to map USRL region");
        return 1;
    }

    UsrlSchema *price_schema = usrl_schema_create(1, "price_quote");
    usrl_schema_add_field(price_schema, "timestamp", USRL_FIELD_U64, 8);
    usrl_schema_add_field(price_schema, "ticker_crc", USRL_FIELD_U32, 4);
    usrl_schema_add_field(price_schema, "bid_price", USRL_FIELD_F64, 8);
    usrl_schema_add_field(price_schema, "ask_price", USRL_FIELD_F64, 8);
    usrl_schema_add_field(price_schema, "volume", USRL_FIELD_U64, 8);
    usrl_schema_finalize(price_schema);

    UsrlPublisher pub;
    usrl_pub_init(&pub, base, "prices", 1);

    USRL_INFO("market_pub", "Publisher started (pub_id=1)");

    time_t start = time(NULL);
    uint64_t msg_count = 0;

    while (time(NULL) - start < 10) {
        PriceQuote quote = {
            .timestamp = (uint64_t)time(NULL) * 1000000000 + 12345,
            .ticker_crc = 0x12345678,
            .bid_price = 150.25,
            .ask_price = 150.30,
            .volume = 1000000 + (msg_count % 5000000),
        };

        UsrlMessage *msg = usrl_message_create(price_schema, 256);
        usrl_message_set(msg, "timestamp", &quote.timestamp, 8);
        usrl_message_set(msg, "ticker_crc", &quote.ticker_crc, 4);
        usrl_message_set(msg, "bid_price", &quote.bid_price, 8);
        usrl_message_set(msg, "ask_price", &quote.ask_price, 8);
        usrl_message_set(msg, "volume", &quote.volume, 8);

        uint8_t buf[256];
        int len = usrl_message_encode(msg, buf, sizeof(buf));
        
        if (usrl_pub_publish(&pub, buf, len) == 0) {
            msg_count++;
            if (msg_count % 10000 == 0) {
                USRL_INFO("market_pub", "Published %lu quotes", msg_count);
            }
        } else {
            USRL_WARN("market_pub", "Publish failed");
        }

        usrl_message_free(msg);
        usleep(1000);
    }

    RingHealth *health = usrl_health_get(base, "prices");
    if (health) {
        printf("\nPublisher Health:\n");
        printf("  Total Published: %lu\n", health->pub_health.total_published);
        printf("  Topic Type: %s\n", health->ring_type == USRL_RING_TYPE_SWMR ? "SWMR" : "MWMR");
        free(health);
    }

    usrl_schema_free(price_schema);
    usrl_logging_shutdown();

    printf("\n✅ Publisher finished: published %lu messages\n", msg_count);
    return 0;
}
