#define _POSIX_C_SOURCE 200809L

#include "http_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_HEADER_BYTES 65536
#define MAX_HEADER_LINE  8192
#define MAX_HEADERS      100

static void set_error(char *error, size_t size, const char *message)
{
    if (error != NULL && size > 0) {
        (void)snprintf(error, size, "%s", message);
    }
}

static int copy_text(char *dest, size_t capacity,
                     const char *src, size_t length)
{
    if (dest == NULL || src == NULL || capacity == 0 ||
        length >= capacity) {
        return -1;
    }

    memcpy(dest, src, length);
    dest[length] = '\0';
    return 0;
}

/* Read one CRLF-terminated line without modifying the input. */
static int read_line(const char **cursor, const char *end,
                     const char **line, size_t *line_length)
{
    const char *p = *cursor;
    const char *start = p;

    while (p < end && *p != '\r' && *p != '\n') {
        if ((size_t)(p - start) >= MAX_HEADER_LINE) {
            return -1;
        }
        ++p;
    }

    if (p >= end || *p != '\r' || p + 1 >= end || p[1] != '\n') {
        return -1;
    }

    *line = start;
    *line_length = (size_t)(p - start);
    *cursor = p + 2;

    return 0;
}

static int valid_token(const char *s, size_t length)
{
    if (length == 0) {
        return 0;
    }

    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)s[i];

        if (!isalnum(c) &&
            strchr("!#$%&'*+-.^_`|~", c) == NULL) {
            return 0;
        }
    }

    return 1;
}

static int valid_target(const char *s, size_t length)
{
    if (length == 0) {
        return 0;
    }

    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)s[i];

        if (c <= 0x20 || c == 0x7f) {
            return 0;
        }
    }

    return 1;
}

static int parse_port(const char *text, size_t length, int *port)
{
    unsigned long value = 0;

    if (length == 0) {
        return -1;
    }

    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];

        if (!isdigit(c)) {
            return -1;
        }

        value = value * 10 + (unsigned long)(c - '0');

        if (value > 65535) {
            return -1;
        }
    }

    if (value == 0) {
        return -1;
    }

    *port = (int)value;
    return 0;
}

/*
 * Parse Host header values such as:
 * example.com
 * example.com:8080
 * [2001:db8::1]:8080
 * [2001:db8::1]
 */
static int parse_host_value(const char *value, size_t length,
                            char *host, size_t host_capacity,
                            int *port, int default_port)
{
    const char *host_start = value;
    size_t host_length = 0;
    const char *port_start = NULL;
    size_t port_length = 0;

    while (length > 0 &&
           (value[length - 1] == ' ' || value[length - 1] == '\t')) {
        --length;
    }

    while (length > 0 && (*host_start == ' ' || *host_start == '\t')) {
        ++host_start;
        --length;
    }

    if (length == 0) {
        return -1;
    }

    if (*host_start == '[') {
        const char *close = memchr(host_start, ']', length);

        if (close == NULL || close == host_start + 1) {
            return -1;
        }

        host_start++;
        host_length = (size_t)(close - host_start);

        size_t consumed = (size_t)(close - value) + 1;

        if (consumed < length) {
            if (value[consumed] != ':') {
                return -1;
            }

            port_start = value + consumed + 1;
            port_length = length - consumed - 1;
        }
    } else {
        const char *colon = memchr(host_start, ':', length);

        if (colon != NULL) {
            /* Unbracketed IPv6 literals are ambiguous; reject them. */
            if (memchr(colon + 1, ':',
                       length - (size_t)(colon + 1 - host_start)) != NULL) {
                return -1;
            }

            host_length = (size_t)(colon - host_start);
            port_start = colon + 1;
            port_length = length - host_length - 1;
        } else {
            host_length = length;
        }
    }

    if (host_length == 0 ||
        copy_text(host, host_capacity, host_start, host_length) != 0) {
        return -1;
    }

    for (size_t i = 0; i < host_length; ++i) {
        unsigned char c = (unsigned char)host[i];

        if (isspace(c) || c < 0x21 || c == '/' || c == '\\' ||
            c == '@' || c == '#') {
            return -1;
        }
    }

    *port = default_port;

    if (port_start != NULL) {
        if (parse_port(port_start, port_length, port) != 0) {
            return -1;
        }
    }

    return 0;
}

