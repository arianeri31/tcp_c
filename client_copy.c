// Classic version using send()

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>

#define PORT 8080

#define IP_ADDR_LOCALHOST "127.0.0.1"
#define IP_ADDR_COMPUTER "172.20.10.5"
#define IP_ADDR_SERVER "10.30.3.52"

//#define BUFFER_SIZE 65536
//#define NB_SENDS 10000
#define DEFAULT_BUFFER_SIZE (256 * 1024)
#define DEFAULT_NB_SENDS 2500

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

int main(int argc, char **argv)
{
    int sock;
    struct sockaddr_in serv_addr;
    unsigned long long total_sent = 0;
    struct timespec t_start, t_end;
    long long total_elapsed_time_us = 0;

    struct config conf = {
        .buffer_size = DEFAULT_BUFFER_SIZE,
        .nb_sends = DEFAULT_NB_SENDS,
    };

    parse_args(argc, argv, &conf);

    long long elapsed_times[conf.nb_sends];

    char *buffer = malloc(conf.buffer_size);

    if (buffer == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    // Fill the buffer with fake data
    memset(buffer, '0', conf.buffer_size);

    sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        perror("socket creation error");
        free(buffer);
        exit(EXIT_FAILURE);
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, IP_ADDR_SERVER, &serv_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock);
        free(buffer);
        exit(EXIT_FAILURE);
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        close(sock);
        free(buffer);
        exit(EXIT_FAILURE);
    }


    for (int i = 0; i < conf.nb_sends; i++) {
        clock_gettime(CLOCK_MONOTONIC, &t_start);
        ssize_t sent = send(sock, buffer, conf.buffer_size, 0);

        clock_gettime(CLOCK_MONOTONIC, &t_end);
        long long elapsed = elapsed_us(t_start, t_end);
        total_elapsed_time_us += elapsed;
        elapsed_times[i] = elapsed;

        if (sent < 0) {
            perror("send");
            close(sock);
            free(buffer);
            exit(EXIT_FAILURE);
        }

        if ((size_t)sent != conf.buffer_size) {
            fprintf(stderr,
                    "Partial send: only %zd bytes sent instead of %zu\n",
                    sent,
                    conf.buffer_size);

            close(sock);
            free(buffer);
            exit(EXIT_FAILURE);
        }

        total_sent += (unsigned long long)sent;
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

    // tell the server that the client has finished sending data,
    // so the server can stop waiting for more data and break the loop
    shutdown(sock, SHUT_WR);


    printf("buffer_size: %zu\n", conf.buffer_size);
    printf("nb_sends: %d\n", conf.nb_sends);
    printf("Client sent %llu bytes\n", total_sent);
    printf("Total elapsed time: %lld us\n", total_elapsed_time_us);
    printf("Average elapsed time per send: %lld us\n", average_elapsed_time_us);
    printf("Min elapsed time per send: %lld us\n", min_elapsed_time_us);
    printf("Max elapsed time per send: %lld us\n", max_elapsed_time_us);


    printf("RESULT,copy_simple,%zu,%d,%llu,%lld,%lld,%lld,%lld,0,0\n",
        conf.buffer_size,
        conf.nb_sends,
        total_sent,
        total_elapsed_time_us,
        average_elapsed_time_us,
        min_elapsed_time_us,
        max_elapsed_time_us);

    close(sock);
    free(buffer);

    return 0;
}