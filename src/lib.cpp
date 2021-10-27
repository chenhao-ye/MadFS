#include "lib.h"

#include <cstdarg>
#include <cstdio>

#include "config.h"
#include "layout.h"
#include "posix.h"

namespace ulayfs {
extern "C" {
int open(const char* pathname, int flags, ...) {
  mode_t mode = 0;

  if (__OPEN_NEEDS_MODE(flags)) {
    va_list arg;
    va_start(arg, flags);
    mode = va_arg(arg, mode_t);
    va_end(arg);
  }

  auto file = new dram::File();
  int fd = file->open(pathname, flags, mode);
  if (file) files[fd] = file;
  return fd;
}

ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) {
  if (auto it = files.find(fd); it != files.end()) {
    return it->second->overwrite(buf, count, offset);
  } else {
    return posix::pwrite(fd, buf, count, offset);
  }
}

ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
  if (auto it = files.find(fd); it != files.end()) {
    return it->second->pread(buf, count, offset);
  } else {
    return posix::pread(fd, buf, count, offset);
  }
}

/**
 * Called when the shared library is first loaded
 *
 * Note that the global variables may not be initialized at this point
 * e.g., all the functions in the ulayfs::posix namespace
 */
void __attribute__((constructor)) ulayfs_ctor() {
  runtime_options.init();
  if (runtime_options.show_config) {
    std::cout << build_options << std::endl;
  }
}

/**
 * Called when the shared library is unloaded
 */
void __attribute__((destructor)) ulayfs_dtor() {}
}  // extern "C"
}  // namespace ulayfs
