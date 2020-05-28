// This file is copyrighted.

#ifndef _LJ_FS_IO_REPLACE
#define _LJ_FS_IO_REPLACE

#include "lj_fs_io.h"

#ifdef __MINGW32__

#define dev_t lin_dev_t
#define ino_t lin_ino_t

#define open(...) lin_open(...)
#define creat(...) lin_creat(...)
#define fopen(...) lin_fopen(...)
#define opendir(...) lin_opendir(...)
#define readdir(...) lin_readdir(...)
#define closedir(...) lin_closedir(...)
#define mkdir(...) lin_mkdir(...)
#define getcwd(...) lin_getcwd(...)
#define getenv(...) lin_getenv(...)

#undef lseek
#define lseek(...) lin_lseek(...)

// Affects both "stat()" and "struct stat".
#undef stat
#define stat lin_stat

#undef fstat
#define fstat(...) lin_fstat(...)

#define utime(...) _utime(...)
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
