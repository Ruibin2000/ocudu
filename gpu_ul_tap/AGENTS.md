# Agent instructions: GPU uplink interference measurement

For AI coding agents helping students on this project. The student guide, with the steps, deliverables and glossary, is
[`plans/gpu_ul_measurement_plan.md`](../plans/gpu_ul_measurement_plan.md). Read it first. The repository-wide rules in
the root [`AGENTS.md`](../AGENTS.md) and [`apps/AGENTS.md`](../apps/AGENTS.md) also apply.

## How to work with the student

- The student is not a wireless engineer. When you use a wireless term, explain it with the guide's glossary (section 7),
  and use the same terms.
- Work on one step of the guide at a time. Build towards that step's deliverable and "Pass if" check, and tell the student
  how to run the check. Do not claim a step passes without the student running it.
- Keep changes small and explain each one, so the student can defend every line in review.
- Stop and ask the student before you change the ring layout, the real-time path (see invariants), or anything outside
  `gpu_ul_tap/`, `apps/units/flexible_o_du/` and the files named below.
- Do not commit, push or open PRs unless the student asks. PR base is `Ruibin2000/ocudu` main, never upstream OCUDU.
- When you fall into a trap not listed under "Known traps", add one line about it there.

## Architecture

```text
RU (ru_sdr split 8 / ru_ofh split 7.2)
  -> rx_tap_symbol_notifier (decorator)  -- memcpy per symbol --> POSIX shm ring (rx_tap_ring.h)
  -> upper_phy_ru_ul_adapter (unchanged)                                  |
FAPI UL_TTI.request / UL PDUs -- stage_allocations() --> tap                v
                                              gpu_consumer.py: reader thread -> compute thread (PyTorch) -> publisher
```

| File | Role | State |
| --- | --- | --- |
| `rx_tap_ring.h` | Single-producer lock-free ring in POSIX shm. `RingHeader` 64 B, per slot `SlotHeader` 64 B + `AllocEntry[64]` (16 B each) + payload `[port][symbol][subcarrier]` cbf16. Seqlock via `seq_begin`/`seq_end`, `write_seq` in the ring header. | Tested with the synthetic producer. |
| `rx_tap_symbol_notifier.h` | Decorator around `ru_uplink_plane_rx_symbol_notifier` (`include/ocudu/ru/ru_uplink_plane.h`). Copies symbols via `resource_grid_reader::get_view()`, forwards, commits at the last symbol. Taps one sector. | Compiles against OCUDU headers. Not wired in. |
| `synthetic_producer.cpp` | Fake gNB: noise, hopping PUSCH, PUCCH at edges, interferer on PRBs `n_prb/2+8 .. +2` (33-35 at 51 PRBs). | Tested. |
| `gpu_consumer.py` | `RingReader`, `UlInterferenceMeasure(x complex64 [B,P,S,K], occupied bool [B,S,N])`, `publish_summary` stub. | Tested on CPU only. CUDA path not run. |

## Invariants

1. **Real-time path.** `on_new_uplink_symbol`, `on_new_prach_window_data` and anything called from RU, lower PHY or
   FAPI threads: no heap allocation, no blocking locks (a mutex held only for a short fixed-size copy is tolerated), no
   file or socket I/O, no logging above debug, no exceptions. Never wait on the consumer.
2. **Forward unchanged.** The decorator forwards every call to the wrapped notifier, in order, with the same arguments,
   whatever happens to the tap. A tap failure must not affect the gNB.
3. **Off by default.** With the tap disabled the gNB behaves exactly as upstream: same objects, same notifier.
4. **Ring layout is shared.** `rx_tap_ring.h` and the dtypes at the top of `gpu_consumer.py` must match. Change both in the
   same commit, bump `kVersion` and `VERSION` together, and rerun the synthetic test (`README.md`).
5. **Lifetime.** The tap must outlive the radio unit, which calls it from its own threads until it is stopped and
   destroyed. `flexible_o_du_impl` destroys members in reverse declaration order, so a tap member must be declared before
   `ru` (`apps/units/flexible_o_du/split_helpers/flexible_o_du_impl.h`).
