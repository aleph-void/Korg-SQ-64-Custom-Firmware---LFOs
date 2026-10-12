# LFOs — original SQ-64 firmware

A from-scratch firmware for the Korg SQ-64 hardware sequencer that turns the
box into a four-channel bank of **tempo-synced LFOs**. It is written entirely
from the interface facts published by the independent
[SQ-64 firmware specification](../../spec) and exercised against the
independent [SQ-64 emulator](../../emulator). **It contains no Korg source
code, no decompiled vendor functions, no vendor assets and no bytes copied
from stock firmware.**

> **Hardware warning.** The specification's board contract is explicitly
> incomplete: exact MCU part, wiring, electrical limits and protected-storage
> ownership remain unverified release blockers. This image passes the spec's
> `image` profile and runs in the emulator; that is **not** a certification
> that it is safe on a physical SQ-64. Flashing real hardware is at your own
> risk, and power loss while internal flash is being programmed is not
> guaranteed to be recoverable (spec VUP-008). If you accept that, the
> sibling [euclidean firmware's HARDWARE.md](../euclidean/HARDWARE.md)
> documents the identical install / operate / restore procedure (both images
> use the same container, updater and storage map).

## What "synced" means here

All four LFOs derive their phase from one shared 24 PPQN transport position —
they never free-drift against each other or against the beat. The position
advances from the internal tempo clock (20–300 BPM, tap tempo, MIDI clock
out while playing), and **follows incoming MIDI clock automatically** when
one is present: the header flips from INT to EXT with the measured BPM,
every LFO locks to the external ticks, and a second after the ticks stop it
falls back to the internal clock. PLAY performs a hard resync (every phase
restarts together) and MIDI start/stop in are honored.

## Features

- Four LFOs (A–D) with six waveforms — sine, triangle, rise, fall, square,
  sample & hold — and sixteen synced rate divisions from 8 bars to 1/32,
  including dotted and triplet values.
- Per-LFO depth (0–100 %), phase offset (sixteenths of a cycle) and MIDI CC
  number; each LFO streams its CC on MIDI channels 1–4 (USB and DIN,
  rate-limited, sent only on change).
- Destinations per LFO: A/B/C drive their **mod CV**, **pitch CV** and
  **gate** jacks (gate = square-thresholded LFO); LFO D pulses **trigger
  jack 1** once per cycle. Toggled with the GATE / PITCH / MOD buttons,
  which also show the edited LFO's routing on their LEDs.
- Pad LEDs are live level meters — one animated bar per LFO; the display
  shows all four lanes plus the edited LFO's parameters and a waveform
  preview with a phase cursor.
- Settings persist to SPI flash (GLOBAL commits; the power button saves and
  shuts down cleanly through the PB7 power hold).
- Boot chords per the spec: hold **GLOBAL** at power-on for version mode,
  **GLOBAL+PATTERN** for update mode, with the same complete VUP self-updater
  as the sibling euclidean firmware (protocol 1, 1024-byte packets, full
  validation before a RAM-resident installer programs anything).

## Controls (normal mode)

| Control | Action |
| --- | --- |
| PLAY / STOP | Resync all LFO phases and start MIDI clock out / stop it (LFOs keep running) |
| TAP | Tap tempo |
| A B C D | Select the edited LFO |
| Pad *n* (any bank) | Set that LFO's rate to division *n* (1=8 bars … 16=1/32) |
| SHIFT + pad *n* | Set that LFO's waveform (1=sine 2=tri 3=rise 4=fall 5=square 6=S+H) |
| GATE / PITCH / MOD | Toggle that destination for the edited LFO (on D, GATE = cycle trigger) |
| Tempo encoder | BPM (internal clock) |
| Swing encoder | MIDI CC number for the edited LFO |
| Param 1–4 encoders | Waveform, rate division, depth, phase offset |
| GLOBAL | Commit settings to SPI flash |
| Power button | Save and shut down |

## Build

Requires the GNU Arm toolchain (`arm-none-eabi-gcc`) and Python 3.

```sh
make            # build/lfos.elf, .bin and .vsb
make check      # run the spec's seven image-profile checks on the VSB
make test       # headless + browser-engine behavioral tests (see below)
```

The code is compiled as the ARMv6-M subset so it runs on both emulator
engines (the native engine's stock Unicorn build cannot deliver interrupts
inside IT blocks); `make ARCH=-mcpu=cortex-m7` builds Thumb-2 instead.
`tools/make_vsb.py` wraps the 64 KiB binary into a spec-conformant VSB,
filling unused payload with 0xFF and balancing the four region checksums.

## Run it

Browser workbench (recommended): in `../../emulator/web`, `npm ci && npm run
dev`, open the printed URL, load `build/lfos.vsb` in **Browser emulator**,
and run. The LFOs animate from the moment the panel comes up. Importable
demo scripts for the workbench's **UI scripts** panel live in
[`scripts/`](scripts/README.md); they walk through every major function
(synced free-running, transport resync, waveforms and rates, destinations,
CC assignment, external MIDI clock sync, persistence, version mode and a
full self-update).

Native headless:

```sh
python -m pip install -e ../../emulator
make smoke      # 2M instructions, report in build/smoke.json
```

## Tests

- `tools/headless_test.py` (native engine): boot, tick interrupts,
  continuous CC streaming, MIDI start/stop/clock out, external-clock
  following without re-emission, version and update boot chords, VUP
  discovery / version / begin, ordered packets, and rejection of
  out-of-order packets, wrong blocks, and installs outside update mode —
  with zero flash writes.
- `tools/browser_update_test.mjs` (browser WASM engine): full end-to-end
  installation — update-mode boot via the panel chord, USB MIDI enumeration,
  all 224 packets through the firmware's own updater under the workbench's
  installation context, then byte-exact verification of internal flash and
  the three installed SPI regions, with no critical hazard tripped.
- `tools/browser_panel_test.mjs` (browser WASM engine): DMA panel scanning,
  LFO animation on the CV observers and LED bars, destination toggles on the
  gate and trigger jacks, MIDI CC output and reassignment, transport,
  GLOBAL commit, power-button shutdown, and settings persistence across a
  cold restart.

## Storage map

| Region | Use |
| --- | --- |
| Internal flash 0x00200000 + 64 KiB | This firmware (VSB payload 0x00000) |
| SPI 0x00000–0x30000 | Installed-image regions; written only by the updater during a host-initiated installation |
| SPI 0x40000 (one 4 KiB sector) | Committed settings (magic `LFO1`, FNV-1a checked); rewritten only when contents change |
| Everything else | Never written |

## Provenance

Original work. Interface facts (container format, update protocol, panel
wiring, peripheral behavior) come from the sibling spec's `board.json`,
`contracts.md` and the emulator's independently authored board model — see
`src/board.h` for the consolidated register map. The hardware-interface
modules are shared with the sibling [euclidean](../euclidean) firmware by
the same authors; the LFO engine, UI and tests are new. Licensed under the
MIT License (see LICENSE).

## Known limits

- The stock backup/restore SysEx transport is not implemented; settings are
  this firmware's only persistent data.
- The analog sync jack is not used; external sync means incoming MIDI clock.
- External clock below ~2.5 BPM-equivalent tick spacing (more than one
  second between ticks) is treated as absent.
- MIDI input notes are ignored; the LFO CC/CV outputs are the product.
- Quantitative timing claims (jitter, latency) require hardware measurement
  and are not made.
