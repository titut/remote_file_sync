/*
 * Remote file synchronization server for Bill and Darian's SoftSys Project
 * Meant to be run on a raspberry pi or similar device to act as a server
 * for remote file sync clients. AKA Google Docs in VSCode.
 * 
 * To run on the raspi:
 * gcc -pthread server.c comm.c -o server && ./server 9000 <file_path>
 * The <file_path> is where the document you're editing is kept.
 */

// Load path to file into memory
// Listen on port 9000
// Accept clients and spawns threads
// Each thread processes C_GET and C_PUT

#define _GNU_SOURCE
#include "comm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>     // Need to specify int lengths 

#include <errno.h>      // Global erno + Codes 
#include <inttypes.h>   // Avoid undefined behavior when printing uint64_t
#include <limits.h>     // PATH_MAX -> Limits of basic types

#include <unistd.h>     // POSIX calls
#include <fcntl.h>      // File control operations and flags 
#include <pthread.h>    // thread per client POSIX threads and mutexes
#include <sys/socket.h> 
#include <sys/stat.h>   // File status (struct stat, stat(), fstat()) --> Macros like S_ISREG, S_ISDIR
#include <sys/types.h>  // System types 

#include <arpa/inet.h>  // Byte order conversion and address conversion
#include <netinet/in.h> // Internet address structures and constants 

#define BACKLOG 64

// Represents the file being synced, global state for the server
static struct State {
    char path[PATH_MAX];   // On-disk file path
    uint8_t *content;      // Heap buffer
    uint32_t content_len;  // Bytes in content
    uint32_t version;      // Version starting at 0
    pthread_mutex_t mu;    // Guard all fields above, ensure thread safety
} g;

// Replace file at 'path' with 'data' of length n atomically
// Writes to a temp file and renames it into place
static int atomic_write_file(const char *path, const uint8_t *data, size_t len) {
    char tmp[PATH_MAX]; // Temporary file path
    snprintf(tmp, sizeof(tmp), "%s.tmp", path); // e.g., "file.txt.tmp"
    
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0) 
        return -1; 
    
    if (len && write_full(fd, data, len) != 1) {
        close(fd);
        return -1;
    }
    if (close(fd) != 0) 
        return -1;
    
    if (rename(tmp, path) != 0) 
        return -1; 
    return 0;
}   

// Load initial state from disk -> See struct above
// If file does not exist, initialize empty state with version 0
static int load_initial(void) {
    int fd = open(g.path, O_RDONLY | O_CLOEXEC); 
    if (fd < 0) {
        if (errno == ENOENT) { 
            g.content = NULL; 
            g.content_len = 0;
            g.version = 0; 
            return 0;
        }
        return -1; 
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }

    size_t len = st.st_size; // File size in bytes
    uint8_t *buf = NULL;

    if (len) {
        buf = malloc(len); // Allocate buffer
        if (!buf) {
            close(fd);
            return -1;
        }
        if (read_full(fd, buf, len) != 1) { // Read full file
            free(buf);
            close(fd);
            return -1;
        }
    }

    close(fd);
    g.content = buf; 
    g.content_len = len;   
    g.version = 0; 
    return 0; 
}

// Combine client changes with server head based on base_version
static int merge_or_conflict(uint32_t base_version,
                             const uint8_t *client_data, uint32_t client_len,
                             const uint8_t *server_data, uint32_t server_len,
                             uint8_t **out_data, uint32_t *out_len) {
    if (base_version == g.version) {
        // No conflict; accept client changes
        uint8_t *cpy = NULL;
        if (client_len) {
            cpy = malloc(client_len);
            if (!cpy) 
                return -1;
            memcpy(cpy, client_data, client_len);
        }
        *out_data = cpy; // merged data is client's data
        *out_len = client_len;
        return 0;
    }

    // Conflict
    const char *pre = "<-- client\n";
    const char *mid = "========\n";
    const char *post = "--> server\n";

    // Calculate total size needed
    size_t total = strlen(pre) + client_len + 1 + 
                   strlen(mid) + server_len + 1 + 
                   strlen(post);

    uint8_t *buf = malloc(total);
    if (!buf) return -1;

    size_t off = 0;
    memcpy(buf + off, pre, strlen(pre));         off += strlen(pre);
    memcpy(buf + off, client_data, client_len);  off += client_len; buf[off++] = '\n'; 
    memcpy(buf + off, mid, strlen(mid));         off += strlen(mid);
    memcpy(buf + off, server_data, server_len);  off += server_len; buf[off++] = '\n';
    memcpy(buf + off, post, strlen(post));       off += strlen(post);

    *out_data = buf; // merged buffer
    *out_len = (uint32_t)off; // merged length
    return 0;
}

