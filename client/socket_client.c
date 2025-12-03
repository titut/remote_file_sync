// socket_client.c
#define _GNU_SOURCE
#include "socket_client.h"
#include "file_watcher.h"
#include "rfs_file.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>

#define IP "127.0.0.1"
#define PORT "8080"
#define MAX_MSG (8u * 1024u * 1024u)  // 8 MB

#define SERVER_HOST "raspberrypi.local" 
#define SERVER_PORT_STR "9000"

/* --<>-- Message Types --<>-- */
enum MsgType {
  C_GET = 0x01,
  C_PUT = 0x02,
  S_STATE = 0x11,
  S_OK = 0x12,
};

/* --<>-- Byte order helpers --<>-- */
// Same logic used in server file
static uint64_t htonll(uint64_t x) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return ((uint64_t)htonl((uint32_t)(x >> 32))) |
         (((uint64_t)htonl((uint32_t)(x & 0xffffffff))) << 32);
#else
  return x;
#endif
}

static uint64_t ntohll(uint64_t x) { return htonll(x); }

/* --<>-- IO helpers --<>-- */
// read_full: read exactly n bytes into buf
// Returns: 1 on success, 0 on EOF, -1 on error 
static int read_full(int fd, void* buf, size_t n) {
  uint8_t* p = (uint8_t*)buf;
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, p + got, n - got);
    if (r == 0) return 0; // EOF
    if (r < 0) {
      if (errno == EINTR) continue; // interrupted by signal, retry
      return -1; // real error
    }
    got += (size_t)r;
  }
  return 1;
}

// write_full: write exactly n bytes from buf
// Returns: 1 on success, -1 on error
static int write_full(int fd, const void* buf, size_t n) {
  const uint8_t* p = (const uint8_t*)buf;
  size_t sent = 0;
  while (sent < n) {
    ssize_t w = write(fd, p + sent, n - sent);
    if (w < 0) {
      if (errno == EINTR) continue; // interrupted, retry
      return -1; // real error
    }
    sent += (size_t)w;
  }
  return 1;
} 

// send_frame: send one protocol frame
static int send_frame(int fd, uint8_t type, const uint8_t* payload, uint32_t plen) {
  uint32_t len = 1u + plen;
  uint32_t be_len = htonl(len); // header length
  uint8_t hdr[5];

  memcpy(hdr, &be_len, 4);   // First 4 bytes: length
  hdr[4] = type; // Fifth byte: type

  // Send header, then payload
  if (write_full(fd, hdr, 5) != 1) return -1;
  if (plen && write_full(fd, payload, plen) != 1) return -1;
  return 1;
}

// recv_frame: read one full frame from the socket
static int recv_frame(int fd, uint8_t* type_out, uint8_t** payload_out, uint32_t* plen_out) {
  uint32_t be_len;
  int r = read_full(fd, &be_len, 4);
  if (r <= 0) return r; // EOF or error
  uint32_t len = ntohl(be_len);
  if (len == 0 || len > MAX_MSG) return -1; // invalid or too large

  uint8_t type;
  r = read_full(fd, &type, 1);
  if (r <= 0) return r;

  uint32_t plen = len - 1; // payload leangth
  uint8_t* buf = NULL;
  if (plen) {
    buf = (uint8_t*)malloc(plen);
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

/* --<>-- Connect to server --<>-- */
// Now with getaddrinfo
// Open TCP connection, return socket fd on success
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
  return fd; // -1 if all attempts failed
}

/* --<>-- Pull from server --<>-- */
// 1. Connect to server
// 2. Send C_GET
// 3. Receives S_STATE (version + bytes)
// 4. If version != last_version, overwrite local file and update last_version
static void pull_from_server(struct args* a) {
  int fd = connect_to_server();
  if (fd < 0) return;

  // Send C_GET
  if (send_frame(fd, C_GET, NULL, 0) != 1) {
    close(fd);
    return;
  }

  // Receive S_STATE
  uint8_t type;
  uint8_t* payload = NULL;
  uint32_t plen = 0;
  int r = recv_frame(fd, &type, &payload, &plen);
  close(fd);
  if (r <= 0) {
    free(payload);
    return;
  }
  if (type != S_STATE || plen < 8 + 4) {
    // S_STATE must at least contain version(8) + len(4)
    free(payload);
    return;
  }

  // Unpack version and length
  uint64_t be_ver;
  memcpy(&be_ver, payload, 8);
  uint64_t ver = ntohll(be_ver);
  uint32_t be_n;
  memcpy(&be_n, payload + 8, 4);
  uint32_t n = ntohl(be_n);
  if (8 + 4 + n != plen) {
    // Malformed frame
    free(payload);
    return;
  }

  const uint8_t* data = payload + 12; // content starts after version + len

  // Compare with local version and if changed, apply
  pthread_mutex_lock(&a->mu);
  if (ver != a->last_version) {
    a->suppress_next = 1;
    if (atomic_write_local(a->file_path, data, n) == 0) {
      a->last_version = ver;
      printf("[client] pulled version %" PRIu64 ", %u bytes\n", ver, n);
    }
  }
  pthread_mutex_unlock(&a->mu);

  free(payload);
}

/* --<>-- Push to Server --<>-- */
// 1. Read entire local file into memory
// 2. Send C_PUT
// 3. Receive S_ON(new_version) and updates last_version
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

  uint64_t base_ver = a->last_version;
  pthread_mutex_unlock(&a->mu);

  // Open TCP connection
  int fd = connect_to_server();
  if (fd < 0) {
    free(data);
    return;
  }

  // Build C_PUT payload: [u64_be base_version][u32_be len][bytes]
  uint8_t* buf = (uint8_t*)malloc(8 + 4 + len);
  if (!buf) {
    free(data);
    close(fd);
    return;
  }

  uint64_t be_base = htonll(base_ver);
  uint32_t be_n = htonl(len);
  memcpy(buf, &be_base, 8);
  memcpy(buf + 8, &be_n, 4);
  if (len) memcpy(buf + 12, data, len);

  // Send frame
  if (send_frame(fd, C_PUT, buf, 8 + 4 + len) != 1) {
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
    uint64_t be_new;
    memcpy(&be_new, payload, 8);
    uint64_t new_ver = ntohll(be_new);

    pthread_mutex_lock(&a->mu);
    a->last_version = new_ver;
    pthread_mutex_unlock(&a->mu);

    printf("[client] pushed version %" PRIu64 ", %u bytes\n", new_ver, len);
  } else {
    printf("[client] push rejected due to conflict\n");
  }

  free(payload);
  free(data);
}

/* --<>-- Thread Entry --<>-- */
// Entry point for the network thread started in main
// Infinite loop:
  // If file_watcher signaled new_message, call push_to_server
  // Always call pull_from_server to pick up remote changes
  // Sleep briefly to avoid tight spinning
void* socket_client(void* arg) {
  struct args* a = (struct args*)arg;

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

    if (ret == 0) continue; // timeout → check stop_flag again

    if (pfd.revents & POLLIN) {

      // If file changed, push
      char buf[100];
      read(a->pipefd[0], buf, sizeof(buf));

      // Check whether read was the shutdown signal
      if (buf[0] == 'X') {
          printf("Reader thread exiting...\n");
          break;
      }

      push_to_server(a);
    }
    
    // Pull from server periodically
    pull_from_server(a);

    usleep(100000);  // 100ms delay
  }

  return NULL;
}