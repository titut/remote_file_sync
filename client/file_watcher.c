// file_watcher.c
#define _GNU_SOURCE
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
#include <inttypes.h>

#define EVENT_SIZE  (sizeof(struct inotify_event))
#define BUF_LEN     (1024 * (EVENT_SIZE + 16))

// // read 4kb of file at a time
// #define SOCKET_CHUNK 4096
// #define IP "192.168.16.178"
// #define PORT        8080

char file_path[PATH_MAX];
char folder_path[PATH_MAX];
const char* home;

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
    int fd, wd;
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

    // Event loop
    while (1) {
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
                    } else {
                        a->new_message = 1;
                    }
                    pthread_mutex_unlock(&a->mu);
                }
            }
            // Call sync callback
            /*if (on_change && (event->mask & (IN_CREATE | IN_MODIFY)))
                on_change(event->name);
            */
            i += EVENT_SIZE + event->len;
        }
    }

    // Cleanup
    inotify_rm_watch(fd, wd);
    close(fd);
    return NULL;
}

int main(void){
    init_file_path();

    // Check if RFS file exists, if not create it
    create_rfs_folder(folder_path);
    if(!check_rfs_file_exists(file_path)){
        create_rfs_file(file_path);
    }

    // Thread 1: File Watcher
    // Thread 2: Socket Thread
    pthread_t file_watcher_thread;
    pthread_t socket_thread;

    // args pointer will be used to communicate between the two threads
    struct args* arguments = malloc(sizeof(struct args));
    if (!arguments) {
        perror("malloc failed");
        exit(EXIT_FAILURE);
    }
    arguments->new_message = 0;
    arguments->message     = NULL;
    arguments->file_path   = file_path;
    arguments->last_version = 0;
    arguments->suppress_next = 0;
    pthread_mutex_init(&arguments->mu, NULL);

    // Start watcher thread
    pthread_create(&socket_thread, NULL, socket_client, arguments);
    pthread_create(&file_watcher_thread, NULL, start_file_watcher, arguments);

    // Wait for thread to finish
    pthread_join(socket_thread, NULL);
    pthread_join(file_watcher_thread, NULL);

    pthread_mutex_destroy(&arguments->mu);
    free(arguments);

    return 0;
}