#define _POSIX_C_SOURCE 200809L

#include "forwarder.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define BUFFER_SIZE 8192
#define REQUEST_SIZE 65536
#define CONNECT_TIMEOUT_SECONDS 10
#define IO_TIMEOUT_SECONDS 15

/* Send all bytes unless an error occurs. */
static int send_all(int fd, const char *data, size_t length)
{
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0) {
            errno = EPIPE;
            return -1;
        }

        sent += (size_t)n;
    }

    return 0;
}

/* Connect to a hostname and TCP port. */
static int connect_to_server(const char *host, int port)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp;
    char port_string[16];
    int fd = -1;

    if (host == NULL || host[0] == '\0' ||
        port < 1 || port > 65535) {
        errno = EINVAL;
        return -1;
    }

    snprintf(port_string, sizeof(port_string), "%d", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(host, port_string, &hints, &result);

    if (status != 0) {
        fprintf(stderr, "[FORWARD] DNS error for %s: %s\n",
                host, gai_strerror(status));
        errno = EHOSTUNREACH;
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if (fd < 0)
            continue;

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;

        fprintf(stderr, "[FORWARD] Connect attempt failed: %s\n",
                strerror(errno));
        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return fd;
}

/*
 * Convert an absolute HTTP URL to its request path.
 * For example: http://example.com/page?q=1 -> /page?q=1
 */
static const char *get_request_path(const char *target)
{
    if (target == NULL || target[0] == '\0')
        return "/";

    if (strncasecmp(target, "http://", 7) == 0) {
        const char *path = strchr(target + 7, '/');
        const char *query = strchr(target + 7, '?');

        if (query != NULL && (path == NULL || query < path))
            return query;

        return path != NULL ? path : "/";
    }

    return target;
}

/*
 * Build an origin-form HTTP request and remove proxy-specific
 * connection headers. This starter supports header-only requests.
 */
static int build_request(
    const HttpRequest *request,
    const char *original,
    char *output,
    size_t output_size)
{
    const char *line_end;
    const char *headers;
    const char *body;
    const char *path;
    size_t used = 0;
    int n;

    if (request == NULL || original == NULL ||
        output == NULL || output_size == 0) {
        return -1;
    }

    line_end = strstr(original, "\r\n");

    if (line_end == NULL)
        return -1;

    path = get_request_path(request->target);

    n = snprintf(output, output_size, "%s %s HTTP/1.1\r\n",
                 request->method, path);

    if (n < 0 || (size_t)n >= output_size)
        return -1;

    used = (size_t)n;
    headers = line_end + 2;

    body = strstr(headers, "\r\n\r\n");

    if (body == NULL)
        return -1;

    while (headers < body) {
        const char *end = strstr(headers, "\r\n");
        size_t length;

        if (end == NULL || end > body)
            return -1;

        length = (size_t)(end - headers);

        if (length == 0)
            break;

        /* Do not forward proxy connection management headers. */
        if (strncasecmp(headers, "Proxy-Connection:", 16) != 0 &&
            strncasecmp(headers, "Connection:", 11) != 0) {
            if (used + length + 2 >= output_size)
                return -1;

            memcpy(output + used, headers, length);
            used += length;
            output[used++] = '\r';
            output[used++] = '\n';
        }

        headers = end + 2;
    }

    n = snprintf(output + used, output_size - used,
                 "Connection: close\r\n\r\n");

    if (n < 0 || (size_t)n >= output_size - used)
        return -1;

    return (int)(used + (size_t)n);
}

int forward_http_request(
    const HttpRequest *request,
    const char *original_request,
    int client_fd)
{
    int server_fd;
    int request_length;
    char outgoing[REQUEST_SIZE];
    char buffer[BUFFER_SIZE];
    struct timeval timeout;
    size_t total_received = 0;

    if (request == NULL || original_request == NULL || client_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    if (request->is_connect ||
        strcasecmp(request->method, "CONNECT") == 0) {
        static const char response[] =
            "HTTP/1.1 501 Not Implemented\r\n"
            "Connection: close\r\n"
            "Content-Length: 0\r\n\r\n";

        (void)send_all(client_fd, response, sizeof(response) - 1);
        return -1;
    }

    fprintf(stderr, "[FORWARD] Connecting to %s:%d\n",
            request->host, request->port);

    server_fd = connect_to_server(request->host, request->port);

    if (server_fd < 0) {
        static const char response[] =
            "HTTP/1.1 502 Bad Gateway\r\n"
            "Connection: close\r\n"
            "Content-Length: 0\r\n\r\n";

        (void)send_all(client_fd, response, sizeof(response) - 1);
        return -1;
    }

    timeout.tv_sec = IO_TIMEOUT_SECONDS;
    timeout.tv_usec = 0;

    if (setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) < 0 ||
        setsockopt(server_fd, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout)) < 0) {
        perror("[FORWARD] setsockopt");
        close(server_fd);
        return -1;
    }

    request_length = build_request(
        request, original_request, outgoing, sizeof(outgoing));

    if (request_length < 0) {
        fprintf(stderr, "[FORWARD] Could not build HTTP request\n");
        close(server_fd);
        return -1;
    }

    if (send_all(server_fd, outgoing, (size_t)request_length) < 0) {
        perror("[FORWARD] Sending request");
        close(server_fd);
        return -1;
    }

    fprintf(stderr, "[FORWARD] Sent %d request bytes to %s:%d\n",
            request_length, request->host, request->port);

    for (;;) {
        ssize_t received = recv(server_fd, buffer, sizeof(buffer), 0);

        if (received < 0) {
            if (errno == EINTR)
                continue;

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (total_received == 0) {
                    fprintf(stderr,
                            "[FORWARD] Timeout before any response bytes\n");
                    close(server_fd);
                    return -1;
                }

                fprintf(stderr,
                        "[FORWARD] Response read timed out after %zu bytes\n",
                        total_received);
                break;
            }

            perror("[FORWARD] Receiving response");
            close(server_fd);
            return -1;
        }

        if (received == 0) {
            fprintf(stderr, "[FORWARD] Destination closed connection\n");
            break;
        }

        if (send_all(client_fd, buffer, (size_t)received) < 0) {
            perror("[FORWARD] Sending response to client");
            close(server_fd);
            return -1;
        }

        total_received += (size_t)received;
    }

    close(server_fd);

    fprintf(stderr, "[FORWARD] Relayed %zu response bytes\n",
            total_received);

    return total_received > 0 ? 0 : -1;
}