6. **Platform independence of the measurement.** `UlInterferenceMeasure` takes tensors and knows nothing about OCUDU,
   the ring or the radio. Keep OCUDU specifics in `RingReader` and the publisher.

## Code map by step

### Step 3: config option and wiring

- Options go in `du_low_unit_expert_upper_phy_config` (`apps/units/flexible_o_du/o_du_low/du_low_config.h`), the CLI
  schema (`du_low_config_cli11_schema.cpp`, `configure_cli11_expert_phy_args`), the YAML writer
  (`du_low_config_yaml_writer.cpp`) and, for cross-checks, `du_low_config_validator.cpp`. Follow `apps/AGENTS.md` for
  what goes in the schema vs the validator. Time-valued options use `std::chrono` types (root `AGENTS.md`).
- Wiring point: `apps/units/flexible_o_du/split_helpers/flexible_o_du_factory.cpp`, where
  `flexible_o_du_ru_dependencies` is built with `du_impl->get_upper_ru_ul_adapter()` as `symbol_notifier` (struct in
  `flexible_o_du_configs.h`). The same notifier serves split 8 and split 7.2.
- Tap parameters from the factory's `du_cells`: ports `cell.ran.ul_carrier.nof_ant`, 14 symbols (normal CP), subcarriers
  `NOF_SUBCARRIERS_PER_RB * nof_prb` (`include/ocudu/ran/resource_block.h`) with `nof_prb` from `band_helper::get_n_rbs_from_bw(...)` as in `generate_o_du_low_config`, SCS from
  `dl_cfg_common.init_dl_bwp.generic_params.scs`.
- Include path: `ocudu_flexible_o_du_helpers` already has `${CMAKE_SOURCE_DIR}`, so `#include
  "gpu_ul_tap/rx_tap_symbol_notifier.h"` works. Keep that include out of `flexible_o_du_impl.h` (forward-declare, the
  destructor is already out of line).
- `rxtap::Producer` throws `std::runtime_error` if `shm_open`/`mmap` fail. Catch it in the factory, log a warning and run
  without the tap (invariant 2).

### Step 5: allocation table

- Recommended hook: decorate the per-sector FAPI `p7_requests_gateway` (`include/ocudu/fapi/p7/p7_requests_gateway.h`)
  in the factory, where `sector_dependencies.p7_gateway` is set. Intercept `send_ul_tti_request`, convert its PDUs, call
  `stage_allocations()`, then forward. One place covers PUSCH, PUCCH, SRS and PRACH. PDU structs:
  `include/ocudu/fapi/p7/messages/ul_{pusch,pucch,srs,prach}_pdu.h`, container `ul_tti_request.h`.
- Alternative: `include/ocudu/phy/upper/uplink_pdu_slot_repository.h` (`add_pusch_pdu`, `add_pucch_pdu`,
  `add_srs_pdu`) inside the upper PHY. PRACH does not go through it.
- Conversion rules:
  - `start_prb` is relative to the grid start: add the PDU's `bwp` start to BWP-relative RB indices.
  - PUSCH: FAPI carries resource allocation type 1 only (`resource_allocation_1.vrbs`, a contiguous interval) over
    `symbols`. If another path delivers a non-contiguous allocation, write one entry per contiguous run.
  - PUCCH: `prbs` over `symbols`. With `second_hop_prb` set, two entries: first half of the symbols at `prbs`, second
    half at `second_hop_prb`.
  - SRS: mark the whole sounding bandwidth on its symbols, ignoring the comb.
  - PRACH: occasion PRBs converted to PUSCH-SCS PRBs, over the occasion's symbols.
  - Overflowing the 64-entry table: mark the slot's table invalid rather than drop entries.
- `stage_allocations()` currently copies a `std::vector` under a mutex. That allocates in the real-time path. Replace it
  with a fixed-capacity array before wiring it.
