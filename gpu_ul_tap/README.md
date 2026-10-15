# gpu_ul_tap: GPU uplink measurements on the OCUDU RX path

Copies the uplink resource grid out of the running gNB into shared memory, measures interference on the unscheduled
resource blocks with PyTorch on the GPU, and produces a small summary for the RIC. The plan behind it is in
[plans/gpu_ul_measurement_plan.md](../plans/gpu_ul_measurement_plan.md).

| File | Side | What it is |
| --- | --- | --- |
| `rx_tap_ring.h` | gNB (C++) | Lock-free single-producer ring in POSIX shared memory: per-slot header, allocation table and `cbf16` grid. |
| `rx_tap_symbol_notifier.h` | gNB (C++) | Decorator around `ru_uplink_plane_rx_symbol_notifier` that copies each RX symbol into the ring. Works for split 8 (USRP) and split 7.2 (O-RU). Not wired into the build yet. |
| `synthetic_producer.cpp` | test | Stands in for the gNB: noise, a hopping PUSCH UE, PUCCH at the edges and an interferer on never-scheduled PRBs. |
| `gpu_consumer.py` | GPU (Python) | Reader thread + compute thread; `UlInterferenceMeasure` PyTorch module; publisher stub. |

## Try it without a radio

```bash
cd gpu_ul_tap
g++ -std=c++17 -O2 -o synthetic_producer synthetic_producer.cpp -lrt
./synthetic_producer --name /rxtap --prb 51 --ports 2 --slots 30000 &
python3 gpu_consumer.py --name /rxtap --device cuda      # or --device cpu
```

Expected output, once per 10 ms batch: a noise floor near 0 dB on each port and `hot_prbs=[33, 34, 35]`, the PRBs the
synthetic interferer occupies. The `overruns`, `torn` and `discarded` counters show slots lost in the ring, overwritten
mid-copy, and dropped because compute fell behind.

Start the consumer while the producer is running: it starts reading from the newest slot, not from the beginning.

## Status

- Ring, synthetic producer and consumer: tested end to end on CPU (20 MHz, 2 ports, 30 kHz). The CUDA path in
  `gpu_consumer.py` has not been run yet.
- `rx_tap_symbol_notifier.h`: compiles against the OCUDU headers; not yet wired into the gNB or run with a radio.
- Allocation staging from the UL PDU path: not implemented (plan, step 4).
