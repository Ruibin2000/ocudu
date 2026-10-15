# SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU testbed contributors
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""gpu_consumer.py - read uplink resource grids and allocation tables from the rx_tap shared-memory
ring, measure interference on the unscheduled resource blocks with PyTorch on the GPU, and hand a
small summary to a publisher (FlexRIC / xApp).

Two threads:
  reader  - drains the ring into pinned batch buffers as fast as slots arrive; never waits on the GPU.
  compute - copies a full batch to the GPU, runs the measurement module, publishes the summary.
If compute falls behind, whole slots are discarded and counted; the ring itself never stalls the gNB.

Run (pinned to cores the gNB does not use):
    taskset -c 16-19 python3 gpu_consumer.py --name /rxtap
Test without a GPU:
    python3 gpu_consumer.py --name /rxtap --device cpu
"""
import argparse
import mmap
import os
import queue
import threading
import time

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

# ---------------------------------------------------------------------------------------------
# Shared-memory layout. Must match rx_tap_ring.h.
# ---------------------------------------------------------------------------------------------
MAGIC = 0x5258544150524E47
VERSION = 2
FMT_CBF16, FMT_CF32 = 1, 2
PUSCH, PUCCH, SRS, PRACH = 1, 2, 3, 4

RING_HDR = np.dtype([
    ("magic", "<u8"), ("version", "<u4"), ("n_slots", "<u4"),
    ("n_ports", "<u4"), ("n_symb", "<u4"), ("n_subc", "<u4"), ("sample_fmt", "<u4"),
    ("slot_bytes", "<u8"), ("write_seq", "<u8"),
    ("scs_khz", "<u4"), ("max_alloc", "<u4"), ("reserved", "<u8"),
])
SLOT_HDR = np.dtype([
    ("seq_begin", "<u8"), ("seq_end", "<u8"),
    ("sfn", "<u4"), ("slot", "<u4"), ("sector", "<u4"), ("n_alloc", "<u4"),
    ("t_ns", "<u8"), ("flags", "<u4"), ("reserved", "u1", 20),
])
ALLOC = np.dtype([
    ("type", "<u2"), ("rnti", "<u2"), ("start_prb", "<u2"), ("nof_prb", "<u2"),
    ("start_symb", "u1"), ("nof_symb", "u1"), ("reserved", "u1", 6),
])
assert RING_HDR.itemsize == 64 and SLOT_HDR.itemsize == 64 and ALLOC.itemsize == 16


# ---------------------------------------------------------------------------------------------
# Measurement module. Platform independent: the Aerial port only needs a new adapter that
# produces the same (grid, occupied) tensors.
# ---------------------------------------------------------------------------------------------
class UlInterferenceMeasure(nn.Module):
    """Interference + noise measurement on unscheduled REs.

    Inputs
      x:        complex64 [B, P, S, K]   batch of RX grids (ports, OFDM symbols, subcarriers)
      occupied: bool      [B, S, N]      True where any UL channel is scheduled (N = K // 12 PRBs)
    Output: dict of small tensors - the summary to report.
    """

    def __init__(self, guard_prb: int = 1, hot_threshold_db: float = 3.0):
        super().__init__()
        self.guard = guard_prb
        self.hot_threshold_db = hot_threshold_db

    def forward(self, x: torch.Tensor, occupied: torch.Tensor) -> dict:
        B, P, S, K = x.shape
        N = K // 12
        x = x[..., : N * 12].reshape(B, P, S, N, 12)

        # Exclude guard PRBs next to every allocation (in-band emission, timing offsets).
        occ = occupied.float().reshape(B * S, 1, N)
        if self.guard > 0:
            occ = F.max_pool1d(occ, 2 * self.guard + 1, stride=1, padding=self.guard)
        free = (occ.reshape(B, S, N) < 0.5)                               # [B, S, N]
        w = free.to(x.real.dtype)
        cnt = w.sum(dim=(0, 1))                                          # [N] free symbols per PRB
        valid = cnt > 0

        # Per-PRB, per-port interference+noise power over free REs.
        pre = (x.real ** 2 + x.imag ** 2).mean(-1)                       # [B, P, S, N]
        interf = (pre * w[:, None]).sum(dim=(0, 2)) / cnt.clamp_min(1)   # [P, N]
        interf = torch.where(valid[None], interf, torch.full_like(interf, float("nan")))
        interf_db = 10 * torch.log10(interf.clamp_min(1e-20))

        # Floor: median over measurable PRBs; "hot" PRBs stick out above it.
        floor_db = torch.nanmedian(interf_db, dim=-1).values             # [P]
        excess_db = (interf_db - floor_db[:, None]).nanmean(0)           # [N]
        hot = valid & (excess_db > self.hot_threshold_db)

        # Spatial covariance of the free REs per PRB and its eigenvalue spread
        # (large spread = spatially coherent interferer rather than thermal noise).
        y = x.permute(0, 2, 3, 1, 4)                                     # [B, S, N, P, 12]
        yw = y * w[..., None, None]
        R = torch.einsum("bsnpk,bsnqk->npq", yw, y.conj()) / (12 * cnt.clamp_min(1))[:, None, None]
        ev = torch.linalg.eigvalsh(R)                                    # [N, P] ascending
        spread_db = 10 * torch.log10(ev[:, -1] / ev[:, 0].clamp_min(1e-20))
        spread_db = torch.where(valid, spread_db, torch.full_like(spread_db, float("nan")))

        return {
            "interf_db": interf_db,          # [P, N]
            "floor_db": floor_db,            # [P]
            "excess_db": excess_db,          # [N]
            "hot_prbs": hot,                 # [N] bool
            "eig_spread_db": spread_db,      # [N]
            "free_frac": cnt / (B * S),      # [N]
        }


# ---------------------------------------------------------------------------------------------
# Ring reader (platform adapter for OCUDU).
# ---------------------------------------------------------------------------------------------
class RingReader:
    def __init__(self, name: str):
        path = "/dev/shm/" + name.lstrip("/")
        while not os.path.exists(path):
            time.sleep(0.1)
        hdr = np.zeros(1, RING_HDR)
        while True:  # Wait for the producer to finish initialising the header.
            fd = os.open(path, os.O_RDONLY)
            size = os.fstat(fd).st_size
            if size >= 64:
                self.mm = mmap.mmap(fd, size, prot=mmap.PROT_READ)
                os.close(fd)
                self.buf = np.frombuffer(self.mm, dtype=np.uint8)
                hdr = self.buf[:64].view(RING_HDR)
                if int(hdr["magic"][0]) == MAGIC:
                    break
                del hdr, self.buf
                self.mm.close()
            else:
                os.close(fd)
            time.sleep(0.05)
        self.hdr = hdr
        h = hdr[0]
        if int(h["version"]) != VERSION:
            raise RuntimeError(f"ring version {int(h['version'])}, expected {VERSION}")
        self.n_slots, self.P, self.S, self.K = (int(h[k]) for k in ("n_slots", "n_ports", "n_symb", "n_subc"))
        self.N = self.K // 12
        self.fmt = int(h["sample_fmt"])
        self.scs_khz = int(h["scs_khz"])
        self.max_alloc = int(h["max_alloc"])
        self.slot_bytes = int(h["slot_bytes"])
        self.sample_bytes = 4 if self.fmt == FMT_CBF16 else 8
        self.alloc_off = 64
        self.payload_off = 64 + self.max_alloc * ALLOC.itemsize
        self.payload_bytes = self.P * self.S * self.K * self.sample_bytes
        self.next_seq = int(hdr["write_seq"][0]) + 1  # Start from "now".
        self.overruns = 0
        self.torn = 0

    def staging_like(self, batch: int, pin: bool):
        """Allocate a batch buffer matching the ring's sample format."""
        if self.fmt == FMT_CBF16:
            g = torch.empty((batch, self.P, self.S, self.K, 2), dtype=torch.bfloat16, pin_memory=pin)
        else:
            g = torch.empty((batch, self.P, self.S, self.K), dtype=torch.complex64, pin_memory=pin)
        m = torch.empty((batch, self.S, self.N), dtype=torch.bool, pin_memory=pin)
        return g, m

    def try_read(self, grid_out: torch.Tensor, occ_out: torch.Tensor):
        """Copy the next slot into grid_out [P,S,K(,2)] and its occupancy into occ_out [S,N].
        Returns metadata, or None if nothing new / slot lost."""
        w = int(self.hdr["write_seq"][0])
        if w < self.next_seq:
            return None
        if w - self.next_seq >= self.n_slots - 1:  # Fell behind: skip to the newest safe slot.
            skip_to = w - (self.n_slots - 2)
            self.overruns += skip_to - self.next_seq
            self.next_seq = skip_to
        seq = self.next_seq
        off = 64 + (seq % self.n_slots) * self.slot_bytes
        sh = self.buf[off: off + 64].view(SLOT_HDR)
        if int(sh["seq_end"][0]) != seq:
            return None                                                  # Not finished yet.
        meta = {k: int(sh[k][0]) for k in ("sfn", "slot", "sector", "t_ns", "flags", "n_alloc")}
        payload = self.buf[off + self.payload_off: off + self.payload_off + self.payload_bytes]
        if self.fmt == FMT_CBF16:
            grid_out.view(torch.int16).numpy().reshape(-1)[...] = payload.view(np.int16)
        else:
            grid_out.numpy().reshape(-1)[...] = payload.view(np.complex64)
        allocs = self.buf[off + self.alloc_off: off + self.alloc_off + meta["n_alloc"] * ALLOC.itemsize].view(ALLOC).copy()
        if int(sh["seq_begin"][0]) != seq:                               # Overwritten mid-copy.
            self.torn += 1
            self.next_seq += 1
            return None
        self.next_seq += 1
        occ = occ_out.numpy()
        if meta["flags"] & 1:
            occ[...] = False
            for a in allocs:
                s0, s1 = int(a["start_symb"]), int(a["start_symb"]) + int(a["nof_symb"])
                p0, p1 = int(a["start_prb"]), int(a["start_prb"]) + int(a["nof_prb"])
                occ[s0:s1, p0:p1] = True
        else:
            occ[...] = True                                              # Unknown: treat as busy.
        meta["seq"] = seq
        return meta


def to_complex(grid: torch.Tensor) -> torch.Tensor:
    """cbf16 [..., 2] or complex64 -> complex64, on whatever device grid lives."""
    if grid.dtype == torch.bfloat16:
        return torch.view_as_complex(grid.float().contiguous())
    return grid


# ---------------------------------------------------------------------------------------------
# Publisher stub. Replace with the FlexRIC path (see plans/gpu_ul_measurement_plan.md, section 6).
# ---------------------------------------------------------------------------------------------
def publish_summary(summary: dict, metas: list, stats: dict):
    m = metas[-1]
    hot = torch.nonzero(summary["hot_prbs"]).flatten().tolist()
    floor = [round(v, 1) for v in summary["floor_db"].tolist()]
    print(f"sfn={m['sfn']:4d} slot={m['slot']:2d} floor_db={floor} hot_prbs={hot} "
          f"overruns={stats['overruns']} torn={stats['torn']} discarded={stats['discarded']}", flush=True)


# ---------------------------------------------------------------------------------------------
def reader_loop(rr: RingReader, free_q: queue.Queue, full_q: queue.Queue, batch: int, stats: dict,
                stop: threading.Event):
    scratch_g, scratch_m = rr.staging_like(1, pin=False)
    buf, n, metas = None, 0, []
    while not stop.is_set():
        if buf is None:
            try:
                buf = free_q.get_nowait()
            except queue.Empty:
                buf = None
        g, m = (buf[0][n], buf[1][n]) if buf is not None else (scratch_g[0], scratch_m[0])
        meta = rr.try_read(g, m)
        stats["overruns"], stats["torn"] = rr.overruns, rr.torn
        if meta is None:
            time.sleep(50e-6)          # Latency tolerant, so short polling sleeps are fine.
            continue
        if buf is None:                # Compute is behind: consume the slot but drop it.
            stats["discarded"] += 1
            continue
        metas.append(meta)
        n += 1
        if n == batch:
            full_q.put((buf, metas))
            buf, n, metas = None, 0, []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="/rxtap")
    ap.add_argument("--batch", type=int, default=20, help="slots per batch (20 = 10 ms at 30 kHz SCS)")
    ap.add_argument("--nbuf", type=int, default=3, help="batch buffers in flight")
    ap.add_argument("--guard", type=int, default=1, help="guard PRBs around each allocation")
    ap.add_argument("--device", default="cuda", help="cuda or cpu")
    ap.add_argument("--max-batches", type=int, default=0, help="stop after this many batches (0 = run forever)")
    args = ap.parse_args()

    dev = torch.device(args.device)
    use_cuda = dev.type == "cuda"
    rr = RingReader(args.name)
    print(f"ring: ports={rr.P} symbols={rr.S} subcarriers={rr.K} prbs={rr.N} scs={rr.scs_khz} kHz "
          f"fmt={'cbf16' if rr.fmt == FMT_CBF16 else 'cf32'} slots={rr.n_slots}", flush=True)

    free_q, full_q = queue.Queue(), queue.Queue()
    for _ in range(args.nbuf):
        free_q.put(rr.staging_like(args.batch, pin=use_cuda))
    model = UlInterferenceMeasure(guard_prb=args.guard).to(dev).eval()
    stream = torch.cuda.Stream() if use_cuda else None
    stats = {"overruns": 0, "torn": 0, "discarded": 0}
    stop = threading.Event()
    threading.Thread(target=reader_loop, args=(rr, free_q, full_q, args.batch, stats, stop), daemon=True).start()

    done = 0
    try:
        while args.max_batches == 0 or done < args.max_batches:
            (g, m), metas = full_q.get()
            with torch.no_grad():
                if use_cuda:
                    with torch.cuda.stream(stream):
                        gd = g.to(dev, non_blocking=True)   # Same DRAM on DGX Spark: cheap.
                        md = m.to(dev, non_blocking=True)
                        summary = {k: v.cpu() for k, v in model(to_complex(gd), md).items()}
                    stream.synchronize()
                else:
                    summary = model(to_complex(g), m)
            free_q.put((g, m))                             # Reuse only after the copy completed.
            publish_summary(summary, metas, stats)
            done += 1
    finally:
        stop.set()


if __name__ == "__main__":
    main()
