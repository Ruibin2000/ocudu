// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU testbed contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

// rx_tap_ring.h - lock-free single-producer ring in POSIX shared memory for tapping the uplink resource grid.
//
// Producer: the gNB (C++), called from the RX symbol path. Consumer: a separate process (gpu_consumer.py).
// The producer never blocks: when the consumer falls behind, old slots are overwritten and the consumer detects
// it through per-slot sequence numbers.
//
// Slot layout in shared memory:
//   [SlotHeader 64 B][AllocEntry x kMaxAlloc][payload: sample[n_ports][n_symb][n_subc]]
// The payload format is cbf16 (bfloat16 re, bfloat16 im; 4 bytes per RE), the native OCUDU resource grid format,
// so each OFDM symbol is a straight memcpy of resource_grid_reader::get_view(port, symbol).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace rxtap {

constexpr uint64_t kMagic     = 0x5258544150524E47ull; // "RXTAPRNG"
constexpr uint32_t kVersion   = 2;
constexpr uint32_t kMaxAlloc  = 64;
constexpr uint32_t kFmtCbf16  = 1; // bfloat16 I/Q, 4 bytes per RE.
constexpr uint32_t kFmtCf32   = 2; // float32 I/Q, 8 bytes per RE.

// Layout must match RING_HDR in gpu_consumer.py.
struct alignas(64) RingHeader {
  uint64_t              magic;
  uint32_t              version;
  uint32_t              n_slots;
  uint32_t              n_ports;    // RX antenna ports.
  uint32_t              n_symb;     // OFDM symbols per slot (14).
  uint32_t              n_subc;     // Subcarriers (n_prb * 12).
  uint32_t              sample_fmt; // kFmtCbf16 or kFmtCf32.
  uint64_t              slot_bytes; // SlotHeader + alloc table + payload, rounded up to 64.
  std::atomic<uint64_t> write_seq;  // Last fully written sequence number; 0 means none yet.
  uint32_t              scs_khz;    // Subcarrier spacing.
  uint32_t              max_alloc;  // Entries in the per-slot allocation table.
  uint64_t              reserved;
};
static_assert(sizeof(RingHeader) == 64, "RingHeader must be 64 bytes");

// Channel types in the allocation table.
enum : uint16_t { kPusch = 1, kPucch = 2, kSrs = 3, kPrach = 4 };

// One contiguous allocation in the slot. A non-contiguous PUSCH allocation is written as several entries.
struct AllocEntry {
  uint16_t type;      // kPusch, kPucch, kSrs, kPrach.
  uint16_t rnti;
  uint16_t start_prb; // Relative to the start of the grid.
  uint16_t nof_prb;
  uint8_t  start_symb;
  uint8_t  nof_symb;
  uint8_t  reserved[6];
};
static_assert(sizeof(AllocEntry) == 16, "AllocEntry must be 16 bytes");

// Layout must match SLOT_HDR in gpu_consumer.py.
struct alignas(64) SlotHeader {
  std::atomic<uint64_t> seq_begin; // Written before the payload.
  std::atomic<uint64_t> seq_end;   // Written after the payload.
  uint32_t              sfn;
  uint32_t              slot;
  uint32_t              sector;
  uint32_t              n_alloc;   // Valid entries in the allocation table; 0 if unknown.
  uint64_t              t_ns;      // Host timestamp (CLOCK_MONOTONIC or similar).
  uint32_t              flags;     // Bit 0: allocation table is valid.
  uint8_t               reserved[20];
};
static_assert(sizeof(SlotHeader) == 64, "SlotHeader must be 64 bytes");

class Producer
{
public:
  Producer(const std::string& name,
           uint32_t           n_slots,
           uint32_t           n_ports,
           uint32_t           n_symb,
           uint32_t           n_subc,
           uint32_t           scs_khz,
           uint32_t           sample_fmt = kFmtCbf16)
  {
    sample_bytes_  = (sample_fmt == kFmtCf32) ? 8 : 4;
    symbol_bytes_  = uint64_t(n_subc) * sample_bytes_;
    payload_bytes_ = uint64_t(n_ports) * n_symb * symbol_bytes_;
    slot_bytes_    = (sizeof(SlotHeader) + kMaxAlloc * sizeof(AllocEntry) + payload_bytes_ + 63) & ~uint64_t(63);
    total_         = sizeof(RingHeader) + slot_bytes_ * n_slots;

    int fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
      throw std::runtime_error("shm_open failed");
    }
    if (ftruncate(fd, total_) != 0) {
      close(fd);
      throw std::runtime_error("ftruncate failed");
    }
    void* p = mmap(nullptr, total_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
      throw std::runtime_error("mmap failed");
    }
    base_ = static_cast<uint8_t*>(p);
    std::memset(base_, 0, total_);
    // Avoid page faults in the real-time path (best effort; may need a raised RLIMIT_MEMLOCK).
    mlock(base_, total_);

