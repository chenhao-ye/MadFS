#pragma once

#include "bitmap.h"
#include "layout.h"

namespace ulayfs::pmem {
/**
 * The base class for all the blocks
 *
 * Remove copy/move constructor and assignment operator to avoid accidental copy
 */
class BaseBlock {
 public:
  BaseBlock(BaseBlock const&) = delete;
  BaseBlock(BaseBlock&&) = delete;
  BaseBlock& operator=(BaseBlock const&) = delete;
  BaseBlock& operator=(BaseBlock&&) = delete;
};

/**
 * In the current design, the inline bitmap in the meta block can manage 16k
 * blocks (64 MB in total); after that, every 32k blocks (128 MB) will have its
 * first block as the bitmap block that manages its allocation.
 * We assign "bitmap_block_id" to these bitmap blocks, where id=0 is the inline
 * one in the meta block (LogicalBlockIdx=0); bitmap block id=1 is the block
 * with LogicalBlockIdx 16384; id=2 is the one with LogicalBlockIdx 32768, etc.
 */
class BitmapBlock : public BaseBlock {
 private:
  Bitmap bitmaps[NUM_BITMAP];

 public:
  // first bit of is the bitmap block itself
  void init() { bitmaps[0].set_allocated(0); }

  // allocate one block; return the index of allocated block
  // accept a hint for which bit to start searching
  // usually hint can just be the last idx return by this function
  BitmapLocalIdx alloc_one(BitmapLocalIdx hint = 0) {
    return Bitmap::alloc_one(bitmaps, NUM_BITMAP, hint);
  }

  // 64 blocks are considered as one batch; return the index of the first block
  BitmapLocalIdx alloc_batch(BitmapLocalIdx hint = 0) {
    return Bitmap::alloc_batch(bitmaps, NUM_BITMAP, hint);
  }

  // map `bitmap_local_idx` from alloc_one/all to the LogicalBlockIdx
  static LogicalBlockIdx get_block_idx(BitmapBlockId bitmap_block_id,
                                       BitmapLocalIdx bitmap_local_idx) {
    if (bitmap_block_id == 0) return bitmap_local_idx;
    return (bitmap_block_id << BITMAP_BLOCK_CAPACITY_SHIFT) +
           INLINE_BITMAP_CAPACITY + bitmap_local_idx;
  }

  // make bitmap id to its block idx
  static LogicalBlockIdx get_bitmap_block_idx(BitmapBlockId bitmap_block_id) {
    return get_block_idx(bitmap_block_id, 0);
  }

  // reverse mapping of get_bitmap_block_idx
  static BitmapBlockId get_bitmap_block_id(LogicalBlockIdx idx) {
    return (idx - INLINE_BITMAP_CAPACITY) >> BITMAP_BLOCK_CAPACITY_SHIFT;
  }
};

class TxLogBlock : public BaseBlock {
  std::atomic<LogicalBlockIdx> prev;
  std::atomic<LogicalBlockIdx> next;
  TxEntry tx_entries[NUM_TX_ENTRY];

 public:
  /**
   * a static helper function for appending a TxEntry
   * also used for managing MetaBlock::inline_tx_entries
   *
   * @param entries a pointer to an array of tx entries
   * @param num_entries the total number of entries in the array
   * @param entry the target entry to be appended
   * @param hint hint to the tail of the log
   * @return the TxEntry local index and whether the operation is successful
   */
  static TxLocalIdx try_append(TxEntry entries[], uint16_t num_entries,
                               TxEntry entry, TxLocalIdx hint) {
    for (TxLocalIdx idx = hint; idx <= num_entries - 1; ++idx) {
      uint64_t expected = 0;
      if (__atomic_compare_exchange_n(&entries[idx].data, &expected, entry.data,
                                      false, __ATOMIC_RELEASE,
                                      __ATOMIC_ACQUIRE)) {
        persist_cl_fenced(&entries[idx]);
        return idx;
      }
    }
    return -1;
  }

  TxLocalIdx try_begin(TxBeginEntry begin_entry, TxLocalIdx hint_tail = 0) {
    return try_append(tx_entries, NUM_TX_ENTRY, begin_entry, hint_tail);
  }

