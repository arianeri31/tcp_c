// pipeline version of client_copy_loop
// Classic client using send()
// client -> server -> client

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include <poll.h>
#include <fcntl.h>

#define PORT 8080
#define SERVER_IP "10.30.3.52"
#define IP_ADDR_LOCALHOST "127.0.0.1"

#define DEFAULT_BUFFER_SIZE 65536
#define DEFAULT_NB_SENDS 10000

struct config {
    size_t buffer_size;
    int nb_sends;
};

// for measuring the elapsed time of each msg
struct msg_time {
    struct timespec t_start;
    long long elapsed_us;
    bool started;
    bool finished;
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

// set the socket to non-blocking mode
static int set_nonblocking(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
        return -1;
    }

    return 0;
}

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

        ssize_t received = recv(sock,
                                recv_buffer,
                                to_recv,
                                0);

        if (received > 0) 
        {
            *total_received += (size_t)received;

            while (*next_recv_msg < nb_sends && *total_received >= ((size_t)(*next_recv_msg + 1) * buffer_size)) 
            {
                struct timespec t_end;
                clock_gettime(CLOCK_MONOTONIC, &t_end);

                if (msg_times[*next_recv_msg].started && !msg_times[*next_recv_msg].finished) 
                {
                    msg_times[*next_recv_msg].elapsed_us = elapsed_us(msg_times[*next_recv_msg].t_start,t_end);
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

        if (errno == EINTR) {
            continue;
        }

        perror("recv");
        return -1;
    }

    return 1;
}

static int send_available_copy(int sock,
                               char *send_buffer,
                               size_t buffer_size,
                               size_t *current_offset,
                               int *next_send_msg,
                               struct config *conf,
                               size_t *total_sent,
                               struct msg_time *msg_times)
{
    while (*next_send_msg < conf->nb_sends) {

        if (*current_offset == 0 && msg_times[*next_send_msg].started == false) {

            memset(send_buffer,
                   '0' + (*next_send_msg % 10),
                   buffer_size);

            clock_gettime(CLOCK_MONOTONIC,&msg_times[*next_send_msg].t_start);

            msg_times[*next_send_msg].started = true;
        }

        size_t left_to_send = buffer_size - *current_offset;

        ssize_t sent = send(sock,
                            send_buffer + *current_offset,
                            left_to_send,
                            0);

        if (sent < 0) {
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

        *current_offset += (size_t)sent;
        *total_sent += (size_t)sent;

        if (*current_offset >= buffer_size) {
            (*next_send_msg)++;
            *current_offset = 0;
        }
    }

    return 1;
}

int main(int argc, char **argv)
{
    int sock;
    struct sockaddr_in serv_addr;

    size_t total_sent = 0;
    size_t total_received = 0;

    long long total_elapsed_time_us = 0;
    long long average_elapsed_time_us = 0;
    long long min_elapsed_time_us = 0;
    long long max_elapsed_time_us = 0;

    struct config conf = {
        .buffer_size = DEFAULT_BUFFER_SIZE,
        .nb_sends = DEFAULT_NB_SENDS,
    };

    parse_args(argc, argv, &conf);

    size_t expected_received =
        (size_t)conf.buffer_size * (size_t)conf.nb_sends;

    int next_send_msg = 0;
    int next_recv_msg = 0;

    // to see where we are in the message that is being sent
    // if its value is 0 it means we are at the beginning of a message
    size_t current_offset = 0;

    struct msg_time *msg_times =
        calloc((size_t)conf.nb_sends, sizeof(*msg_times));

    if (msg_times == NULL) {
        perror("calloc msg_times");
        exit(EXIT_FAILURE);
    }

    char *send_buffer = malloc(conf.buffer_size);
    char *recv_buffer = malloc(conf.buffer_size);

    if (send_buffer == NULL || recv_buffer == NULL) {
        perror("malloc");
        free(send_buffer);
        free(recv_buffer);
        free(msg_times);
        exit(EXIT_FAILURE);
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        perror("socket creation error");
        free(send_buffer);
        free(recv_buffer);
        free(msg_times);
        exit(EXIT_FAILURE);
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0) {
        perror("inet_pton");

        close(sock);
        free(send_buffer);
        free(recv_buffer);
        free(msg_times);
        exit(EXIT_FAILURE);
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");

        close(sock);
        free(send_buffer);
        free(recv_buffer);
        free(msg_times);
        exit(EXIT_FAILURE);
    }

    // send the struct config to the server
    if (send(sock, &conf, sizeof(conf), 0) < 0) {
        perror("send config");

        close(sock);
        free(send_buffer);
        free(recv_buffer);
        free(msg_times);
        exit(EXIT_FAILURE);
    }

    // switch to non-blocking mode for the pipeline loop
    if (set_nonblocking(sock) < 0) {
        close(sock);
        free(send_buffer);
        free(recv_buffer);
        free(msg_times);
        exit(EXIT_FAILURE);
    }

    while (next_send_msg < conf.nb_sends ||
           total_received < expected_received) {

        struct pollfd pfd;

        pfd.fd = sock;
        pfd.events = POLLERR;
        pfd.revents = 0;

        if (total_received < expected_received) {
            pfd.events |= POLLIN;
        }

        if (next_send_msg < conf.nb_sends) {
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
            fprintf(stderr, "poll returned POLLERR\n");
            exit(EXIT_FAILURE);
        }

        if (pfd.revents & POLLIN) {
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
            if (send_available_copy(sock,
                                    send_buffer,
                                    conf.buffer_size,
                                    &current_offset,
                                    &next_send_msg,
                                    &conf,
                                    &total_sent,
                                    msg_times) < 0) {
                exit(EXIT_FAILURE);
            }
        }

        if ((pfd.revents & POLLHUP) &&
            total_received < expected_received) {
            fprintf(stderr,
                    "Connection closed before all expected data was received\n");
            exit(EXIT_FAILURE);
        }
    }

    // calculate some stats about the elapsed time for each send and receive
    for (int i = 0; i < conf.nb_sends; i++) {
        if (msg_times[i].started && msg_times[i].finished) {
            total_elapsed_time_us += msg_times[i].elapsed_us;

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
    average_elapsed_time_us = total_elapsed_time_us / conf.nb_sends;


    // tell the server that the client has finished sending data
    shutdown(sock, SHUT_WR);

    printf("buffer_size: %zu\n", conf.buffer_size);
    printf("nb_sends: %d\n", conf.nb_sends);
    printf("Client sent: %zu bytes\n", total_sent);
    printf("Client received: %zu bytes\n", total_received);
    printf("Elapsed loop time: %lld us\n", total_elapsed_time_us);
    printf("Average elapsed time per send: %lld us\n", average_elapsed_time_us);
    printf("Min elapsed time per send: %lld us\n", min_elapsed_time_us);
    printf("Max elapsed time per send: %lld us\n", max_elapsed_time_us);

    printf("RESULT,copy_loop_pipeline,%zu,%d,%d,%zu,%zu,%lld,%lld,%lld,%lld,%d,%d\n",
           conf.buffer_size,
           conf.nb_sends,
           0,
           total_sent,
           total_received,
           total_elapsed_time_us,
           average_elapsed_time_us,
           min_elapsed_time_us,
           max_elapsed_time_us,
           0,
           0);

    printf("RESULT_COMP_COPY,copy_loop_pipeline,%zu,%d,%zu,%zu,%lld,%lld\n",
           conf.buffer_size,
           conf.nb_sends,
           total_sent,
           total_received,
           total_elapsed_time_us,
           average_elapsed_time_us);

    close(sock);
    free(send_buffer);
    free(recv_buffer);
    free(msg_times);

    return 0;
}