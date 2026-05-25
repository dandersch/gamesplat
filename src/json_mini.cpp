#include "json_mini.h"
#include <cstring>
#include <cstdlib>

void json_init(Json* j, const char* buf, size_t len) {
    j->p = buf;
    j->end = buf + len;
    j->base = buf;
    j->ok = true;
    j->err_offset = 0;
    j->err_msg = NULL;
}

void json_set_error(Json* j, const char* msg) {
    if (!j->ok) return; // first error wins
    j->ok = false;
    j->err_offset = (int)(j->p - j->base);
    j->err_msg = msg;
}

void json_skip_ws(Json* j) {
    if (!j->ok) return;
    while (j->p < j->end) {
        char c = *j->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { j->p++; continue; }
        break;
    }
}

bool json_peek_char(Json* j, char c) {
    json_skip_ws(j);
    if (!j->ok || j->p >= j->end) return false;
    return *j->p == c;
}

bool json_try_char(Json* j, char c) {
    json_skip_ws(j);
    if (!j->ok || j->p >= j->end) return false;
    if (*j->p != c) return false;
    j->p++;
    return true;
}

bool json_expect_char(Json* j, char c) {
    json_skip_ws(j);
    if (!j->ok) return false;
    if (j->p >= j->end || *j->p != c) {
        static char buf[32];
        snprintf(buf, sizeof(buf), "expected '%c'", c);
        json_set_error(j, buf);
        return false;
    }
    j->p++;
    return true;
}

bool json_parse_string(Json* j, char* out, size_t out_size) {
    json_skip_ws(j);
    if (!j->ok) return false;
    if (j->p >= j->end || *j->p != '"') {
        json_set_error(j, "expected string");
        return false;
    }
    j->p++; // consume opening quote
    size_t w = 0;
    while (j->p < j->end) {
        char c = *j->p++;
        if (c == '"') {
            if (w < out_size) out[w] = '\0';
            else if (out_size > 0) out[out_size - 1] = '\0';
            return true;
        }
        if (c == '\\') {
            if (j->p >= j->end) { json_set_error(j, "unterminated escape"); return false; }
            char esc = *j->p++;
            if (esc == '"' || esc == '\\') c = esc;
            else if (esc == 'n') c = '\n';
            else if (esc == 't') c = '\t';
            else if (esc == 'r') c = '\r';
            else if (esc == '/') c = '/';
            else { json_set_error(j, "unsupported escape"); return false; }
        }
        if (w + 1 < out_size) out[w++] = c;
        else if (out_size > 0) out[out_size - 1] = '\0';
        // else silently truncate
    }
    json_set_error(j, "unterminated string");
    return false;
}

// Walk over a numeric token; copy into stack buffer for strtof/strtol.
static bool scan_number(Json* j, char* tmp, size_t tmp_size) {
    json_skip_ws(j);
    if (!j->ok) return false;
    const char* start = j->p;
    if (j->p < j->end && (*j->p == '-' || *j->p == '+')) j->p++;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    if (j->p < j->end && *j->p == '.') {
        j->p++;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    }
    if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
        j->p++;
        if (j->p < j->end && (*j->p == '-' || *j->p == '+')) j->p++;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    }
    size_t n = (size_t)(j->p - start);
    if (n == 0) { json_set_error(j, "expected number"); return false; }
    if (n >= tmp_size) { json_set_error(j, "number too long"); return false; }
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    return true;
}

bool json_parse_int(Json* j, int* out) {
    char tmp[64];
    if (!scan_number(j, tmp, sizeof(tmp))) return false;
    *out = (int)strtol(tmp, NULL, 10);
    return true;
}

bool json_parse_float(Json* j, float* out) {
    char tmp[64];
    if (!scan_number(j, tmp, sizeof(tmp))) return false;
    *out = strtof(tmp, NULL);
    return true;
}

bool json_parse_bool(Json* j, bool* out) {
    json_skip_ws(j);
    if (!j->ok) return false;
    if (j->end - j->p >= 4 && memcmp(j->p, "true", 4) == 0) {
        j->p += 4;
        *out = true;
        return true;
    }
    if (j->end - j->p >= 5 && memcmp(j->p, "false", 5) == 0) {
        j->p += 5;
        *out = false;
        return true;
    }
    json_set_error(j, "expected bool");
    return false;
}

void json_skip_value(Json* j) {
    json_skip_ws(j);
    if (!j->ok || j->p >= j->end) { json_set_error(j, "expected value"); return; }
    char c = *j->p;
    if (c == '{') {
        j->p++;
        if (json_try_char(j, '}')) return;
        do {
            char dummy[256];
            if (!json_parse_string(j, dummy, sizeof(dummy))) return;
            if (!json_expect_char(j, ':')) return;
            json_skip_value(j);
            if (!j->ok) return;
        } while (json_try_char(j, ','));
        json_expect_char(j, '}');
    } else if (c == '[') {
        j->p++;
        if (json_try_char(j, ']')) return;
        do {
            json_skip_value(j);
            if (!j->ok) return;
        } while (json_try_char(j, ','));
        json_expect_char(j, ']');
    } else if (c == '"') {
        char dummy[256];
        json_parse_string(j, dummy, sizeof(dummy));
    } else if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
        float dummy;
        json_parse_float(j, &dummy);
    } else if (c == 't') {
        if (j->end - j->p >= 4 && memcmp(j->p, "true", 4) == 0) j->p += 4;
        else json_set_error(j, "expected 'true'");
    } else if (c == 'f') {
        if (j->end - j->p >= 5 && memcmp(j->p, "false", 5) == 0) j->p += 5;
        else json_set_error(j, "expected 'false'");
    } else if (c == 'n') {
        if (j->end - j->p >= 4 && memcmp(j->p, "null", 4) == 0) j->p += 4;
        else json_set_error(j, "expected 'null'");
    } else {
        json_set_error(j, "expected value");
    }
}
