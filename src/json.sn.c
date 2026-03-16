/*
 * json/json.sn.c — JSON Encoder/Decoder
 *
 * Encoder: builds JSON strings directly with a growable buffer (no library).
 * Decoder: recursive-descent parser into a lightweight node tree, then
 *          vtable methods do key/index lookups.
 *
 * Zero external dependencies. All memory is managed explicitly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

/* =========================================================================
 * Growable Buffer
 * ========================================================================= */

typedef struct {
    char  *data;
    size_t length;
    size_t capacity;
} JsonBuffer;

/* Allocate a new buffer with the given initial capacity (minimum 64 bytes). */
static JsonBuffer *json_buffer_new(size_t initial_capacity) {
    JsonBuffer *buf = (JsonBuffer *)calloc(1, sizeof(JsonBuffer));
    buf->capacity = initial_capacity > 64 ? initial_capacity : 64;
    buf->data = (char *)malloc(buf->capacity);
    buf->data[0] = '\0';
    return buf;
}

/* Ensure the buffer has room for at least `needed` more bytes. */
static void json_buffer_ensure(JsonBuffer *buf, size_t needed) {
    if (buf->length + needed >= buf->capacity) {
        buf->capacity = (buf->capacity + needed) * 2;
        buf->data = (char *)realloc(buf->data, buf->capacity);
    }
}

/* Append a single character. */
static void json_buffer_append_char(JsonBuffer *buf, char c) {
    json_buffer_ensure(buf, 1);
    buf->data[buf->length++] = c;
    buf->data[buf->length] = '\0';
}

/* Append `n` bytes from a raw string. */
static void json_buffer_append_raw(JsonBuffer *buf, const char *str, size_t n) {
    json_buffer_ensure(buf, n);
    memcpy(buf->data + buf->length, str, n);
    buf->length += n;
    buf->data[buf->length] = '\0';
}

/* Append a null-terminated string. */
static void json_buffer_append_str(JsonBuffer *buf, const char *str) {
    json_buffer_append_raw(buf, str, strlen(str));
}

/* Append a JSON-escaped string (without surrounding quotes).
 * Scans for runs of safe characters and copies them in bulk. */
static void json_buffer_append_escaped(JsonBuffer *buf, const char *str) {
    if (!str) return;
    const char *p = str;
    while (*p) {
        /* Scan for a run of characters that don't need escaping. */
        const char *run_start = p;
        while (*p && (unsigned char)*p >= 0x20 && *p != '"' && *p != '\\') p++;
        if (p > run_start) {
            json_buffer_append_raw(buf, run_start, p - run_start);
        }
        if (!*p) break;
        /* Handle the special character. */
        switch (*p) {
            case '"':  json_buffer_append_raw(buf, "\\\"", 2); break;
            case '\\': json_buffer_append_raw(buf, "\\\\", 2); break;
            case '\n': json_buffer_append_raw(buf, "\\n", 2);  break;
            case '\r': json_buffer_append_raw(buf, "\\r", 2);  break;
            case '\t': json_buffer_append_raw(buf, "\\t", 2);  break;
            case '\b': json_buffer_append_raw(buf, "\\b", 2);  break;
            case '\f': json_buffer_append_raw(buf, "\\f", 2);  break;
            default: {
                /* Control character < 0x20: emit \uXXXX directly */
                char escaped[7];
                unsigned char c = (unsigned char)*p;
                escaped[0] = '\\'; escaped[1] = 'u'; escaped[2] = '0'; escaped[3] = '0';
                escaped[4] = "0123456789abcdef"[c >> 4];
                escaped[5] = "0123456789abcdef"[c & 0x0f];
                escaped[6] = '\0';
                json_buffer_append_raw(buf, escaped, 6);
                break;
            }
        }
        p++;
    }
}

/* Free a buffer and its data. */
static void json_buffer_free(JsonBuffer *buf) {
    if (buf) { free(buf->data); free(buf); }
}

/* =========================================================================
 * JSON Encoder
 * ========================================================================= */