  TxLocalIdx try_commit(TxCommitEntry commit_entry, TxLocalIdx hint_tail = 0) {
    // FIXME: this one is actually wrong. In OCC, we have to verify there is no
    // new transaction overlap with our range
    return try_append(tx_entries, NUM_TX_ENTRY, commit_entry, hint_tail);
  }

  TxEntry get_entry(TxLocalIdx idx) {
    assert(idx >= 0 && idx < NUM_TX_ENTRY);
    return tx_entries[idx];
  }

  [[nodiscard]] LogicalBlockIdx get_next_block_idx() const {
    return next.load(std::memory_order_acquire);
  }

  /**
   * Set the next block index
   * @return true on success, false if there is a race condition
   */
  bool set_next_block_idx(LogicalBlockIdx next) {
    LogicalBlockIdx expected = 0;
    bool success = this->next.compare_exchange_strong(
        expected, next, std::memory_order_release, std::memory_order_acquire);
    persist_cl_fenced(&this->next);
    return success;
  }
};

// LogEntryBlock is per-thread to avoid contention
class LogEntryBlock : public BaseBlock {
  LogEntry log_entries[NUM_LOG_ENTRY];

 public:
  /**
   * @param log_entry the log entry to be appended to the block
   * @param tail_idx the current log tail
   */
  void append(LogEntry log_entry, LogLocalIdx tail_idx) {
    log_entries[tail_idx] = log_entry;
    persist_cl_fenced(&log_entries[tail_idx]);
  };

  [[nodiscard]] LogEntry get_entry(LogLocalIdx idx) {
    assert(idx >= 0 && idx < NUM_LOG_ENTRY);
    return log_entries[idx];
  }
};

class DataBlock : public BaseBlock {
 public:
  char data[BLOCK_SIZE];
};

/*
 * LogicalBlockIdx 0 -> MetaBlock; other blocks can be any type of blocks
 */
class MetaBlock : public BaseBlock {
 private:
  // contents in the first cache line
  union {
    struct {
      // file signature
      char signature[SIGNATURE_SIZE];

      // file size in bytes (logical size to users)
      uint64_t file_size;

      // total number of blocks actually in this file (including unused ones)
      uint32_t num_blocks;

      // if inline_tx_entries is used up, this points to the next log block
      TxEntryIdx tx_log_head;

      // hint to find tx log tail; not necessarily up-to-date
      TxEntryIdx tx_log_tail;
    };

    // padding avoid cache line contention
    char cl1[CACHELINE_SIZE];
  };

  union {
    // address for futex to lock, 4 bytes in size
    // this lock is ONLY used for ftruncate
    Futex meta_lock;

    // set futex to another cacheline to avoid futex's contention affect
    // reading the metadata above
    char cl2[CACHELINE_SIZE];
  };

  // for the rest of 62 cache lines:
  // 32 cache lines for bitmaps (~16k blocks = 64M)
  Bitmap inline_bitmaps[NUM_INLINE_BITMAP];

  // 30 cache lines for tx log (~120 txs)
  TxEntry inline_tx_entries[NUM_INLINE_TX_ENTRY];

  static_assert(sizeof(inline_bitmaps) == 32 * CACHELINE_SIZE,
                "inline_bitmaps must be 32 cache lines");

  static_assert(sizeof(inline_tx_entries) == 30 * CACHELINE_SIZE,
                "inline_tx_entries must be 30 cache lines");

 public:
  /**
   * only called if a new file is created
   * We can assume that all other fields are zero-initialized upon ftruncate
   */
  void init() {
    // the first block is always used (by MetaBlock itself)
    meta_lock.init();
    memcpy(signature, FILE_SIGNATURE, SIGNATURE_SIZE);

    persist_cl_fenced(&cl1);
  }

  // check whether the meta block is valid
  bool is_valid() {
    return std::memcmp(signature, FILE_SIGNATURE, SIGNATURE_SIZE) == 0;
  }

  // acquire/release meta lock (usually only during allocation)
  // we don't need to call persistence since futex is robust to crash
  void lock() { meta_lock.acquire(); }
  void unlock() { meta_lock.release(); }

  /*
   * Getters and setters
   */

  // called by other public functions with lock held
  void set_num_blocks_no_lock(uint32_t num_blocks) {
    this->num_blocks = num_blocks;
    persist_cl_fenced(&cl1);
  }

