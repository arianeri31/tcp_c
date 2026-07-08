// Client loop using MSG_ZEROCOPY for client -> server sends
// client -> server -> client

// to see the sent data in the terminal
// strace -e trace=sendto,sendmsg,setsockopt,recvmsg ./client_zc_loop

// ping-pong way of sending and receiving data,
// i will try to implement a more pipeline version after
// but it is easier for now to send and then wait
// for the answer before sending the next thing

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
#include <time.h>
#include <linux/errqueue.h>
#include <poll.h>

#ifndef SO_ZEROCOPY
#define SO_ZEROCOPY 60
#endif

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif

#define PORT 8080
#define SERVER_IP "10.30.3.52"
#define IP_ADDR_LOCALHOST "127.0.0.1"

#define DEFAULT_BUFFER_SIZE 65536
#define DEFAULT_NB_SENDS 10000
#define DEFAULT_POOL_SIZE 16

#define MAX_SEND_IDS_PER_BUFFER 128
#define ZC_DRAIN_INTERVAL 32

#define CHUNK_SIZE 4194304

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

struct config {
    size_t buffer_size;
    int nb_sends;
    int pool_size;
};

static void parse_args(int argc, char **argv, struct config *conf)
{
    int opt;

    while ((opt = getopt(argc, argv, "s:n:p:")) != -1) {
        switch (opt) {
            case 's':
                conf->buffer_size = strtoull(optarg, NULL, 10);
                break;

            case 'n':
                conf->nb_sends = atoi(optarg);
                break;

            case 'p':
                conf->pool_size = atoi(optarg);
                break;

            default:
                fprintf(stderr,
                        "Invalid option. Use: %s -s buffer_size -n nb_sends -p pool_size\n",
                        argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (conf->buffer_size == 0 ||
        conf->nb_sends <= 0 ||
        conf->pool_size <= 0) {
        fprintf(stderr, "Invalid arguments: values must be greater than 0\n");
        exit(EXIT_FAILURE);
    }
}

static long long elapsed_us(struct timespec start, struct timespec end)
{
    return (long long)(end.tv_sec - start.tv_sec) * 1000000LL
         + (end.tv_nsec - start.tv_nsec) / 1000;
}

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

/*
read all the zerocopy notifications that are available in the error queue of the socket

return the number of notifications received
*/
static int read_zc_notif(int sock,
                         struct zc_buffer *pool,
                         int pool_size,
                         int *fallback_count)
{
    int count = 0;

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

                    // printf("ZEROCOPY notif: ee_info=%u, ee_data=%u, ee_code=%u\n",
                    //        first_send_id, last_send_id, serr->ee_code);

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
        // over 8MB, there is a ENOBUFS error that doesn't go away even by reading the notification queue and waiting
        // which means that the kernel dosen't accept to send 8MB in one go, so we need to send data in chunks
        // , so we need to send data in chunks 
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
                //printf("send ENOBUFS: total_sent=%zu / %zu\n",total_sent,buf->size);
                //fflush(stdout);
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

// for zerocopy notification, i can't use recv because i can't read in the errorqueue
// i have to use recvmsg with the MSG_ERRQUEUE flag to read the notifications about the sent messages
// the notifications are sent by the kernel
int main(int argc, char **argv)
{
    int sock;
    struct sockaddr_in serv_addr;
    struct timespec t_start, t_end;

    long long total_elapsed_time_us = 0;

    size_t total_sent = 0;
    size_t total_received = 0;

    int total_notif = 0;
    int fallback_count = 0;

    uint32_t next_zc_id = 0;

    struct config conf = {
        .buffer_size = DEFAULT_BUFFER_SIZE,
        .nb_sends = DEFAULT_NB_SENDS,
        .pool_size = DEFAULT_POOL_SIZE,
    };

    parse_args(argc, argv, &conf);

    long long elapsed_times[conf.nb_sends];
    struct zc_buffer *pool = calloc((size_t)conf.pool_size, sizeof(*pool));

    if (pool == NULL) {
        perror("calloc pool");
        exit(EXIT_FAILURE);
    }

    char *recv_buffer = malloc(conf.buffer_size);

    if (recv_buffer == NULL) {
        perror("malloc recv_buffer");
        free_pool(pool, conf.pool_size);
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < conf.pool_size; i++) {
        pool[i].data = malloc(conf.buffer_size);

        if (pool[i].data == NULL) {
            perror("malloc pool buffer");
            free_pool(pool, conf.pool_size);
            free(recv_buffer);
            exit(EXIT_FAILURE);
        }

        pool[i].size = conf.buffer_size;
        pool[i].in_use = false;
        pool[i].send_nb = 0;
        pool[i].confirmed_nb = 0;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        perror("socket creation error");
        free_pool(pool, conf.pool_size);
        free(recv_buffer);
        exit(EXIT_FAILURE);
    }

    int one = 1;

    if (setsockopt(sock, SOL_SOCKET, SO_ZEROCOPY, &one, sizeof(one)) < 0) {
        perror("setsockopt SO_ZEROCOPY");
        fprintf(stderr, "the system doesn't support SO_ZEROCOPY.\n");

        close(sock);
        free_pool(pool, conf.pool_size);
        free(recv_buffer);
        exit(EXIT_FAILURE);
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0) {
        perror("inet_pton");

        close(sock);
        free_pool(pool, conf.pool_size);
        free(recv_buffer);
        exit(EXIT_FAILURE);
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");

        close(sock);
        free_pool(pool, conf.pool_size);
        free(recv_buffer);
        exit(EXIT_FAILURE);
    }

    // send the struct config to the server so it can allocate the same buffer size, pool size and number of sends
    if (send(sock, &conf, sizeof(conf), 0) < 0) {
        perror("send config");
        close(sock);
        free_pool(pool, conf.pool_size);
        free(recv_buffer);
        exit(EXIT_FAILURE);
    }



    for (int i = 0; i < conf.nb_sends; i++) {
        int buf_index = -1;

        while (buf_index < 0) {
            total_notif += read_zc_notif(sock,
                                         pool,
                                         conf.pool_size,
                                         &fallback_count);

            buf_index = find_free_buffer(pool, conf.pool_size);

            if (buf_index < 0) {
                printf("CLIENT waiting free buffer, busy=%d, notif=%d\n",count_busy_buffers(pool, conf.pool_size),total_notif);
                fflush(stdout);
                usleep(10);
            }
        }

        // Fill the chosen free buffer with fake data.
        // The data changes to see if the pool of buffers works as intended.
        memset(pool[buf_index].data,
               '0' + (i % 10),
               pool[buf_index].size);
        // we get the time before sending the data, and after receiving the data, to measure the time of the send and receive 
        // for each send, and then we can calculate the average time for all sends at the end

        clock_gettime(CLOCK_MONOTONIC, &t_start);

        // printf("CLIENT i=%d before send\n", i);
        // fflush(stdout);

        send_all_zc_from_buffer(sock,&pool[buf_index],pool,conf.pool_size,&next_zc_id,&total_notif,&fallback_count);
        
        
        // printf("CLIENT i=%d after send, before recv\n", i);
        // fflush(stdout);

        recv_all(sock, recv_buffer, conf.buffer_size);

        // printf("CLIENT i=%d after recv\n", i);
        // fflush(stdout);

        clock_gettime(CLOCK_MONOTONIC, &t_end);
        
        total_sent += (size_t)conf.buffer_size;
        total_received += (size_t)conf.buffer_size;


        long long elapsed = elapsed_us(t_start, t_end);
        total_elapsed_time_us += elapsed;
        elapsed_times[i] = elapsed;
        // read notif regularly to avoid overflowing the error queue of the socket,
        // but not after every send
        if (i % ZC_DRAIN_INTERVAL == 0) {
            total_notif += read_zc_notif(sock,
                                         pool,
                                         conf.pool_size,
                                         &fallback_count);
        }
    }

    // calculate the average elapsed time for all sends
    long long average_elapsed_time_us = total_elapsed_time_us / conf.nb_sends;
    // find the min and max elapsed time for all sends
    long long min_elapsed_time_us = elapsed_times[0];
    long long max_elapsed_time_us = elapsed_times[0];
    for (int i = 1; i < conf.nb_sends; i++) {
        if (elapsed_times[i] < min_elapsed_time_us) {
            min_elapsed_time_us = elapsed_times[i];
        }
        if (elapsed_times[i] > max_elapsed_time_us) {
            max_elapsed_time_us = elapsed_times[i];
        }
    }

    // tell the server that the client has finished sending data
    shutdown(sock, SHUT_WR);

    // Wait until all zerocopy sends have been confirmed.
    // Then, all pool buffers can be safely freed.
    while (count_busy_buffers(pool, conf.pool_size) > 0) {
        total_notif += read_zc_notif(sock,
                                     pool,
                                     conf.pool_size,
                                     &fallback_count);
        usleep(10);
    }

    

    printf("buffer_size: %zu\n", conf.buffer_size);
    printf("nb_sends: %d\n", conf.nb_sends);
    printf("pool_size: %d\n", conf.pool_size);
    printf("MAX_SEND_IDS_PER_BUFFER: %d\n", MAX_SEND_IDS_PER_BUFFER);
    printf("Client sent: %zu bytes with MSG_ZEROCOPY\n", total_sent);
    printf("Client received: %zu bytes\n", total_received);
    printf("Elapsed loop time: %lld us\n", total_elapsed_time_us);
    printf("Client zerocopy notifications received: %d\n", total_notif);
    printf("Client fallback copy notifications: %d\n", fallback_count);
    printf("Average elapsed time per send: %lld us\n", average_elapsed_time_us);
    printf("Min elapsed time per send: %lld us\n", min_elapsed_time_us);
    printf("Max elapsed time per send: %lld us\n", max_elapsed_time_us);

    printf("RESULT,zc_loop,%zu,%d,%d,%zu,%zu,%lld,%lld,%lld,%lld,%d,%d\n",
           conf.buffer_size,
           conf.nb_sends,
           conf.pool_size,
           total_sent,
           total_received,
           total_elapsed_time_us,
           average_elapsed_time_us,
           min_elapsed_time_us,
           max_elapsed_time_us,
           total_notif,
           fallback_count);

      printf("RESULT_COMP,zc_loop,%zu,%d,%d,%zu,%zu,%lld,%lld,%d,%d\n",
           conf.buffer_size,
           conf.nb_sends,
           conf.pool_size,
           total_sent,
           total_received,
           total_elapsed_time_us,
           average_elapsed_time_us,
           total_notif,
           fallback_count);

    close(sock);
    free_pool(pool, conf.pool_size);
    free(recv_buffer);

    return 0;
}