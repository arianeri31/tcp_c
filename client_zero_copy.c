// to see the sent data in the terminal strace -e trace=sendto,sendmsg,setsockopt ./client_zc (wireshark vibe lol)
// memset is to fill a buffer with a value or a data

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

#ifndef SO_ZEROCOPY
#define SO_ZEROCOPY 60
#endif

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif

#define PORT 8080
#define IP_ADDR_LOCALHOST "127.0.0.1"
#define IP_ADDR_COMPUTER "172.20.10.5"
#define IP_ADDR_SERVER "10.30.3.52"

// changing the buffer size and the number of sends to see more clearly 
// the difference between zerocopy and copy
// the kernel choses to fallback to copy so i need to see 
// if its because of the buffer size or because of something else
//update : it's because i used it on localhost or on computer addr, it works on a external server addr 

//#define BUFFER_SIZE (2 * 1024 * 1024)
//#define NB_SENDS 312
#define DEFAULT_BUFFER_SIZE (256 * 1024)
#define DEFAULT_NB_SENDS 2500
//#define BUFFER_SIZE (64 * 1024)
//#define NB_SENDS 10000


#define POOL_SIZE 32
#define ZC_DRAIN_INTERVAL 4

struct zc_buffer {
    char *data;
    size_t size;
    bool in_use;
    uint32_t send_id; // id for the zerocopy send associated with this buffer
};

struct config {
    size_t buffer_size;
    int nb_sends;
};

static void parse_args(int argc, char **argv, struct config *conf)
{
    int opt;

    while ((opt = getopt(argc, argv, "s:n:")) != -1) {
        switch (opt) {
            case 's':
                conf->buffer_size = strtoull(optarg, NULL, 10);
                break;

            case 'n':
                conf->nb_sends = atoi(optarg);
                break;

            default:
                fprintf(stderr,
                        "Invalid option. Use: %s -s buffer_size -n nb_sends\n",
                        argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (conf->buffer_size == 0 || conf->nb_sends <= 0) {
        fprintf(stderr, "Invalid arguments: values must be greater than 0\n");
        exit(EXIT_FAILURE);
    }
}


static long long elapsed_us(struct timespec start, struct timespec end)
{
    return (long long)(end.tv_sec - start.tv_sec) * 1000000LL
         + (end.tv_nsec - start.tv_nsec) / 1000;
}

static int find_free_buffer(struct zc_buffer *pool)
{
    for (int i = 0; i < POOL_SIZE; i++) {
        if (!pool[i].in_use) {
            return i;
        }
    }
    return -1; // no free buffer found
}

static int count_busy_buffers(struct zc_buffer *pool)
{
    int count = 0;
    for (int i = 0; i < POOL_SIZE; i++) {
        if (pool[i].in_use) {
            count++;
        }
    }
    return count;
}

static void release_buffer_from_range(struct zc_buffer *pool, uint32_t first_send_id,uint32_t last_send_id)
{
    for (int i = 0; i < POOL_SIZE; i++) {
        if (pool[i].in_use && pool[i].send_id >= first_send_id && pool[i].send_id <= last_send_id) {
            pool[i].in_use = false;
        }
    }
}
/*
read all the zerocopy notifications that are available in the error queue of the socket

return the number of notifications received
*/
static int read_zc_notif(int sock, struct zc_buffer *pool, int *fallback_count, struct timespec *send_start_times,long long *elapsed_times)
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
            if (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVERR) {
                struct sock_extended_err *serr =
                    (struct sock_extended_err *)CMSG_DATA(cmsg);

                if (serr->ee_origin == SO_EE_ORIGIN_ZEROCOPY) {
                    uint32_t first_send_id = serr->ee_info;
                    uint32_t last_send_id = serr->ee_data;

                    struct timespec notif_time;

                    clock_gettime(CLOCK_MONOTONIC, &notif_time);

                    for (uint32_t id = first_send_id; id <= last_send_id; id++) {
                        elapsed_times[id] = elapsed_us(send_start_times[id], notif_time);
                    }

                    count++;

                    if (serr->ee_code == SO_EE_CODE_ZEROCOPY_COPIED) {
                        (*fallback_count)++;
                    }
                    release_buffer_from_range(pool, first_send_id, last_send_id);
                }
            }
        }
    }

    return count;
}

static void free_pool(struct zc_buffer *pool)
{
    for (int i = 0; i < POOL_SIZE; i++) {
        free(pool[i].data);
    }
}

