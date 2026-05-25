#pragma once
#include <cstddef>
#include <cstdint>

// Minimal hand-rolled JSON tokenizer for the .hotspots sidecar grammar.
// Not a general JSON library — supports only the subset we emit:
//   - objects, arrays, strings (with \" and \\ escapes only), numbers,
//     true/false/null
//   - no \uXXXX, no comments, no trailing commas
// Errors are sticky: once `ok` flips to false, all subsequent calls no-op
// and return false. The caller logs once at the top level.

struct Json {
    const char* p;          // current cursor
    const char* end;        // one past last byte
    const char* base;       // start of buffer (for byte-offset error reporting)
    bool        ok;
    int         err_offset;
    const char* err_msg;
};

void json_init(Json* j, const char* buf, size_t len);
void json_set_error(Json* j, const char* msg);

void json_skip_ws(Json* j);

// Returns true (without consuming) if next non-ws byte == c.
bool json_peek_char(Json* j, char c);

// If next non-ws byte == c, consume it and return true. Otherwise return
// false without erroring.
bool json_try_char(Json* j, char c);

// Consumes next non-ws byte; errors if it isn't `c`.
bool json_expect_char(Json* j, char c);

// Parses a JSON string into out (null-terminated, truncated to out_size-1).
// Only \" and \\ escapes are honored.
bool json_parse_string(Json* j, char* out, size_t out_size);

// Parses a number as int / float, or a literal as bool.
bool json_parse_int(Json* j, int* out);
bool json_parse_float(Json* j, float* out);
bool json_parse_bool(Json* j, bool* out);

// Skips any JSON value (object, array, string, number, true, false, null).
// Used to ignore unknown keys for forward-compat.
void json_skip_value(Json* j);
