// Classic echo server using recv() and send()
// server receives buffer_size bytes, then sends the same data back

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>

#define PORT 8080

#define DEFAULT_POOL_SIZE 16

// read is almost the same thing as recv with the flags parameter set to 0
// same thing for write and send

struct config {
    size_t buffer_size;
    int nb_sends;
};

enum buffer_state {
    FREE, 
    RECEIVING,
    READY_TO_SEND,
    SENDING
};

struct buffer {
    char *data;
    size_t size;
    enum buffer_state state;

};


// set the socket to non-blocking mode so i don't have to use the MSG_DONTWAIT flag on every send and recv 
static int set_nonblocking(int fd)
{
    int flags;


    // get the current flags of the socket
    flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return -1;
    }

    // set the O_NONBLOCK flag on the flags that we got from the socket
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
        return -1;
    }

    return 0;
}

static int count_busy_buffers(struct buffer *pool, int pool_size)
{
    int count = 0;

    for (int i = 0; i < pool_size; i++) {
        if (pool[i].state != FREE) {
            count++;
        }
    }

    return count;
}

static void free_pool(struct buffer *pool, int pool_size)
{
    if (pool == NULL) {
        return;
    }

    for (int i = 0; i < pool_size; i++) {
        free(pool[i].data);
    }

    free(pool);
}

static int recv_available(int sock,
                            struct buffer *pool,
                            int pool_size,
                            size_t *recv_index,
                            size_t *recv_offset,
                            size_t *total_received,
                            size_t expected_received)
{
    while (*total_received < expected_received) {
        struct buffer *buf = &pool[*recv_index];
        
        // the next buffer to receive data is not free, 
        // so we can't receive data in it
        // so we return to the main loop 
        if(buf->state != FREE && buf->state != RECEIVING) {
            return 1;
        }

        if(buf->state == FREE) {
            buf->state = RECEIVING;
            *recv_offset = 0;
        }

        size_t left_to_recv = buf->size - *recv_offset;
        ssize_t received = recv(sock,
                            buf->data + *recv_offset,
                            left_to_recv,
                            0);

        if(received > 0 ){
            *recv_offset += (size_t)received;
            *total_received += (size_t)received;

            if(*recv_offset >= buf->size) {
                buf->state = READY_TO_SEND;
                *recv_index = (*recv_index + 1) % (size_t)pool_size;
                *recv_offset = 0;
            }
            continue;
        }

        if (received == 0) {
            fprintf(stderr, "Connection closed before all expected data was received\n");
            return -1;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 1;
        }

        perror("recv");
        return -1;
    }
    return 1;
}

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


/////////////////////////////////////////////





