// Echo server using MSG_ZEROCOPY for server -> client replies
// the server is going to be open until the user closes it manually 
// so that we can run multiple clients sequentially without restarting the server
// 
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <linux/errqueue.h>
#include <poll.h>

#ifndef SO_ZEROCOPY
#define SO_ZEROCOPY 60
#endif

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif

#define PORT 8080
#define MAX_SEND_IDS_PER_BUFFER 128
#define ZC_DRAIN_INTERVAL 16

#define CHUNK_SIZE 4194304

struct config {
    size_t buffer_size;
    int nb_sends;
    int pool_size;
};

struct zc_buffer {
    char *data;
    size_t size;
    bool in_use;

    // One buffer can be associated with multiple send_ids in case of partial sends.
    uint32_t send_ids[MAX_SEND_IDS_PER_BUFFER];

    // Number of successful send() calls for this buffer.
    int send_nb;

    // Number of send_ids confirmed as completed by notifications from the kernel.
    int confirmed_nb;
};

static int find_free_buffer(struct zc_buffer *pool, int pool_size)
{
    for (int i = 0; i < pool_size; i++) {
        if (!pool[i].in_use) {
            return i;
        }
    }

    return -1;
}

static int count_busy_buffers(struct zc_buffer *pool, int pool_size)
{
    int count = 0;

    for (int i = 0; i < pool_size; i++) {
        if (pool[i].in_use) {
            count++;
        }
    }

    return count;
}

static void reset_buffer_tracking(struct zc_buffer *buf)
{
    buf->in_use = true;
    buf->send_nb = 0;
    buf->confirmed_nb = 0;
}

static void free_pool(struct zc_buffer *pool, int pool_size)
{
    if (pool == NULL) {
        return;
    }

    for (int i = 0; i < pool_size; i++) {
        free(pool[i].data);
    }

    free(pool);
}

static void confirm_send_ids(struct zc_buffer *buf,
                             uint32_t first_send_id,
                             uint32_t last_send_id)
{
    for (int j = 0; j < buf->send_nb; j++) {
        uint32_t id = buf->send_ids[j];

        if (id >= first_send_id && id <= last_send_id) {
            buf->confirmed_nb++;
        }
    }

    if (buf->send_nb > 0 && buf->confirmed_nb == buf->send_nb) {
        buf->in_use = false;
    }
}

static void release_buffer_from_range(struct zc_buffer *pool,
                                      int pool_size,
                                      uint32_t first_send_id,
                                      uint32_t last_send_id)
{
    for (int i = 0; i < pool_size; i++) {
        if (pool[i].in_use) {
            confirm_send_ids(&pool[i], first_send_id, last_send_id);
        }
    }
}

//read all the zerocopy notifications that are available in the error queue of the socket
//return the number of notifications received
static int read_zc_notif(int sock,
                         struct zc_buffer *pool,
                         int pool_size,
                         int *fallback_count)
{
    int count = 0;

    // for (;;) équivaut à while (1) ou while (true) en C, c'est une boucle infinie
    for (;;) {
        char data_idc = 0;
        char control_buf[512] = {0}; // for control datas

        struct iovec iov = {
            .iov_base = &data_idc,
            .iov_len = sizeof(data_idc),
        };

        struct msghdr msg = {
            .msg_iov = &iov,
            .msg_iovlen = 1,
            .msg_control = control_buf,
            .msg_controllen = sizeof(control_buf),
        };

        ssize_t ret = recvmsg(sock, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);

        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            perror("recvmsg MSG_ERRQUEUE");
            exit(EXIT_FAILURE);
        }

        // msg_control can contain multiple control messages,
        // i need to iterate over them to find the one with the ZEROCOPY notification
        for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
             cmsg != NULL;
             cmsg = CMSG_NXTHDR(&msg, cmsg)) {

            // zerocopy notifications are received as IP extended errors
            // IP extended errors are not a real error but a control message
            if (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVERR) {
                struct sock_extended_err *serr =
                    (struct sock_extended_err *)CMSG_DATA(cmsg);

                if (serr->ee_origin == SO_EE_ORIGIN_ZEROCOPY) {
                    uint32_t first_send_id = serr->ee_info;
                    uint32_t last_send_id = serr->ee_data;

                    count++;

                    if (serr->ee_code == SO_EE_CODE_ZEROCOPY_COPIED ||
                        serr->ee_code == 1) {
                        (*fallback_count)++;
                    }

                    release_buffer_from_range(pool,
                                              pool_size,
                                              first_send_id,
                                              last_send_id);
                }
            }
        }
    }

    return count;
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

        //printf("recv_all progress: %zu / %zu\n", total_received, size);
        fflush(stdout);
    }
}

