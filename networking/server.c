#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "connection_handler.h"

#define SERVER_HOST "127.0.0.1"
#define SERVER_PORT 8888
#define BACKLOG 50

static volatile sig_atomic_t server_running = 1;

/*
 * Handles Ctrl+C and allows the server to shut down
 * cleanly.
 */
void handle_signal(int signal_number)
{
    (void)signal_number;
    server_running = 0;
}

int main(void)
{
    int server_fd;
    int option = 1;

    struct sockaddr_in server_addr;

    /*
     * Handle termination signals.
     */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /*
     * Ignore SIGPIPE so that the server does not
     * terminate when a client disconnects unexpectedly.
     */
    signal(SIGPIPE, SIG_IGN);

    /*
     * Create a TCP socket.
     */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    /*
     * Allow reuse of the local address/port.
     */
    if (setsockopt(
            server_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &option,
            sizeof(option)) < 0) {

        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /*
     * Initialize server address structure.
     */
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_HOST);

    /*
     * Bind socket to 127.0.0.1:8888.
     */
    if (bind(
            server_fd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)) < 0) {

        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /*
     * Start listening for incoming connections.
     */
    if (listen(server_fd, BACKLOG) < 0) {

        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("========================================\n");
    printf("      HTTP Proxy Server - Stage 1\n");
    printf("      Listening on %s:%d\n", SERVER_HOST, SERVER_PORT);
    printf("      Press Ctrl+C to stop.\n");
    printf("========================================\n");

    /*
     * Accept clients continuously.
     */
    while (server_running) {

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        /*
         * Allocate memory for the client socket.
         * The pointer is passed to the worker thread.
         */
        int *client_fd = malloc(sizeof(int));

        if (client_fd == NULL) {

            fprintf(stderr,
                    "Memory allocation failed.\n");

            continue;
        }

        /*
         * Accept an incoming client.
         */
        *client_fd = accept(
            server_fd,
            (struct sockaddr *)&client_addr,
            &client_len
        );

        if (*client_fd < 0) {

            free(client_fd);

            if (errno == EINTR) {
                continue;
            }

            perror("accept");
            continue;
        }

        printf(
            "[+] Client connected: %s:%d\n",
            inet_ntoa(client_addr.sin_addr),
            ntohs(client_addr.sin_port)
        );

        pthread_t thread_id;

        /*
         * Create a separate thread for this client.
         */
        if (pthread_create(
                &thread_id,
                NULL,
                handle_client,
                client_fd) != 0) {

            perror("pthread_create");

            close(*client_fd);
            free(client_fd);

            continue;
        }

        /*
         * Detach the thread because the main server
         * does not need to wait for it.
         */
        pthread_detach(thread_id);
    }

    close(server_fd);

    printf("\nServer stopped.\n");

    return EXIT_SUCCESS;
}