// Handle C_GET: send current state to client 
static int handle_get(int fd) {
    pthread_mutex_lock(&g.mu); // lock to read consistent state

    // uint64_t ver = g.version;
    // uint32_t n = g.content_len;
    // const uint8_t *data = g.content;

    uint8_t *buf = malloc(8 + g.content_len); // 12 + data 
    if (!buf) {
        pthread_mutex_unlock(&g.mu);
        return -1;
    }

    uint32_t be_ver = htonl(g.version);    // version in network order 
    uint32_t be_len = htonl(g.content_len);  // length in network order

    memcpy(buf, &be_ver, 4);        // version 
    memcpy(buf + 4, &be_len, 4);   // length 
    if (g.content_len) memcpy(buf + 8, g.content, g.content_len); // content 

    pthread_mutex_unlock(&g.mu);

    int ok = send_frame(fd, S_STATE, buf, 8 + g.content_len); // send response 
    free(buf);
    return ok;
}

// Handle C_PUT: process client submission 
static int handle_put(int fd, const uint8_t *payload, uint32_t plen) {
    if (plen < 8) 
        return -1; // must at least have version + length 

    uint32_t be_base;
    memcpy(&be_base, payload, 4);
    uint32_t base_version = ntohl(be_base);

    uint32_t be_len;
    memcpy(&be_len, payload + 4, 4);
    uint32_t client_len = ntohl(be_len);

    if (8 + client_len != plen) 
        return -1; // malformed frame

    const uint8_t *client_data = payload + 8;    

    pthread_mutex_lock(&g.mu);
    // uint64_t current_version = g.version; // Current server version
    const uint8_t *server_data = g.content;
    uint32_t server_len = g.content_len;

    uint8_t *merged = NULL;
    uint32_t merged_len = 0;

    // Combine client changes with server head 
    if (merge_or_conflict(base_version, 
                            client_data, client_len, 
                            g.content, g.content_len,
                            &merged, &merged_len) != 0) {
        pthread_mutex_unlock(&g.mu);
        return -1;
    }

    if (atomic_write_file(g.path, merged, merged_len) != 0) {
        free(merged);
        pthread_mutex_unlock(&g.mu);
        return -1;
    }

    // Replace state with merged content and increment version
    free(g.content);
    g.content = merged;
    g.content_len = merged_len;
    g.version++; // Increment verison

    uint32_t new_version = g.version;

    pthread_mutex_unlock(&g.mu);

    // Send S_OK with new version to client 
    uint32_t be_lenew = htonl(new_version);
    return send_frame(fd, S_OK, (uint8_t *)&be_lenew, 8);
}

static void *client_thread(void *arg) {
    int fd = (int)(uintptr_t)arg; // Client socket

    for (;;) {
        uint8_t  type;
        uint8_t *payload = NULL;
        uint32_t plen = 0;

        int r = recv_frame(fd, &type, &payload, &plen); // Read next frame 
        if (r <= 0) { // EOF or error
            free(payload);
            break;
        }

        int ok = 1;
        if (type == C_GET) {
            ok = handle_get(fd); // handle GET
        } else if (type == C_PUT) {
            ok = handle_put(fd, payload, plen); // handle PUT
        } else {
            ok = -1; // Unknown message type
        }

        free(payload); // Free payload buffer
        if (ok != 1) break; // Handle error by closing connection
    }

    close(fd); // Close client socket
    return NULL;
}

// Listener setup 
static int listen_on(uint16_t port) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0); // TCP Socket
    if (fd < 0) return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)); // Reuse addr

// Allow both IPv4 and IPv6
#ifdef IPV6_V6ONLY
    int v6only = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif

    struct sockaddr_in6 addr6;
    memset(&addr6, 0, sizeof(addr6));
    addr6.sin6_family = AF_INET6; // IPv6
    addr6.sin6_addr   = in6addr_any; // Listen on all interfaces
    addr6.sin6_port   = htons(port); // Port

    if (bind(fd, (struct sockaddr *)&addr6, sizeof(addr6)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, BACKLOG) != 0) { // Start listening 
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    if (argc != 3) { 
        fprintf(stderr, "Usage: %s <port> <file_path>\n", argv[0]);
        return 2;
    }

    uint16_t port = atoi(argv[1]); 
    const char *path = argv[2];

    memset(&g, 0, sizeof(g)); 
    pthread_mutex_init(&g.mu, NULL); 

    if (strlen(path) >= sizeof(g.path)) { 
        fprintf(stderr, "File path too long\n");
        return 2;
    }
    strncpy(g.path, path, sizeof(g.path) - 1); 
    g.path[sizeof(g.path) - 1] = 0; 

    if (load_initial() != 0) { 
        perror("load_initial");
        return 1;
    }

    int lfd = listen_on(port); // Create listening socket
    if (lfd < 0) {
        perror("listen");
        return 1;
    }

    printf("Serving %s on port %u (version=%" PRIu32 ")\n",
            g.path, port, g.version);

    for (;;) { 
        int cfd = accept(lfd, NULL, NULL); // Wait for client
        if (cfd < 0) {
            if (errno == EINTR) 
                continue; 
            perror("accept");
            continue;
        }

        pthread_t th;
        // Spawn thread to handle client 
        if (pthread_create(&th, NULL, 
                            client_thread, 
                            (void *)(intptr_t)cfd) == 0) {
            pthread_detach(th);
        } else {
            close(cfd);
        }
    }

    return 0;
}