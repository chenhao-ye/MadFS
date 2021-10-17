#include "file.h"

namespace ulayfs::dram {

int File::open(const char* pathname, int flags, mode_t mode) {
  int ret;
  bool is_create = false;
  fd = posix::open(pathname, flags, mode);
  if (fd < 0) return fd;  // fail to open the file
  open_flags = flags;

  if (flags & O_CREAT) {
    struct stat stat_buf;
    ret = posix::fstat(fd, &stat_buf);
    if (ret) throw std::runtime_error("Fail to fstat!");
    is_create = stat_buf.st_size == 0;
  }

  if (is_create) {
    ret = posix::ftruncate(fd, LayoutOptions::prealloc_size);
    if (ret) throw std::runtime_error("Fail to ftruncate!");
  }

  meta = idx_map.init(fd);
  allocator.init(fd, meta, &idx_map);

  if (is_create)
    meta->init();
  else
    meta->verify_ready();
  return fd;
}

};  // namespace ulayfs::dram
