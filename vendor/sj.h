// sj.h - v0.4 - rxi 2025
// public domain - no warranty implied, use at your own risk
//
// Local changes from upstream:
// - Made reader/value slices and error strings const-correct so the header can
//   be compiled as C++ and can read immutable input buffers/string literals.
// - Replaced C99 designated compound literals in the implementation with
//   aggregate initialization accepted by both C99 and C++.
// - Added project-local sjp_* helper functions in a separate section below.

#ifndef SJ_H
#define SJ_H

#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *data, *cur, *end;
    int depth;
    const char *error;
} sj_Reader;

typedef struct {
    int type;
    const char *start, *end;
    int depth;
} sj_Value;

enum { SJ_ERROR, SJ_END, SJ_ARRAY, SJ_OBJECT, SJ_NUMBER, SJ_STRING, SJ_BOOL, SJ_NULL };

sj_Reader sj_reader(const char *data, size_t len);
sj_Value sj_read(sj_Reader *r);
bool sj_iter_array(sj_Reader *r, sj_Value arr, sj_Value *val);
bool sj_iter_object(sj_Reader *r, sj_Value obj, sj_Value *key, sj_Value *val);
void sj_location(sj_Reader *r, int *line, int *col);

// --- Local extensions ------------------------------------------------------

static inline bool sjp_eq(sj_Value val, const char *s) {
    size_t len = (size_t)(val.end - val.start);
    return val.type == SJ_STRING && strlen(s) == len && memcmp(val.start, s, len) == 0;
}

static inline void sjp_set_error(sj_Reader *r, const char *msg) {
    if (!r->error) r->error = msg;
}

static inline bool sjp_copy_string(sj_Reader *r, sj_Value val, char *out, size_t out_size) {
    if (val.type != SJ_STRING) {
        sjp_set_error(r, "expected string");
        return false;
    }

    size_t w = 0;
    for (const char *p = val.start; p != val.end; p++) {
        char c = *p;
        if (c == '\\') {
            if (++p == val.end) {
                sjp_set_error(r, "unterminated escape");
                return false;
            }
            char esc = *p;
            if (esc == '"' || esc == '\\') c = esc;
            else if (esc == 'n') c = '\n';
            else if (esc == 't') c = '\t';
            else if (esc == 'r') c = '\r';
            else if (esc == '/') c = '/';
            else {
                sjp_set_error(r, "unsupported escape");
                return false;
            }
        }

        if (w + 1 < out_size) out[w++] = c;
        else if (out_size > 0) out[out_size - 1] = '\0';
    }
    if (out_size > 0) out[w < out_size ? w : out_size - 1] = '\0';
    return true;
}

static inline bool sjp_copy_token(sj_Reader *r, sj_Value val, char *out, size_t out_size, const char *too_long_msg) {
    size_t len = (size_t)(val.end - val.start);
    if (len >= out_size) {
        sjp_set_error(r, too_long_msg);
        return false;
    }
    memcpy(out, val.start, len);
    out[len] = '\0';
    return true;
}

static inline bool sjp_parse_int(sj_Reader *r, sj_Value val, int *out) {
    if (val.type != SJ_NUMBER) {
        sjp_set_error(r, "expected number");
        return false;
    }
    char tmp[64];
    if (!sjp_copy_token(r, val, tmp, sizeof(tmp), "number too long")) return false;
    *out = (int)strtol(tmp, NULL, 10);
    return true;
}

static inline bool sjp_parse_float(sj_Reader *r, sj_Value val, float *out) {
    if (val.type != SJ_NUMBER) {
        sjp_set_error(r, "expected number");
        return false;
    }
    char tmp[64];
    if (!sjp_copy_token(r, val, tmp, sizeof(tmp), "number too long")) return false;
    *out = strtof(tmp, NULL);
    return true;
}

static inline bool sjp_parse_bool(sj_Reader *r, sj_Value val, bool *out) {
    if (val.type != SJ_BOOL) {
        sjp_set_error(r, "expected bool");
        return false;
    }
    *out = val.start[0] == 't';
    return true;
}

static inline bool sjp_parse_float3(sj_Reader *r, sj_Value arr, float out[3]) {
    if (arr.type != SJ_ARRAY) {
        sjp_set_error(r, "expected array");
        return false;
    }
    int count = 0;
    sj_Value val;
    while (sj_iter_array(r, arr, &val)) {
        if (count >= 3) {
            sjp_set_error(r, "array too long");
            return false;
        }
        if (!sjp_parse_float(r, val, &out[count])) return false;
        count++;
    }
    if (r->error) return false;
    if (count != 3) {
        sjp_set_error(r, "array length mismatch");
        return false;
    }
    return true;
}

static inline bool sjp_parse_float_array(sj_Reader *r, sj_Value arr, float *out, int expected) {
    if (arr.type != SJ_ARRAY) {
        sjp_set_error(r, "expected array");
        return false;
    }
    int count = 0;
    sj_Value val;
    while (sj_iter_array(r, arr, &val)) {
        if (count >= expected) {
            sjp_set_error(r, "array too long");
            return false;
        }
        if (!sjp_parse_float(r, val, &out[count])) return false;
        count++;
    }
    if (r->error) return false;
    if (count != expected) {
        sjp_set_error(r, "array length mismatch");
        return false;
    }
    return true;
}

