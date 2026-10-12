#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

#define HTTP_METHOD_MAX   16
#define HTTP_TARGET_MAX   2048
#define HTTP_VERSION_MAX  16
#define HTTP_HOST_MAX     256
#define HTTP_ERROR_MAX    160

typedef struct {
    char method[HTTP_METHOD_MAX];
    char target[HTTP_TARGET_MAX];
    char version[HTTP_VERSION_MAX];
    char host[HTTP_HOST_MAX];
    int port;
    int is_connect;
} HttpRequest;

/*
 * Returns 0 on success and -1 on failure.
 * raw must contain a complete HTTP request header section.
 * The request body, if any, is not parsed by this function.
 */
int http_parse_request(
    const char *raw,
    HttpRequest *request,
    char *error,
    size_t error_size
);

#endif