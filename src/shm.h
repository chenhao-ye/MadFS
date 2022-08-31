#pragma once

#include <ostream>

#include "const.h"
#include "logging.h"
#include "posix.h"
#include "utils.h"

namespace ulayfs::dram {

class ShmMgr {
  int fd = -1;
  void* addr = nullptr;
  char path[SHM_PATH_LEN];

 public:
  ~ShmMgr() {
    if (fd >= 0) posix::close(fd);
    if (addr != nullptr) posix::munmap(addr, SHM_SIZE);
  }

  /**
   * Open and memory map the shared memory. If the shared memory does not exist,
   * create it.
   *
   * @param file_fd the file descriptor of the file that uses this shared memory
   * @param stat the stat of the file that uses this shared memory
   * @return the starting address of the shared memory
   */
  void* init(int file_fd, const struct stat& stat) {
    init_shm_path(file_fd, stat, path);

    // use posix::open instead of shm_open since shm_open calls open, which is
    // overloaded by ulayfs
    fd = posix::open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
      fd = create(path, stat.st_mode, stat.st_uid, stat.st_gid);
    }
    LOG_DEBUG("posix::open(%s) = %d", path, fd);

    addr = posix::mmap(nullptr, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fd, 0);
    if (addr == MAP_FAILED) {
      posix::close(fd);
      PANIC("mmap shared memory failed");
    }

    return addr;
  }

  /**
   * Remove the shared memory object associated.
   */
  void unlink() const { unlink_by_shm_path(path); }

  /**
   * Initialize the path of the shared memory object. Read from the xattr of the
   * file if it exists. Otherwise, generate a new path and set to the xattr.
   *
   * @param[in] file_fd the file descriptor of the file that uses this shared
   * memory
   * @param[in] stat the stat of the file that uses this shared memory
   * @param[out] path the returned path of the shared memory object
   */
  static void init_shm_path(int file_fd, const struct stat& stat, char* path) {
    ssize_t rc = fgetxattr(file_fd, SHM_XATTR_NAME, path, SHM_PATH_LEN);
    if (rc == -1 && errno == ENODATA) {  // no shm_path attribute, create one
      sprintf(path, "/dev/shm/ulayfs_%016lx_%013lx", stat.st_ino,
              (stat.st_ctim.tv_sec * 1000000000 + stat.st_ctim.tv_nsec) >> 3);
      rc = fsetxattr(file_fd, SHM_XATTR_NAME, path, SHM_PATH_LEN, 0);
      PANIC_IF(rc == -1, "failed to set shm_path attribute");
    } else if (rc == -1) {
      PANIC("failed to get shm_path attribute");
    }
  }

  /**
   * Create a shared memory object.
   *
   * @param shm_path the path of the shared memory object
   * @param mode the mode of the shared memory object
   * @param uid the uid of the shared memory object
   * @param gid the gid of the shared memory object
   * @return the file descriptor of the shared memory object
   */
  static int create(const char* shm_path, mode_t mode, uid_t uid, gid_t gid) {
    // We create a temporary file first, and then use `linkat` to put the file
    // into the directory `/dev/shm`. This ensures the atomicity of the creating
    // the shared memory file and setting its permission.
    int shm_fd =
        posix::open("/dev/shm", O_TMPFILE | O_RDWR | O_NOFOLLOW | O_CLOEXEC,
                    S_IRUSR | S_IWUSR);
    if (unlikely(shm_fd < 0)) {
      PANIC("create the temporary file failed");
    }

    // change permission and ownership of the new shared memory
    if (fchmod(shm_fd, mode) < 0) {
      posix::close(shm_fd);
      PANIC("fchmod on shared memory failed");
    }

    if (fchown(shm_fd, uid, gid) < 0) {
      posix::close(shm_fd);
      PANIC("fchown on shared memory failed");
    }

    if (posix::fallocate(shm_fd, 0, 0, static_cast<off_t>(SHM_SIZE)) < 0) {
      posix::close(shm_fd);
      PANIC("fallocate on shared memory failed");
    }

    // publish the created tmpfile.
    char tmpfile_path[PATH_MAX];
    sprintf(tmpfile_path, "/proc/self/fd/%d", shm_fd);
    int rc =
        linkat(AT_FDCWD, tmpfile_path, AT_FDCWD, shm_path, AT_SYMLINK_FOLLOW);
    if (rc < 0) {
      // Another process may have created a new shared memory before us. Retry
      // opening.
      posix::close(shm_fd);
      shm_fd = posix::open(shm_path, O_RDWR | O_NOFOLLOW | O_CLOEXEC,
                           S_IRUSR | S_IWUSR);
      if (shm_fd < 0) {
        PANIC("cannot open or create the shared memory object %s", shm_path);
      }
    }

    return shm_fd;
  }

  /**
   * Remove the shared memory object given its path.
   * @param shm_path the path of the shared memory object
   */
  static void unlink_by_shm_path(const char* shm_path) {
    int ret = posix::unlink(shm_path);
    LOG_TRACE("posix::unlink(%s) = %d", shm_path, ret);
    if (unlikely(ret < 0))
      LOG_WARN("Could not unlink shm file \"%s\": %m", shm_path);
  }

  /**
   * Remove the shared memory object given the path of the file that uses it.
   * @param filepath the path of the file that uses the shared memory object
   */
  static void unlink_by_file_path(const char* filepath) {
    char shm_path[SHM_PATH_LEN];
    if (getxattr(filepath, SHM_XATTR_NAME, &shm_path, SHM_PATH_LEN) <= 0)
      return;
    unlink_by_shm_path(shm_path);
  }

  friend std::ostream& operator<<(std::ostream& os, const ShmMgr& mgr) {
    os << "ShmMgr: fd = " << mgr.fd << ", addr = " << mgr.addr
       << ", path = " << mgr.path << "\n";
    return os;
  }
};

}  // namespace ulayfs::dram