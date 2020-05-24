// This file is copyrighted.

#define lj_fs_io_c
#define LUA_CORE

#ifdef __MINGW32__

#include <windows.h>

#include <assert.h>
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#include <strings.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>

#include "lj_fs_io.h"

// Like free(), but do not clobber errno and the win32 error state. It is
// unknown whether MSVCRT's free() clobbers these, but do not take any chances.
static void free_keep_err(void *ptr)
{
    int err = errno;
    DWORD bill = GetLastError();

    free(ptr);

    errno = err;
    SetLastError(bill);
}

static void set_errno_from_lasterror(void)
{
    // This just handles the error codes expected from CreateFile at the moment
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
        errno = ENOENT;
        break;
    case ERROR_SHARING_VIOLATION:
    case ERROR_ACCESS_DENIED:
        errno = EACCES;
        break;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        errno = EEXIST;
        break;
    case ERROR_PIPE_BUSY:
        errno = EAGAIN;
        break;
    default:
        errno = EINVAL;
        break;
    }
}

// Returns NULL + errno on failure.
static wchar_t *lin_utf8_to_winchar(const char *in)
{
    int res = MultiByteToWideChar(CP_UTF8, 0, in, -1, NULL, 0);
    if (res <= 0) {
        set_errno_from_lasterror();
        return NULL;
    }
    wchar_t *str = malloc(res * sizeof(wchar_t));
    if (!str) {
        errno = ENOMEM;
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, in, -1, str, res) <= 0) {
        set_errno_from_lasterror();
        return NULL;
    }
    return str;
}

// Returns NULL + errno on failure.
static char *lin_winchar_to_utf8(const wchar_t *in)
{
    int res = WideCharToMultiByte(CP_UTF8, 0, in, -1, NULL, 0, NULL, NULL);
    if (res <= 0) {
        set_errno_from_lasterror();
        return NULL;
    }
    char *str = malloc(res);
    if (!str) {
        errno = ENOMEM;
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, in, -1, str, res, NULL, NULL) <= 0) {
        set_errno_from_lasterror();
        return NULL;
    }
    return str;
}

static time_t filetime_to_unix_time(int64_t wintime)
{
    static const int64_t hns_per_second = 10000000ll;
    static const int64_t win_to_unix_epoch = 11644473600ll;
    return wintime / hns_per_second - win_to_unix_epoch;
}

static bool get_file_ids_win8(HANDLE h, lin_dev_t *dev, lin_ino_t *ino)
{
    FILE_ID_INFO ii;
    if (!GetFileInformationByHandleEx(h, FileIdInfo, &ii, sizeof(ii)))
        return false;
    *dev = ii.VolumeSerialNumber;
    // The definition of FILE_ID_128 differs between mingw-w64 and the Windows
    // SDK, but we can ignore that by just memcpying it. This will also
    // truncate the file ID on 32-bit Windows, which doesn't support __int128.
    // 128-bit file IDs are only used for ReFS, so that should be okay.
    assert(sizeof(*ino) <= sizeof(ii.FileId));
    memcpy(ino, &ii.FileId, sizeof(*ino));
    return true;
}

static bool get_file_ids(HANDLE h, lin_dev_t *dev, lin_ino_t *ino)
{
    // GetFileInformationByHandle works on FAT partitions and Windows 7, but
    // doesn't work in UWP and can produce non-unique IDs on ReFS
    BY_HANDLE_FILE_INFORMATION bhfi;
    if (!GetFileInformationByHandle(h, &bhfi))
        return false;
    *dev = bhfi.dwVolumeSerialNumber;
    *ino = ((lin_ino_t)bhfi.nFileIndexHigh << 32) | bhfi.nFileIndexLow;
    return true;
}

