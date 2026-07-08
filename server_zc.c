#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080
#define BUFFER_SIZE (1024 * 1024)

// read is the same thing as recv with the flags parameter set to 0 
// same thing for write and send

int main()
{
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;
    char buffer[BUFFER_SIZE];

    // create socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
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

        // accept incoming connection
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen)) < 0) {
            perror("accept");
            continue;
        }

        printf("Connection accepted\n");

        ssize_t valread;
        unsigned long long total = 0;

        // read data from the client and count received bytes
        while ((valread = read(new_socket, buffer, BUFFER_SIZE)) > 0) {
            total += (unsigned long long)valread;
        }

        if (valread < 0) {
            perror("read");
            close(new_socket);
            continue;
        }

        printf("Total received: %llu bytes\n", total);
        printf("Connection closed, waiting for another client...\n");

        close(new_socket);
    }

    close(server_fd);

    return 0;
}