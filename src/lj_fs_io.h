// This file is copyrighted.

#ifndef _LJ_FS_IO_H
#define _LJ_FS_IO_H

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __MINGW32__

int lin_open(const char *filename, int oflag, ...);
int lin_creat(const char *filename, int mode);
FILE *lin_fopen(const char *filename, const char *mode);
DIR *lin_opendir(const char *path);
struct dirent *lin_readdir(DIR *dir);
int lin_closedir(DIR *dir);
int lin_mkdir(const char *path, int mode);
char *lin_getcwd(char *buf, size_t size);
off_t lin_lseek(int fd, off_t offset, int whence);

// lin_stat types. MSVCRT's dev_t and ino_t are way too short to be unique.
typedef uint64_t lin_dev_t;
#ifdef _WIN64
typedef unsigned __int128 lin_ino_t;
#else
// 32-bit Windows doesn't have a __int128-type, which means ReFS file IDs will
// be truncated and might collide. This is probably not a problem because ReFS
// is not available in consumer versions of Windows.
typedef uint64_t lin_ino_t;
#endif

// lin_stat uses a different structure to MSVCRT, with 64-bit inodes
struct lin_stat {
    lin_dev_t st_dev;
    lin_ino_t st_ino;
    unsigned short st_mode;
    unsigned int st_nlink;
    short st_uid;
    short st_gid;
    lin_dev_t st_rdev;
    int64_t st_size;
    time_t st_atime;
    time_t st_mtime;
    time_t st_ctime;
};

int lin_stat(const char *path, struct lin_stat *buf);
int lin_fstat(int fd, struct lin_stat *buf);

char *lin_getenv(const char *name);

#endif /* __MINGW32__ */

#endif
