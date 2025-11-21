#ifndef SOCKET_CLIENT_H
#define SOCKET_CLIENT_H

int start_client();

void* send_thread(void* arg);

void close_client(int client_fd);

int send_file(int socket, char* path);

#endif