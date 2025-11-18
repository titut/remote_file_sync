// file_watcher.c
#include "file_watcher.h"
#include "socket_client.h"
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

int check_rfs_file_exists(char* file_path){
    struct stat sb;

    if (stat(file_path, &sb) == 0 && S_ISREG(sb.st_mode)) {
        printf("File '%s' exists.\n\n", file_path);
        return 1;
    } else {
        printf("File '%s' does not exist or is not a directory.\n", file_path);
        return 0;
    }
}

void create_rfs_folder(char* folder_path){
    pid_t child_pid = fork();

    if(child_pid < 0){
        // fork failed
        perror("fork failed");
        exit(EXIT_FAILURE);
    } else if(child_pid == 0){
        // child process: create the folder
        char *args[]={"mkdir", "-p", folder_path, NULL};
        execvp(args[0], args);

        perror("folder creation failed");
        printf("errno: %d\n", errno);
        exit(EXIT_FAILURE);
    } else {
        // parent process: wait for child process to complete
        int child_status;
        printf("Parent: waiting for child (PID %d)...\n", child_pid);
        wait(&child_status);

        if (WIFEXITED(child_status)) {
            printf("Child exited normally with code %d\n", WEXITSTATUS(child_status));
        } else {
            printf("Child exited abnormally\n");
        }

        printf("Continuing to watcher now.\n\n");
    }
}

void create_rfs_file(char* file_path){
    // create fork to call mkdir using exec
    pid_t child_pid = fork();

    if(child_pid < 0){
        // fork failed
        perror("fork failed");
        exit(EXIT_FAILURE);
    } else if(child_pid == 0){
        // child process: create the folder
        char *args[]={"touch", file_path, NULL};
        execvp(args[0], args);
        printf("%s file created\n", file_path);

        perror("folder creation failed");
        printf("errno: %d\n", errno);
        exit(EXIT_FAILURE);
    } else {
        // parent process: wait for child process to complete
        int child_status;
        printf("Parent: waiting for child (PID %d)...\n", child_pid);
        wait(&child_status);

        if (WIFEXITED(child_status)) {
            printf("Child exited normally with code %d\n", WEXITSTATUS(child_status));
        } else {
            printf("Child exited abnormally\n");
        }

        printf("Continuing to watcher now.\n\n");
    }
}