/* Fast integer-to-buffer: write a long long directly without snprintf. */
static void json_buffer_append_int(JsonBuffer *buf, long long val) {
    char tmp[21]; /* enough for -9223372036854775808 */
    char *end = tmp + sizeof(tmp);
    char *p = end;
    int negative = 0;

    if (val < 0) {
        negative = 1;
        /* Handle LLONG_MIN safely: negate in unsigned */
        unsigned long long uval = (unsigned long long)(-(val + 1)) + 1;
        do { *--p = '0' + (char)(uval % 10); uval /= 10; } while (uval);
    } else {
        unsigned long long uval = (unsigned long long)val;
        do { *--p = '0' + (char)(uval % 10); uval /= 10; } while (uval);
    }
    if (negative) *--p = '-';
    json_buffer_append_raw(buf, p, end - p);
}

/* Write a JSON object key prefix: "key": */
static void json_buffer_append_key(JsonBuffer *buf, const char *key) {
    size_t key_len = strlen(key);
    json_buffer_ensure(buf, key_len + 3); /* "key": */
    buf->data[buf->length++] = '"';
    memcpy(buf->data + buf->length, key, key_len);
    buf->length += key_len;
    buf->data[buf->length++] = '"';
    buf->data[buf->length++] = ':';
    buf->data[buf->length] = '\0';
}

typedef struct {
    JsonBuffer *buffer;     /* shared across all sub-encoders */
    int         needs_comma; /* 0 = next value is first (no comma), 1 = prepend comma */
    int         is_array;
} JsonEncoderCtx;

/* Write a comma separator if this is not the first value at this level. */
static void encoder_write_comma(JsonEncoderCtx *ctx) {
    if (ctx->needs_comma) json_buffer_append_char(ctx->buffer, ',');
    ctx->needs_comma = 1;
}

static __sn__Encoder *encoder_create_sub(JsonBuffer *buffer, int is_array);

/* --- Keyed writers (for object fields) --- */

static void encoder_write_str(__sn__Encoder *self, const char *key, const char *val) {
    JsonEncoderCtx *ctx = (JsonEncoderCtx *)self->__sn__ctx;
    encoder_write_comma(ctx);
    json_buffer_append_key(ctx->buffer, key);
    json_buffer_append_char(ctx->buffer, '"');
    json_buffer_append_escaped(ctx->buffer, val);
    json_buffer_append_char(ctx->buffer, '"');
}

static void encoder_write_int(__sn__Encoder *self, const char *key, long long val) {
    JsonEncoderCtx *ctx = (JsonEncoderCtx *)self->__sn__ctx;
    encoder_write_comma(ctx);
    json_buffer_append_key(ctx->buffer, key);
    json_buffer_append_int(ctx->buffer, val);
}

static void encoder_write_double(__sn__Encoder *self, const char *key, double val) {
    JsonEncoderCtx *ctx = (JsonEncoderCtx *)self->__sn__ctx;
    encoder_write_comma(ctx);
    json_buffer_append_key(ctx->buffer, key);
    /* Fast path: whole numbers use integer formatting. */
    if (val == (long long)val && fabs(val) < 1e15) {
        json_buffer_append_int(ctx->buffer, (long long)val);
    } else {
        char tmp[32];
        int n = snprintf(tmp, sizeof(tmp), "%.17g", val);
        json_buffer_append_raw(ctx->buffer, tmp, n);
    }
}

static void encoder_write_bool(__sn__Encoder *self, const char *key, long long val) {
    JsonEncoderCtx *ctx = (JsonEncoderCtx *)self->__sn__ctx;
    encoder_write_comma(ctx);
    json_buffer_append_key(ctx->buffer, key);
    if (val) json_buffer_append_raw(ctx->buffer, "true", 4);
    else     json_buffer_append_raw(ctx->buffer, "false", 5);
}

static void encoder_write_null(__sn__Encoder *self, const char *key) {
    JsonEncoderCtx *ctx = (JsonEncoderCtx *)self->__sn__ctx;
    encoder_write_comma(ctx);
    json_buffer_append_key(ctx->buffer, key);
    json_buffer_append_raw(ctx->buffer, "null", 4);
}

/* --- Nested structure writers --- */

