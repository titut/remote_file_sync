// rfs_file.c
#define _GNU_SOURCE

#include "file_watcher.h"
#include "rfs_file.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>

int check_rfs_file_exists(char* file_path) {
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
        char *args[] = {"mkdir", "-p", folder_path, NULL};
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

    if(child_pid < 0) {
        // fork failed
        perror("fork failed");
        exit(EXIT_FAILURE);
    } else if(child_pid == 0) {
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

// Read the entire file at `path` into a heap buffer
int read_file_into_buf(const char *path, uint8_t **data_out, uint32_t *len_out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        // If file is missing, treat as empty file
        if (errno == ENOENT) {
            *data_out = NULL;
            *len_out  = 0;
            return 0;
        }
        return -1;
    }

    // Get file size
    struct stat sb;
    if (fstat(fd, &sb) != 0) {
        close(fd);
        return -1;
    }

    size_t n = (size_t)sb.st_size;
    uint8_t *buf = NULL;
    if (n) {
        // Allocate a buffer of size n
        buf = (uint8_t *)malloc(n);
        if (!buf) {
            close(fd);
            return -1;
        }
        // Read until we've received n bytes or EOF
        size_t got = 0;
        while (got < n) {
            ssize_t r = read(fd, buf + got, n - got);
            if (r == 0) break; // EOF
            if (r < 0) {
                if (errno == EINTR) continue; // Interrupted; retry
                // Real error: clean up and abort
                free(buf);
                close(fd);
                return -1;
            }
            got += (size_t)r;
        }
    }
    close(fd);

    *data_out = buf;
    *len_out  = (uint32_t)n;
    return 0;
}

// Atomically write new content to `path` using a temp file + rename
// This is used by the client when applying a new version pulled from the server
    // 1. Construct "<path>.tmp"
    // 2. Open temp file, write data, close
    // 3. rename(tmp, path) 
int atomic_write_local(const char *path, const uint8_t *data, uint32_t len) {
    char tmp[PATH_MAX];

    // Compose "<path>.tmp" safely
    int needed = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (needed < 0 || (size_t)needed >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    // Open the temporary file
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0) return -1;

    // Write the entire buffer
    if (len && write(fd, data, len) != (ssize_t)len) {
        close(fd);
        return -1;
    }

    // Ensure bytes are disk and close descriptor
    if (close(fd) != 0) return -1;

    // Replace the new file with the old one
    if (rename(tmp, path) != 0) return -1;
    return 0;
}