  void set_tx_log_head(TxEntryIdx tx_log_head) {
    if (tx_log_head <= this->tx_log_head) return;
    this->tx_log_head = tx_log_head;
    persist_cl_fenced(&cl1);
  }

  void set_tx_log_tail(TxEntryIdx tx_log_tail) {
    if (tx_log_tail <= this->tx_log_tail) return;
    this->tx_log_tail = tx_log_tail;
    persist_cl_fenced(&cl1);
  }

  [[nodiscard]] uint32_t get_num_blocks() const { return num_blocks; }
  [[nodiscard]] TxEntryIdx get_tx_log_head() const { return tx_log_head; }
  [[nodiscard]] TxEntryIdx get_tx_log_tail() const { return tx_log_tail; }

  [[nodiscard]] TxEntry get_inline_tx_entry(TxLocalIdx idx) const {
    assert(idx >= 0 && idx < NUM_INLINE_TX_ENTRY);
    return inline_tx_entries[idx];
  }

  /*
   * Methods for inline metadata
   */

  // allocate one block; return the index of allocated block
  // accept a hint for which bit to start searching
  // usually hint can just be the last idx return by this function
  BitmapLocalIdx inline_alloc_one(BitmapLocalIdx hint = 0) {
    return Bitmap::alloc_one(inline_bitmaps, NUM_INLINE_BITMAP, hint);
  }

  // 64 blocks are considered as one batch; return the index of the first
  // block
  BitmapLocalIdx inline_alloc_batch(BitmapLocalIdx hint = 0) {
    return Bitmap::alloc_batch(inline_bitmaps, NUM_INLINE_BITMAP, hint);
  }

  TxLocalIdx inline_try_begin(TxBeginEntry begin_entry,
                              TxLocalIdx hint_tail = 0) {
    return TxLogBlock::try_append(inline_tx_entries, NUM_INLINE_TX_ENTRY,
                                  begin_entry, hint_tail);
  }

  TxLocalIdx inline_try_commit(TxCommitEntry commit_entry,
                               TxLocalIdx hint_tail = 0) {
    // TODO: OCC
    return TxLogBlock::try_append(inline_tx_entries, NUM_INLINE_TX_ENTRY,
                                  commit_entry, hint_tail);
  }

  friend std::ostream& operator<<(std::ostream& out, const MetaBlock& block) {
    out << "MetaBlock: \n";
    out << "\tsignature: \"" << block.signature << "\"\n";
    out << "\tfilesize: " << block.file_size << "\n";
    out << "\tnum_blocks: " << block.num_blocks << "\n";
    out << "\ttx_log_head: " << block.tx_log_head << "\n";
    out << "\ttx_log_tail: " << block.tx_log_tail << "\n";
    return out;
  }
};

union Block {
  MetaBlock meta_block;
  BitmapBlock bitmap_block;
  TxLogBlock tx_log_block;
  LogEntryBlock log_entry_block;
  DataBlock data_block;
  char data[BLOCK_SIZE];
};

static_assert(sizeof(LogEntryIdx) == 5, "LogEntryIdx must of 5 bytes");
static_assert(sizeof(Bitmap) == 8, "Bitmap must of 64 bits");
static_assert(sizeof(TxEntry) == 8, "TxEntry must be 64 bits");
static_assert(sizeof(TxBeginEntry) == 8, "TxEntry must be 64 bits");
static_assert(sizeof(TxCommitEntry) == 8, "TxEntry must be 64 bits");
static_assert(sizeof(LogEntry) == 16, "LogEntry must of size 16 bytes");
static_assert(sizeof(MetaBlock) == BLOCK_SIZE,
              "MetaBlock must be of size BLOCK_SIZE");
static_assert(sizeof(BitmapBlock) == BLOCK_SIZE,
              "BitmapBlock must be of size BLOCK_SIZE");
static_assert(sizeof(TxLogBlock) == BLOCK_SIZE,
              "TxLogBlock must be of size BLOCK_SIZE");
static_assert(sizeof(LogEntryBlock) == BLOCK_SIZE,
              "LogEntryBlock must be of size BLOCK_SIZE");
static_assert(sizeof(DataBlock) == BLOCK_SIZE,
              "DataBlock must be of size BLOCK_SIZE");
static_assert(sizeof(Block) == BLOCK_SIZE, "Block must be of size BLOCK_SIZE");

}  // namespace ulayfs::pmem