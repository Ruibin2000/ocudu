// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU testbed contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

// synthetic_producer.cpp - stands in for the gNB so the consumer can be developed and tested without a radio.
//
// Writes resource grids into the rx_tap ring at the real slot rate:
//   - complex Gaussian noise on every RE,
//   - a "UE" PUSCH allocation that moves around the band (rank 1 across the RX ports),
//   - PUCCH at both band edges,
//   - a narrowband "interferer" on a fixed set of PRBs that are never scheduled,
// together with the matching allocation table. The interferer is what the consumer should find.
//
// Build:  g++ -std=c++17 -O2 -o synthetic_producer synthetic_producer.cpp -lrt
// Run:    ./synthetic_producer [--name /rxtap] [--prb 51] [--ports 2] [--scs 30] [--slots 4000]

#include "rx_tap_ring.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

// float -> bfloat16 with round-to-nearest-even.
uint16_t to_bf16(float f)
{
  uint32_t u;
  std::memcpy(&u, &f, sizeof(u));
  u += 0x7fffu + ((u >> 16) & 1u);
  return static_cast<uint16_t>(u >> 16);
}

} // namespace

int main(int argc, char** argv)
{
  std::string name    = "/rxtap";
  uint32_t    n_prb   = 51; // 20 MHz at 30 kHz SCS.
  uint32_t    n_ports = 2;
  uint32_t    scs_khz = 30;
  uint64_t    n_run   = 4000;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string a = argv[i];
    if (a == "--name") {
      name = argv[i + 1];
    } else if (a == "--prb") {
      n_prb = std::stoul(argv[i + 1]);
    } else if (a == "--ports") {
      n_ports = std::stoul(argv[i + 1]);
    } else if (a == "--scs") {
      scs_khz = std::stoul(argv[i + 1]);
    } else if (a == "--slots") {
      n_run = std::stoull(argv[i + 1]);
    }
  }
  const uint32_t n_symb = 14, n_subc = n_prb * 12;
  const auto     slot_dur = std::chrono::microseconds(1000 * 15 / scs_khz);
  const uint32_t slots_per_frame = 10 * scs_khz / 15;

  rxtap::Producer prod(name, 64, n_ports, n_symb, n_subc, scs_khz, rxtap::kFmtCbf16);

  std::mt19937                    rng(1234);
  std::normal_distribution<float> g(0.f, 1.f);
  const float                     noise_std = std::sqrt(0.5f);         // Unit-power noise per RE.
  const float                     ue_amp    = std::pow(10.f, 20 / 20.f); // UE 20 dB above noise.
  const float                     intf_amp  = std::pow(10.f, 6 / 20.f);  // Interferer 6 dB above noise.
  const uint32_t                  intf_start = n_prb / 2 + 8, intf_len = 3;
  std::vector<uint16_t>           sym(2 * n_subc);

  auto next = std::chrono::steady_clock::now();
  for (uint64_t k = 0; k < n_run; ++k) {
    const uint32_t sfn = (k / slots_per_frame) % 1024, slot = k % slots_per_frame;
    auto           w = prod.begin(sfn, slot, 0, std::chrono::steady_clock::now().time_since_epoch().count());

    // Allocations: PUCCH at both edges, PUSCH hopping inside the band but never on the interferer PRBs.
    const uint32_t pusch_len   = 12;
    uint32_t       pusch_start = 2 + (k * 7) % (n_prb - pusch_len - 4);
    if (pusch_start + pusch_len > intf_start && pusch_start < intf_start + intf_len) {
      pusch_start = 2;
    }
    w.add_alloc({rxtap::kPucch, 0, 0, 1, 0, 14, {}});
    w.add_alloc({rxtap::kPucch, 0, uint16_t(n_prb - 1), 1, 0, 14, {}});
    w.add_alloc({rxtap::kPusch, 0x4601, uint16_t(pusch_start), uint16_t(pusch_len), 0, 14, {}});
    w.mark_alloc_valid();

    auto in_alloc = [&](uint32_t prb) {
      return prb == 0 || prb == n_prb - 1 || (prb >= pusch_start && prb < pusch_start + pusch_len);
    };
    for (uint32_t l = 0; l < n_symb; ++l) {
      // Rank-1 UE signal: the same symbol on every port with a per-port phase.
      std::vector<float> ue_re(n_subc), ue_im(n_subc), if_re(n_subc), if_im(n_subc);
      for (uint32_t s = 0; s < n_subc; ++s) {
        ue_re[s] = (g(rng) > 0 ? 1.f : -1.f) * ue_amp * float(M_SQRT1_2);
        ue_im[s] = (g(rng) > 0 ? 1.f : -1.f) * ue_amp * float(M_SQRT1_2);
        if_re[s] = g(rng) * intf_amp * noise_std;
        if_im[s] = g(rng) * intf_amp * noise_std;
      }
      for (uint32_t p = 0; p < n_ports; ++p) {
        const float c = std::cos(0.7f * p), sn = std::sin(0.7f * p);
        for (uint32_t s = 0; s < n_subc; ++s) {
          const uint32_t prb = s / 12;
          float          re = g(rng) * noise_std, im = g(rng) * noise_std;
          if (in_alloc(prb)) {
            re += ue_re[s] * c - ue_im[s] * sn;
            im += ue_re[s] * sn + ue_im[s] * c;
          }
          if (prb >= intf_start && prb < intf_start + intf_len) {
            re += if_re[s] * c + if_im[s] * sn; // Different spatial signature from the UE.
            im += if_im[s] * c - if_re[s] * sn;
          }
          sym[2 * s]     = to_bf16(re);
          sym[2 * s + 1] = to_bf16(im);
        }
        std::memcpy(w.symbol_ptr(p, l), sym.data(), w.symbol_bytes());
      }
    }
    prod.commit(w);

    next += slot_dur;
    std::this_thread::sleep_until(next);
  }
  std::printf("synthetic_producer: wrote %llu slots; interferer on PRBs %u-%u\n",
              static_cast<unsigned long long>(n_run),
              intf_start,
              intf_start + intf_len - 1);
  return 0;
}
