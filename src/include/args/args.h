#ifndef ARGS_H
#define ARGS_H

#include <stdint.h>
#include <pthread.h>
#include <signal.h>

struct args {
    int new_message;
    int pipefd[2];
    char *message;
    char *file_path;
    uint64_t last_version;
    int suppress_next;
    pthread_mutex_t mu;
    volatile sig_atomic_t* stop_flag_addr;
};

#endif
