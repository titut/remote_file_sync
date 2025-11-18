// file_watcher.h
#ifndef FILE_WATCHER_H
#define FILE_WATCHER_H

void* start_file_watcher(void* arg);

void* socket_client(void* arg);

int check_rfs_folder_exists();

void create_rfs_file();

void create_rfs_folder();

int send_file(int socket, char* path);

#endif
