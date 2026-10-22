// Small libc bridges required by the Switch graphics executable.
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <stdlib.h>

// Horizon homebrew has no set-user-ID execution or separate effective identity.
// SQLite's Unix VFS queries this before its ownership-preservation operation.
// Keep it consistent with the process identity libnx reports.
uid_t geteuid(void) {
  return getuid();
}

// posix_memalign over newlib's memalign. It used to come from the NVK compatibility object;
// it stays here, independent of the graphics stack.
int posix_memalign(void **out, size_t alignment, size_t size) {
  if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0 ||
      alignment % sizeof(void *) != 0)
    return EINVAL;
  const int saved_errno = errno;
  void *allocation = memalign(alignment, size ? size : 1);
  errno = saved_errno;
  if (!allocation)
    return ENOMEM;
  *out = allocation;
  return 0;
}

// Tint shares a command utility with desktop validation tools. Horizon cannot
// launch child processes; report that capability accurately if it is requested.
int execvp(const char *file, char *const arguments[]) {
  (void)file;
  (void)arguments;
  errno = ENOSYS;
  return -1;
}
pid_t waitpid(pid_t pid, int *status, int options) {
  (void)pid;
  (void)status;
  (void)options;
  errno = ENOSYS;
  return -1;
}
