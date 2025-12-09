#ifndef USRL_SCHEMA_H
#define USRL_SCHEMA_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define USRL_MAX_FIELDS 32

typedef enum {
    USRL_FIELD_U64,
    USRL_FIELD_I64,
    USRL_FIELD_F64,
    USRL_FIELD_U32,
    USRL_FIELD_I32,
    USRL_FIELD_F32,
    USRL_FIELD_BYTES,
    USRL_FIELD_STRING,
} UsrlFieldType;

typedef struct {
    const char *name;
    UsrlFieldType type;
    uint32_t offset;
    uint32_t size;
    uint32_t fingerprint;
} UsrlField;

typedef struct {
    uint32_t schema_id;
    uint32_t version;
    uint32_t fingerprint;
    const char *name;
    uint32_t field_count;
    UsrlField fields[USRL_MAX_FIELDS];
    uint32_t total_size;
} UsrlSchema;

typedef struct {
    UsrlSchema *schema;
    uint8_t *data;
    uint32_t len;
    uint32_t capacity;
} UsrlMessage;

UsrlSchema *usrl_schema_create(uint32_t schema_id, const char *name);
int usrl_schema_add_field(UsrlSchema *schema, const char *field_name,
                         UsrlFieldType type, uint32_t size);
int usrl_schema_finalize(UsrlSchema *schema);
UsrlMessage *usrl_message_create(UsrlSchema *schema, uint32_t capacity);
int usrl_message_set(UsrlMessage *msg, const char *field_name,
                    const void *value, uint32_t len);
int usrl_message_get(UsrlMessage *msg, const char *field_name,
                    void *out_value, uint32_t max_len);
int usrl_message_encode(UsrlMessage *msg, uint8_t *out_buf, uint32_t max_len);
int usrl_message_decode(UsrlMessage *msg, const uint8_t *data, uint32_t len);
void usrl_message_free(UsrlMessage *msg);
void usrl_schema_free(UsrlSchema *schema);

#endif /* USRL_SCHEMA_H */
