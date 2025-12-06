// file_watcher.c
#define _GNU_SOURCE
#include "socket_client.h"
#include "rfs_file.h"
#include "args.h"

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
#include <inttypes.h>
#include <poll.h>

#define EVENT_SIZE  (sizeof(struct inotify_event))
#define BUF_LEN     (1024 * (EVENT_SIZE + 16))

// file watcher variables
int fd, wd;

// path variables
char file_path[50];
char folder_path[50];
const char* home;

// Ctrl + C handling variables
volatile sig_atomic_t stop_flag = 0;


struct args* arguments;

void handle_sigint(int sig) {
    stop_flag = 1;
    write(arguments->pipefd[1], "X", 1);
    printf("\nCtrl+C detected\n");
}

void init_file_path(void) {
    home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "Could not get HOME environment variable\n");
        exit(EXIT_FAILURE);
    }
    snprintf(file_path, sizeof(file_path), "%s/rfs/main.py", home);
    snprintf(folder_path, sizeof(folder_path), "%s/rfs", home);
}

void* start_file_watcher(void* arg) {
    struct args *a = (struct args *)arg;
    char buffer[BUF_LEN];

    // Initialize inotify and catch err
    fd = inotify_init();
    if (fd < 0) {
        perror("inotify_init");
        exit(EXIT_FAILURE);
    }

    printf("inotify initialized!\n");

    // Add a watch to the directory and catch err
    wd = inotify_add_watch(fd, folder_path, IN_CREATE | IN_MODIFY | IN_DELETE);
    if (wd == -1) {
        fprintf(stderr, "Cannot watch '%s'\n", folder_path);
        close(fd);
        exit(EXIT_FAILURE);
    }
    printf("Watching directory: %s\n\n", folder_path);


    struct pollfd pfd = {
        .fd = fd,
        .events = POLLIN
    };

    // Event loop
    while (!stop_flag) {
        int ret = poll(&pfd, 1, 500); // timeout every 500ms so loop can check stop_flag

        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (ret == 0) continue; // timeout → check stop_flag again

        if (pfd.revents & POLLIN) {
            int length = read(fd, buffer, BUF_LEN);
            if (length < 0) {
                if (errno == EINTR) continue;
                perror("read");
                break;
            }

            int i = 0;
            while (i < length) {
                struct inotify_event *event = (struct inotify_event *)&buffer[i];

                if (event->len) {
                    if (strcmp(event->name, "main.py") == 0) {
                        if (event->mask & IN_CREATE) 
                            printf("File created: %s\n", event->name);
                        else if (event->mask & IN_MODIFY) 
                            printf("File modified: %s\n", event->name);
                        else if (event->mask & IN_DELETE) 
                            printf("File deleted: %s\n", event->name);

                        pthread_mutex_lock(&a->mu);
                        if (a->suppress_next) {
                            a->suppress_next = 0;
                        }
                        pthread_mutex_unlock(&a->mu);

                        // Send message to pipe
                        char* msg = "modify";
                        write(a->pipefd[1], msg, strlen(msg) + 1);
                    }
                }
                // Call sync callback
                /*if (on_change && (event->mask & (IN_CREATE | IN_MODIFY)))
                    on_change(event->name);
                */
                i += EVENT_SIZE + event->len;
            }
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
    arguments = malloc(sizeof(struct args));
    if (!arguments) {
        perror("malloc failed");
        exit(EXIT_FAILURE);
    }
    arguments->new_message = 0;
    arguments->message     = NULL;
    arguments->file_path   = file_path;
    arguments->last_version = 0;
    arguments->suppress_next = 0;
    arguments->stop_flag_addr = &stop_flag;
    pthread_mutex_init(&arguments->mu, NULL);

    // Create pipe
    if(pipe(arguments->pipefd) == -1){
        perror("Pipe failed!");
        exit(EXIT_FAILURE);
    }

    // Start watcher thread
    pthread_create(&socket_thread, NULL, socket_client, arguments);
    pthread_create(&file_watcher_thread, NULL, start_file_watcher, arguments);

    // Wait for thread to finish
    pthread_join(socket_thread, NULL);
    pthread_join(file_watcher_thread, NULL);

    printf("Safe clean up...\n");
    close_file_watcher();
    pthread_mutex_destroy(&arguments->mu);
    free(arguments);

    return 0;
}