static void send_all_zc_from_buffer(int sock,
                                    struct zc_buffer *buf,
                                    struct zc_buffer *pool,
                                    int pool_size,
                                    uint32_t *next_zc_id,
                                    int *total_notif,
                                    int *fallback_count)
{
    size_t total_sent = 0;

    reset_buffer_tracking(buf);

    while (total_sent < buf->size) {
        size_t left_to_send = buf->size - total_sent;
        if (left_to_send > CHUNK_SIZE) {
            left_to_send = CHUNK_SIZE;
        }
        ssize_t sent = send(sock,
                            buf->data + total_sent,
                            left_to_send,
                            MSG_ZEROCOPY);

        if (sent < 0) {
            // if (errno == EAGAIN || errno == EWOULDBLOCK) {
            //     struct pollfd pfd;

            //     *total_notif += read_zc_notif(sock,
            //                                 pool,
            //                                 pool_size,
            //                                 fallback_count);

            //     pfd.fd = sock;
            //     pfd.events = POLLOUT | POLLERR;
            //     pfd.revents = 0;

            //     if (poll(&pfd, 1, -1) < 0) {
            //         perror("poll");
            //         exit(EXIT_FAILURE);
            //     }

            //     if (pfd.revents & POLLERR) {
            //         *total_notif += read_zc_notif(sock,
            //                                     pool,
            //                                     pool_size,
            //                                     fallback_count);
            //     }

            //     continue;
            // }

            if (errno == ENOBUFS) {
                *total_notif += read_zc_notif(sock,
                                              pool,
                                              pool_size,
                                              fallback_count);
                usleep(1000);
                continue;
            }

            perror("send MSG_ZEROCOPY");
            exit(EXIT_FAILURE);
        }

        if (sent == 0) {
            fprintf(stderr, "send returned 0, stopping to avoid infinite loop\n");
            exit(EXIT_FAILURE);
        }

        if (buf->send_nb >= MAX_SEND_IDS_PER_BUFFER) {
            fprintf(stderr,
                    "Too many partial sends for one buffer. "
                    "Increase MAX_SEND_IDS_PER_BUFFER.\n");
            exit(EXIT_FAILURE);
        }

        /*
         * Linux assigns an increasing zerocopy id to each successful
         * send(..., MSG_ZEROCOPY).
         *
         * This id is not returned directly by send().
         * So we need to keep track of the ids manually, with the same order:
         * first successful send -> id 0, second -> id 1, etc.
         *
         * Later, zerocopy notifications use these ids through:
         * serr->ee_info = first completed id
         * serr->ee_data = last completed id
         */
        buf->send_ids[buf->send_nb] = *next_zc_id;
        buf->send_nb++;

        (*next_zc_id)++;

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

    // accept incoming connection 
    // this server is going to be open until the user closes it manually
    for (;;) {
        int new_socket;
        socklen_t addrlen = sizeof(address);

        unsigned long long total_received = 0;
        unsigned long long total_sent = 0;

        int total_notif = 0;
        int fallback_count = 0;

        uint32_t next_zc_id = 0;

        new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen);
        if (new_socket < 0) {
            perror("accept");
            continue;
        }

        printf("Connection accepted\n");

        int one = 1;
        if (setsockopt(new_socket, SOL_SOCKET, SO_ZEROCOPY, &one, sizeof(one)) < 0) {
            perror("setsockopt SO_ZEROCOPY");
            close(new_socket);
            continue;
        }

        // read the struct config from the client to get the buffer size and number of sends
        struct config conf;

        recv_all(new_socket, (char *)&conf, sizeof(conf));

        if (conf.buffer_size == 0 ||
            conf.nb_sends <= 0 ||
            conf.pool_size <= 0) {
            fprintf(stderr, "Invalid config received from client\n");
            close(new_socket);
            continue;
        }

        printf("Client config received\n");
        printf("buffer_size: %zu\n", conf.buffer_size);
        printf("nb_sends: %d\n", conf.nb_sends);
        printf("pool_size: %d\n", conf.pool_size);

        // allocate the pool of buffers for zerocopy with calloc to initialize the memory to zero
        // malloc would work if we initialize the memory to zero manually after 
        // but it could leads to more errors (like in the free(pool)) if the allocation doesn't work and we try to 
        // initialize the memory to zero after, so calloc is safer
        struct zc_buffer *pool = calloc((size_t)conf.pool_size, sizeof(*pool));

        if (pool == NULL) {
            perror("calloc pool");
            close(new_socket);
            continue;
        }

        for (int i = 0; i < conf.pool_size; i++) {
            pool[i].data = malloc(conf.buffer_size);

            if (pool[i].data == NULL) {
                perror("malloc pool buffer");
                free_pool(pool, conf.pool_size);
                close(new_socket);
                continue;
            }

            pool[i].size = conf.buffer_size;
            pool[i].in_use = false;
            pool[i].send_nb = 0;
            pool[i].confirmed_nb = 0;
        }

        for (int i = 0; i < conf.nb_sends; i++) {
            int buf_index = -1;

            while (buf_index < 0) {
                total_notif += read_zc_notif(new_socket,
                                             pool,
                                             conf.pool_size,
                                             &fallback_count);

                buf_index = find_free_buffer(pool, conf.pool_size);

                if (buf_index < 0) {
                    usleep(10);
                }
            }


            // printf("SERVER before recv_all\n");
            // fflush(stdout);            

            recv_all(new_socket, pool[buf_index].data, conf.buffer_size);
            total_received += conf.buffer_size;

            // printf("SERVER after recv_all, before send echo\n");
            // fflush(stdout);

            send_all_zc_from_buffer(new_socket,
                                    &pool[buf_index],
                                    pool,
                                    conf.pool_size,
                                    &next_zc_id,
                                    &total_notif,
                                    &fallback_count);

                        
            // printf("SERVER after send echo\n");
            // fflush(stdout);

            total_sent += conf.buffer_size;

            // read notif regularly to avoid overflowing the error queue of the socket,
            // but not after every send
            if (i % ZC_DRAIN_INTERVAL == 0) {
                total_notif += read_zc_notif(new_socket,
                                             pool,
                                             conf.pool_size,
                                             &fallback_count);
            }
        }

        // Wait until all zerocopy replies have been confirmed.
        // Then, all pool buffers can be safely freed.
        while (count_busy_buffers(pool, conf.pool_size) > 0) {
            total_notif += read_zc_notif(new_socket,
                                         pool,
                                         conf.pool_size,
                                         &fallback_count);
            usleep(10);
        }

        printf("buffer_size: %zu\n", conf.buffer_size);
        printf("nb_sends: %d\n", conf.nb_sends);
        printf("pool_size: %d\n", conf.pool_size);
        printf("MAX_SEND_IDS_PER_BUFFER: %d\n", MAX_SEND_IDS_PER_BUFFER);
        printf("Server received: %llu bytes\n", total_received);
        printf("Server sent: %llu bytes with MSG_ZEROCOPY\n", total_sent);
        printf("Server zerocopy notifications received: %d\n", total_notif);
        printf("Server fallback copy notifications: %d\n", fallback_count);

        printf("RESULT,server_zc_loop,%zu,%d,%d,%llu,%llu,%d,%d\n",
               conf.buffer_size,
               conf.nb_sends,
               conf.pool_size,
               total_received,
               total_sent,
               total_notif,
               fallback_count);

        free_pool(pool, conf.pool_size);

        // close the socket but not the server socket, so that the server can accept new connections from other clients
        close(new_socket);

        printf("Connection closed, waiting for another client...\n");
    }

    close(server_fd);

    return 0;
}