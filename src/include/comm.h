#ifndef COMM_H
#define COMM_H

#include <stdint.h>
#include <stddef.h>

#define MAX_MSG (8u * 1024u * 1024u) // 8 MB

enum MsgType {
    C_GET   = 0x01,  // Poll current state
    C_PUT   = 0x02,  // Submit new state based on base_version
    S_STATE = 0x11,  // Current version and bytes
    S_OK    = 0x12,  // PUT accepted new version included
};

int read_full(int fd, void *buf, size_t n);
int write_full(int fd, const void *buf, size_t n);

int send_frame(int fd, uint8_t type, const uint8_t *payload, uint32_t plen);
int recv_frame(int fd, uint8_t *type_out, uint8_t **payload_out, uint32_t *plen_out);

#endif