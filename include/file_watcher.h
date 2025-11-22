// file_watcher.h
#ifndef FILE_WATCHER_H
#define FILE_WATCHER_H

#include <stdint.h>
#include <pthread.h>
#include <signal.h>

struct args {
    int new_message;
    char *message;
    char *file_path;
    uint64_t last_version;
    int suppress_next;
    pthread_mutex_t mu;
    volatile sig_atomic_t* stop_flag_addr;
};

void* start_file_watcher(void* arg);

#endif
