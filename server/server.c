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
    S_STATE = 0x11, // Current version and bytes. 
    S_OK = 0x12,    // PUT accepted; new version included. 
};

/* --<>-- Byte-order helpers --<>-- */
// 64-bit host/network conversion stuff 
static uint64_t u64_hton(uint64_t x) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(x >> 32))) |
           (((uint64_t)htonl((uint32_t)(x & 0xffffffff))) << 32);
#else
    return x;
#endif
}
static uint64_t u64_ntoh(uint64_t x) {return u64_hton(x);}


/* --<>-- IO primitives --<>-- */
// Read exactly n bytes 
// return 1 on success, 0 on EOF, -1 on error.
static int read_full(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return 1;
}

// Write exactly n bytes; return 1 on success, -1 on error.
static int write_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, p + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)w;
    }
    return 1;
}


/* --<>-- Frame encode/decode --<>-- */
// Send: [uint32_be length][uint8 type][payload…], length = 1 + payload.
static int send_frame(int fd, uint8_t type, const uint8_t *payload,
                      uint32_t plen) {
    uint32_t len = 1u + plen;
    uint32_t be_len = htonl(len);
    uint8_t hdr[5];
    memcpy(hdr, &be_len, 4);

    hdr[4] = type;
    if (write_full(fd, hdr, 5) != 1) return -1;
    if (plen && write_full(fd, payload, plen) != 1) return -1;
    return 1;
}

static int recv_frame(int fd, uint8_t *type_out, uint8_t **payload_out, uint32_t *plen_out) {
    uint32_t be_len;
    int r = read_full(fd, &be_len, 4);
    if (r <= 0) return r;
    uint32_t len = ntohl(be_len);
    if (len == 0 || len > MAX_MSG) return -1;

    uint8_t type;
    r = read_full(fd, &type, 1);
    if (r <= 0) return r;

    uint32_t plen = len - 1;
    uint8_t *buf = NULL;
    if (plen) {
        buf = (uint8_t *)malloc(plen);
        if (!buf) return -1;
        r = read_full(fd, buf, plen);
        if (r <= 0) {
            free(buf);
            return r;
        }
    }

    *type_out = type;
    *payload_out = buf;
    *plen_out = plen;
    return 1;
}

// Receive a full frame. Caller frees *payload_out if plen_out > 0.
static int atomic_write_file(const char *path, const uint8_t *data, size_t n) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    if (n && write_full(fd, data, n) != 1) {
        close(fd);
        return -1;
    }
    if (close(fd) != 0) return -1;
    if (rename(tmp, path) != 0) return -1;
    return 0;
}   

/* --<>-- Global state --<>-- */
struct State {
    char path[PATH_MAX];   // On-disk file path
    uint8_t *content;      // Heap buffer
    uint32_t content_len;  // Bytes in content
    uint64_t version;      // Version starting at 0
    pthread_mutex_t mu;    // Guard all fields above
};

static struct State g_state;

static int load_initial(struct State *st) {
    int fd = open(st->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            st->content = NULL;
            st->content_len = 0;
            st->version = 0;
            return 0;
        }
        return -1;
    }

    struct stat sb;
    if (fstat(fd, &sb) != 0) {
        close(fd);
        return -1;
    }

    size_t n = (size_t)sb.st_size;
    uint8_t *buf = NULL;
    if (n) {
        buf = (uint8_t *)malloc(n);
        if (!buf) {
            close(fd);
            return -1;
        }
        if (read_full(fd, buf, n) != 1) {
            free(buf);
            close(fd);
            return -1;
        }
    }
    close(fd);

    st->content = buf;
    st->content_len = (uint32_t)n;
    st->version = 0;
    return 0;
}

static int merge_or_conflict(uint64_t base_version,
                             const uint8_t *client_data, uint32_t client_n,
                             const uint8_t *head_data, uint32_t head_n,
                             uint64_t head_version, uint8_t **out_data,
                             uint32_t *out_n) {
    if (base_version == head_version) {
        uint8_t *cpy = NULL;
        if (client_n) {
            cpy = (uint8_t *)malloc(client_n);
            if (!cpy) return -1;
            memcpy(cpy, client_data, client_n);
        }
        *out_data = cpy;
        *out_n = client_n;
        return 0;
    }

    const char *pre = "<<<<<<< client\n";
    const char *mid = "=======\n";
    const char *post = ">>>>>>> server\n";

    size_t pre_n = strlen(pre);
    size_t mid_n = strlen(mid);
    size_t post_n = strlen(post);

    size_t total = pre_n + client_n + 1 + mid_n + head_n + 1 + post_n;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;

    size_t off = 0;
    memcpy(buf + off, pre, pre_n);
    off += pre_n;

    if (client_n) memcpy(buf + off, client_data, client_n);
    off += client_n;
    buf[off++] = '\n';
    memcpy(buf + off, mid, mid_n);
    off += mid_n;

    if (head_n) memcpy(buf + off, head_data, head_n);
    off += head_n;
    buf[off++] = '\n';
    memcpy(buf + off, post, post_n);
    off += post_n;

    *out_data = buf;
    *out_n = (uint32_t)off;
    return 0;
}

