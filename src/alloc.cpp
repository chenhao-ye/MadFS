#include "alloc.h"

#include <bit>
#include <cassert>
#include <cstdint>
#include <utility>

#include "bitmap.h"
#include "block.h"
#include "const.h"
#include "entry.h"
#include "file.h"
#include "idx.h"
#include "utils.h"

namespace ulayfs::dram {

LogicalBlockIdx Allocator::alloc(uint32_t num_blocks) {
  assert(num_blocks <= BITMAP_CAPACITY);

  if (!free_lists[num_blocks - 1].empty()) {
    LogicalBlockIdx lidx = free_lists[num_blocks - 1].back();
    free_lists[num_blocks - 1].pop_back();
    TRACE(
        "Allocator::alloc: allocating from free list (fully consumed): "
        "[n_blk: %d, lidx: %d]",
        lidx, num_blocks);
    return lidx;
  }

  for (uint32_t n = num_blocks + 1; n <= BITMAP_CAPACITY; ++n) {
    if (!free_lists[n - 1].empty()) {
      LogicalBlockIdx lidx = free_lists[n - 1].back();
      free_lists[n - 1].pop_back();
      free_lists[n - num_blocks - 1].push_back(lidx + num_blocks);
      TRACE(
          "Allocator::alloc: allocating from free list (partially consumed): "
          "[n_blk: %d, lidx: %d] -> [n_blk: %d, lidx: %d]",
          n, lidx, n - num_blocks, lidx + num_blocks);
      return lidx;
    }
  }

  bool is_found = false;
  uint32_t num_bits_left;
  BitmapIdx allocated_idx;
  LogicalBlockIdx allocated_block_idx;
  uint64_t allocated_bits;

retry:
  // then we have to allocate from global bitmaps
  // but try_alloc doesn't necessarily return the number of blocks we want
  allocated_idx =
      Bitmap::try_alloc(bitmap, NUM_BITMAP, recent_bitmap_idx, allocated_bits);
  PANIC_IF(allocated_idx < 0, "Allocator::alloc: failed to alloc from Bitmap");
  TRACE("Allocator::alloc: allocating from bitmap %d: 0x%lx", allocated_idx,
        allocated_bits);

  // add available bits to the local free list
  num_bits_left = BITMAP_CAPACITY;
  while (num_bits_left > 0) {
    // first remove all trailing ones
    uint32_t num_right_ones =
        static_cast<uint32_t>(std::countr_one(allocated_bits));
    allocated_bits >>= num_right_ones;
    num_bits_left -= num_right_ones;

    // allocated_bits should have many trailing zeros
    uint32_t num_right_zeros = std::min(
        static_cast<uint32_t>(std::countr_zero(allocated_bits)), num_bits_left);
    // if not, it means no bits left
    if (num_right_zeros == 0) break;

    if (!is_found && num_right_zeros >= num_blocks) {
      is_found = true;
      allocated_block_idx = allocated_idx + BITMAP_CAPACITY - num_bits_left;
      TRACE("Allocator::alloc: allocated blocks: [n_blk: %d, lidx: %d]",
            num_right_zeros, allocated_block_idx);
      if (num_right_zeros > num_blocks) {
        free_lists[num_right_zeros - num_blocks - 1].emplace_back(
            allocated_idx + BITMAP_CAPACITY - num_bits_left + num_blocks);
        TRACE("Allocator::alloc: unused blocks saved: [n_blk: %d, lidx: %d]",
              num_right_zeros - num_blocks,
              allocated_idx + BITMAP_CAPACITY - num_bits_left + num_blocks);
      }
    } else {
      free_lists[num_right_zeros - 1].emplace_back(
          allocated_idx + BITMAP_CAPACITY - num_bits_left);
      TRACE("Allocator::alloc: unused blocks saved: [n_blk: %d, lidx: %d]",
            num_right_zeros, allocated_idx + BITMAP_CAPACITY - num_bits_left);
    }
    allocated_bits >>= num_right_zeros;
    num_bits_left -= num_right_zeros;
  }
  // this recent is not useful because we have taken all bits; move on
  recent_bitmap_idx = allocated_idx + BITMAP_CAPACITY;

  // don't have the right size, retry
  if (!is_found) goto retry;
  return allocated_block_idx;
}

void Allocator::free(LogicalBlockIdx block_idx, uint32_t num_blocks) {
  if (block_idx == 0) return;
  TRACE("Allocator::alloc: adding to free list: [%u, %u)", block_idx,
        num_blocks + block_idx);
  free_lists[num_blocks - 1].emplace_back(block_idx);
}

void Allocator::free(const LogicalBlockIdx recycle_image[],
                     uint32_t image_size) {
  // try to group blocks
  // we don't try to merge the blocks with existing free list since the
  // searching is too expensive
  if (image_size == 0) return;
  uint32_t group_begin = 0;
  LogicalBlockIdx group_begin_lidx = 0;

  for (uint32_t curr = group_begin; curr < image_size; ++curr) {
    if (group_begin_lidx == 0) {  // new group not started yet
      if (recycle_image[curr] == 0) continue;
      // start a new group
      group_begin = curr;
      group_begin_lidx = recycle_image[curr];
    } else {
      // continue the group if it matches the expectation
      if (recycle_image[curr] == group_begin_lidx + (curr - group_begin))
        continue;
      TRACE("Allocator::free: adding to free list: [%u, %u)", group_begin_lidx,
            curr - group_begin + group_begin_lidx);
      free_lists[curr - group_begin - 1].emplace_back(group_begin_lidx);
      group_begin_lidx = recycle_image[curr];
      if (group_begin_lidx != 0) group_begin = curr;
    }
  }
  if (group_begin_lidx != 0) {
    TRACE("Allocator::free: adding to free list: [%u, %u)", group_begin_lidx,
          group_begin_lidx + image_size - group_begin);
    free_lists[image_size - group_begin - 1].emplace_back(group_begin_lidx);
  }
}

pmem::LogEntry* Allocator::alloc_log_entry(uint32_t num_blocks,
                                           LogEntryIdx& first_idx,
                                           pmem::LogEntryBlock*& first_block) {
  // for a log entry with only one logical block index, it takes 16 bytes
  // if smaller than that, do not try to allocate log entry there
  constexpr uint32_t min_required_size =
      pmem::LogEntry::fixed_size + sizeof(LogicalBlockIdx);
  if (!curr_log_block_idx || BLOCK_SIZE - curr_log_offset < min_required_size) {
    // no enough space left, do block allocation
    curr_log_block_idx = alloc(1);
    curr_log_block =
        &file->lidx_to_addr_rw(curr_log_block_idx)->log_entry_block;
    curr_log_offset = 0;
  }

  first_idx = {curr_log_block_idx, curr_log_offset};
  first_block = curr_log_block;
  pmem::LogEntry* first_entry = curr_log_block->get(curr_log_offset);
  pmem::LogEntry* curr_entry = first_entry;
  uint32_t needed_lidxs_cnt =
      ALIGN_UP(num_blocks, BITMAP_CAPACITY) >> BITMAP_CAPACITY_SHIFT;
  while (true) {
    assert(curr_entry);
    curr_log_offset += pmem::LogEntry::fixed_size;
    uint32_t avail_lidxs_cnt =
        (BLOCK_SIZE - curr_log_offset) / sizeof(LogicalBlockIdx);
    assert(avail_lidxs_cnt > 0);
    if (needed_lidxs_cnt <= avail_lidxs_cnt) {
      curr_entry->has_next = false;
      curr_entry->num_blocks = num_blocks;
      curr_log_offset += needed_lidxs_cnt * sizeof(LogicalBlockIdx);
      return first_entry;
    }

    curr_entry->has_next = true;
    curr_entry->num_blocks = avail_lidxs_cnt << BITMAP_CAPACITY_SHIFT;
    curr_log_offset += avail_lidxs_cnt * sizeof(LogicalBlockIdx);
    needed_lidxs_cnt -= avail_lidxs_cnt;
    num_blocks -= curr_entry->num_blocks;

    assert(curr_log_offset <= BLOCK_SIZE);
    if (BLOCK_SIZE - curr_log_offset < min_required_size) {
      curr_log_block_idx = alloc(1);
      curr_log_block =
          &file->lidx_to_addr_rw(curr_log_block_idx)->log_entry_block;
      curr_log_offset = 0;
      curr_entry->is_next_same_block = false;
      curr_entry->next.block_idx = curr_log_block_idx;
    } else {
      curr_entry->is_next_same_block = true;
      curr_entry->next.local_offset = curr_log_offset;
    }
    curr_entry = curr_log_block->get(curr_log_offset);
  }
}

}  // namespace ulayfs::dram
