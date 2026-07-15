#include "mcp/json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------- parse --*/

typedef struct Parser {
    const char *p;
    int depth;
} Parser;

/* Recursion is bounded so a hostile or malformed document cannot overflow the
   stack by nesting brackets. */
#define JSON_MAX_DEPTH 64

static JsonValue *parse_value(Parser *ps);

static void skip_ws(Parser *ps) {
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r') ps->p++;
}

static JsonValue *value_new(JsonType type) {
    JsonValue *v = (JsonValue *)calloc(1, sizeof(JsonValue));
    if (v) v->type = type;
    return v;
}

void json_free(JsonValue *v) {
    if (!v) return;
    free(v->string);
    for (int i = 0; i < v->count; i++) {
        if (v->keys) free(v->keys[i]);
        if (v->values) json_free(v->values[i]);
    }
    free(v->keys);
    free(v->values);
    free(v);
}

static int hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static void utf8_emit(char **out, unsigned cp) {
    char *o = *out;
    if (cp < 0x80u) {
        *o++ = (char)cp;
    } else if (cp < 0x800u) {
        *o++ = (char)(0xC0u | (cp >> 6));
        *o++ = (char)(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
        *o++ = (char)(0xE0u | (cp >> 12));
        *o++ = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        *o++ = (char)(0x80u | (cp & 0x3Fu));
    } else {
        *o++ = (char)(0xF0u | (cp >> 18));
        *o++ = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        *o++ = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        *o++ = (char)(0x80u | (cp & 0x3Fu));
    }
    *out = o;
}

/* Decodes a quoted string.  The output is always shorter than the input, so a
   buffer the size of the remaining text is always sufficient. */
static char *parse_string_raw(Parser *ps) {
    if (*ps->p != '"') return NULL;
    ps->p++;
    size_t max = strlen(ps->p) + 1u;
    char *out = (char *)malloc(max);
    if (!out) return NULL;
    char *o = out;

    while (*ps->p && *ps->p != '"') {
        if (*ps->p == '\\') {
            ps->p++;
            switch (*ps->p) {
            case '"': *o++ = '"'; ps->p++; break;
            case '\\': *o++ = '\\'; ps->p++; break;
            case '/': *o++ = '/'; ps->p++; break;
            case 'b': *o++ = '\b'; ps->p++; break;
            case 'f': *o++ = '\f'; ps->p++; break;
            case 'n': *o++ = '\n'; ps->p++; break;
            case 'r': *o++ = '\r'; ps->p++; break;
            case 't': *o++ = '\t'; ps->p++; break;
            case 'u': {
                unsigned cp;
                if (!hex4(ps->p + 1, &cp)) { free(out); return NULL; }
                ps->p += 5;
                /* Surrogate pair: JSON escapes astral characters as two halves,
                   which must be recombined before UTF-8 encoding. */
                if (cp >= 0xD800u && cp <= 0xDBFFu && ps->p[0] == '\\' && ps->p[1] == 'u') {
                    unsigned lo;
                    if (hex4(ps->p + 2, &lo) && lo >= 0xDC00u && lo <= 0xDFFFu) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        ps->p += 6;
                    }
                }
                utf8_emit(&o, cp);
                break;
            }
            default: free(out); return NULL;
            }
        } else {
            *o++ = *ps->p++;
        }
    }
    if (*ps->p != '"') { free(out); return NULL; }
    ps->p++;
    *o = 0;
    return out;
}

static int push_slot(JsonValue *v, char *key, JsonValue *child) {
    JsonValue **nv = (JsonValue **)realloc(v->values, sizeof(JsonValue *) * (size_t)(v->count + 1));
    if (!nv) return 0;
    v->values = nv;
    if (v->type == JSON_OBJECT) {
        char **nk = (char **)realloc(v->keys, sizeof(char *) * (size_t)(v->count + 1));
        if (!nk) return 0;
        v->keys = nk;
        v->keys[v->count] = key;
    }
    v->values[v->count] = child;
    v->count++;
    return 1;
}

static JsonValue *parse_object(Parser *ps) {
    JsonValue *v = value_new(JSON_OBJECT);
    if (!v) return NULL;
    ps->p++; /* '{' */
    skip_ws(ps);
    if (*ps->p == '}') { ps->p++; return v; }
    for (;;) {
        skip_ws(ps);
        char *key = parse_string_raw(ps);
        if (!key) { json_free(v); return NULL; }
        skip_ws(ps);
        if (*ps->p != ':') { free(key); json_free(v); return NULL; }
        ps->p++;
        JsonValue *child = parse_value(ps);
        if (!child || !push_slot(v, key, child)) {
            free(key);
            json_free(child);
            json_free(v);
            return NULL;
        }
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == '}') { ps->p++; return v; }
        json_free(v);
        return NULL;
    }
}

static JsonValue *parse_array(Parser *ps) {
    JsonValue *v = value_new(JSON_ARRAY);
    if (!v) return NULL;
    ps->p++; /* '[' */
    skip_ws(ps);
    if (*ps->p == ']') { ps->p++; return v; }
    for (;;) {
        JsonValue *child = parse_value(ps);
        if (!child || !push_slot(v, NULL, child)) {
            json_free(child);
            json_free(v);
            return NULL;
        }
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == ']') { ps->p++; return v; }
        json_free(v);
        return NULL;
    }
}

