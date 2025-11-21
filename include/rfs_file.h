#ifndef RFS_FILE_H
#define RFS_FILE_H

#include <stdint.h>

int check_rfs_file_exists(char* file_path);
void create_rfs_folder(char* folder_path);
void create_rfs_file(char* file_path);

int  read_file_into_buf(const char *path, uint8_t **data_out, uint32_t *len_out);
int  atomic_write_local(const char *path, const uint8_t *data, uint32_t len);

#endif
