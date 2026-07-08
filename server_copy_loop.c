// Classic echo server using recv() and send()
// server receives buffer_size bytes, then sends the same data back

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080

// read is almost the same thing as recv with the flags parameter set to 0
// same thing for write and send

struct config {
    size_t buffer_size;
    int nb_sends;
};

static void recv_all(int sock, char *buffer, size_t size)
{
    size_t total_received = 0;

    while (total_received < size) {
        ssize_t received = recv(sock,
                                buffer + total_received,
                                size - total_received,
                                0);

        if (received < 0) {
            perror("recv");
            exit(EXIT_FAILURE);
        }

        if (received == 0) {
            fprintf(stderr, "Connection closed before receiving all data\n");
            exit(EXIT_FAILURE);
        }

        total_received += (size_t)received;
    }
}

static void send_all(int sock, const char *buffer, size_t size)
{
    size_t total_sent = 0;

    while (total_sent < size) {
        ssize_t sent = send(sock,
                            buffer + total_sent,
                            size - total_sent,
                            0);

        if (sent < 0) {
            perror("send");
            exit(EXIT_FAILURE);
        }

        total_sent += (size_t)sent;
    }
}

int main(void)
{
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    // create socket file descriptor
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // allow quick reuse of the address after restarting the server
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // setup the server address
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;          // IPv4
    address.sin_addr.s_addr = INADDR_ANY;  // accept connections on any interface
    address.sin_port = htons(PORT);        // port in network byte order

    // bind the socket to the address and port
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // start listening for incoming connections
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server is listening on port %d\n", PORT);

    for (;;) {
        int new_socket;
        socklen_t addrlen = sizeof(address);

        unsigned long long total_received = 0;
        unsigned long long total_sent = 0;

        // accept incoming connection
        new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen);
        if (new_socket < 0) {
            perror("accept");
            continue;
        }

        printf("Connection accepted\n");

        struct config conf;

        // read the struct config from the client to get the buffer size and number of sends
        recv_all(new_socket, (char *)&conf, sizeof(conf));

        if (conf.buffer_size == 0 || conf.nb_sends <= 0) {
            fprintf(stderr, "Invalid config received from client\n");
            close(new_socket);
            continue;
        }

        printf("Client config received\n");
        printf("buffer_size: %zu\n", conf.buffer_size);
        printf("nb_sends: %d\n", conf.nb_sends);

        char *buffer = malloc(conf.buffer_size);

        if (buffer == NULL) {
            perror("malloc");
            close(new_socket);
            continue;
        }

        for (int i = 0; i < conf.nb_sends; i++) {
            recv_all(new_socket, buffer, conf.buffer_size);
            total_received += (unsigned long long)conf.buffer_size;

            send_all(new_socket, buffer, conf.buffer_size);
            total_sent += (unsigned long long)conf.buffer_size;
        }

        printf("buffer_size: %zu\n", conf.buffer_size);
        printf("nb_sends: %d\n", conf.nb_sends);
        printf("Server received: %llu bytes\n", total_received);
        printf("Server sent: %llu bytes\n", total_sent);

        printf("RESULT,server_copy_loop,%zu,%d,%llu,%llu\n",
               conf.buffer_size,
               conf.nb_sends,
               total_received,
               total_sent);

        close(new_socket);
        free(buffer);

        printf("Connection closed, waiting for another client...\n");
    }

    close(server_fd);

    return 0;
}