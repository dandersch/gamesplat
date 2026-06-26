#pragma once

// NOTE: strings.h
static bool file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// NOTE: strings.h
static void copy_string(char* dst, size_t dst_size, const char* src) {
    snprintf(dst, dst_size, "%s", src ? src : "");
}

// NOTE: strings.h
static void join_path(char* dst, size_t dst_size, const char* a, const char* b) {
    copy_string(dst, dst_size, a);
    size_t len = strlen(dst);
    if (len + 1 < dst_size) {
        dst[len++] = '/';
        dst[len] = '\0';
    }
    if (len < dst_size) {
        strncat(dst, b, dst_size - len - 1);
    }
}

// NOTE: strings.h
static bool is_numeric_name(const char* name) {
    if (!name || !name[0]) return false;
    for (const char* p = name; *p; p++) {
        if (!isdigit((unsigned char)*p)) return false;
    }
    return true;
}

// NOTE: strings.h
// Skip the rest of the current line (handles arbitrarily long POINTS2D lines)
static void skip_line(FILE* f) {
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n') {}
}
