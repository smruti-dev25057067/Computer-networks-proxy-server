#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "connection_handler.h"

#define BUFFER_SIZE 4096
#define CLIENT_TIMEOUT_SECONDS 10

void *handle_client(void *arg)
{
    int client_fd = *(int *)arg;

    /*
     * The dynamically allocated socket descriptor
     * is no longer needed after copying it.
     */
    free(arg);

    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    /*
     * Set receive timeout.
     */
    struct timeval timeout;

    timeout.tv_sec = CLIENT_TIMEOUT_SECONDS;
    timeout.tv_usec = 0;

    if (setsockopt(
            client_fd,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            sizeof(timeout)) < 0) {

        perror("setsockopt");

        close(client_fd);
        return NULL;
    }

    /*
     * Receive data from the client.
     */
    bytes_received = recv(
        client_fd,
        buffer,
        sizeof(buffer) - 1,
        0
    );

    if (bytes_received < 0) {

        if (errno == EAGAIN || errno == EWOULDBLOCK) {

            printf("[-] Client receive timeout.\n");

        } else {

            perror("recv");
        }

        close(client_fd);
        return NULL;
    }

    if (bytes_received == 0) {

        printf("[-] Client disconnected.\n");

        close(client_fd);
        return NULL;
    }

    /*
     * Convert received bytes into a string.
     */
    buffer[bytes_received] = '\0';

    printf("\n[+] Received request (%zd bytes):\n", bytes_received);

    printf("----------------------------------------\n");

    printf("%s\n", buffer);

    printf("----------------------------------------\n");

    /*
     * Temporary Stage 1 response.
     *
     * In the next stage this will be replaced by
     * real HTTP forwarding.
     */
    const char *response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 39\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Proxy Stage 1: connection successful!\n";

    if (send(
            client_fd,
            response,
            strlen(response),
            0) < 0) {

        perror("send");
    }

    close(client_fd);

    printf("[+] Client connection closed.\n");

    return NULL;
}