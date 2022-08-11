#pragma once

#include <tbb/concurrent_unordered_map.h>

#include <memory>

#include "file.h"

namespace ulayfs {

extern bool initialized;
extern tbb::concurrent_unordered_map<int, std::shared_ptr<dram::File>> files;

static std::shared_ptr<dram::File> get_file(int fd) {
  if (!initialized) return {};
  if (fd < 0) return {};
  auto it = files.find(fd);
  if (it != files.end()) return it->second;
  return {};
}

template <typename... Args>
static auto add_file(int fd, Args&&... args) {
  return files.emplace(
      fd, std::make_shared<dram::File>(fd, std::forward<Args>(args)...));
}
}  // namespace ulayfs
