#define _GNU_SOURCE
#include <arpa/inet.h> 
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BACKLOG 64
// Cap for incoming frames to avoid unbounded allocation.
#define MAX_MSG (8u * 1024u * 1024u) // 8 MB

/* --<>-- Message Types --<>-- */
enum MsgType {
    C_GET = 0x01,       // Poll current state.
    C_PUT = 0x02,       // Submit new state based on base_version. 
    S_STATE = 0x11,     // Current version and bytes. 
    S_OK = 0x12,        // PUT accepted; new version included. 
};

/* --<>-- Byte-order helpers --<>-- */
// 64-bit host/network conversion stuff 
    // u64_hton: host to network
    // u64_ntoh: network to host
static uint64_t u64_hton(uint64_t x) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    // Split into 2 32-bit halves and swap
    return ((uint64_t)htonl((uint32_t)(x >> 32))) |
           (((uint64_t)htonl((uint32_t)(x & 0xffffffff))) << 32);
#else
    return x;
#endif
}
static uint64_t u64_ntoh(uint64_t x) {return u64_hton(x);}


/* --<>-- IO primitives --<>-- */
// read_full/write_full implement exactly n bytes of reading/writing
    // Necessary because read()/write() may do partial transfers
    // We loop until done or error
// Read exactly n bytes 
// return 1 on success, 0 on EOF, -1 on error.
static int read_full(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf; // Byte pointer arithmetic
    size_t got = 0; // Bytes read so far
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return 0; // EOF before n bytes read
        if (r < 0) {
            if (errno == EINTR) continue; // try again
            return -1; // error
        }
        got += (size_t)r;
    }
    return 1; // success
}

// Write exactly n bytes; return 1 on success, -1 on error
static int write_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf; // Byte pointer arithmetic
    size_t sent = 0; // Bytes written so far 
    while (sent < n) {
        ssize_t w = write(fd, p + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR) continue; // try again
            return -1; // error
        }
        sent += (size_t)w;
    }
    return 1; // success
}


/* --<>-- Frame encode/decode --<>-- */
// Send: [uint32_be length][uint8 type][payload…], length = 1 + payload.
static int send_frame(int fd, uint8_t type, const uint8_t *payload, uint32_t plen) {
    uint32_t len = 1u + plen; // type + payload
    uint32_t be_len = htonl(len); // length 
    uint8_t hdr[5]; // 4 + 1 bytes header
    memcpy(hdr, &be_len, 4); // copy length 

    hdr[4] = type; // 5th byte: message type
    if (write_full(fd, hdr, 5) != 1) return -1; // write header
    if (plen && write_full(fd, payload, plen) != 1) return -1; // write payload
    return 1;
}

// Receive a full frame. Caller frees *payload_out if plen_out > 0
// On success, returns 1; on EOF, returns 0; on error, returns -1
    // *typeout: message type
    // *payload_out: pointer to payload buffer
    // *plen_out: length of payload
    // Caller is responsible for freeing *payload_out if *plen_out > 0
static int recv_frame(int fd, uint8_t *type_out, uint8_t **payload_out, uint32_t *plen_out) {
    uint32_t be_len;
    int r = read_full(fd, &be_len, 4); // Read 4-byte length
    if (r <= 0) return r; // EOF = 0 or error = 1
    uint32_t len = ntohl(be_len); // Convert length to host order
    if (len == 0 || len > MAX_MSG) return -1; // Sanity check length

    uint8_t type;
    r = read_full(fd, &type, 1); // Read 1-byte type
    if (r <= 0) return r;

    uint32_t plen = len - 1; // Payload length
    uint8_t *buf = NULL;
    if (plen) {
        buf = (uint8_t *)malloc(plen); // Allocate payload buffer
        if (!buf) return -1;
        r = read_full(fd, buf, plen); // Read payload
        if (r <= 0) {
            free(buf);
            return r;
        }
    }

    *type_out = type; // Return type
    *payload_out = buf; // Return payload buffer
    *plen_out = plen; // Return payload length
    return 1;
}

/* --<>-- Atomic file write --<>-- */
// Replace file at 'path' with 'data' of length n atomically
    // Writes to a temp file and renames it into place
static int atomic_write_file(const char *path, const uint8_t *data, size_t n) {
    char tmp[PATH_MAX]; // Temporary file path
    snprintf(tmp, sizeof(tmp), "%s.tmp", path); // e.g., "file.txt.tmp"
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    if (n && write_full(fd, data, n) != 1) {
        close(fd);
        return -1;
    }
    if (close(fd) != 0) return -1;
    if (rename(tmp, path) != 0) return -1; // Atomic rename
    return 0;
}   

