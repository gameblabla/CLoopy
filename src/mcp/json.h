#ifndef LOOPY_MCP_JSON_H
#define LOOPY_MCP_JSON_H
#include <stddef.h>

/*
 * Minimal JSON reader/writer, sized for JSON-RPC traffic and nothing more.
 * The emulator has no external dependencies and this keeps it that way.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum JsonType {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

struct JsonValue {
    JsonType type;
    double number;
    int boolean;
    char *string;        /* JSON_STRING: decoded, NUL-terminated. */
    int count;           /* JSON_ARRAY/JSON_OBJECT element count. */
    char **keys;         /* JSON_OBJECT only, parallel to values. */
    JsonValue **values;  /* JSON_ARRAY and JSON_OBJECT. */
};

/* Returns NULL on malformed input.  The caller owns the result. */
JsonValue *json_parse(const char *text);
void json_free(JsonValue *value);

/* NULL-safe lookups: any of these accept a NULL value and return the fallback,
   so a chain of gets on absent params does not need guarding at every step. */
const JsonValue *json_object_get(const JsonValue *object, const char *key);
const char *json_string_or(const JsonValue *value, const char *fallback);
double json_number_or(const JsonValue *value, double fallback);
int json_bool_or(const JsonValue *value, int fallback);

/* Growable output buffer. */
typedef struct JsonWriter {
    char *buf;
    size_t len;
    size_t cap;
    int error;
} JsonWriter;

void json_writer_init(JsonWriter *w);
void json_writer_free(JsonWriter *w);
void json_write_raw(JsonWriter *w, const char *text);
/* Writes a quoted, escaped JSON string. */
void json_write_string(JsonWriter *w, const char *text);
void json_write_fmt(JsonWriter *w, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
