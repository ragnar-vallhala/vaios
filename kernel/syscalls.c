/* newlib retargeting for bare metal. See kernel/CMakeLists.txt: this file is
   linked into a firmware as an OBJECT, not as an archive member, so these
   definitions win over libnosys's whatever the link order. _sbrk in particular
   returns 0 on purpose: vaios has its own heap allocator, and libnosys's _sbrk
   would grow newlib's malloc from `end`, which is where that heap starts. */
#include <sys/types.h>
#include <unistd.h>

/* Referenced by kernel/vaios.c: if this file is not part of the link, or loses
   a race with libnosys's own _sbrk/_write, the link fails instead of the
   firmware quietly using the wrong ones. */
const char v_newlib_retarget = 1;

caddr_t _sbrk(int incr) { return 0; }
int _write(int fd, const void *buf, size_t count) { return count; }
int _read(int fd, void *buf, size_t count) { return 0; }
int _close(int fd) { return -1; }
int _lseek(int fd, int offset, int whence) { return 0; }
void _exit(int status) { while(1); }
