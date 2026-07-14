// pipeline version of client_zc_loop 
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
#include <fcntl.h>

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
#define DEFAULT_POOL_SIZE 32

#define MAX_SEND_IDS_PER_BUFFER 64
#define ZC_DRAIN_INTERVAL 512

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

// for measuring the elapsed time of each msg 
struct msg_time {
    struct timespec t_start;
    long long elapsed_us;
    bool started; // to know if the send has started or not so we don't start a mesurement for every send if the sends are partials 
    bool finished; // to indicate if the receiving is done
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

// set the socket to non-blocking mode so i don't have to use the MSG_DONTWAIT flag on every send and recv 
// tarpin pratique 
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

static void confirm_send_ids(struct zc_buffer *buf, uint32_t first_send_id, uint32_t last_send_id)
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

// receive the data from the socket
static int recv_available(int sock,
                          char *recv_buffer,
                          size_t buffer_size,
                          size_t *total_received,
                          size_t expected_received,
                          int *next_recv_msg,
                          int nb_sends,
                          struct msg_time *msg_times)
{
    while (*total_received < expected_received) {
        size_t to_recv = buffer_size;
        size_t remaining = expected_received - *total_received;

        if (remaining < to_recv) {
            to_recv = remaining;
        }

        // the socket is already set to non-blocking mode by fcntl so no need to set the MSG_DONTWAIT flag here
        ssize_t received = recv(sock,
                                recv_buffer,
                                to_recv,0);

    if (received > 0) {
        *total_received += (size_t)received;

        while (*next_recv_msg < (size_t)nb_sends &&
            *total_received >= (*next_recv_msg + 1) * buffer_size) {

            struct timespec t_end;
            clock_gettime(CLOCK_MONOTONIC, &t_end);

            if (msg_times[*next_recv_msg].started && !msg_times[*next_recv_msg].finished) {

                msg_times[*next_recv_msg].elapsed_us = elapsed_us(msg_times[*next_recv_msg].t_start, t_end);
                msg_times[*next_recv_msg].finished = true;
            }
            (*next_recv_msg)++;
        }

        continue;
    }

        if (received == 0) {
            fprintf(stderr, "Connection closed before receiving all data\n");
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

static int send_available_zc(int sock,
                             struct zc_buffer *pool,
                             int pool_size,
                             int *current_buf_index,
                             size_t *current_offset,
                             int *next_send_msg,
                             struct config *conf,
                             uint32_t *next_zc_id,
                             size_t *total_sent,
                             int *total_notif,
                             int *fallback_count,
                             struct msg_time *msg_times)
{
    while (*current_buf_index >= 0 || *next_send_msg < conf->nb_sends) {

        if (*current_buf_index < 0) {
            int buf_index = find_free_buffer(pool, pool_size);

            if (buf_index < 0) {
                return 1;
            }

            *current_buf_index = buf_index;
            *current_offset = 0;

            reset_buffer_tracking(&pool[buf_index]);

            // Fill the chosen free buffer with fake data.
            memset(pool[buf_index].data,
                   '0' + (*next_send_msg % 10),
                   pool[buf_index].size);
        }

        struct zc_buffer *buf = &pool[*current_buf_index];

        //while (*current_offset < conf->buffer_size) {
        size_t left_to_send = conf->buffer_size - *current_offset;

        if (left_to_send > CHUNK_SIZE) {
            left_to_send = CHUNK_SIZE;
        }

        if(msg_times[*next_send_msg].started == false) {
            clock_gettime(CLOCK_MONOTONIC, &msg_times[*next_send_msg].t_start);
            msg_times[*next_send_msg].started = true;
        }

        ssize_t sent = send(sock,
                            buf->data + *current_offset,
                            left_to_send,
                            MSG_ZEROCOPY);
        // to see what is sent each send 
        //printf("return of the send: %zd, errno=%s\n", sent, strerror(errno));

        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 1;
            }

            if (errno == ENOBUFS) {

                *total_notif += read_zc_notif(sock,pool,pool_size,fallback_count);

                usleep(1000);
                return 1;
            }

            perror("send MSG_ZEROCOPY");
            return -1;
            }

        if (sent == 0) {
            fprintf(stderr, "send returned 0, stopping to avoid infinite loop\n");
            return -1;
        }

        if (buf->send_nb >= MAX_SEND_IDS_PER_BUFFER) {
            fprintf(stderr,
                    "Too many partial sends for one buffer. "
                    "Increase MAX_SEND_IDS_PER_BUFFER.\n");
            return -1;
        }

        buf->send_ids[buf->send_nb] = *next_zc_id;
        buf->send_nb++;

        (*next_zc_id)++;

        *current_offset += (size_t)sent;
        *total_sent += (size_t)sent;
        //}

        if (*current_offset >= conf->buffer_size) {
            (*next_send_msg)++;
            *current_buf_index = -1; // when the message is sent, no buffer is used to send so it goes back to -1
            *current_offset = 0;
        }
    }

    return 1;
}

// for zerocopy notification, i can't use recv because i can't read in the errorqueue
// i have to use recvmsg with the MSG_ERRQUEUE flag to read the notifications about the sent messages
// the notifications are sent by the kernel
int main(int argc, char **argv)
{
    int sock;
    struct sockaddr_in serv_addr;
    struct timespec t_start_tot, t_end_tot;
    
    long long total_elapsed_time_us = 0;
    long long average_msg_time_us = 0;
    long long min_elapsed_time_us = 0;
    long long max_elapsed_time_us = 0;
    long long total_msg_time_us = 0;

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

    // the expected total number of bytes to be received back from the server
    size_t expected_received =(size_t)conf.buffer_size *(size_t)conf.nb_sends;
    
    // next_msg is the index of the message that is being sent 
    // as it is a pipeline version, there's no loop for sending then receiving
    // so we need to keep track of what is being sent 
    int next_send_msg = 0;

    int next_recv_msg = 0; // to track the message that is being received to calculate the elapsed time for each message

    // current_buf_index is the index of the buffer in the pool that is currently used to send data
    // same as buf_index in the pingpong version but as it as non blocking version, the index can not be declared in the loop 
    // as we would lose its value 
    int current_buf_index = -1;

    // current_offset is the offset in the buffer that is currently being sent
    // important because we need to know when the buffer is completely sent 
    size_t current_offset = 0;

    struct msg_time *msg_times = calloc((size_t)conf.nb_sends, sizeof(*msg_times));

    if (msg_times == NULL) {
        perror("calloc msg_times");
        exit(EXIT_FAILURE);
    }

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

    // put the socket in non-blocking mode to avoid blocking on send and recv
    // easier than using MSG_DONTWAIT on every send and recv
    if (set_nonblocking(sock) < 0) {
        close(sock);
        free_pool(pool, conf.pool_size);
        free(recv_buffer);
        exit(EXIT_FAILURE);
    }



    clock_gettime(CLOCK_MONOTONIC, &t_start_tot);

    while (next_send_msg < conf.nb_sends || current_buf_index >= 0 || total_received < expected_received || 
            count_busy_buffers(pool, conf.pool_size) > 0) 
    {
        struct pollfd pfd;
        int busy_buffers;

        //printf("CLIENT next_send_msg=%d, current_buf_index=%d, total_received=%zu, expected_received=%zu, busy_buffers=%d\n",
        //      next_send_msg, current_buf_index, total_received, expected_received, count_busy_buffers(pool, conf.pool_size));

        total_notif += read_zc_notif(sock,
                                    pool,
                                    conf.pool_size,
                                    &fallback_count);

        busy_buffers = count_busy_buffers(pool, conf.pool_size);

        // we always need to read the notifications 
        // but we don't have a POLLIN or POLLOUT every time 
        // at the end for example, we need to read the notifications but there is nothing left to send or receive
        // so we add the POLLIN and POLLOUT events only if we have something to read or write
        pfd.fd = sock;
        pfd.events = POLLERR;
        pfd.revents = 0;

        if (total_received < expected_received) 
        {
            //printf("add POLLIN)\n");
            pfd.events |= POLLIN;
        }

        if (current_buf_index >= 0 || (next_send_msg < conf.nb_sends && busy_buffers < conf.pool_size)) 
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

        if (pfd.revents & POLLERR) {
            //printf("In POLLERR\n");
            total_notif += read_zc_notif(sock,
                                        pool,
                                        conf.pool_size,
                                        &fallback_count);
        }

        if (pfd.revents & POLLIN) {
            //printf("In POLLIN\n");
            if (recv_available(sock,
                            recv_buffer,
                            conf.buffer_size,
                            &total_received,
                            expected_received,
                            &next_recv_msg,
                            conf.nb_sends,
                            msg_times) < 0) {
                exit(EXIT_FAILURE);
            }
        }

        if (pfd.revents & POLLOUT) {
            //printf("In POLLOUT\n");
            if (send_available_zc(sock,
                                pool,
                                conf.pool_size,
                                &current_buf_index,
                                &current_offset,
                                &next_send_msg,
                                &conf,
                                &next_zc_id,
                                &total_sent,
                                &total_notif,
                                &fallback_count,
                                msg_times) < 0) {
                exit(EXIT_FAILURE);
            }
        }

        if ((pfd.revents & POLLHUP) &&
            total_received < expected_received) {
                //printf("In POLLHUP\n");
                fprintf(stderr, "Connection closed before all expected data was received\n");
                exit(EXIT_FAILURE);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end_tot);



    // calculate some stats about the elapsed time for each send and receive
    for (int i = 0; i < conf.nb_sends; i++) {
        if (msg_times[i].started && msg_times[i].finished) {
            total_msg_time_us += msg_times[i].elapsed_us;

            if (i == 0 || msg_times[i].elapsed_us < min_elapsed_time_us) {
                min_elapsed_time_us = msg_times[i].elapsed_us;
            }

            if (i == 0 || msg_times[i].elapsed_us > max_elapsed_time_us) {
                max_elapsed_time_us = msg_times[i].elapsed_us;
            }
        } 
        else {
            fprintf(stderr, "Message %d did not complete properly\n", i);
        }
    }
    total_elapsed_time_us = elapsed_us(t_start_tot, t_end_tot);
    average_msg_time_us = total_msg_time_us / conf.nb_sends;

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
    printf("Total time for all msgs: %lld us\n", total_msg_time_us);
    printf("Client zerocopy notifications received: %d\n", total_notif);
    printf("Client fallback copy notifications: %d\n", fallback_count);
    printf("Average elapsed time per send: %lld us\n", average_msg_time_us);
    printf("Minimum elapsed time per send: %lld us\n", min_elapsed_time_us);
    printf("Maximum elapsed time per send: %lld us\n", max_elapsed_time_us);


    printf("RESULT,zc_loop_pipeline,%zu,%d,%d,%zu,%zu,%lld,%lld,%lld,%lld,%d,%d\n",
           conf.buffer_size,
           conf.nb_sends,
           conf.pool_size,
           total_sent,
           total_received,
           total_elapsed_time_us,
           average_msg_time_us,
           min_elapsed_time_us,
           max_elapsed_time_us,
           total_notif,
           fallback_count);

    printf("RESULT_COMP,zc_loop_pipeline,%zu,%d,%d,%zu,%zu,%lld,%lld,%lld,%d,%d\n",
           conf.buffer_size,
           conf.nb_sends,
           conf.pool_size,
           total_sent,
           total_received,
           total_elapsed_time_us,
           total_msg_time_us,
           average_msg_time_us,
           total_notif,
           fallback_count);

    close(sock);
    free_pool(pool, conf.pool_size);
    free(recv_buffer);
    free(msg_times);

    return 0;
}