// Like fstat(), but with a Windows HANDLE
static int hstat(HANDLE h, struct lin_stat *buf)
{
    // Handle special (or unknown) file types first
    switch (GetFileType(h) & ~FILE_TYPE_REMOTE) {
    case FILE_TYPE_PIPE:
        *buf = (struct lin_stat){ .st_nlink = 1, .st_mode = _S_IFIFO | 0644 };
        return 0;
    case FILE_TYPE_CHAR: // character device
        *buf = (struct lin_stat){ .st_nlink = 1, .st_mode = _S_IFCHR | 0644 };
        return 0;
    case FILE_TYPE_UNKNOWN:
        errno = EBADF;
        return -1;
    }

    struct lin_stat st = { 0 };

    FILE_BASIC_INFO bi;
    if (!GetFileInformationByHandleEx(h, FileBasicInfo, &bi, sizeof(bi))) {
        errno = EBADF;
        return -1;
    }
    st.st_atime = filetime_to_unix_time(bi.LastAccessTime.QuadPart);
    st.st_mtime = filetime_to_unix_time(bi.LastWriteTime.QuadPart);
    st.st_ctime = filetime_to_unix_time(bi.ChangeTime.QuadPart);

    FILE_STANDARD_INFO si;
    if (!GetFileInformationByHandleEx(h, FileStandardInfo, &si, sizeof(si))) {
        errno = EBADF;
        return -1;
    }
    st.st_nlink = si.NumberOfLinks;

    // Here we pretend Windows has POSIX permissions by pretending all
    // directories are 755 and regular files are 644
    if (si.Directory) {
        st.st_mode |= _S_IFDIR | 0755;
    } else {
        st.st_mode |= _S_IFREG | 0644;
        st.st_size = si.EndOfFile.QuadPart;
    }

    if (!get_file_ids_win8(h, &st.st_dev, &st.st_ino)) {
        // Fall back to the Windows 7 method (also used for FAT in Win8)
        if (!get_file_ids(h, &st.st_dev, &st.st_ino)) {
            errno = EBADF;
            return -1;
        }
    }

    *buf = st;
    return 0;
}

int lin_stat(const char *path, struct lin_stat *buf)
{
    wchar_t *wpath = lin_utf8_to_winchar(path);
    if (!wpath)
        return -1;
    HANDLE h = CreateFileW(wpath, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | SECURITY_SQOS_PRESENT |
        SECURITY_IDENTIFICATION, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) {
        set_errno_from_lasterror();
        return -1;
    }

    int ret = hstat(h, buf);
    CloseHandle(h);
    return ret;
}

int lin_fstat(int fd, struct lin_stat *buf)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    // Use our hstat() function rather than MSVCRT's fstat() because ours
    // supports directories and device/inode numbers.
    return hstat(h, buf);
}

