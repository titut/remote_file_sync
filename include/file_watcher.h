// file_watcher.h
#ifndef FILE_WATCHER_H
#define FILE_WATCHER_H

void start_file_watcher(const char *path, void (*on_change)(const char *filename));

#endif
