/*
 * Minimal POSIX syscall stubs for bare-metal ARM (Playdate device).
 *
 * vgmstream links against newlib functions that reference these syscalls
 * (via streamfile_stdio.c, miniz.c, etc.). We never call that code at
 * runtime — we use a custom Playdate streamfile — but the linker still
 * needs the symbols. These stubs satisfy the linker with no-op / error
 * returns.
 */
#ifdef TARGET_EXTENSION  /* device build only */

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#undef errno
extern int errno;

int _close(int fd)              { (void)fd; errno = EBADF;  return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; (void)st; errno = EBADF; return -1; }
int _isatty(int fd)             { (void)fd; return 0; }
int _lseek(int fd, int off, int w) { (void)fd; (void)off; (void)w; errno = EBADF; return -1; }
int _open(const char *p, int f, int m) { (void)p; (void)f; (void)m; errno = ENOENT; return -1; }
int _read(int fd, char *b, int l) { (void)fd; (void)b; (void)l; errno = EBADF; return -1; }
int _write(int fd, char *b, int l) { (void)fd; (void)b; (void)l; errno = EBADF; return -1; }
int _unlink(const char *p)      { (void)p; errno = ENOENT; return -1; }
int _stat(const char *p, struct stat *st) { (void)p; (void)st; errno = ENOENT; return -1; }
int _kill(int pid, int sig)     { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _getpid(void)               { return 1; }
int _gettimeofday(void *tv, void *tz) { (void)tv; (void)tz; return 0; }
void _exit(int status)          { (void)status; while (1) {} }
int dup(int fd)                 { (void)fd; errno = EBADF; return -1; }
int utime(const char *p, const void *t) { (void)p; (void)t; errno = ENOENT; return -1; }

#endif
