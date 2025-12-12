#define _GNU_SOURCE
#include "socket_client.h"
#include "file_watcher.h"
#include "rfs_file.h"
#include "comm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>

#define SERVER_HOST "raspberrypi.local" 
#define SERVER_PORT_STR "9000"

static int connect_to_server(void) {
    struct addrinfo hints, *res = NULL, *rp = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; // allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP

    int gai = getaddrinfo(SERVER_HOST, SERVER_PORT_STR, &hints, &res);
    if (gai != 0){
        fprintf(stderr, "[client] getaddrinfo(%s:%s): %s\n", SERVER_HOST, SERVER_PORT_STR, gai_strerror(gai));
        return -1;
    }

    int fd = -1;

    for (rp = res; rp != NULL; rp = rp->ai_next){
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
        perror("[client] socket");
        continue;
    }

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            // Success
            break;
        }

        if (errno == ECONNREFUSED) {
            fprintf(stderr, "[client] connection refused to %s:%s\n",
                SERVER_HOST, SERVER_PORT_STR);
        } else if (errno == ETIMEDOUT) {
            fprintf(stderr, "[client] connection timed out to %s:%s\n",
                SERVER_HOST, SERVER_PORT_STR);
        } else if (errno == EHOSTUNREACH || errno == ENETUNREACH) {
            fprintf(stderr, "[client] host/network unreachable to %s:%s\n",
                SERVER_HOST, SERVER_PORT_STR);
        } else {
            perror("[client] connect");
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd; // -1
}

// Connect to server
// Send C_GET + Receives S_STATE (version + bytes)
static void pull_from_server(struct args* a) {
    int fd = connect_to_server();
    if (fd < 0) return;

    // Send C_GET
    if (send_frame(fd, C_GET, NULL, 0) != 1) {
        close(fd);
        return;
    }

    // Receive S_STATE
    uint8_t  type;
    uint8_t* payload = NULL;
    uint32_t plen = 0;

    int r = recv_frame(fd, &type, &payload, &plen);
    close(fd);

    if (r <= 0 || type != S_STATE || plen < 12) {
        free(payload);
        return;
    }

    // Unpack version and length
    uint32_t be_ver;
    memcpy(&be_ver, payload, 4);
    uint32_t ver = ntohl(be_ver);

    uint32_t be_n;
    memcpy(&be_n, payload + 4, 4);
    uint32_t n = ntohl(be_n);

    if (8 + n != plen) {
        // Malformed frame
        free(payload);
        return;
    }

    const uint8_t* data = payload + 8; // content starts after version + len

    // Compare with local version and if changed, apply
    pthread_mutex_lock(&a->mu);

    if (ver != a->last_version) {
        a->suppress_next = 1;

        if (atomic_write_local(a->file_path, data, n) == 0) {
            a->last_version = ver;
            printf("[client] pulled version %" PRIu32 ", %u bytes\n", ver, n);
        }
    }

    pthread_mutex_unlock(&a->mu);
    free(payload);
}

// Read entire local file into memory
// Send C_PUT -> Receive S_ON(new_version) and update last_version
static void push_to_server(struct args* a) {
    uint8_t* data = NULL;
    uint32_t len = 0;
    // Read the current local file into memory
    if (read_file_into_buf(a->file_path, &data, &len) != 0) {
        free(data);
        return;
    }

    pthread_mutex_lock(&a->mu);
    if (a->suppress_next) {
        // Ignore
        a->suppress_next = 0;
        pthread_mutex_unlock(&a->mu);
        free(data);
        return;
    }

    uint32_t base_ver = a->last_version;
    pthread_mutex_unlock(&a->mu);

    // Open TCP connection
    int fd = connect_to_server();
    if (fd < 0) {
        free(data);
        return;
    }

    uint8_t* buf = malloc(8 + len);
    if (!buf) {
        free(data);
        close(fd); 
        return; 
    }

    uint32_t be_base = htonl(base_ver);
    uint32_t be_n = htonl(len);

    memcpy(buf, &be_base, 4);
    memcpy(buf + 4, &be_n, 4);
    memcpy(buf + 8, data, len);

    // Send frame
    if (send_frame(fd, C_PUT, buf, 8 + len) != 1) {
        free(data);
        free(buf);
        close(fd); 
        return;
    }
    free(buf);

    // Read S_OK or error from server 
    uint8_t type; 
    uint8_t* payload = NULL; 
    uint32_t plen = 0;

    int r = recv_frame(fd, &type, &payload, &plen);
    close(fd);

    if (r <= 0) {
        free(data);
        free(payload);
        return;
    }

    if (type == S_OK && plen == 8) { 
        // S_OK returns new version
        uint32_t be_new;
        memcpy(&be_new, payload, 8);
        uint32_t new_ver = ntohl(be_new);

        pthread_mutex_lock(&a->mu);
        a->last_version = new_ver;
        pthread_mutex_unlock(&a->mu);

        printf("[client] pushed version %" PRIu32 ", %u bytes\n", new_ver, len);
    } else {
        printf("[client] push rejected due to conflict\n");
    }

    free(payload);  
    free(data); 
}

// If file_watcher signaled new_message, call push_to_server
// Always call pull_from_server to pick up remote changes + Sleep briefly 
void* socket_client(void* arg) {
    struct args* a = arg;

    // poll to refresh if nothing received from read
    struct pollfd pfd = { 
        .fd = a->pipefd[0],  
        .events = POLLIN
    };

    // Initial pull to sync local file with local change
    pull_from_server(a);

    while(!*(a->stop_flag_addr)) {  
        int ret = poll(&pfd, 1, 100); // timeout every 500ms so loop can check stop_flag

        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (ret == 0) 
            continue; // timeout -> check stop_flag again

        if (pfd.revents & POLLIN) {
            char buf[100];
            read(a->pipefd[0], buf, sizeof(buf));

            if (buf[0] == 'X') {
                printf("Reader thread exiting...\n");
                break; 
            }

            push_to_server(a);  
            pull_from_server(a); 

            usleep(100000); // 100ms delay
        }
    }

    return NULL;
}