/* --<>-- Global state --<>-- */
// Represents the file being synced 
struct State {
    char path[PATH_MAX];   // On-disk file path
    uint8_t *content;      // Heap buffer
    uint32_t content_len;  // Bytes in content
    uint64_t version;      // Version starting at 0
    pthread_mutex_t mu;    // Guard all fields above, ensure thread safety
};

static struct State g_state; // Global state for the server

/* --<>-- Stage Management --<>-- */
// Load initial state from disk into st
// On success, returns 0; on error, returns -1
// If file does not exist, initializes empty state with version 0
static int load_initial(struct State *st) {
    int fd = open(st->path, O_RDONLY | O_CLOEXEC); // Open file
    if (fd < 0) {
        if (errno == ENOENT) { // File does not exist 
            st->content = NULL; // Empty content
            st->content_len = 0;
            st->version = 0; // Initial version set to 0
            return 0;
        }
        return -1; // Other error
    }

    struct stat sb;
    if (fstat(fd, &sb) != 0) { // Get file size
        close(fd);
        return -1;
    }

    size_t n = (size_t)sb.st_size; // File size in bytes
    uint8_t *buf = NULL;
    if (n) {
        buf = (uint8_t *)malloc(n); // Allocate buffer
        if (!buf) {
            close(fd);
            return -1;
        }
        if (read_full(fd, buf, n) != 1) { // Read full file
            free(buf);
            close(fd);
            return -1;
        }
    }
    close(fd);

    st->content = buf; // Store buffer pointer
    st->content_len = (uint32_t)n; // Store length
    st->version = 0; // Initial version set to 0
    return 0; 
}

/* --<>-- Merge/conflict logic --<>-- */
// Combine client changes with server head based on base_version
static int merge_or_conflict(uint64_t base_version,
                             const uint8_t *client_data, uint32_t client_n,
                             const uint8_t *head_data, uint32_t head_n,
                             uint64_t head_version, uint8_t **out_data,
                             uint32_t *out_n) {
    if (base_version == head_version) {
        // No conflict; accept client changes
        uint8_t *cpy = NULL;
        if (client_n) {
            cpy = (uint8_t *)malloc(client_n);
            if (!cpy) return -1;
            memcpy(cpy, client_data, client_n);
        }
        *out_data = cpy; // merged data is client's data
        *out_n = client_n;
        return 0;
    }

    // Conflict, embed both versions in conflict markers
    const char *pre = "<<<<<<< client\n";
    const char *mid = "=======\n";
    const char *post = ">>>>>>> server\n";

    size_t pre_n = strlen(pre); // Length of "clinet" header
    size_t mid_n = strlen(mid); // Length of separator
    size_t post_n = strlen(post); // Length of "server" footer

    // Calculate total size needed
    size_t total = pre_n + client_n + 1 + mid_n + head_n + 1 + post_n;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;

    size_t off = 0;
    memcpy(buf + off, pre, pre_n); // write client header
    off += pre_n;

    if (client_n) memcpy(buf + off, client_data, client_n); // client content
    off += client_n;
    buf[off++] = '\n'; // extra newline
    memcpy(buf + off, mid, mid_n); // separator
    off += mid_n;

    if (head_n) memcpy(buf + off, head_data, head_n); // server content
    off += head_n;
    buf[off++] = '\n';
    memcpy(buf + off, post, post_n);
    off += post_n;

    *out_data = buf; // merged buffer
    *out_n = (uint32_t)off; // merged length
    return 0;
}

/* --<>-- Message Handlers --<>-- */
// Handle C_GET: send current state to client 
// On success, returns 1; on error, returns -1
static int handle_get(int fd) {
    pthread_mutex_lock(&g_state.mu); // lock to read consistent state
    uint64_t ver = g_state.version;
    uint32_t n = g_state.content_len;
    const uint8_t *data = g_state.content;

    uint8_t *buf = (uint8_t *)malloc(8 + 4 + n); // version + length + data 
    if (!buf) {
        pthread_mutex_unlock(&g_state.mu);
        return -1;
    }

    uint64_t be_ver = u64_hton(ver);     // version in network order 
    uint32_t be_n = htonl(n);            // length in network order
    memcpy(buf, &be_ver, 8);             // version 
    memcpy(buf + 8, &be_n, 4);           // length 
    if (n) memcpy(buf + 12, data, n);    // content 
    pthread_mutex_unlock(&g_state.mu);

    int ok = send_frame(fd, S_STATE, buf, 12 + n); // send response 
    free(buf);
    return ok;
}

