#ifndef SOCKET_CLIENT_H
#define SOCKET_CLIENT_H

void* socket_client(void* arg);

int send_file(int socket, char* path);

#endif