int lin_open(const char *filename, int oflag, ...)
{
    // Always use all share modes, which is useful for opening files that are
    // open in other processes, and also more POSIX-like
    static const DWORD share =
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    // Setting FILE_APPEND_DATA and avoiding GENERIC_WRITE/FILE_WRITE_DATA
    // will make the file handle use atomic append behavior
    static const DWORD append =
        FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA;

    DWORD access = 0;
    DWORD disposition = 0;
    DWORD flags = 0;

    switch (oflag & (_O_RDONLY | _O_RDWR | _O_WRONLY | _O_APPEND)) {
    case _O_RDONLY:
        access = GENERIC_READ;
        flags |= FILE_FLAG_BACKUP_SEMANTICS; // For opening directories
        break;
    case _O_RDWR:
        access = GENERIC_READ | GENERIC_WRITE;
        break;
    case _O_RDWR | _O_APPEND:
    case _O_RDONLY | _O_APPEND:
        access = GENERIC_READ | append;
        break;
    case _O_WRONLY:
        access = GENERIC_WRITE;
        break;
    case _O_WRONLY | _O_APPEND:
        access = append;
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    switch (oflag & (_O_CREAT | _O_EXCL | _O_TRUNC)) {
    case 0:
    case _O_EXCL: // Like MSVCRT, ignore invalid use of _O_EXCL
        disposition = OPEN_EXISTING;
        break;
    case _O_TRUNC:
    case _O_TRUNC | _O_EXCL:
        disposition = TRUNCATE_EXISTING;
        break;
    case _O_CREAT:
        disposition = OPEN_ALWAYS;
        flags |= FILE_ATTRIBUTE_NORMAL;
        break;
    case _O_CREAT | _O_TRUNC:
        disposition = CREATE_ALWAYS;
        break;
    case _O_CREAT | _O_EXCL:
    case _O_CREAT | _O_EXCL | _O_TRUNC:
        disposition = CREATE_NEW;
        flags |= FILE_ATTRIBUTE_NORMAL;
        break;
    }

    // Opening a named pipe as a file can allow the pipe server to impersonate
    // the calling process, which could be a security issue. Set SQOS flags, so
    // pipe servers can only identify our process, not impersonate it.
    if (disposition != CREATE_NEW)
        flags |= SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION;

    // Keep the same semantics for some MSVCRT-specific flags
    if (oflag & _O_TEMPORARY) {
        flags |= FILE_FLAG_DELETE_ON_CLOSE;
        access |= DELETE;
    }
    if (oflag & _O_SHORT_LIVED)
        flags |= FILE_ATTRIBUTE_TEMPORARY;
    if (oflag & _O_SEQUENTIAL) {
        flags |= FILE_FLAG_SEQUENTIAL_SCAN;
    } else if (oflag & _O_RANDOM) {
        flags |= FILE_FLAG_RANDOM_ACCESS;
    }

    // Open the Windows file handle
    wchar_t *wpath = lin_utf8_to_winchar(filename);
    if (!wpath)
        return -1;
    HANDLE h = CreateFileW(wpath, access, share, NULL, disposition, flags, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) {
        set_errno_from_lasterror();
        return -1;
    }

    // Map the Windows file handle to a CRT file descriptor. Note: MSVCRT only
    // cares about the following oflags.
    oflag &= _O_APPEND | _O_RDONLY | _O_RDWR | _O_WRONLY;
    oflag |= _O_NOINHERIT; // We never create inheritable handles
    int fd = _open_osfhandle((intptr_t)h, oflag);
    if (fd < 0) {
        CloseHandle(h);
        return -1;
    }

    return fd;
}

int lin_creat(const char *filename, int mode)
{
    return lin_open(filename, _O_CREAT | _O_WRONLY | _O_TRUNC, mode);
}

FILE *lin_fopen(const char *filename, const char *mode)
{
    if (!mode[0]) {
        errno = EINVAL;
        return NULL;
    }

    int rwmode;
    int oflags = 0;
    switch (mode[0]) {
    case 'r':
        rwmode = _O_RDONLY;
        break;
    case 'w':
        rwmode = _O_WRONLY;
        oflags |= _O_CREAT | _O_TRUNC;
        break;
    case 'a':
        rwmode = _O_WRONLY;
        oflags |= _O_CREAT | _O_APPEND;
        break;
    default:
        errno = EINVAL;
        return NULL;
    }

    // Parse extra mode flags
    for (const char *pos = mode + 1; *pos; pos++) {
        switch (*pos) {
        case '+': rwmode = _O_RDWR;  break;
        case 'x': oflags |= _O_EXCL; break;
        // Ignore unknown flags (glibc does too)
        default: break;
        }
    }

    // Open a CRT file descriptor
    int fd = lin_open(filename, rwmode | oflags);
    if (fd < 0)
        return NULL;

    // Add 'b' to the mode so the CRT knows the file is opened in binary mode
    char bmode[] = { mode[0], 'b', rwmode == _O_RDWR ? '+' : '\0', '\0' };
    FILE *fp = fdopen(fd, bmode);
    if (!fp) {
        close(fd);
        return NULL;
    }

    return fp;
}

// Windows' MAX_PATH/PATH_MAX/FILENAME_MAX is fixed to 260, but this limit
// applies to unicode paths encoded with wchar_t (2 bytes on Windows). The UTF-8
// version could end up bigger in memory. In the worst case each wchar_t is
// encoded to 3 bytes in UTF-8, so in the worst case we have:
//      wcslen(wpath) * 3 <= strlen(utf8path)
// Thus we need LIN_PATH_MAX as the UTF-8/char version of PATH_MAX.
// Also make sure there's free space for the terminating \0.
// (For codepoints encoded as UTF-16 surrogate pairs, UTF-8 has the same length.)
#define LIN_PATH_MAX (FILENAME_MAX * 3 + 1)

struct lin_dir {
    DIR crap;   // must be first member, unused, will be set to garbage
    _WDIR *wdir;
    union {
        struct dirent dirent;
        // dirent has space only for FILENAME_MAX bytes. _wdirent has space for
        // FILENAME_MAX wchar_t, which might end up bigger as UTF-8 in some
        // cases. Guarantee we can always hold _wdirent.d_name converted to
        // UTF-8 (see LIN_PATH_MAX).
        // This works because dirent.d_name is the last member of dirent.
        char space[LIN_PATH_MAX];
    };
};

DIR* lin_opendir(const char *path)
{
    wchar_t *wpath = lin_utf8_to_winchar(path);
    if (!wpath)
        return NULL;
    _WDIR *wdir = _wopendir(wpath);
    free_keep_err(wpath);
    if (!wdir)
        return NULL;
    struct lin_dir *ldir = calloc(1, sizeof(*ldir));
    if (!ldir) {
        _wclosedir(wdir);
        errno = ENOMEM;
        return NULL;
    }
    // DIR is supposed to be opaque, but unfortunately the MinGW headers still
    // define it. Make sure nobody tries to use it.
    memset(&ldir->crap, 0xCD, sizeof(ldir->crap));
    ldir->wdir = wdir;
    return (DIR*)ldir;
}

struct dirent* lin_readdir(DIR *dir)
{
    struct lin_dir *ldir = (struct lin_dir*)dir;
    struct _wdirent *wdirent = _wreaddir(ldir->wdir);
    if (!wdirent)
        return NULL;
    size_t buffersize = sizeof(ldir->space) - offsetof(struct dirent, d_name);
    if (WideCharToMultiByte(CP_UTF8, 0, wdirent->d_name, -1, ldir->dirent.d_name,
                            buffersize, NULL, NULL) <= 0)
    {
        set_errno_from_lasterror();
        return NULL;
    }
    ldir->dirent.d_ino = 0;
    ldir->dirent.d_reclen = 0;
    ldir->dirent.d_namlen = strlen(ldir->dirent.d_name);
    return &ldir->dirent;
}

int lin_closedir(DIR *dir)
{
    struct lin_dir *ldir = (struct lin_dir*)dir;
    int res = _wclosedir(ldir->wdir);
    free_keep_err(ldir);
    return res;
}

int lin_mkdir(const char *path, int mode)
{
    wchar_t *wpath = lin_utf8_to_winchar(path);
    if (!wpath)
        return -1;
    int res = _wmkdir(wpath);
    free(wpath);
    return res;
}

char *lin_getcwd(char *buf, size_t size)
{
    if (size >= SIZE_MAX / 3 - 1) {
        errno = ENOMEM;
        return NULL;
    }
    size_t wbuffer = size * 3 + 1;
    wchar_t *wres = malloc(sizeof(wchar_t) * wbuffer);
    if (!wres) {
        errno = ENOMEM;
        return NULL;
    }
    DWORD wlen = GetFullPathNameW(L".", wbuffer, wres, NULL);
    if (wlen >= wbuffer || wlen == 0) {
        free(wres);
        errno = wlen ? ERANGE : ENOENT;
        return NULL;
    }
    char *t = lin_winchar_to_utf8(wres);
    free_keep_err(wres);
    if (!t)
        return NULL;
    size_t st = strlen(t);
    if (st >= size) {
        free(t);
        errno = ERANGE;
        return NULL;
    }
    memcpy(buf, t, st + 1);
    free(t);
    return buf;
}

off_t lin_lseek(int fd, off_t offset, int whence)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_DISK) {
        errno = ESPIPE;
        return (off_t)-1;
    }
    return _lseeki64(fd, offset, whence);
}

