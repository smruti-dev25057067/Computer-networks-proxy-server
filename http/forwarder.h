#ifndef FORWARDER_H
#define FORWARDER_H

#include "http_parser.h"

/*
 * Forwards a parsed HTTP request to its destination and relays
 * the response to client_fd.
 *
 * Returns 0 on success, -1 on failure.
 */
int forward_http_request(
    const HttpRequest *request,
    const char *original_request,
    int client_fd
);

#endif