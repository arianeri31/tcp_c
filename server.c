#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 8080
#define BUFFER_SIZE 1024

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    char buffer[BUFFER_SIZE] = {0};

    //create socket file descriptor
    if((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    //set socket options
    // SO_REUSEADDR allows the socket to be bound to an address that is already in use
    // SO_REUSEPORT allows multiple sockets on the same host to bind to the same port
    // writing SO_REUSEADDR | SO_REUSEPORT is not clean, so i need to write both cases
    // if i want to set both options, i need to call setsockopt twice, once for each option
    if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt SO_REUSEADDR");
        exit(EXIT_FAILURE);
    }
    if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt SO_REUSEPORT");
        exit(EXIT_FAILURE);
    }

    //setup the server address
    address.sin_family = AF_INET; //IPv4

    //accepting connections on any available network interface
    address.sin_addr.s_addr = INADDR_ANY;

    // set the port number in network byte order
    address.sin_port = htons(PORT);

    // bind the socket to the address and port
    if(bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    // start listening for incoming connections
    if(listen(server_fd, 10) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    printf("Server is listening on port %d\n", PORT);

    // accept incoming connection
    if((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
        perror("accept");
        exit(EXIT_FAILURE);
    }
    
    printf("Connection accepted\n");

    //read data from the client and print it
    ssize_t valread;
    while((valread = read(new_socket, buffer, BUFFER_SIZE)) > 0) {
        printf("Received message: %s\n", buffer);
        memset(buffer, 0, BUFFER_SIZE); // Clear the buffer for the next message
    }


    //close the socket
    close(server_fd);
    return 0;
}