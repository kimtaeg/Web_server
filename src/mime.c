#include "mime.h"

#include <string.h>
#include <strings.h>

static const struct {
    const char *ext;
    const char *type;
} mime_table[] = {
    {"html", "text/html; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"js", "text/javascript; charset=utf-8"},
    {"mjs", "text/javascript; charset=utf-8"},
    {"json", "application/json"},
    {"txt", "text/plain; charset=utf-8"},
    {"md", "text/markdown; charset=utf-8"},
    {"csv", "text/csv; charset=utf-8"},
    {"xml", "application/xml"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"webp", "image/webp"},
    {"svg", "image/svg+xml"},
    {"ico", "image/x-icon"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"ttf", "font/ttf"},
    {"pdf", "application/pdf"},
    {"wasm", "application/wasm"},
    {"mp4", "video/mp4"},
    {"webm", "video/webm"},
    {"mp3", "audio/mpeg"},
    {"wav", "audio/wav"},
    {"zip", "application/zip"},
    {"gz", "application/gzip"},
};

const char *mime_type_for_path(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    if (dot && dot != base && dot[1]) {
        for (size_t i = 0; i < sizeof(mime_table) / sizeof(mime_table[0]); i++)
            if (strcasecmp(dot + 1, mime_table[i].ext) == 0)
                return mime_table[i].type;
    }
    return "application/octet-stream";
}