static inline bool sjp_parse_string_array(sj_Reader *r, sj_Value arr, char *out, size_t string_size, int expected) {
    if (arr.type != SJ_ARRAY) {
        sjp_set_error(r, "expected array");
        return false;
    }
    int count = 0;
    sj_Value val;
    while (sj_iter_array(r, arr, &val)) {
        if (count >= expected) {
            sjp_set_error(r, "array too long");
            return false;
        }
        if (!sjp_copy_string(r, val, out + (size_t)count * string_size, string_size)) return false;
        count++;
    }
    if (r->error) return false;
    if (count != expected) {
        sjp_set_error(r, "array length mismatch");
        return false;
    }
    return true;
}

static inline bool sjp_expect_object(sj_Reader *r, sj_Value val) {
    if (val.type == SJ_OBJECT) return true;
    sjp_set_error(r, "expected object");
    return false;
}

static inline bool sjp_expect_array(sj_Reader *r, sj_Value val) {
    if (val.type == SJ_ARRAY) return true;
    sjp_set_error(r, "expected array");
    return false;
}

static inline int sjp_error_offset(sj_Reader *r) {
    return (int)(r->cur - r->data);
}

#endif // #ifndef SJ_H

#ifdef SJ_IMPL


sj_Reader sj_reader(const char *data, size_t len) {
    sj_Reader r = { data, data, data + len, 0, NULL };
    return r;
}


static bool sj__is_number_cont(char c) {
    return (c >= '0' && c <= '9')
        ||  c == 'e' || c == 'E' || c == '.' || c == '-' || c == '+';
}

static bool sj__is_string(const char *cur, const char *end, const char *expect) {
    while (*expect) {
        if (cur == end || *cur != *expect) {
            return false;
        }
        expect++, cur++;
    }
    return true;
}


sj_Value sj_read(sj_Reader *r) {
    sj_Value res;
top:
    if (r->error) {
        sj_Value err = { SJ_ERROR, r->cur, r->cur, 0 };
        return err;
    }
    if (r->cur == r->end) { r->error = "unexpected eof"; goto top; }
    res.start = r->cur;

    switch (*r->cur) {
    case ' ': case '\n': case '\r': case '\t':
    case ':': case ',':
        r->cur++;
        goto top;

    case '-': case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        res.type = SJ_NUMBER;
        while (r->cur != r->end && sj__is_number_cont(*r->cur)) { r->cur++; }
        break;

    case '"':
        res.type = SJ_STRING;
        res.start = ++r->cur;
        for (;;) {
            if ( r->cur == r->end) { r->error = "unclosed string"; goto top; }
            if (*r->cur ==    '"') { break; }
            if (*r->cur ==   '\\') { r->cur++; }
            if ( r->cur != r->end) { r->cur++; }
        }
        res.end = r->cur++;
        return res;

    case '{': case '[':
        res.type = (*r->cur == '{') ? SJ_OBJECT : SJ_ARRAY;
        res.depth = ++r->depth;
        r->cur++;
        break;

    case '}': case ']':
        res.type = SJ_END;
        if (--r->depth < 0) {
            r->error = (*r->cur == '}') ? "stray '}'" : "stray ']'";
            goto top;
        }
        r->cur++;
        break;

    case 'n': case 't': case 'f':
        res.type = (*r->cur == 'n') ? SJ_NULL : SJ_BOOL;
        if (sj__is_string(r->cur, r->end,  "null")) { r->cur += 4; break; }
        if (sj__is_string(r->cur, r->end,  "true")) { r->cur += 4; break; }
        if (sj__is_string(r->cur, r->end, "false")) { r->cur += 5; break; }
        // fallthrough

    default:
        r->error = "unknown token";
        goto top;
    }
    res.end = r->cur;
    return res;
}


static void sj__discard_until(sj_Reader *r, int depth) {
    sj_Value val;
    val.type = SJ_NULL;
    while (r->depth != depth && val.type != SJ_ERROR) {
        val = sj_read(r);
    }
}


bool sj_iter_array(sj_Reader *r, sj_Value arr, sj_Value *val) {
    sj__discard_until(r, arr.depth);
    *val = sj_read(r);
    if (val->type == SJ_ERROR || val->type == SJ_END) { return false; }
    return true;
}


bool sj_iter_object(sj_Reader *r, sj_Value obj, sj_Value *key, sj_Value *val) {
    sj__discard_until(r, obj.depth);
    *key = sj_read(r);
    if (key->type == SJ_ERROR || key->type == SJ_END) { return false; }
    *val = sj_read(r);
    if (val->type == SJ_END)   { r->error = "unexpected object end"; return false; }
    if (val->type == SJ_ERROR) { return false; }
    return true;
}


void sj_location(sj_Reader *r, int *line, int *col) {
    int ln = 1, cl = 1;
    for (const char *p = r->data; p != r->cur; p++) {
        if (*p == '\n') { ln++; cl = 0; }
        cl++;
    }
    *line = ln;
    *col = cl;
}

#endif // #ifdef SJ_IMPL