- UL_TTI.request arrives a few slots before the slot is received, so the 16-entry staging ring suffices only while the
  lead is under 16 slots. Check it against `max_proc_delay` and the TDD pattern.
- TDD special slot: the downlink and guard symbols carry no uplink. Mark them occupied, from `cell.ran.tdd_cfg`.

### Step 2: offline dump format

- Options under `log:` (`du_low_config_cli11_schema.cpp`, `configure_cli11_log_args`). The dump is trigger-based
  (`lib/phy/upper/upper_phy_rx_results_notifier_printer_decorator.h`): only slots with a PUSCH result that meets a
  trigger are written. `phy_rx_symbols_pusch_threshold_sinr_dB: 100` writes every PUSCH slot.
- Format (`lib/phy/upper/rx_resource_grid_printer_backend.h`): raw `cf_t` (complex64), per slot
  `[port][symbol][subcarrier]` for the selected ports, no header. Slot numbers are only in the PHY log
  (`phy_level: info`): `RX_SYMBOL: sector=.. offset=.. size=..`, with `offset` and `size` in complex samples.

### Step 6: reserved PRBs

- `pusch` section, `--start_rb` / `--end_rb`
  (`apps/units/flexible_o_du/o_du_high/du_high/du_high_config_cli11_schema.cpp`). PUCCH resources are configured
  separately and stay where they are.

### Steps 8 and 9: reporting

- Step 8: replace `publish_summary` in `gpu_consumer.py`. Keep it non-blocking for the compute thread (UDP or a ZMQ
  PUB socket with a bounded queue).
- Step 9: custom measurements in `lib/e2/e2sm/e2sm_kpm/e2sm_kpm_du_meas_provider_impl.cpp`. Feed it from the consumer
  through a second small shm region written by the consumer and read by the DU, with the same seqlock pattern.
  Never make the DU wait for it. Check which KPM version and metric names the FlexRIC xApp expects.

## Testing

- Linux only (POSIX shm, `/dev/shm`). The gNB and the consumer run on the DGX Spark (aarch64).
- Synthetic end-to-end test: see `README.md`. Use it as a regression test for any ring or consumer change.
- Real-time check: late/underflow/overflow counts in the gNB console with the tap off vs on, same iperf load.
- Consumer counters: `overruns` (ring lapped the reader), `torn` (slot overwritten during copy), `discarded` (compute too
  slow).

## Known traps

- `expert_phy` already has `--enable_phy_tap` and `--phy_tap_arguments`, for an unrelated PHY tap plugin. Use distinct
  names for this tap, for example `enable_ul_grid_tap` and `ul_grid_tap_shm_name`.
- The RU may call `on_new_uplink_symbol` with `symbol_id` jumping ahead (several symbols ready at once). The decorator
  copies every symbol up to `symbol_id`; keep that behaviour.
- The consumer starts at the newest slot, not slot 0. Start it while the producer runs.
- With no valid allocation table the consumer treats the whole slot as occupied, so a live run before step 5 reports no
  measurable PRBs. That is expected.
- Forgetting PUCCH or SRS in the allocation table makes the noise floor rise with UE load. This is the main symptom of
  an incomplete table.

## Later: 100 MHz, O-RU, Aerial

- OCUDU with an O-RU (split 7.2): the same decorator works on the `ru_ofh` path. Check that the O-RU returns
  unscheduled PRBs: the DU's C-plane decides which PRBs and symbols come back. Example configs:
  `configs/gnb_ru_ran550_tdd_n78_100mhz_4x2.yml`, `configs/gnb_ru_rpqn4800e_tdd_n78_20mhz_2x2.yml`.
- NVIDIA Aerial L1 (GPU PHY): the tap is replaced by an E3 client of the Aerial dApp framework producing the same
  `(x, occupied)` tensors. The GPU is shared with the PHY's slot deadlines; check whether MIG is available on the GB10.
- Sizes: 4 ports, 273 PRBs, about 0.7 MB per slot (1.4 GB/s at 30 kHz). Nothing may assume 2 ports or 51 PRBs.
