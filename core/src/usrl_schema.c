#include "usrl_schema.h"
#include <stdlib.h>
#include <stdio.h>

static uint32_t usrl_schema_hash(const char *str)
{
    uint32_t hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

UsrlSchema *usrl_schema_create(uint32_t schema_id, const char *name)
{
    UsrlSchema *s = malloc(sizeof(*s));
    if (!s) return NULL;
    
    memset(s, 0, sizeof(*s));
    s->schema_id = schema_id;
    s->version = 1;
    s->name = strdup(name);
    return s;
}

int usrl_schema_add_field(UsrlSchema *schema, const char *field_name,
                         UsrlFieldType type, uint32_t size)
{
    if (!schema || schema->field_count >= USRL_MAX_FIELDS)
        return -1;

    uint32_t idx = schema->field_count;
    schema->fields[idx].name = strdup(field_name);
    schema->fields[idx].type = type;
    schema->fields[idx].size = size;
    schema->fields[idx].offset = schema->total_size;
    schema->fields[idx].fingerprint = usrl_schema_hash(field_name);

    switch (type) {
        case USRL_FIELD_U64:
        case USRL_FIELD_I64:
        case USRL_FIELD_F64:
            schema->total_size += 8;
            break;
        case USRL_FIELD_U32:
        case USRL_FIELD_I32:
        case USRL_FIELD_F32:
            schema->total_size += 4;
            break;
        default:
            schema->total_size += size;
            break;
    }

    schema->field_count++;
    return 0;
}

int usrl_schema_finalize(UsrlSchema *schema)
{
    if (!schema || schema->field_count == 0)
        return -1;

    uint32_t hash = 5381;
    for (uint32_t i = 0; i < schema->field_count; i++) {
        hash ^= schema->fields[i].fingerprint;
        hash = ((hash << 5) + hash) + schema->fields[i].type;
    }
    schema->fingerprint = hash;
    return 0;
}

UsrlMessage *usrl_message_create(UsrlSchema *schema, uint32_t capacity)
{
    if (!schema) return NULL;

    UsrlMessage *msg = malloc(sizeof(*msg));
    if (!msg) return NULL;

    msg->schema = schema;
    msg->capacity = capacity > schema->total_size ? capacity : schema->total_size;
    msg->data = calloc(1, msg->capacity);
    msg->len = schema->total_size;

    if (!msg->data) {
        free(msg);
        return NULL;
    }

    return msg;
}

int usrl_message_set(UsrlMessage *msg, const char *field_name,
                    const void *value, uint32_t len)
{
    if (!msg || !field_name || !value)
        return -1;

    for (uint32_t i = 0; i < msg->schema->field_count; i++) {
        if (strcmp(msg->schema->fields[i].name, field_name) == 0) {
            UsrlField *f = &msg->schema->fields[i];
            uint32_t copy_len = len < f->size ? len : f->size;
            memcpy(msg->data + f->offset, value, copy_len);
            return 0;
        }
    }
    return -1;
}

int usrl_message_get(UsrlMessage *msg, const char *field_name,
                    void *out_value, uint32_t max_len)
{
    if (!msg || !field_name || !out_value)
        return -1;

    for (uint32_t i = 0; i < msg->schema->field_count; i++) {
        if (strcmp(msg->schema->fields[i].name, field_name) == 0) {
            UsrlField *f = &msg->schema->fields[i];
            uint32_t copy_len = max_len < f->size ? max_len : f->size;
            memcpy(out_value, msg->data + f->offset, copy_len);
            return (int)copy_len;
        }
    }
    return -1;
}

int usrl_message_encode(UsrlMessage *msg, uint8_t *out_buf, uint32_t max_len)
{
    if (!msg || !out_buf || max_len < msg->len)
        return -1;

    memcpy(out_buf, msg->data, msg->len);
    return (int)msg->len;
}

int usrl_message_decode(UsrlMessage *msg, const uint8_t *data, uint32_t len)
{
    if (!msg || !data || len < msg->schema->total_size)
        return -1;

    memcpy(msg->data, data, msg->schema->total_size);
    msg->len = msg->schema->total_size;
    return 0;
}

void usrl_message_free(UsrlMessage *msg)
{
    if (!msg) return;
    if (msg->data) free(msg->data);
    free(msg);
}

void usrl_schema_free(UsrlSchema *schema)
{
    if (!schema) return;
    if (schema->name) free((void *)schema->name);
    for (uint32_t i = 0; i < schema->field_count; i++) {
        if (schema->fields[i].name)
            free((void *)schema->fields[i].name);
    }
    free(schema);
}
