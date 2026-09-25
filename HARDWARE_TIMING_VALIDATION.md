# Hardware timing validation

Validated on 2026-09-25 against the supplied retail BIOS, standalone timing ROMs, and the three hardware timing-lab reference result sets.

## Regression ROMs

- `noploop_timing.bin`: PASS, counted 123 scanlines (expected 123), off by 0%.
- `vblank_interrupts_test`: all displayed NMI/IRQ0/IRQ1 checks PASS.

## Timing lab

### Round 1

All whole-number targets match. Fractional results are within 0.02 cycles of the hardware medians. The frame/scanline result is exactly `1019.00 / 267.97`.

Notable small differences versus hardware are limited to fractional refresh-sensitive measurements: `LDW ALT ROM` and `STW ALT ROM` are 6.05 vs 6.03, `LDW ROW ORM` is 1.53 vs 1.54, and the same-row side of `GAP S/A` is 3.52 vs 3.53.

### Round 2

Core DRAM, VDP read, arithmetic, exception, NMI, and IRQ0 medians are at or within 0.02 cycles of the hardware values. `NMI V/H` is exactly `473 / 447`; `IRQ0 V/H` is exactly `100 / 84`.

Three-cycle VDP/SRAM writes report 6.00 including cartridge fetch rather than the unexplained hardware 6.03 fractional effect. The supplied round-2 notes explicitly describe 6.00 as an acceptable first step for that effect.

### Round 3

All raster/interrupt medians match the hardware reference exactly. NOP-loop is `37.62`; `NOPLOOP -LD` is `36.23`; `NOPLOOP -ST` is 35.40 vs the 35.39 hardware median. IRQ0, NMI, frame/line raster IRQ1, and the level-triggered storm result all match the reference values.

## Follow-up interrupt round-trip / NOP-loop probes

The follow-up hardware measurements identify two NOP-loop pipeline effects that remain explicitly represented by the bus model: a same-row work-RAM load immediately before `BRA`/`BSR` can hide its one-cycle data phase, while a same-row load immediately after a store still pays its full data cycle. The latter must not be accidentally hidden by the store.

The interrupt round-trip comparison also showed that treating IRQ presentation latency as a post-accept CPU stall double-counts time. The CPU now continues retiring instructions while the pending IRQ/NMI presentation countdown elapses; only the measured controller/exception hand-off is charged when the request is accepted. The supplied follow-up table itself was an image rather than an executable ROM, so its rows 08-13 cannot be rerun directly here. The existing executable interrupt probes are used as the regression gate instead.

## Unit tests

`make test` passes after the timing changes.

## Doom / SCI regression investigation

The supplied LoopyDOOM build uses SCI1 for uPD937 MIDI at 31250 baud. Its `midi_tx()` waits for `SSR1.TDRE` (bit 7), writes TDR1, then clears TDRE through SSR. The previous emulator returned zero for every SCI register read, so every MIDI byte took LoopyDOOM's entire defensive TDRE timeout even though the transmitter should normally report an empty TDR.

The SCI model now follows the SH7021/MiSTer register and transmit-buffer behavior instead of special-casing Doom: reset `SSR` is `0x84` (`TDRE|TEND`), SMR/BRR/SCR/TDR/SSR/RDR are readable, TDR is a holding register, clearing `SSR.TDRE` moves the byte into the transmitter, TDRE reasserts when TDR becomes available, TEND follows the final frame, SCI TXI/TEI are driven from the status/enable bits, and TX DMA acknowledge clears TDRE as on the hardware interface. Asynchronous frame completion includes start/data/parity-or-MP/stop bits at the programmed BRR/CKS rate. The historical 208-byte SCI save-state chunk size is retained, with migration for the older register layout.

A deterministic gameplay comparison used the same input sequence on `edb5286` and this revision, then profiled 600 Loopy video frames (~10 seconds). `I_FinishUpdate_e32` writes DMAC CHCR once for each of the 160 output rows, so executions of its row-programming instruction divided by 160 are rendered Doom frames:

- `edb5286`: 3,040 row writes = 19 Doom frames, about 1.9 fps.
- SCI fix: 16,160 row writes = 101 Doom frames, about 10.1 fps.

That result is within LoopyDOOM's own README estimate of roughly 8-15 fps on hardware. No cartridge/CPU timing was relaxed to obtain it. The supplied MiSTer SCI implementation was used as the behavioral reference; the ITU model was not changed because the existing ITU/raster hardware probes remain on target and the Doom slowdown reproduced specifically through SCI status polling.

A dedicated `sh7021_serial_test` now gates reset register values, TDRE/TEND semantics, 31250-baud 8N1 frame timing, double buffering, TXI, TX-DMA acknowledgement, TE abort/idle behavior, synchronous-mode programming, and the historical serial save-state chunk size.
