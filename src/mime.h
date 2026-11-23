#ifndef MIME_H
#define MIME_H

/* MIME type for a file path, chosen by extension (case-insensitive). */
const char *mime_type_for_path(const char *path);

#endif
