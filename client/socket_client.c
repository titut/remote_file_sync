#include "socket_client.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>

// read 4kb of file at a time
#define SOCKET_CHUNK 4096
#define IP "127.0.0.1"
#define PORT        8080

struct args {
    int new_message;
    int client_fd;
    char* message;
    char* file_path;
};

volatile sig_atomic_t* stop_flag_addr;

int start_client(volatile sig_atomic_t* flag_addr){
    //set stop flag address
    stop_flag_addr = flag_addr;

    // Create socket (client-side)
    int socket_status, valread, client_fd;
    struct sockaddr_in serv_addr;
    char buffer[1024] = { 0 };

    // Create socket endpoint
    if ((client_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation error");
        exit(EXIT_FAILURE);
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, IP, &serv_addr.sin_addr) <= 0) {
        perror("nInvalid address/Address not supported");
        exit(EXIT_FAILURE);
    }

    // Connect to the server
    if ((socket_status = connect(client_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr))) < 0) {
        perror("Connection to server failed");
        exit(EXIT_FAILURE);
    }

    return client_fd;
}

void* send_thread(void* arg){
    while(!*stop_flag_addr){
        if(((struct args*)arg)->new_message){
            // Send message to socket
            send_file(((struct args*)arg)->client_fd, ((struct args*)arg)->file_path);
            puts("Message sent");
            ((struct args*)arg)->new_message = 0;
        }
    }
}

void close_client(int client_fd){
    // Close connection
    close(client_fd);
    puts("Client closed");
}

int send_file(int socket, char* path){
    // Open file
    FILE *fp = fopen(path, "rb");
    if(!fp){
        perror("Failed to open file!");
        return -1;
    }

    char buffer[SOCKET_CHUNK];
    size_t bytes_read;

    // read file and assign it to buffer
    while ((bytes_read = fread(buffer, 1, SOCKET_CHUNK, fp)) > 0) {
        size_t total_sent = 0;
        
        // send information in buffer
        // loop to make sure all of buffer has been sent
        while (total_sent < bytes_read) {
            ssize_t sent = send(socket, buffer + total_sent, bytes_read - total_sent, 0);
            if (sent <= 0) {
                perror("Send unsuccessful");
                fclose(fp);
                return -1;
            }
            total_sent += sent;
        }
    }

    fclose(fp);
    return 0;
}