static __sn__Encoder *encoder_begin_object(__sn__Encoder *self, const char *key) {
    JsonEncoderCtx *ctx = (JsonEncoderCtx *)self->__sn__ctx;
    encoder_write_comma(ctx);
    json_buffer_append_key(ctx->buffer, key);
    json_buffer_append_char(ctx->buffer, '{');
    return encoder_create_sub(ctx->buffer, 0);
}

static __sn__Encoder *encoder_begin_array(__sn__Encoder *self, const char *key) {
    JsonEncoderCtx *ctx = (JsonEncoderCtx *)self->__sn__ctx;
    encoder_write_comma(ctx);
    json_buffer_append_key(ctx->buffer, key);
    json_buffer_append_char(ctx->buffer, '[');
    return encoder_create_sub(ctx->buffer, 1);
}

static void encoder_end(__sn__Encoder *self) {
    JsonEncoderCtx *ctx = (JsonEncoderCtx *)self->__sn__ctx;
    json_buffer_append_char(ctx->buffer, ctx->is_array ? ']' : '}');
    free(ctx);
    free(self);
}

/* --- Array element appenders (for array encoders) --- */

static void encoder_append_str(__sn__Encoder *self, const char *val) {
    JsonEncoderCtx *ctx = (JsonEncoderCtx *)self->__sn__ctx;
    encoder_write_comma(ctx);
    json_buffer_append_char(ctx->buffer, '"');
    json_buffer_append_escaped(ctx->buffer, val);
    json_buffer_append_char(ctx->buffer, '"');
}

static void encoder_append_int(__sn__Encoder *self, long long val) {
    JsonEncoderCtx *ctx = (JsonEncoderCtx *)self->__sn__ctx;
    encoder_write_comma(ctx);
    json_buffer_append_int(ctx->buffer, val);
}

static void encoder_append_double(__sn__Encoder *self, double val) {
    JsonEncoderCtx *ctx = (JsonEncoderCtx *)self->__sn__ctx;
    encoder_write_comma(ctx);
    if (val == (long long)val && fabs(val) < 1e15) {
        json_buffer_append_int(ctx->buffer, (long long)val);
    } else {
        char tmp[32];
        int n = snprintf(tmp, sizeof(tmp), "%.17g", val);
        json_buffer_append_raw(ctx->buffer, tmp, n);
    }
}

static void encoder_append_bool(__sn__Encoder *self, long long val) {
    JsonEncoderCtx *ctx = (JsonEncoderCtx *)self->__sn__ctx;
    encoder_write_comma(ctx);
    json_buffer_append_str(ctx->buffer, val ? "true" : "false");
}

static __sn__Encoder *encoder_append_object(__sn__Encoder *self) {
    JsonEncoderCtx *ctx = (JsonEncoderCtx *)self->__sn__ctx;
    encoder_write_comma(ctx);
    json_buffer_append_char(ctx->buffer, '{');
    return encoder_create_sub(ctx->buffer, 0);
}

/* Finalize the top-level encoder: close the root bracket and return the JSON string. */
static char *encoder_result(__sn__Encoder *self) {
    JsonEncoderCtx *ctx = (JsonEncoderCtx *)self->__sn__ctx;
    json_buffer_append_char(ctx->buffer, ctx->is_array ? ']' : '}');
    char *result = strdup(ctx->buffer->data);
    json_buffer_free(ctx->buffer);
    free(ctx);
    self->__sn__ctx = NULL;
    return result;
}

/* Encoder vtable — wired into every encoder instance. */
static __sn__EncoderVTable encoder_vtable = {
    .writeStr     = encoder_write_str,
    .writeInt     = encoder_write_int,
    .writeDouble  = encoder_write_double,
    .writeBool    = encoder_write_bool,
    .writeNull    = encoder_write_null,
    .beginObject  = encoder_begin_object,
    .beginArray   = encoder_begin_array,
    .end          = encoder_end,
    .appendStr    = encoder_append_str,
    .appendInt    = encoder_append_int,
    .appendDouble = encoder_append_double,
    .appendBool   = encoder_append_bool,
    .appendObject = encoder_append_object,
    .result       = encoder_result,
};

