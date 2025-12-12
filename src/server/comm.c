#define _GNU_SOURCE
#include "comm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h> 
#include <errno.h>  
#include <unistd.h> 

// read_full/write_full implement exactly n bytes of reading/writing
    // Necessary because read()/write() may do partial transfers
int read_full(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf; 
    size_t got = 0; // Bytes read
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return 0; 
        if (r < 0) {
            if (errno == EINTR) continue; // try again
            return -1; // error
        }
        got += (size_t)r;
    }
    return 1; // success
}

int write_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0; // Bytes written
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

// read_full / write_full loop until theyve read exactly n bytes
int send_frame(int fd, uint8_t type, const uint8_t *payload, uint32_t plen) {
    uint32_t len = 1u + plen; 
    uint32_t be_len = htonl(len);
    uint8_t hdr[5];
    memcpy(hdr, &be_len, 4); // copy length 

    hdr[4] = type;
    if (write_full(fd, hdr, 5) != 1) return -1; // write header
    if (plen && write_full(fd, payload, plen) != 1) return -1;
    return 1;
}

// Receive a full frame. Caller frees *payload_out if plen_out > 0
int recv_frame(int fd, uint8_t *type_out, uint8_t **payload_out, uint32_t *plen_out) {
    uint32_t be_len;
    int r = read_full(fd, &be_len, 4); 
    if (r <= 0) return r; 
    uint32_t len = ntohl(be_len); // Convert length to host order
    if (len == 0 || len > MAX_MSG) return -1; 

    uint8_t type;
    r = read_full(fd, &type, 1); // Read 1-byte type
    if (r <= 0) return r;

    uint32_t plen = len - 1; // Payload length
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