static int send_available(int sock,
                            struct buffer *pool,
                            int pool_size,
                            size_t *send_index,
                            size_t *send_offset,
                            size_t *total_sent,
                            size_t expected_sent)
{
    while (*total_sent < expected_sent) {
        struct buffer *buf = &pool[*send_index];

        // the next buffer to send data is not ready to send, 
        // so we can't send data from it
        // so we return to the main loop 
        if(buf->state != READY_TO_SEND && buf->state != SENDING) {
            return 1;
        }

        if(buf->state == READY_TO_SEND) {
            buf->state = SENDING;
            *send_offset = 0;
        }

        size_t left_to_send = buf->size - *send_offset;

        ssize_t sent = send(sock,
                            buf->data + *send_offset,
                            left_to_send,
                            0);

        if(sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 1;
            }

            if (errno == EINTR) {
                continue;
            }

            perror("send");
            return -1;
        }

        if (sent == 0) {
            fprintf(stderr, "send returned 0, stopping to avoid infinite loop\n");
            return -1;
        }

        *send_offset += (size_t)sent;
        *total_sent += (size_t)sent;

        if(*send_offset == buf->size){
            // the buffer is fully sent so it can be freed
            buf->state = FREE;
            *send_index = (*send_index + 1)%(size_t)pool_size;
            *send_offset = 0;
        }
    }

    return 1;
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

        size_t total_received = 0;
        size_t total_sent = 0;

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
        int pool_size = DEFAULT_POOL_SIZE;

        if (conf.buffer_size == 0 || conf.nb_sends <= 0) 
        {
            fprintf(stderr, "Invalid config received from client\n");
            close(new_socket);
            continue;
        }

        size_t expected_received =(size_t)conf.buffer_size *(size_t)conf.nb_sends;

        printf("Client config received\n");
        printf("buffer_size: %zu\n", conf.buffer_size);
        printf("nb_sends: %d\n", conf.nb_sends);

        struct buffer *pool = calloc((size_t)pool_size, sizeof(*pool));

        if (pool == NULL) {
            perror("calloc pool");
            close(new_socket);
            continue;
        }

        bool allocation_failed = false;

        for (int i = 0; i < pool_size; i++) {
            pool[i].data = malloc(conf.buffer_size);

            if (pool[i].data == NULL) {
                perror("malloc pool buffer");
                allocation_failed = true;
                break;
            }

            pool[i].size = conf.buffer_size;
            pool[i].state = FREE;
        }

        if (allocation_failed) {
            free_pool(pool, pool_size);
            close(new_socket);
            continue;
        }

        if(set_nonblocking(new_socket)<0){
            free_pool(pool,pool_size);
            close(new_socket);
            continue;
        }

        // to keep track of the buffer in the pool that is currently used to send back the data
        size_t send_index = 0;
        // to keep track of the buffer in the pool that is currently used to receive the data
        size_t recv_index = 0;
        /*
        The idea is to keep the data that were received in the buffers of the pool
        but to keep them in the chronological order (the index will always increase but being modulo the pool size)
        so that we can send them back in the same order as they were received
        */

        size_t recv_offset = 0;
        size_t send_offset =0 ;




        while (total_received < expected_received || count_busy_buffers(pool, pool_size) > 0 
                || total_sent < expected_received) 
            {
            struct pollfd pfd;


            pfd.fd = new_socket;
            pfd.events = 0;
            pfd.revents = 0;
            
            // if all the data has not been received yet and if a buffer is free or is receiving 
            if (total_received < expected_received && (pool[recv_index].state == FREE || pool[recv_index].state == RECEIVING)) 
            {
                //printf("add POLLIN)\n");
                pfd.events |= POLLIN;
            }

            if (total_sent < expected_received && (pool[send_index].state == READY_TO_SEND || pool[send_index].state == SENDING)) 
            {
                //printf("add POLLOUT\n");
                pfd.events |= POLLOUT;
            }
            if (poll(&pfd, 1, -1) < 0) {
                if (errno == EINTR) {
                    continue;
                }

                perror("poll");
                exit(EXIT_FAILURE);
            }

            if (pfd.revents & POLLIN) {
                //printf("In POLLIN\n");
                if (recv_available(new_socket,
                                    pool,
                                    pool_size,
                                    &recv_index,
                                    &recv_offset,
                                    &total_received,
                                    expected_received) < 0) {
                    return -1;
                }
            }

            if (pfd.revents & POLLOUT) {
                //printf("In POLLOUT\n");
                if (send_available(new_socket,
                                    pool,
                                    pool_size,
                                    &send_index,
                                    &send_offset,
                                    &total_sent,
                                    expected_received) < 0) {
                    return -1;
                }
            }

            if ((pfd.revents & POLLHUP) && total_received < expected_received) {
                //printf("In POLLHUP\n");
                fprintf(stderr, "Connection closed before all expected data was received\n");
                return -1;
            }

            if (pfd.revents & POLLERR) {
                fprintf(stderr, "Socket error reported by poll\n");
                return -1;
            }

            if (pfd.revents & POLLNVAL) {
                fprintf(stderr, "Invalid socket reported by poll\n");
                return -1;
            }
        }

        printf("buffer_size: %zu\n", conf.buffer_size);
        printf("nb_sends: %d\n", conf.nb_sends);
        printf("pool_size: %d\n", pool_size);
        printf("Server received: %zu bytes\n", total_received);
        printf("Server sent: %zu bytes\n", total_sent);

        printf("RESULT,server_copy_loop_pipeline,%zu,%d,%zu,%zu\n",
               conf.buffer_size,
               conf.nb_sends,
               total_received,
               total_sent);

        free_pool(pool, pool_size);
        close(new_socket);

        printf("Connection closed, waiting for another client...\n");
    }

    close(server_fd);

    return 0;
}