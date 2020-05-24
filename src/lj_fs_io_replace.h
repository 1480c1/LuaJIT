// This file is copyrighted.

#ifndef _LJ_FS_IO_REPLACE
#define _LJ_FS_IO_REPLACE

#include "lj_fs_io.h"

#ifdef __MINGW32__

#define dev_t lin_dev_t
#define ino_t lin_ino_t

#define open(...) lin_open(__VA_ARGS__)
#define creat(...) lin_creat(__VA_ARGS__)
#define fopen(...) lin_fopen(__VA_ARGS__)
#define opendir(...) lin_opendir(__VA_ARGS__)
#define readdir(...) lin_readdir(__VA_ARGS__)
#define closedir(...) lin_closedir(__VA_ARGS__)
#define mkdir(...) lin_mkdir(__VA_ARGS__)
#define getcwd(...) lin_getcwd(__VA_ARGS__)
#define getenv(...) lin_getenv(__VA_ARGS__)

#undef lseek
#define lseek(...) lin_lseek(__VA_ARGS__)

// Affects both "stat()" and "struct stat".
#undef stat
#define stat lin_stat

#undef fstat
#define fstat(...) lin_fstat(__VA_ARGS__)

#define utime(...) _utime(__VA_ARGS__)
#define utimbuf _utimbuf

#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef FD_CLOEXEC
#define FD_CLOEXEC 0
#endif

#endif /* __MINGW32__ */

#endif
