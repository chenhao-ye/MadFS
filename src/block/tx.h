#pragma once

#include <atomic>
#include <cassert>
#include <cstring>

#include "const.h"
#include "entry.h"
#include "idx.h"
#include "logging.h"
#include "utils.h"

namespace ulayfs::pmem {

class TxBlock : public noncopyable {
  std::atomic<TxEntry> tx_entries[NUM_TX_ENTRY_PER_BLOCK];
  // next is placed after tx_entires so that it could be flushed with tx_entries
  std::atomic<LogicalBlockIdx> next;
  // seq is used to construct total order between tx entries, so it must
  // increase monotonically
  // when compare two TxEntryIdx
  // if within same block, compare local index
  // if not, compare their block's seq number
  uint32_t tx_seq;

 public:
  [[nodiscard]] TxLocalIdx find_tail(TxLocalIdx hint = 0) const {
    return TxEntry::find_tail<NUM_TX_ENTRY_PER_BLOCK>(tx_entries, hint);
  }

  TxEntry try_append(TxEntry entry, TxLocalIdx idx) {
    return TxEntry::try_append(tx_entries, entry, idx);
  }

  // THIS FUNCTION IS NOT THREAD SAFE
  void store(TxEntry entry, TxLocalIdx idx) {
    tx_entries[idx].store(entry, std::memory_order_relaxed);
  }

  [[nodiscard]] TxEntry get(TxLocalIdx idx) const {
    assert(idx >= 0 && idx < NUM_TX_ENTRY_PER_BLOCK);
    return tx_entries[idx].load(std::memory_order_acquire);
  }

  // it should be fine not to use any fence since there will be fence for flush
  void set_tx_seq(uint32_t seq) { tx_seq = seq; }
  [[nodiscard]] uint32_t get_tx_seq() const { return tx_seq; }

  [[nodiscard]] LogicalBlockIdx get_next_tx_block() const {
    return next.load(std::memory_order_acquire);
  }

  /**
   * Set the next block index
   * @return true on success, false if there is a race condition
   */
  bool set_next_tx_block(LogicalBlockIdx block_idx) {
    LogicalBlockIdx expected = 0;
    return next.compare_exchange_strong(expected, block_idx,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire);
  }

  /**
   * flush the current block starting from `begin_idx` (including two pointers)
   *
   * @param begin_idx where to start flush
   */
  void flush_tx_block(TxLocalIdx begin_idx = 0) {
    persist_unfenced(&tx_entries[begin_idx],
                     sizeof(TxEntry) * (NUM_TX_ENTRY_PER_BLOCK - begin_idx) +
                         2 * sizeof(LogicalBlockIdx));
  }

  /**
   * flush a range of tx entries
   *
   * @param begin_idx
   */
  void flush_tx_entries(TxLocalIdx begin_idx, TxLocalIdx end_idx) {
    assert(end_idx > begin_idx);
    persist_unfenced(&tx_entries[begin_idx],
                     sizeof(TxEntry) * (end_idx - begin_idx));
  }
};

static_assert(sizeof(TxBlock) == BLOCK_SIZE,
              "TxBlock must be of size BLOCK_SIZE");

}  // namespace ulayfs::pmem