int http_parse_request(const char *raw,
                       HttpRequest *request,
                       char *error,
                       size_t error_size)
{
    const char *cursor;
    const char *end;
    const char *line;
    size_t line_length;
    size_t method_length;
    size_t target_length;
    size_t version_length;
    int host_seen = 0;
    unsigned int header_count = 0;

    if (raw == NULL || request == NULL) {
        set_error(error, error_size, "Invalid argument");
        return -1;
    }

    memset(request, 0, sizeof(*request));
    set_error(error, error_size, "");

    size_t raw_length = strlen(raw);

    if (raw_length == 0 || raw_length > MAX_HEADER_BYTES) {
        set_error(error, error_size, "Request is empty or too large");
        return -1;
    }

    cursor = raw;
    end = raw + raw_length;

    /* Request line */
    if (read_line(&cursor, end, &line, &line_length) != 0) {
        set_error(error, error_size, "Malformed request line");
        return -1;
    }

    const char *first_space = memchr(line, ' ', line_length);

    if (first_space == NULL) {
        set_error(error, error_size, "Missing method separator");
        return -1;
    }

    method_length = (size_t)(first_space - line);

    const char *target_start = first_space + 1;
    size_t remaining = line_length - method_length - 1;
    const char *second_space = memchr(target_start, ' ', remaining);

    if (second_space == NULL) {
        set_error(error, error_size, "Missing target or HTTP version");
        return -1;
    }

    target_length = (size_t)(second_space - target_start);
    version_length = line_length -
        (size_t)(second_space + 1 - line);

    if (!valid_token(line, method_length) ||
        !valid_target(target_start, target_length) ||
        version_length == 0) {
        set_error(error, error_size, "Invalid request line");
        return -1;
    }

    const char *version_start = second_space + 1;

    if (memchr(version_start, ' ', version_length) != NULL) {
        set_error(error, error_size, "Extra fields in request line");
        return -1;
    }

    if (copy_text(request->method, sizeof(request->method),
                  line, method_length) != 0 ||
        copy_text(request->target, sizeof(request->target),
                  target_start, target_length) != 0 ||
        copy_text(request->version, sizeof(request->version),
                  version_start, version_length) != 0) {
        set_error(error, error_size, "Request line field too long");
        return -1;
    }

    if (strcmp(request->version, "HTTP/1.0") != 0 &&
        strcmp(request->version, "HTTP/1.1") != 0) {
        set_error(error, error_size, "Unsupported HTTP version");
        return -1;
    }

    request->is_connect = (strcmp(request->method, "CONNECT") == 0);
    request->port = request->is_connect ? 443 : 80;

    /* Headers */
    while (cursor < end) {
        if (read_line(&cursor, end, &line, &line_length) != 0) {
            set_error(error, error_size, "Malformed header line");
            return -1;
        }

        /* Empty line terminates the header section. */
        if (line_length == 0) {
            return 0;
        }

        if (++header_count > MAX_HEADERS) {
            set_error(error, error_size, "Too many HTTP headers");
            return -1;
        }

        /* Obsolete folded headers are deliberately rejected. */
        if (line[0] == ' ' || line[0] == '\t') {
            set_error(error, error_size, "Folded headers are unsupported");
            return -1;
        }

        const char *colon = memchr(line, ':', line_length);

        if (colon == NULL) {
            set_error(error, error_size, "Header missing colon");
            return -1;
        }

        size_t name_length = (size_t)(colon - line);

        if (!valid_token(line, name_length)) {
            set_error(error, error_size, "Invalid header name");
            return -1;
        }

        const char *value = colon + 1;
        size_t value_length = line_length - name_length - 1;

        for (size_t i = 0; i < value_length; ++i) {
            unsigned char c = (unsigned char)value[i];

            if ((c < 0x20 && c != '\t') || c == 0x7f) {
                set_error(error, error_size, "Invalid header value");
                return -1;
            }
        }

        if (name_length == 4 &&
            strncasecmp(line, "Host", 4) == 0) {
            if (host_seen) {
                set_error(error, error_size, "Duplicate Host header");
                return -1;
            }

            if (parse_host_value(value, value_length,
                                 request->host,
                                 sizeof(request->host),
                                 &request->port,
                                 request->is_connect ? 443 : 80) != 0) {
                set_error(error, error_size, "Invalid Host header");
                return -1;
            }

            host_seen = 1;
        }
    }

    set_error(error, error_size, "Missing terminating blank line");
    return -1;
}