static int handle_get(int fd) {
    pthread_mutex_lock(&g_state.mu);
    uint64_t ver = g_state.version;
    uint32_t n = g_state.content_len;
    const uint8_t *data = g_state.content;

    uint8_t *buf = (uint8_t *)malloc(8 + 4 + n);
    if (!buf) {
        pthread_mutex_unlock(&g_state.mu);
        return -1;
    }

    uint64_t be_ver = u64_hton(ver);
    uint32_t be_n = htonl(n);
    memcpy(buf, &be_ver, 8);
    memcpy(buf + 8, &be_n, 4);
    if (n) memcpy(buf + 12, data, n);
    pthread_mutex_unlock(&g_state.mu);

    int ok = send_frame(fd, S_STATE, buf, 12 + n);
    free(buf);
    return ok;
}

static int handle_put(int fd, const uint8_t *payload, uint32_t plen) {
    if (plen < 8 + 4) return -1;

    uint64_t be_base;
    memcpy(&be_base, payload, 8);
    uint64_t base_version = u64_ntoh(be_base);

    uint32_t be_n;
    memcpy(&be_n, payload + 8, 4);
    uint32_t n = ntohl(be_n);
    if (8 + 4 + n != plen) return -1;

    const uint8_t *client_data = payload + 12;    

    pthread_mutex_lock(&g_state.mu);
    uint64_t head_version = g_state.version;
    const uint8_t *head_data = g_state.content;
    uint32_t head_n = g_state.content_len;

    uint8_t *merged = NULL;
    uint32_t merged_n = 0;
    if (merge_or_conflict(base_version, client_data, n, head_data, head_n,
                            head_version, &merged, &merged_n) != 0) {
        pthread_mutex_unlock(&g_state.mu);
        return -1;
    }

    if (atomic_write_file(g_state.path, merged, merged_n) != 0) {
        free(merged);
        pthread_mutex_unlock(&g_state.mu);
        return -1;
    }

    free(g_state.content);
    g_state.content = merged;
    g_state.content_len = merged_n;
    g_state.version = head_version + 1;
    uint64_t new_version = g_state.version;
    pthread_mutex_unlock(&g_state.mu);

    uint64_t be_new = u64_hton(new_version);
    return send_frame(fd, S_OK, (uint8_t *)&be_new, 8);
}

/* --<>-- Per-client thread --<>-- */
static void *client_thread(void *arg) {
    int fd = (int)(uintptr_t)arg;
    for (;;) {
        uint8_t type;
        uint8_t *payload = NULL;
        uint32_t plen = 0;
        int r = recv_frame(fd, &type, &payload, &plen);
        if (r <= 0) {
            free(payload);
            break;
        }

        int ok = 1;
        if (type == C_GET) {
            ok = handle_get(fd);
        } else if (type == C_PUT) {
            ok = handle_put(fd, payload, plen);
        } else {
            ok = -1; // Unknown message type: close connection.
        }

        free(payload);
        if (ok != 1) break;
    }

    close(fd);
    return NULL;
}

/* --<>-- Listener setup --<>-- */
static int listen_on(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, BACKLOG) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <port> <file_path>\n", prog);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        usage(argv[0]);
        return 2;
    }

    uint16_t port = (uint16_t)atoi(argv[1]);
    const char *path = argv[2];

    memset(&g_state, 0, sizeof(g_state));
    pthread_mutex_init(&g_state.mu, NULL);

    if (strlen(path) >= sizeof(g_state.path)) {
        fprintf(stderr, "File path too long\n");
        return 2;
    }
    strncpy(g_state.path, path, sizeof(g_state.path) - 1);
    g_state.path[sizeof(g_state.path) - 1] = 0;

    if (load_initial(&g_state) != 0) {
        perror("load_initial");
        return 1;
    }

    int lfd = listen_on(port);
    if (lfd < 0) {
        perror("listen");
        return 1;
    }

    printf("Serving %s on port %u (version=%" PRIu64 ")\n",
            g_state.path, (unsigned)port, g_state.version);

    for (;;) {
        struct sockaddr_in cli;
        socklen_t slen = sizeof(cli);
        int cfd = accept(lfd, (struct sockaddr *)&cli, &slen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        pthread_t th;
        if (pthread_create(&th, NULL, client_thread, 
                            (void *)(intptr_t)cfd) != 0) {
            close(cfd);
            continue;
        }
        pthread_detach(th);
    }

    close(lfd);
    return 0;
}