static INIT_ONCE init_getenv_once = INIT_ONCE_STATIC_INIT;
static char **utf8_environ;

static void free_env(void)
{
    for (size_t n = 0; utf8_environ && utf8_environ[n]; n++)
        free(utf8_environ[n]);
    free(utf8_environ);
    utf8_environ = NULL;
}

// Note: UNIX getenv() returns static strings, and we try to do the same. Since
// using putenv() is not multithreading safe, we don't expect env vars to change
// at runtime, and converting/allocating them in advance is ok.
static void init_getenv(void)
{
    wchar_t *wenv = GetEnvironmentStringsW();
    if (!wenv)
        goto done;

    wchar_t *wenv_cur = wenv;
    size_t num_env = 0;
    while (1) {
        size_t len = wcslen(wenv_cur);
        if (!len)
            break;
        num_env++;
        wenv_cur += len + 1;
    }

    utf8_environ = calloc(sizeof(utf8_environ[0]), num_env + 1);
    if (!utf8_environ)
        goto done;

    wenv_cur = wenv;
    size_t cur_env = 0;
    while (1) {
        size_t len = wcslen(wenv_cur);
        if (!len)
            break;
        assert(cur_env < num_env);
        // On OOM, best-effort
        utf8_environ[cur_env] = lin_winchar_to_utf8(wenv_cur);
        if (utf8_environ[cur_env])
            cur_env++;
        wenv_cur += len + 1;
    }

done:
    FreeEnvironmentStringsW(wenv);

    // Avoid showing up in leak detectors etc.
    atexit(free_env);
}

char *lin_getenv(const char *name)
{
    BOOL pending;
    if (!InitOnceBeginInitialize(&init_getenv_once, 0, &pending, NULL))
        return NULL;
    if (pending) {
        init_getenv();
        InitOnceComplete(&init_getenv_once, 0, NULL);
    }

    size_t name_len = strlen(name);
    for (size_t n = 0; utf8_environ && utf8_environ[n]; n++) {
        char *env = utf8_environ[n];
        if (strncasecmp(env, name, name_len) == 0 && env[name_len] == '=')
            return env + name_len + 1;
    }

    return NULL;
}

#endif
