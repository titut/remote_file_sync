// file_watcher.h
#ifndef FILE_WATCHER_H
#define FILE_WATCHER_H

#include <stdint.h>
#include <pthread.h>

struct args {
    int new_message;
    char *message;
    char *file_path;
    uint64_t last_version;
    int suppress_next;
    pthread_mutex_t mu;
};

int check_rfs_folder_exists(char *file_path);
void create_rfs_folder(char *folder_path);
void create_rfs_file(char *file_path);

void* start_file_watcher(void* arg);
void* socket_client(void* arg);

int  read_file_into_buf(const char *path, uint8_t **data_out, uint32_t *len_out);
int  atomic_write_local(const char *path, const uint8_t *data, uint32_t len);

#endif
