#ifndef STATIC_FILE_H
#define STATIC_FILE_H

#include <stddef.h>
#include <sys/stat.h>

#include "server.h"

/*
 * Map a normalized URL path onto a regular file under root_real.
 * Returns 0 (out holds the real path), 301 (directory without trailing slash),
 * 403 (outside root, dotfile, unreadable, directory without index), 404, 414 or 500.
 */
int static_resolve(const char *root_real, const char *index_file, const char *url_path,
                   char *out, size_t out_cap, struct stat *st);

int static_serve(req_ctx_t *ctx);

#endif