// Handle C_PUT: process client submission 
// On sucess, returns 1; on error, returns -1
static int handle_put(int fd, const uint8_t *payload, uint32_t plen) {
    if (plen < 8 + 4) return -1; // must at least have version + length 

    // Extract base_version and client data from payload 
    uint64_t be_base;
    memcpy(&be_base, payload, 8);
    uint64_t base_version = u64_ntoh(be_base);

    uint32_t be_n;
    memcpy(&be_n, payload + 8, 4);
    uint32_t n = ntohl(be_n);
    if (8 + 4 + n != plen) return -1; // malformed frame

    // Pointer to client data bytes
    const uint8_t *client_data = payload + 12;    

    pthread_mutex_lock(&g_state.mu);
    uint64_t head_version = g_state.version; // Current server version
    const uint8_t *head_data = g_state.content;
    uint32_t head_n = g_state.content_len;

    uint8_t *merged = NULL;
    uint32_t merged_n = 0;

    // Combine client changes with server head 
    if (merge_or_conflict(base_version, client_data, n, head_data, head_n,
                            head_version, &merged, &merged_n) != 0) {
        pthread_mutex_unlock(&g_state.mu);
        return -1;
    }

    // Persist merged content to disk 
    if (atomic_write_file(g_state.path, merged, merged_n) != 0) {
        free(merged);
        pthread_mutex_unlock(&g_state.mu);
        return -1;
    }

    // Replace in-memory state with merged content and increment version
    free(g_state.content);
    g_state.content = merged;
    g_state.content_len = merged_n;
    g_state.version = head_version + 1; // Increment verison
    uint64_t new_version = g_state.version;
    pthread_mutex_unlock(&g_state.mu);

    // Send S_OK with new version to client 
    uint64_t be_new = u64_hton(new_version);
    return send_frame(fd, S_OK, (uint8_t *)&be_new, 8);
}

/* --<>-- Per-client thread --<>-- */
static void *client_thread(void *arg) {
    int fd = (int)(uintptr_t)arg; // Client socket
    for (;;) {
        uint8_t type;
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
            ok = -1; // Unknown message type: close connection
        }

        free(payload); // Free payload buffer
        if (ok != 1) break; // Handle error by closing connection
    }

    close(fd); // Close client socket
    return NULL;
}

/* --<>-- Listener setup --<>-- */
static int listen_on(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); // TCP Socket
    if (fd < 0) return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)); // Reuse addr

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; // IPv4
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // Listen on all interfaces
    addr.sin_port = htons(port); // Port

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, BACKLOG) != 0) { // Start listening 
        close(fd);
        return -1;
    }
    return fd;
}

// Print usage message
static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <port> <file_path>\n", prog);
}

/* --<>-- Main server loop --<>-- */
int main(int argc, char **argv) {
    if (argc != 3) { // Export port and file path 
        usage(argv[0]);
        return 2;
    }

    uint16_t port = (uint16_t)atoi(argv[1]); // Parse port 
    const char *path = argv[2]; // File path to sync 

    memset(&g_state, 0, sizeof(g_state)); // Zero out global state 
    pthread_mutex_init(&g_state.mu, NULL); // Initialize mutex 

    if (strlen(path) >= sizeof(g_state.path)) { // Check path length 
        fprintf(stderr, "File path too long\n");
        return 2;
    }
    strncpy(g_state.path, path, sizeof(g_state.path) - 1); // Copy path length
    g_state.path[sizeof(g_state.path) - 1] = 0; // Ensure null-termination

    if (load_initial(&g_state) != 0) { // Load file from disk 
        perror("load_initial");
        return 1;
    }

    int lfd = listen_on(port); // Create listening socket
    if (lfd < 0) {
        perror("listen");
        return 1;
    }

    printf("Serving %s on port %u (version=%" PRIu64 ")\n",
            g_state.path, (unsigned)port, g_state.version);

    for (;;) { // Main accept loop 
        struct sockaddr_in cli;
        socklen_t slen = sizeof(cli);
        int cfd = accept(lfd, (struct sockaddr *)&cli, &slen); // Wait for client
        if (cfd < 0) {
            if (errno == EINTR) continue; // Interrupted, try again
            perror("accept");
            continue;
        }

        pthread_t th;
        // Spawn thread to handle client 
        if (pthread_create(&th, NULL, client_thread, 
                            (void *)(intptr_t)cfd) != 0) {
            close(cfd); // Failed to create thread, close
            continue;
        }
        pthread_detach(th); // Detach Thread
    }

    close(lfd);
    return 0;
}