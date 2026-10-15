// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU testbed contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

// rx_tap_symbol_notifier.h - decorator that copies the uplink resource grid into the rx_tap shared-memory ring.
//
// It wraps the ru_uplink_plane_rx_symbol_notifier that the Radio Unit calls for every received OFDM symbol, so it
// works unchanged for split 8 (ru_sdr, USRP) and split 7.2 (ru_ofh, O-RU). Each symbol is copied (one memcpy per
// port of the native cbf16 data) before the call is forwarded to the upper PHY; at the last symbol of the slot
// the slot is committed to the ring.
//
// Wiring (see plans/gpu_ul_measurement_plan.md, section 4): construct it around the upper_phy_ru_ul_adapter and pass
// it as flexible_o_du_ru_dependencies::symbol_notifier instead of the adapter.
//
// Allocations: whoever knows the UL PDUs for a slot (PUSCH/PUCCH/SRS/PRACH) calls stage_allocations() before the
// slot's last symbol arrives; the table is attached to the slot on commit. If nothing was staged the slot is
// published with the allocation table marked invalid and the consumer treats every RB as occupied.
#pragma once

#include "rx_tap_ring.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/support/shared_resource_grid.h"
#include "ocudu/ru/ru_uplink_plane.h"
#include <array>
#include <chrono>
#include <mutex>
#include <optional>
#include <vector>

namespace ocudu {

class rx_tap_symbol_notifier : public ru_uplink_plane_rx_symbol_notifier
{
public:
  rx_tap_symbol_notifier(ru_uplink_plane_rx_symbol_notifier& base_,
                         const std::string&                  shm_name,
                         unsigned                            nof_ports,
                         unsigned                            nof_symbols,
                         unsigned                            nof_subcarriers,
                         unsigned                            scs_khz,
                         unsigned                            tapped_sector = 0) :
    base(base_),
    producer(shm_name, /*n_slots=*/64, nof_ports, nof_symbols, nof_subcarriers, scs_khz, rxtap::kFmtCbf16),
    nof_ports(nof_ports),
    nof_symbols(nof_symbols),
    sector(tapped_sector)
  {
  }

  // See interface for documentation.
  void on_new_uplink_symbol(const ru_uplink_rx_symbol_context& context,
                            const shared_resource_grid&        grid,
                            bool                               is_valid) override
  {
    if (context.sector == sector) {
      tap_symbol(context, grid, is_valid);
    }
    base.on_new_uplink_symbol(context, grid, is_valid);
  }

  // See interface for documentation.
  void on_new_prach_window_data(const prach_buffer_context& context, shared_prach_buffer buffer) override
  {
    base.on_new_prach_window_data(context, std::move(buffer));
  }

  /// Stages the UL allocations of a slot. Call before the last symbol of that slot is received.
  void stage_allocations(slot_point slot, const std::vector<rxtap::AllocEntry>& allocs)
  {
    std::lock_guard<std::mutex> lock(staged_mutex);
    auto&                       e = staged[slot.system_slot() % staged.size()];
    e.slot                        = slot;
    e.allocs                      = allocs;
  }

private:
  void tap_symbol(const ru_uplink_rx_symbol_context& context, const shared_resource_grid& grid, bool is_valid)
  {
    // A new slot started before the previous one completed: publish what we have.
    if (open && open_slot != context.slot) {
      finish_slot();
    }
    if (!open) {
      writer    = producer.begin(context.slot.sfn(), context.slot.slot_index(), context.sector, now_ns());
      open      = true;
      open_slot = context.slot;
      next_symb = 0;
      all_valid = true;
    }
    all_valid = all_valid && is_valid;

    // symbol_id is the last processed symbol; copy every symbol not copied yet up to it.
    const resource_grid_reader& reader = grid.get_reader();
    const unsigned              ports  = std::min(nof_ports, reader.get_nof_ports());
    for (; next_symb <= context.symbol_id && next_symb < nof_symbols; ++next_symb) {
      for (unsigned port = 0; port != ports; ++port) {
        span<const cbf16_t> view  = reader.get_view(port, next_symb);
        const std::size_t   bytes = std::min<std::size_t>(view.size() * sizeof(cbf16_t), writer.symbol_bytes());
        std::memcpy(writer.symbol_ptr(port, next_symb), view.data(), bytes);
      }
    }

    if (context.symbol_id + 1 >= nof_symbols) {
      finish_slot();
    }
  }

  void finish_slot()
  {
    {
      std::lock_guard<std::mutex> lock(staged_mutex);
      auto&                       e = staged[open_slot.system_slot() % staged.size()];
      if (e.slot.has_value() && *e.slot == open_slot && next_symb >= nof_symbols && all_valid) {
        for (const auto& a : e.allocs) {
          writer.add_alloc(a);
        }
        writer.mark_alloc_valid();
      }
    }
    producer.commit(writer);
    open = false;
  }

  static uint64_t now_ns()
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  struct staged_entry {
    std::optional<slot_point>       slot;
    std::vector<rxtap::AllocEntry>  allocs;
  };

  ru_uplink_plane_rx_symbol_notifier& base;
  rxtap::Producer                     producer;
  const unsigned                      nof_ports;
  const unsigned                      nof_symbols;
  const unsigned                      sector;

  // State of the slot being written. Only touched from the RX symbol path (one thread per sector).
  rxtap::Producer::SlotWriter writer;
  bool                        open      = false;
  slot_point                  open_slot;
  unsigned                    next_symb = 0;
  bool                        all_valid = true;

  // Allocations staged by the PDU path, indexed by slot. The mutex is held only for a short copy.
  std::mutex                    staged_mutex;
  std::array<staged_entry, 16>  staged;
};

} // namespace ocudu