/* Create a sub-encoder that shares the parent's buffer. */
static __sn__Encoder *encoder_create_sub(JsonBuffer *buffer, int is_array) {
    __sn__Encoder *enc = (__sn__Encoder *)calloc(1, sizeof(__sn__Encoder));
    JsonEncoderCtx *ctx = (JsonEncoderCtx *)calloc(1, sizeof(JsonEncoderCtx));
    ctx->buffer = buffer;
    ctx->needs_comma = 0;
    ctx->is_array = is_array;
    enc->__sn__vt = &encoder_vtable;
    enc->__sn__ctx = ctx;
    return enc;
}

/* Cleanup handler for the root encoder (frees buffer + context). */
static void encoder_cleanup(__sn__Encoder *self) {
    JsonEncoderCtx *ctx = (JsonEncoderCtx *)self->__sn__ctx;
    if (ctx) {
        json_buffer_free(ctx->buffer);
        free(ctx);
        self->__sn__ctx = NULL;
    }
}

/* Public: create a root object encoder (starts with '{'). */
__sn__Encoder *sn_json_encoder(void) {
    JsonBuffer *buffer = json_buffer_new(256);
    json_buffer_append_char(buffer, '{');
    __sn__Encoder *enc = (__sn__Encoder *)calloc(1, sizeof(__sn__Encoder));
    JsonEncoderCtx *ctx = (JsonEncoderCtx *)calloc(1, sizeof(JsonEncoderCtx));
    ctx->buffer = buffer;
    ctx->needs_comma = 0;
    ctx->is_array = 0;
    enc->__sn__vt = &encoder_vtable;
    enc->__sn__ctx = ctx;
    enc->__sn__cleanup = encoder_cleanup;
    return enc;
}

/* Public: create a root array encoder (starts with '['). */
__sn__Encoder *sn_json_array_encoder(void) {
    JsonBuffer *buffer = json_buffer_new(256);
    json_buffer_append_char(buffer, '[');
    __sn__Encoder *enc = (__sn__Encoder *)calloc(1, sizeof(__sn__Encoder));
    JsonEncoderCtx *ctx = (JsonEncoderCtx *)calloc(1, sizeof(JsonEncoderCtx));
    ctx->buffer = buffer;
    ctx->needs_comma = 0;
    ctx->is_array = 1;
    enc->__sn__vt = &encoder_vtable;
    enc->__sn__ctx = ctx;
    enc->__sn__cleanup = encoder_cleanup;
    return enc;
}

/* =========================================================================
 * JSON Decoder — Recursive-Descent Parser
 * ========================================================================= */

/*
 * Parses JSON into a lightweight node tree. Nodes are allocated individually.
 * The tree is freed when the root decoder is cleaned up.
 */

typedef enum {
    JSON_NODE_OBJECT,
    JSON_NODE_ARRAY,
    JSON_NODE_STRING,
    JSON_NODE_INT,
    JSON_NODE_DOUBLE,
    JSON_NODE_BOOL,
    JSON_NODE_NULL
} JsonNodeType;

typedef struct JsonNode JsonNode;

typedef struct {
    char     *key;
    JsonNode *value;
} JsonKeyValue;

struct JsonNode {
    JsonNodeType type;
    union {
        struct { JsonKeyValue *entries; int count; int capacity; } object;
        struct { JsonNode    **items;   int count; int capacity; } array;
        char      *string_value;
        long long  int_value;
        double     double_value;
        int        bool_value;
    };
};

