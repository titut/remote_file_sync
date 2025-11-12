// file_watcher.c
#include "file_watcher.h"
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

#define EVENT_SIZE  (sizeof(struct inotify_event))
#define BUF_LEN     (1024 * (EVENT_SIZE + 16))

void start_file_watcher(const char *path) {
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
    wd = inotify_add_watch(fd, path, IN_CREATE | IN_MODIFY | IN_DELETE);
    if (wd == -1) {
        fprintf(stderr, "Cannot watch '%s'\n", path);
        exit(EXIT_FAILURE);
    }

    printf("Watching directory: %s\n", path);

    // Event loop
    while (1) {
        int length = read(fd, buffer, BUF_LEN);
        if (length < 0) {
            perror("read");
            break;
        }

        int i = 0;
        while (i < length) {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];
            if (event->len) {
                if (event->mask & IN_CREATE)
                    printf("File created: %s\n", event->name);
                else if (event->mask & IN_MODIFY)
                    printf("File modified: %s\n", event->name);
                else if (event->mask & IN_DELETE)
                    printf("File deleted: %s\n", event->name);

                // Call sync callback
                /*if (on_change && (event->mask & (IN_CREATE | IN_MODIFY)))
                    on_change(event->name);
                */
            }
            i += EVENT_SIZE + event->len;
        }
    }

    // Cleanup
    inotify_rm_watch(fd, wd);
    close(fd);
}

int check_rfs_folder_exists(){
    const char* directoryPath = "/opt/rfs"; // Replace with your directory path
    struct stat sb;

    if (stat(directoryPath, &sb) == 0 && S_ISDIR(sb.st_mode)) {
        printf("Directory '%s' exists.\n", directoryPath);
        return 1;
    } else {
        printf("Directory '%s' does not exist or is not a directory.\n", directoryPath);
        return 0;
    }
}

void create_rfs_folder(){
    // create fork to call mkdir using exec
    pid_t child_pid = fork();

    if(child_pid < 0){
        // fork failed
        perror("fork failed");
        exit(EXIT_FAILURE);
    } else if(child_pid == 0){
        // child process: create the folder
        char *args[]={"mkdir", "/opt/rfs", NULL};
        execvp(args[0], args);
        puts("/opt/rfs folder created");

        perror("folder creation failed");
        printf("errno: %d\n", errno);
        exit(EXIT_FAILURE);
    } else {
        // parent process: wait for child process to complete
        int status;
        printf("Parent: waiting for child (PID %d)...\n", child_pid);
        wait(&status);

        if (WIFEXITED(status)) {
            printf("Child exited normally with code %d\n", WEXITSTATUS(status));
        } else {
            printf("Child exited abnormally\n");
        }

        printf("Continuing to watcher now.\n");
    }
}

int main(void){
    // Check if RFS folder exists
    int rfs_is_folder = check_rfs_folder_exists();
    if(rfs_is_folder){
        puts("/opt/rfs folder exist");
    } else {
        puts("/opt/rfs does not exist");
        create_rfs_folder();
    }

    // Start watcher
    start_file_watcher("/opt/rfs");
    return 0;
}