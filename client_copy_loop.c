// client_copy_loop.c
// Classic client loop using send()
// client -> server -> client

// ping-pong way of sending and receiving data
// same as the zc loop,
// i will do the pipeline version after

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>

#define PORT 8080
#define SERVER_IP "10.30.3.52"
#define IP_ADDR_LOCALHOST "127.0.0.1"

#define DEFAULT_BUFFER_SIZE 65536
#define DEFAULT_NB_SENDS 10000

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

static void send_all(int sock, const char *buffer, size_t size)
{
    size_t total_sent = 0;

    while (total_sent < size) {
        ssize_t sent = send(sock, buffer + total_sent, size - total_sent, 0);

        if (sent < 0) {
            perror("send");
            exit(EXIT_FAILURE);
        }

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

int main(int argc, char **argv)
{
    int sock;
    struct sockaddr_in serv_addr;
    struct timespec t_start, t_end;

    size_t total_sent = 0;
    size_t total_received = 0;

    long long total_elapsed_time_us = 0;

    struct config conf = {
        .buffer_size = DEFAULT_BUFFER_SIZE,
        .nb_sends = DEFAULT_NB_SENDS,
    };

    parse_args(argc, argv, &conf);

    long long elapsed_times[conf.nb_sends];

    char *send_buffer = malloc(conf.buffer_size);
    char *recv_buffer = malloc(conf.buffer_size);

    if (send_buffer == NULL || recv_buffer == NULL) {
        perror("malloc");
        free(send_buffer);
        free(recv_buffer);
        exit(EXIT_FAILURE);
    }

    // Fill the buffer with fake data
    memset(send_buffer, '0', conf.buffer_size);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket creation error");
        free(send_buffer);
        free(recv_buffer);
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
        exit(EXIT_FAILURE);
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        close(sock);
        free(send_buffer);
        free(recv_buffer);
        exit(EXIT_FAILURE);
    }

    // send the struct config to the server so it can use the same buffer size and number of sends
    send_all(sock, (char *)&conf, sizeof(conf));

    for (int i = 0; i < conf.nb_sends; i++) {
        memset(send_buffer, '0' + (i % 10), conf.buffer_size);

        clock_gettime(CLOCK_MONOTONIC, &t_start);
        send_all(sock, send_buffer, conf.buffer_size);
        recv_all(sock, recv_buffer, conf.buffer_size);
        clock_gettime(CLOCK_MONOTONIC, &t_end);

        total_sent += conf.buffer_size;
        total_received += conf.buffer_size;

        long long elapsed = elapsed_us(t_start, t_end);
        total_elapsed_time_us += elapsed;
        elapsed_times[i] = elapsed;
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

    printf("buffer_size: %zu\n", conf.buffer_size);
    printf("nb_sends: %d\n", conf.nb_sends);
    printf("Client sent: %zu bytes\n", total_sent);
    printf("Client received: %zu bytes\n", total_received);
    printf("Elapsed loop time: %lld us\n", total_elapsed_time_us);
    printf("Average elapsed time per send: %lld us\n", average_elapsed_time_us);
    printf("Min elapsed time per send: %lld us\n", min_elapsed_time_us);
    printf("Max elapsed time per send: %lld us\n", max_elapsed_time_us);

    printf("RESULT,copy_loop,%zu,%d,%d,%zu,%zu,%lld,%lld,%lld,%lld,%d,%d\n",
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

    printf("RESULT_COMP_COPY,copy_loop,%zu,%d,%zu,%zu,%lld,%lld,%lld\n",
           conf.buffer_size,
           conf.nb_sends,
           total_sent,
           total_received,
           total_elapsed_time_us,
           total_elapsed_time_us,
           average_elapsed_time_us);

    printf("RESULT_ALL,copy_loop,%zu,%d,%d,%zu,%zu,%lld,%lld,%lld,%d,%d\n",
           conf.buffer_size,
           conf.nb_sends,
           0,
           total_sent,
           total_received,
           total_elapsed_time_us,
           total_elapsed_time_us,
           average_elapsed_time_us,
           0,
           0);

    close(sock);
    free(send_buffer);
    free(recv_buffer);

    return 0;
}