/* Skip whitespace characters (space, tab, newline, carriage return). */
static const char *json_skip_whitespace(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Parse a JSON string literal, handling escape sequences. Returns a heap copy. */
static char *json_parse_string(const char **cursor) {
    const char *p = *cursor;
    if (*p != '"') return NULL;
    p++;

    JsonBuffer *buf = json_buffer_new(64);
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"':  json_buffer_append_char(buf, '"');  break;
                case '\\': json_buffer_append_char(buf, '\\'); break;
                case '/':  json_buffer_append_char(buf, '/');  break;
                case 'n':  json_buffer_append_char(buf, '\n'); break;
                case 'r':  json_buffer_append_char(buf, '\r'); break;
                case 't':  json_buffer_append_char(buf, '\t'); break;
                case 'b':  json_buffer_append_char(buf, '\b'); break;
                case 'f':  json_buffer_append_char(buf, '\f'); break;
                case 'u': {
                    unsigned codepoint = 0;
                    for (int i = 0; i < 4 && p[1]; i++) {
                        p++;
                        char c = *p;
                        codepoint <<= 4;
                        if (c >= '0' && c <= '9') codepoint |= c - '0';
                        else if (c >= 'a' && c <= 'f') codepoint |= 10 + c - 'a';
                        else if (c >= 'A' && c <= 'F') codepoint |= 10 + c - 'A';
                    }
                    if (codepoint < 128) json_buffer_append_char(buf, (char)codepoint);
                    else json_buffer_append_char(buf, '?');
                    break;
                }
                default: json_buffer_append_char(buf, *p); break;
            }
        } else {
            json_buffer_append_char(buf, *p);
        }
        p++;
    }
    if (*p == '"') p++;

    *cursor = p;
    char *result = strdup(buf->data);
    json_buffer_free(buf);
    return result;
}

/* Forward declaration for recursive parsing. */
static JsonNode *json_parse_value(const char **cursor);

/* Parse any JSON value: object, array, string, number, bool, or null. */
static JsonNode *json_parse_value(const char **cursor) {
    const char *p = json_skip_whitespace(*cursor);
    JsonNode *node = (JsonNode *)calloc(1, sizeof(JsonNode));

    if (*p == '{') {
        /* Object */
        node->type = JSON_NODE_OBJECT;
        p++;
        node->object.capacity = 8;
        node->object.entries = (JsonKeyValue *)malloc(sizeof(JsonKeyValue) * node->object.capacity);
        p = json_skip_whitespace(p);
        while (*p && *p != '}') {
            if (node->object.count >= node->object.capacity) {
                node->object.capacity *= 2;
                node->object.entries = (JsonKeyValue *)realloc(
                    node->object.entries, sizeof(JsonKeyValue) * node->object.capacity);
            }
            p = json_skip_whitespace(p);
            char *key = json_parse_string(&p);
            p = json_skip_whitespace(p);
            if (*p == ':') p++;
            JsonNode *value = json_parse_value(&p);
            node->object.entries[node->object.count++] = (JsonKeyValue){ key, value };
            p = json_skip_whitespace(p);
            if (*p == ',') p++;
        }
        if (*p == '}') p++;

    } else if (*p == '[') {
        /* Array */
        node->type = JSON_NODE_ARRAY;
        p++;
        node->array.capacity = 8;
        node->array.items = (JsonNode **)malloc(sizeof(JsonNode *) * node->array.capacity);
        p = json_skip_whitespace(p);
        while (*p && *p != ']') {
            if (node->array.count >= node->array.capacity) {
                node->array.capacity *= 2;
                node->array.items = (JsonNode **)realloc(
                    node->array.items, sizeof(JsonNode *) * node->array.capacity);
            }
            node->array.items[node->array.count++] = json_parse_value(&p);
            p = json_skip_whitespace(p);
            if (*p == ',') p++;
        }
        if (*p == ']') p++;

    } else if (*p == '"') {
        /* String */
        node->type = JSON_NODE_STRING;
        node->string_value = json_parse_string(&p);

    } else if (*p == 't') {
        /* true */
        node->type = JSON_NODE_BOOL;
        node->bool_value = 1;
        p += 4;

    } else if (*p == 'f') {
        /* false */
        node->type = JSON_NODE_BOOL;
        node->bool_value = 0;
        p += 5;

    } else if (*p == 'n') {
        /* null */
        node->type = JSON_NODE_NULL;
        p += 4;

    } else {
        /* Number — detect int vs double by scanning for '.', 'e', or 'E' */
        char *end;
        double d = strtod(p, &end);
        int is_integer = 1;
        for (const char *c = p; c < end; c++) {
            if (*c == '.' || *c == 'e' || *c == 'E') { is_integer = 0; break; }
        }
        if (is_integer) {
            node->type = JSON_NODE_INT;
            node->int_value = (long long)d;
        } else {
            node->type = JSON_NODE_DOUBLE;
            node->double_value = d;
        }
        p = end;
    }

    *cursor = p;
    return node;
}

