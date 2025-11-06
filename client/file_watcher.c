// file_watcher.c
#include "file_watcher.h"
#include <sys/inotify.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#define EVENT_SIZE  (sizeof(struct inotify_event))
#define BUF_LEN     (1024 * (EVENT_SIZE + 16))

void start_file_watcher(const char *path, void (*on_change)(const char *filename)) {
    int fd, wd;
    char buffer[BUF_LEN];

    // 1. Initialize inotify
    fd = inotify_init();
    if (fd < 0) {
        perror("inotify_init");
        exit(EXIT_FAILURE);
    }

    // 2. Add a watch to the directory
    wd = inotify_add_watch(fd, path, IN_CREATE | IN_MODIFY | IN_DELETE);
    if (wd == -1) {
        fprintf(stderr, "Cannot watch '%s'\n", path);
        exit(EXIT_FAILURE);
    }

    printf("Watching directory: %s\n", path);

    // 3. Event loop
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
                if (on_change && (event->mask & (IN_CREATE | IN_MODIFY)))
                    on_change(event->name);
            }
            i += EVENT_SIZE + event->len;
        }
    }

    // Cleanup
    inotify_rm_watch(fd, wd);
    close(fd);
}
