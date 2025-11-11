#include <sys/types.h>
#include <unistd.h>

caddr_t _sbrk(int incr) { return 0; }
int _write(int fd, const void *buf, size_t count) { return count; }
int _read(int fd, void *buf, size_t count) { return 0; }
int _close(int fd) { return -1; }
int _lseek(int fd, int offset, int whence) { return 0; }
void _exit(int status) { while(1); }