/* Recursively free a node tree. */
static void json_node_free(JsonNode *node) {
    if (!node) return;
    switch (node->type) {
        case JSON_NODE_OBJECT:
            for (int i = 0; i < node->object.count; i++) {
                free(node->object.entries[i].key);
                json_node_free(node->object.entries[i].value);
            }
            free(node->object.entries);
            break;
        case JSON_NODE_ARRAY:
            for (int i = 0; i < node->array.count; i++) {
                json_node_free(node->array.items[i]);
            }
            free(node->array.items);
            break;
        case JSON_NODE_STRING:
            free(node->string_value);
            break;
        default:
            break;
    }
    free(node);
}

/* Look up a key in an object node. Returns NULL if not found. */
static JsonNode *json_object_get(JsonNode *node, const char *key) {
    if (!node || node->type != JSON_NODE_OBJECT) return NULL;
    for (int i = 0; i < node->object.count; i++) {
        if (strcmp(node->object.entries[i].key, key) == 0)
            return node->object.entries[i].value;
    }
    return NULL;
}

/* =========================================================================
 * JSON Decoder — Sindarin Vtable Implementation
 * ========================================================================= */

typedef struct {
    JsonNode *node;   /* borrowed reference into the parse tree */
    JsonNode *root;   /* root node — only non-NULL for the root decoder (owns the tree) */
} JsonDecoderCtx;

static __sn__Decoder *decoder_create(JsonNode *node, JsonNode *root);

/* --- Keyed readers (for object fields) --- */

static char *decoder_read_str(__sn__Decoder *self, const char *key) {
    JsonDecoderCtx *ctx = (JsonDecoderCtx *)self->__sn__ctx;
    JsonNode *val = json_object_get(ctx->node, key);
    return (val && val->type == JSON_NODE_STRING) ? strdup(val->string_value) : strdup("");
}

static long long decoder_read_int(__sn__Decoder *self, const char *key) {
    JsonDecoderCtx *ctx = (JsonDecoderCtx *)self->__sn__ctx;
    JsonNode *val = json_object_get(ctx->node, key);
    return (val && val->type == JSON_NODE_INT) ? val->int_value : 0;
}

static double decoder_read_double(__sn__Decoder *self, const char *key) {
    JsonDecoderCtx *ctx = (JsonDecoderCtx *)self->__sn__ctx;
    JsonNode *val = json_object_get(ctx->node, key);
    if (val && val->type == JSON_NODE_DOUBLE) return val->double_value;
    if (val && val->type == JSON_NODE_INT)    return (double)val->int_value;
    return 0.0;
}

static long long decoder_read_bool(__sn__Decoder *self, const char *key) {
    JsonDecoderCtx *ctx = (JsonDecoderCtx *)self->__sn__ctx;
    JsonNode *val = json_object_get(ctx->node, key);
    return (val && val->type == JSON_NODE_BOOL) ? val->bool_value : 0;
}

static long long decoder_has_key(__sn__Decoder *self, const char *key) {
    JsonDecoderCtx *ctx = (JsonDecoderCtx *)self->__sn__ctx;
    return json_object_get(ctx->node, key) != NULL;
}

static __sn__Decoder *decoder_read_object(__sn__Decoder *self, const char *key) {
    JsonDecoderCtx *ctx = (JsonDecoderCtx *)self->__sn__ctx;
    return decoder_create(json_object_get(ctx->node, key), NULL);
}

static __sn__Decoder *decoder_read_array(__sn__Decoder *self, const char *key) {
    JsonDecoderCtx *ctx = (JsonDecoderCtx *)self->__sn__ctx;
    return decoder_create(json_object_get(ctx->node, key), NULL);
}

/* --- Array accessors --- */

static long long decoder_length(__sn__Decoder *self) {
    JsonDecoderCtx *ctx = (JsonDecoderCtx *)self->__sn__ctx;
    if (ctx->node && ctx->node->type == JSON_NODE_ARRAY) return ctx->node->array.count;
    return 0;
}

