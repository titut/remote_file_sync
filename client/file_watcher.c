// file_watcher.c
#include "file_watcher.h"
#include "socket_client.h"
#include "rfs_file.h"
#include <arpa/inet.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>

#define EVENT_SIZE  (sizeof(struct inotify_event))
#define BUF_LEN     (1024 * (EVENT_SIZE + 16))
// read 4kb of file at a time
#define SOCKET_CHUNK 4096
#define IP "192.168.16.178"
#define PORT        8080

struct args {
    int new_message;
    int client_fd;
    char* message;
    char* file_path;
};

// file watcher variables
int fd, wd;

// path variables
char file_path[50];
char folder_path[50];
const char* home;

volatile sig_atomic_t stop_flag = 0;

void handle_sigint(int sig) {
    stop_flag = 1;
    printf("\nStopping...\n");
}

void init_file_path(){
    home = getenv("HOME");

    snprintf(file_path, sizeof(file_path), "%s/rfs/rfs.py", home);
    snprintf(folder_path, sizeof(folder_path), "%s/rfs", home);
}

void* start_file_watcher(void* arg) {
    char buffer[BUF_LEN];

    // Initialize inotify and catch err
    fd = inotify_init();
    if (fd < 0) {
        perror("inotify_init");
        exit(EXIT_FAILURE);
    }

    printf("inotify initialized!\n");

    // Add a watch to the directory and catch err
    wd = inotify_add_watch(fd, file_path, IN_CREATE | IN_MODIFY | IN_DELETE);
    if (wd == -1) {
        fprintf(stderr, "Cannot watch '%s'\n", file_path);
        exit(EXIT_FAILURE);
    }

    printf("Watching directory: %s\n\n", file_path);

    // Event loop
    while (!stop_flag) {
        int length = read(fd, buffer, BUF_LEN);
        if (length < 0) {
            perror("read");
            break;
        }

        int i = 0;
        while (i < length) {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];
            if (event->mask & IN_CREATE){
                printf("File created\n");
                ((struct args*)arg)->message = "File created";
            } else if (event->mask & IN_MODIFY) {
                printf("File modified\n");
                ((struct args*)arg)->message = "File modified";
            }
            else if (event->mask & IN_DELETE) {
                printf("File deleted\n");
                ((struct args*)arg)->message = "File deleted";
            }
            ((struct args*)arg)->new_message=1;

            // Call sync callback
            /*if (on_change && (event->mask & (IN_CREATE | IN_MODIFY)))
                on_change(event->name);
            */
            i += EVENT_SIZE + event->len;
        }
    }
}

void close_file_watcher(){
    // Cleanup
    inotify_rm_watch(fd, wd);
    close(fd);
    printf("File watcher cleaned\n");
}

int main(void){
    // Handle Ctrl + C
    signal(SIGINT, handle_sigint);

    init_file_path();
    // Check if RFS file exists, if not create it
    create_rfs_folder(folder_path);
    int rfs_is_folder = check_rfs_file_exists(file_path);
    if(!rfs_is_folder){
        create_rfs_file(file_path);
    }

    // Thread 1: File Watcher
    // Thread 2: Socket Thread
    pthread_t file_watcher_thread;
    pthread_t socket_thread;

    // args pointer will be used to communicate between the two threads
    struct args* arguments = malloc(sizeof(struct args));
    arguments->new_message=0;
    arguments->message="";
    arguments->file_path=file_path;
    arguments->client_fd=start_client(&stop_flag);

    // Start watcher thread
    pthread_create(&socket_thread, NULL, send_thread, arguments);
    pthread_create(&file_watcher_thread, NULL, start_file_watcher, arguments);

    // Wait for thread to finish
    pthread_join(socket_thread, NULL);
    pthread_cancel(file_watcher_thread);
    pthread_join(file_watcher_thread, NULL);

    printf("Cleaning up\n");
    close_file_watcher();
    close_client(arguments->client_fd);
    printf("free arguments\n");
    free(arguments);

    return 0;
}