static JsonValue *parse_value(Parser *ps) {
    if (ps->depth >= JSON_MAX_DEPTH) return NULL;
    ps->depth++;
    skip_ws(ps);
    JsonValue *v = NULL;

    if (*ps->p == '{') {
        v = parse_object(ps);
    } else if (*ps->p == '[') {
        v = parse_array(ps);
    } else if (*ps->p == '"') {
        char *s = parse_string_raw(ps);
        if (s) {
            v = value_new(JSON_STRING);
            if (v) v->string = s; else free(s);
        }
    } else if (!strncmp(ps->p, "true", 4)) {
        ps->p += 4;
        v = value_new(JSON_BOOL);
        if (v) v->boolean = 1;
    } else if (!strncmp(ps->p, "false", 5)) {
        ps->p += 5;
        v = value_new(JSON_BOOL);
    } else if (!strncmp(ps->p, "null", 4)) {
        ps->p += 4;
        v = value_new(JSON_NULL);
    } else {
        char *end = NULL;
        double d = strtod(ps->p, &end);
        if (end && end != ps->p) {
            ps->p = end;
            v = value_new(JSON_NUMBER);
            if (v) v->number = d;
        }
    }
    ps->depth--;
    return v;
}

JsonValue *json_parse(const char *text) {
    if (!text) return NULL;
    Parser ps = { text, 0 };
    JsonValue *v = parse_value(&ps);
    if (!v) return NULL;
    skip_ws(&ps);
    if (*ps.p) { json_free(v); return NULL; } /* Trailing garbage. */
    return v;
}

const JsonValue *json_object_get(const JsonValue *object, const char *key) {
    if (!object || object->type != JSON_OBJECT || !key) return NULL;
    for (int i = 0; i < object->count; i++) {
        if (object->keys[i] && !strcmp(object->keys[i], key)) return object->values[i];
    }
    return NULL;
}

const char *json_string_or(const JsonValue *v, const char *fallback) {
    return (v && v->type == JSON_STRING) ? v->string : fallback;
}

double json_number_or(const JsonValue *v, double fallback) {
    if (!v) return fallback;
    if (v->type == JSON_NUMBER) return v->number;
    /* Addresses are naturally written "0x0E000480", which JSON has no syntax
       for, so accept a numeric string too rather than making callers convert. */
    if (v->type == JSON_STRING && v->string) {
        char *end = NULL;
        double d = (double)strtoul(v->string, &end, 0);
        if (end && end != v->string && !*end) return d;
    }
    return fallback;
}

int json_bool_or(const JsonValue *v, int fallback) {
    if (!v) return fallback;
    if (v->type == JSON_BOOL) return v->boolean;
    if (v->type == JSON_NUMBER) return v->number != 0.0;
    return fallback;
}

/* ----------------------------------------------------------------- write --*/

void json_writer_init(JsonWriter *w) {
    memset(w, 0, sizeof(*w));
}

void json_writer_free(JsonWriter *w) {
    if (!w) return;
    free(w->buf);
    memset(w, 0, sizeof(*w));
}

static int writer_reserve(JsonWriter *w, size_t extra) {
    if (w->error) return 0;
    if (w->len + extra + 1u <= w->cap) return 1;
    size_t ncap = w->cap ? w->cap : 256u;
    while (ncap < w->len + extra + 1u) ncap *= 2u;
    char *nbuf = (char *)realloc(w->buf, ncap);
    if (!nbuf) { w->error = 1; return 0; }
    w->buf = nbuf;
    w->cap = ncap;
    return 1;
}

void json_write_raw(JsonWriter *w, const char *text) {
    if (!w || !text) return;
    size_t n = strlen(text);
    if (!writer_reserve(w, n)) return;
    memcpy(w->buf + w->len, text, n);
    w->len += n;
    w->buf[w->len] = 0;
}

void json_write_string(JsonWriter *w, const char *text) {
    if (!w) return;
    if (!text) { json_write_raw(w, "null"); return; }
    /* Worst case is \u00XX per byte, six characters, plus the quotes. */
    if (!writer_reserve(w, strlen(text) * 6u + 2u)) return;
    char *o = w->buf + w->len;
    *o++ = '"';
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        switch (*p) {
        case '"': *o++ = '\\'; *o++ = '"'; break;
        case '\\': *o++ = '\\'; *o++ = '\\'; break;
        case '\n': *o++ = '\\'; *o++ = 'n'; break;
        case '\r': *o++ = '\\'; *o++ = 'r'; break;
        case '\t': *o++ = '\\'; *o++ = 't'; break;
        case '\b': *o++ = '\\'; *o++ = 'b'; break;
        case '\f': *o++ = '\\'; *o++ = 'f'; break;
        default:
            if (*p < 0x20u) {
                o += sprintf(o, "\\u%04x", (unsigned)*p);
            } else {
                *o++ = (char)*p;
            }
            break;
        }
    }
    *o++ = '"';
    w->len = (size_t)(o - w->buf);
    w->buf[w->len] = 0;
}

void json_write_fmt(JsonWriter *w, const char *fmt, ...) {
    if (!w || w->error) return;
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { w->error = 1; va_end(ap2); return; }
    if (writer_reserve(w, (size_t)n)) {
        vsnprintf(w->buf + w->len, (size_t)n + 1u, fmt, ap2);
        w->len += (size_t)n;
    }
    va_end(ap2);
}