static __sn__Decoder *decoder_at(__sn__Decoder *self, long long index) {
    JsonDecoderCtx *ctx = (JsonDecoderCtx *)self->__sn__ctx;
    if (ctx->node && ctx->node->type == JSON_NODE_ARRAY && index < ctx->node->array.count)
        return decoder_create(ctx->node->array.items[index], NULL);
    return decoder_create(NULL, NULL);
}

static char *decoder_at_str(__sn__Decoder *self, long long index) {
    JsonDecoderCtx *ctx = (JsonDecoderCtx *)self->__sn__ctx;
    if (ctx->node && ctx->node->type == JSON_NODE_ARRAY && index < ctx->node->array.count) {
        JsonNode *val = ctx->node->array.items[index];
        if (val->type == JSON_NODE_STRING) return strdup(val->string_value);
    }
    return strdup("");
}

static long long decoder_at_int(__sn__Decoder *self, long long index) {
    JsonDecoderCtx *ctx = (JsonDecoderCtx *)self->__sn__ctx;
    if (ctx->node && ctx->node->type == JSON_NODE_ARRAY && index < ctx->node->array.count) {
        JsonNode *val = ctx->node->array.items[index];
        if (val->type == JSON_NODE_INT) return val->int_value;
    }
    return 0;
}

static double decoder_at_double(__sn__Decoder *self, long long index) {
    JsonDecoderCtx *ctx = (JsonDecoderCtx *)self->__sn__ctx;
    if (ctx->node && ctx->node->type == JSON_NODE_ARRAY && index < ctx->node->array.count) {
        JsonNode *val = ctx->node->array.items[index];
        if (val->type == JSON_NODE_DOUBLE) return val->double_value;
        if (val->type == JSON_NODE_INT)    return (double)val->int_value;
    }
    return 0.0;
}

static long long decoder_at_bool(__sn__Decoder *self, long long index) {
    JsonDecoderCtx *ctx = (JsonDecoderCtx *)self->__sn__ctx;
    if (ctx->node && ctx->node->type == JSON_NODE_ARRAY && index < ctx->node->array.count) {
        JsonNode *val = ctx->node->array.items[index];
        if (val->type == JSON_NODE_BOOL) return val->bool_value;
    }
    return 0;
}

/* Decoder vtable — wired into every decoder instance. */
static __sn__DecoderVTable decoder_vtable = {
    .readStr    = decoder_read_str,
    .readInt    = decoder_read_int,
    .readDouble = decoder_read_double,
    .readBool   = decoder_read_bool,
    .hasKey     = decoder_has_key,
    .readObject = decoder_read_object,
    .readArray  = decoder_read_array,
    .length     = decoder_length,
    .at         = decoder_at,
    .atStr      = decoder_at_str,
    .atInt      = decoder_at_int,
    .atDouble   = decoder_at_double,
    .atBool     = decoder_at_bool,
};

/* Cleanup handler for the root decoder (frees the entire parse tree). */
static void decoder_cleanup(__sn__Decoder *self) {
    JsonDecoderCtx *ctx = (JsonDecoderCtx *)self->__sn__ctx;
    if (ctx) {
        if (ctx->root) json_node_free(ctx->root);
        free(ctx);
        self->__sn__ctx = NULL;
    }
}

/* Create a decoder wrapping a node. If `root` is non-NULL, this decoder owns the tree. */
static __sn__Decoder *decoder_create(JsonNode *node, JsonNode *root) {
    __sn__Decoder *dec = (__sn__Decoder *)calloc(1, sizeof(__sn__Decoder));
    JsonDecoderCtx *ctx = (JsonDecoderCtx *)calloc(1, sizeof(JsonDecoderCtx));
    ctx->node = node;
    ctx->root = root;
    dec->__sn__vt = &decoder_vtable;
    dec->__sn__ctx = ctx;
    dec->__sn__cleanup = decoder_cleanup;
    return dec;
}

/* Public: parse a JSON string and return a root decoder. */
__sn__Decoder *sn_json_decoder(const char *input) {
    const char *cursor = input;
    JsonNode *root = json_parse_value(&cursor);
    return decoder_create(root, root);
}