    hdr_             = reinterpret_cast<RingHeader*>(base_);
    hdr_->version    = kVersion;
    hdr_->n_slots    = n_slots;
    hdr_->n_ports    = n_ports;
    hdr_->n_symb     = n_symb;
    hdr_->n_subc     = n_subc;
    hdr_->sample_fmt = sample_fmt;
    hdr_->slot_bytes = slot_bytes_;
    hdr_->scs_khz    = scs_khz;
    hdr_->max_alloc  = kMaxAlloc;
    hdr_->write_seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    hdr_->magic = kMagic; // The consumer waits for this before reading the header.
  }

  ~Producer()
  {
    if (base_ != nullptr) {
      munmap(base_, total_);
    }
  }

  Producer(const Producer&)            = delete;
  Producer& operator=(const Producer&) = delete;

  // Handle to a slot being written. Obtain with begin(), fill symbols and allocations, then commit().
  class SlotWriter
  {
  public:
    // Destination for one OFDM symbol of one port; copy exactly symbol_bytes() into it.
    void* symbol_ptr(uint32_t port, uint32_t symb) const
    {
      return payload_ + (uint64_t(port) * n_symb_ + symb) * symbol_bytes_;
    }
    uint64_t symbol_bytes() const { return symbol_bytes_; }

    // Appends one allocation. Returns false if the table is full.
    bool add_alloc(const AllocEntry& e)
    {
      if (hdr_->n_alloc >= kMaxAlloc) {
        return false;
      }
      allocs_[hdr_->n_alloc++] = e;
      return true;
    }
    void mark_alloc_valid() { hdr_->flags |= 1u; }

  private:
    friend class Producer;
    SlotHeader* hdr_          = nullptr;
    AllocEntry* allocs_       = nullptr;
    uint8_t*    payload_      = nullptr;
    uint64_t    seq_          = 0;
    uint64_t    symbol_bytes_ = 0;
    uint32_t    n_symb_       = 0;
  };

  // Starts writing the next slot. Single producer only: do not interleave two open slots.
  SlotWriter begin(uint32_t sfn, uint32_t slot, uint32_t sector, uint64_t t_ns)
  {
    const uint64_t seq = ++seq_;
    auto*          s   = reinterpret_cast<SlotHeader*>(base_ + sizeof(RingHeader) + (seq % hdr_->n_slots) * slot_bytes_);
    s->seq_begin.store(seq, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s->sfn     = sfn;
    s->slot    = slot;
    s->sector  = sector;
    s->n_alloc = 0;
    s->t_ns    = t_ns;
    s->flags   = 0;

    SlotWriter w;
    w.hdr_          = s;
    w.allocs_       = reinterpret_cast<AllocEntry*>(reinterpret_cast<uint8_t*>(s) + sizeof(SlotHeader));
    w.payload_      = reinterpret_cast<uint8_t*>(w.allocs_) + kMaxAlloc * sizeof(AllocEntry);
    w.seq_          = seq;
    w.symbol_bytes_ = symbol_bytes_;
    w.n_symb_       = hdr_->n_symb;
    return w;
  }

  // Publishes a slot started with begin().
  void commit(const SlotWriter& w)
  {
    w.hdr_->seq_end.store(w.seq_, std::memory_order_release);
    hdr_->write_seq.store(w.seq_, std::memory_order_release);
  }

private:
  uint8_t*    base_          = nullptr;
  RingHeader* hdr_           = nullptr;
  uint64_t    sample_bytes_  = 0;
  uint64_t    symbol_bytes_  = 0;
  uint64_t    payload_bytes_ = 0;
  uint64_t    slot_bytes_    = 0;
  uint64_t    total_         = 0;
  uint64_t    seq_           = 0;
};

} // namespace rxtap
