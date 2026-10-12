# UI demo scripts

Portable `sq64-ui-script` files for the browser workbench's **UI scripts**
panel. Together they demonstrate every major function of this firmware in the
emulator — no hardware involved.

## Running a script

1. In `../../../../emulator/web`: `npm ci && npm run dev`, open the printed
   URL and expand **Browser emulator**.
2. Load `build/lfos.vsb` (build it first with `make`).
3. Expand **UI scripts** below the instrument panel, choose **Import
   script**, select one of these files, then **Run script**.

Scripts drive the normal emulator input path, so you can watch the pad-LED
level bars, the display lanes with the live waveform preview, and the
top-edge mod/pitch/gate/trigger indicators animate in sync. Each script that
boots the instrument waits **20 s** after `power-on` for the firmware to
start scanning the panel; on a slow machine or a throttled background tab,
raise the `wait` after each `power-on` before reporting a problem. An
instance keeps running after a script finishes unless the script powers it
off.

## The scripts

| Script | Demonstrates | Watch for |
| --- | --- | --- |
| `01-synced-lfos-and-transport.json` | Free-running synced LFOs, PLAY resync, tap tempo, tempo encoder | All four LED bars undulate from boot; PLAY restarts every phase together; four TAPs ~300 ms apart speed everything up at once; STOP keeps the LFOs running but stops the MIDI clock |
| `02-waveforms-and-rates.json` | Pads = rate division, SHIFT+pad = waveform, param encoders 1–4 (wave, rate, depth, phase) | Lane B flutters at 1/32, sweeps at 1 bar, squares off, returns to sine; depth shrinks the bar; phase 8/16 puts B a half-cycle against A |
| `03-destinations-and-trigger.json` | Destination routing inside the PITCH/GATE/MOD edit modes, LFO D cycle trigger | PITCH mode's Param 1 adds the A pitch-CV sweep, GATE mode's Param 1 toggles the A gate jack at half-cycle; on LFO D the same edit pulses trigger indicator 1 once per cycle; the mode key LED shows the active edit mode |
| `04-midi-cc-assignment.json` | Per-LFO MIDI CC numbers in the CONTROL mode (Param 1) | The CC readout on the display moves from 11 to 74 for LFO C, then LFO A joins it on CC 74 (hear/see doubled controllers downstream) |
| `05-external-midi-clock.json` | Automatic external MIDI clock sync | After MIDI start, a bar and a half of injected clock at ~125 BPM: the header flips to **EXT** with the measured BPM and every LFO follows the external ticks; a second after the ticks stop it falls back to **INT** |
| `06-save-and-persistence.json` | SHIFT+GLOBAL settings commit, power-button shutdown, persistence | LFO B is set to S+H at 1/8T, committed with SHIFT+GLOBAL, powered off, rebooted: B comes back selected with the committed wave and rate |
| `07-version-mode.json` | GLOBAL boot chord, PLAY continues boot | `LFOS SQ-64 / VERSION 1.1 / ORIGINAL SYNCED-LFO FIRMWARE / PRESS PLAY TO CONTINUE` with the PLAY LED lit; pressing PLAY resumes the normal boot to the LFO screen, then a clean power-off |
| `08-update-mode-install.json` | GLOBAL+PATTERN boot chord and the full VUP self-updater | See below |
| `09-project-management.json` | The PROJECT manager: save, new project, load, empty-slot report, delete | LFO B is set to square, saved to slot A1 (its pad lights and blinks as current); SHIFT+PROJECT resets the workspace to the sine/tri defaults; loading A1 brings the square back; empty slot C5 reports `EMPTY C5` without changing anything; RECORD+pad deletes A1 (its pad goes dark); PROJECT returns to the lanes |
| `10-patterns-and-global.json` | The PATTERN selector and the GLOBAL settings mode | Pattern B2 activates with the default triangle and is squared off with the wave encoder; switching B1/B2 flips the lane between the two stored shapes; GLOBAL opens the settings page, the CC channels move to 5–8 and the trigger pulse widens, GLOBAL commits (`GLB SAVED`); SHIFT+GLOBAL commits the workspace |
| `11-steps-loop-and-swing.json` | GATE-mode step editing, the bar keys, the bar loop and the swing encoder | With every second step of bar 1 cleared on LFO A and the loop on that bar, the A gate chops at sixteenth notes (watch the step cells on the display and the A pad row); swing 75 shuffles the chop long-short; the loop follows the bar keys and clearing it resumes the four-bar sequence; PITCH mode's pads then slide A's phase offset by half a cycle and back |

## Notes on script 08 (updater)

Before running it, expand **Send a second firmware image** and select a
complete `.VSB` there (for a harmless demonstration, select
`build/lfos.vsb` itself — `allowOlder` is set because reinstalling the
same version is not an upgrade). The `firmware-update` action sends that
file through the firmware's own updater: the display walks through
`UPDATE MODE / WAITING FOR HOST` → `RECEIVING UPDATE` →
`PROGRAMMING / DO NOT REMOVE POWER` → `UPDATE COMPLETE / RESTART THE
DEVICE`. The action waits up to two minutes for USB enumeration first, so
the short startup wait is intentional. The workbench's critical flash-safety
guards stay active throughout. Installation writes the emulated flash only;
the booted image stays loaded, so a later `power-on` reboots the original.

These scripts are demonstrations, not assertions — the repeatable behavioral
checks live in `tools/headless_test.py`, `tools/browser_panel_test.mjs` and
`tools/browser_update_test.mjs` (see `make test`).