// for zerocopy notification, i can't use recv because i can't read in the errorqueue
// i have to use recvmsg with the MSG_ERRQUEUE flag to read the notifications about the sent messages
// the notifications are sent by the kernel
int main(int argc, char** argv)
{
    int sock;
    struct sockaddr_in serv_addr;
    unsigned long long total_sent = 0;
    
    long long total_elapsed_time_us = 0;

    int total_notif = 0;
    int fallback_count = 0;

    uint32_t next_zc_id = 0;

    struct config conf = {
        .buffer_size = DEFAULT_BUFFER_SIZE,
        .nb_sends = DEFAULT_NB_SENDS,
    };

    parse_args(argc, argv, &conf);
    struct zc_buffer pool[POOL_SIZE];

    long long elapsed_times[conf.nb_sends];
    struct timespec send_start_times[conf.nb_sends];

    memset(elapsed_times, 0, sizeof(elapsed_times));
    memset(send_start_times, 0, sizeof(send_start_times));
        

    // Fill the buffer with fake data
    memset(pool, 0, sizeof(pool));

    for (int i = 0; i < POOL_SIZE; i++) {
        pool[i].size = conf.buffer_size;
        pool[i].data = malloc(pool[i].size);
        pool[i].in_use = false;
        pool[i].send_id = 0;

        if (pool[i].data == NULL) {
            perror("malloc");
            for (int j = 0; j < i; j++) {
                free(pool[j].data);
            }
            exit(EXIT_FAILURE);
        }
    }

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket creation error");
        free_pool(pool);
        exit(EXIT_FAILURE);
    }

    int one = 1;

    if (setsockopt(sock, SOL_SOCKET, SO_ZEROCOPY, &one, sizeof(one)) < 0) {
        perror("setsockopt SO_ZEROCOPY");
        fprintf(stderr, "the system doesn't support SO_ZEROCOPY.\n");
        close(sock);
        free_pool(pool);
        exit(EXIT_FAILURE);
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, IP_ADDR_SERVER, &serv_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock);
        free_pool(pool);
        exit(EXIT_FAILURE);
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        close(sock);
        free_pool(pool);
        exit(EXIT_FAILURE);
    }


    

    for (int i = 0; i < conf.nb_sends; i++) {
        int buf_index = -1;

        while (buf_index < 0) {
            total_notif += read_zc_notif(sock, pool, &fallback_count, send_start_times, elapsed_times);
            
            buf_index = find_free_buffer(pool);
            if (buf_index < 0) {
                usleep(10); // we wait a bit before trying again to find a free buffer
            }  
        }

        memset(pool[buf_index].data, '0' + (i % 10), pool[buf_index].size);

        clock_gettime(CLOCK_MONOTONIC, &send_start_times[next_zc_id]);
        ssize_t sent = send(sock, pool[buf_index].data, pool[buf_index].size, MSG_ZEROCOPY);

        //clock_gettime(CLOCK_MONOTONIC, &t_end);
        //long long elapsed = elapsed_us(t_start, t_end);
        //total_elapsed_time_us += elapsed;
        //elapsed_times[i] = elapsed;

        if (sent < 0) {
            perror("send MSG_ZEROCOPY");
            close(sock);
            free_pool(pool);
            exit(EXIT_FAILURE);
        }
        if ((size_t)sent != pool[buf_index].size) {
            fprintf(stderr, "Partial send: only %zd bytes sent instead of %zu\n", sent, pool[buf_index].size);
            close(sock);
            free_pool(pool);
            exit(EXIT_FAILURE);
        }

        pool[buf_index].in_use = true;
        pool[buf_index].send_id = next_zc_id;
        next_zc_id++;

        total_sent += (unsigned long long)sent;

        // read notif regularly to avoid overflowing the error queue of the socket,
        // but not after every send
        if (i % ZC_DRAIN_INTERVAL == 0) {
            total_notif += read_zc_notif(sock, pool, &fallback_count, send_start_times, elapsed_times);
        }
    }

    // tell the server that the client has finished sending data,
    // so the server can stop waiting for more data and break the loop
    shutdown(sock, SHUT_WR);

    while (count_busy_buffers(pool) > 0) {
        total_notif += read_zc_notif(sock, pool, &fallback_count, send_start_times, elapsed_times);
        usleep(10); // wait a bit before checking again for notifications
    }

    // calculate the total, average, min and max elapsed time for all zerocopy notifications
    total_elapsed_time_us = 0;

    long long min_elapsed_time_us = elapsed_times[0];
    long long max_elapsed_time_us = elapsed_times[0];

    for (int i = 0; i < conf.nb_sends; i++) {
        total_elapsed_time_us += elapsed_times[i];

        if (elapsed_times[i] < min_elapsed_time_us) {
            min_elapsed_time_us = elapsed_times[i];
        }

        if (elapsed_times[i] > max_elapsed_time_us) {
            max_elapsed_time_us = elapsed_times[i];
        }
    }

    long long average_elapsed_time_us = total_elapsed_time_us / conf.nb_sends;
    
    
    printf("buffer_size: %zu\n", conf.buffer_size);
    printf("nb_sends: %d\n", conf.nb_sends);
    printf("pool size: %d\n", POOL_SIZE);
    printf("Client sent %llu bytes with MSG_ZEROCOPY\n", total_sent);
    printf("Total send + zc notif time: %lld us\n", total_elapsed_time_us);
    printf("Total zerocopy notifications received: %d\n", total_notif);
    printf("Fallback copy notifications: %d\n", fallback_count);
    printf("Average elapsed time per send + zc notif: %lld us\n", average_elapsed_time_us);
    printf("Min elapsed time per send + zc notif: %lld us\n", min_elapsed_time_us);
    printf("Max elapsed time per send + zc notif: %lld us\n", max_elapsed_time_us);

    printf("RESULT,zc_simple,%zu,%d,%llu,%lld,%lld,%lld,%lld,%d,%d\n",
        conf.buffer_size,
        conf.nb_sends,
        total_sent,
        total_elapsed_time_us,
        average_elapsed_time_us,
        min_elapsed_time_us,
        max_elapsed_time_us,
        total_notif,
        fallback_count);

    close(sock);
    free_pool(pool);

    return 0;
}