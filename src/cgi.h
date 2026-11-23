#ifndef CGI_H
#define CGI_H

#include <stddef.h>

#include "buffer.h"
#include "server.h"

#define CGI_MAX_OUTPUT (16u * 1024 * 1024)

/*
 * Split CGI output (RFC 3875 section 6) into status, content type, extra headers to
 * forward (CRLF lines) and the body offset. Returns 0, or -1 if the header block is bad.
 */
int cgi_parse_output(const char *out, size_t len, int *status, char *content_type,
                     size_t ctype_cap, buf_t *extra_headers, size_t *body_off);

/* NULL-terminated "KEY=value" array for execve; free with cgi_free_env. */
char **cgi_build_env(const req_ctx_t *ctx, const char *script_name, const char *path_info);
void cgi_free_env(char **env);

int cgi_handle(req_ctx_t *ctx);

#endif
