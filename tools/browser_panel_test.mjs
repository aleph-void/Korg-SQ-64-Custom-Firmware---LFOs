#!/usr/bin/env node
// Normal-mode interaction test against the browser (WASM) engine: panel DMA
// scanning, LFO animation on the CV observers and pad LEDs, transport,
// destination toggles, MIDI CC output, settings commit and persistence.
//
// Usage: node tools/browser_panel_test.mjs [path/to/emulator/web] [image.vsb]
import {readFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';
import {resolve} from 'node:path';

const webRoot = resolve(process.argv[2] || new URL('../../../emulator/web', import.meta.url).pathname);
const imagePath = resolve(process.argv[3] || new URL('../build/lfos.vsb', import.meta.url).pathname);
const load = path => import(pathToFileURL(resolve(webRoot, path)));

const {default: createUnicorn} = await load('vendor/unicorn-arm.js');
const {Machine} = await load('emulator/core.js');
const {loadImage} = await load('emulator/image.js');
const {outputLevels} = await load('emulator/outputs.js');

const failures = [];
const check = (condition, label) => {
  console.log((condition ? 'ok  ' : 'FAIL') + ' ' + label);
  if (!condition) failures.push(label);
};

const engine = await createUnicorn();
const midi = [];
const machine = new Machine(engine, await loadImage(new Uint8Array(readFileSync(imagePath))),
  {onMidi: m => midi.push(Array.from(m))});
const run = n => {
  while (n > 0) {
    const report = machine.run(Math.min(n, 200000));
    if (report.error) throw Error('emulator: ' + report.error);
    n -= 200000;
  }
};
const press = (control, instructions = 400000) => {
  machine.event({type: 'button', control, down: true});
  run(instructions);
  machine.event({type: 'button', control, down: false});
  run(instructions);
};

run(1500000); // boot into normal mode
check(machine.board.panel.displayFrames > 0, 'display rendered');
check(machine.board.panel.leds.trackA > 128, 'selected-LFO LED lit');
check(machine.board.panel.leds.stop > 128 && machine.board.panel.leds.play < 64,
  'stop LED lit while transport stopped');
check(machine.board.panel.leds.mod > 128, 'MOD destination LED lit for LFO A');

// The LFOs free-run from boot: the A mod CV level must keep moving.
const samples = [];
for (let i = 0; i < 6; i++) { samples.push(outputLevels(machine.board).Amod); run(800000); }
check(new Set(samples).size > 2, `A mod CV animates while stopped (${samples.join(',')})`);
check(midi.some(m => (m[0] & 0xf0) === 0xb0 && (m[0] & 15) === 0),
  'LFO A streams MIDI CC on channel 1');
check(midi.some(m => (m[0] & 0xf0) === 0xb0 && (m[0] & 15) === 3 && m[1] === 74),
  'LFO D streams CC 74 on channel 4');

// Transport: PLAY resyncs phases and starts the MIDI clock.
press('play');
run(3000000);
check(midi.some(m => m[0] === 0xfa), 'MIDI start sent');
check(midi.some(m => m[0] === 0xf8), 'MIDI clock sent while playing');
check(machine.board.panel.leds.play > 128, 'play LED lit');

// Gate destination: select LFO A, toggle GATE, expect square gate activity.
press('trackA');
press('gate');
check(machine.board.panel.leds.gate > 128, 'gate destination LED lit after toggle');
let gateHigh = false, gateLow = false;
for (let i = 0; i < 40 && !(gateHigh && gateLow); i++) {
  run(300000);
  const g = outputLevels(machine.board).Agate;
  if (g === 255) gateHigh = true;
  if (g === 0) gateLow = true;
}
check(gateHigh && gateLow, 'square-thresholded gate toggles on LFO A jack');
press('gate'); // toggle back off
run(400000);
check(outputLevels(machine.board).Agate === 0, 'gate off after destination cleared');

// LFO D trigger pulses: enable GATE destination on D. The 20 ms pulse needs
// fine sampling (~3 ms) to be observed reliably.
press('trackD');
press('gate');
let trigSeen = false;
for (let i = 0; i < 400 && !trigSeen; i++) { run(50000); trigSeen = outputLevels(machine.board).D1 === 255; }
check(trigSeen, 'LFO D pulses trigger jack 1 each cycle');

// Pad B7 selects LFO B and sets its rate division (pad 7 = dotted quarter).
press('B7');
run(600000);
check(machine.board.panel.leds.trackB > 128 && machine.board.panel.leds.trackD < 64,
  'pad press selected LFO B');

// The CC encoder (swing position) moves LFO B from CC 2 to CC 7.
machine.event({type: 'encoder', control: 'swing', delta: 5});
run(4000000);
const ccSeen = midi.length;
run(4000000);
check(midi.slice(ccSeen).some(m => (m[0] & 0xff) === 0xb1 && m[1] === 7),
  'LFO B streams on its re-assigned controller (CC 7)');

press('stop');
run(1000000);
check(midi.some(m => m[0] === 0xfc), 'MIDI stop sent');

// --- Project management (PROJECT-001..004) --------------------------------
// Slot n's record lives in the first bytes of SPI 0xC0000 + n * 0x3D000.
const slotBytes = (m, slot, n = 64) => {
  const base = 0xc0000 + slot * 0x3d000;
  return Array.from(m.board.flash.data.slice(base, base + n));
};
const settingsSector = m => Array.from(m.board.flash.data.slice(0x40000, 0x41000)).join();
const streams = from => new Set(midi.slice(from).filter(m => (m[0] & 0xf0) === 0xb0)
  .map(m => (m[0] & 15) + ':' + m[1]));
const hold = (control, down) => { machine.event({type: 'button', control, down}); run(400000); };

press('project'); // enter the project manager
run(400000);
check(machine.board.panel.leds.project > 128, 'PROJECT LED lit in project mode');

// PROJECT-003: SHIFT+pad saves the workspace (LFO B on CC 7) to slot A1 only.
const settingsBefore = settingsSector(machine);
hold('shift', true);
press('A1');
hold('shift', false);
const a1 = slotBytes(machine, 0);
check(a1[0] === 0x4c && a1[1] === 0x46 && a1[2] === 0x50 && a1[3] === 0x32,
  'save wrote a project record to slot A1 (magic LFP2)');
check(slotBytes(machine, 1).every(b => b === 0xff), 'save left neighbor slot A2 untouched');
check(settingsSector(machine) === settingsBefore, 'save left the settings sector untouched');

// PROJECT-001: SHIFT+PROJECT starts a new default project in the workspace
// (LFO B returns to CC 2) without touching any saved slot or the flash.
const writesBeforeNew = machine.board.flash.writes;
const a1Saved = slotBytes(machine, 0).join();
hold('shift', true);
press('project');
hold('shift', false);
check(machine.board.flash.writes === writesBeforeNew, 'new project performed no flash writes');
check(slotBytes(machine, 0).join() === a1Saved, 'new project left saved slot A1 untouched');
let mark = midi.length;
run(6000000);
check(streams(mark).has('1:2'), 'new project restored declared defaults (LFO B on CC 2)');

// PROJECT-002: loading slot A1 reproduces the saved workspace exactly.
press('A1');
mark = midi.length;
run(6000000);
check(streams(mark).has('1:7'), 'loading slot A1 restored the saved workspace (LFO B on CC 7)');

// PROJECT-002: an empty slot is reported without changing the workspace.
press('C5');
mark = midi.length;
run(6000000);
check(streams(mark).has('1:7') && !streams(mark).has('1:2'),
  'empty-slot load left the workspace unchanged');

// PROJECT-002: a corrupted record is rejected without changing the workspace.
machine.board.flash.data[0xc0000] ^= 0xff;
press('A1');
mark = midi.length;
run(6000000);
check(streams(mark).has('1:7') && !streams(mark).has('1:2'),
  'invalid-slot load left the workspace unchanged');

// PROJECT-004: RECORD+pad deletes exactly one slot, observably empty after.
hold('shift', true); // repopulate A1 first (over the corrupted record)
press('A1');
hold('shift', false);
hold('record', true);
press('A1');
hold('record', false);
check(slotBytes(machine, 0, 4096).every(b => b === 0xff), 'deleted slot A1 is observably empty');
press('A1'); // a load attempt now reports empty and changes nothing
mark = midi.length;
run(6000000);
check(streams(mark).has('1:7'), 'deleted-slot load left the workspace unchanged');

// Save to the last slot D16 (63) for the cold-restart persistence check.
hold('shift', true);
press('D16');
hold('shift', false);
check(slotBytes(machine, 63)[0] === 0x4c, 'project saved to last slot D16');
press('project'); // leave the project manager
run(400000);
check(machine.board.panel.leds.project < 64, 'PROJECT LED off after leaving project mode');

// --- Patterns (PATTERN-001..002) -------------------------------------------
press('pattern');
run(400000);
check(machine.board.panel.leds.pattern > 128, 'PATTERN LED lit in pattern mode');

// LFO B plays pattern 1 (CC 7). Selecting pattern 2 activates a fresh default
// pattern (CC 2) and the selection takes effect immediately.
press('B2');
mark = midi.length;
run(6000000);
check(streams(mark).has('1:2'), 'selected pattern B2 took effect (default CC 2)');

// Edit isolation: leave pattern mode, edit B2's CC, then switch back to B1.
press('pattern');
machine.event({type: 'encoder', control: 'swing', delta: 3}); // B2: CC 2 -> 5
run(4000000);
mark = midi.length;
run(4000000);
check(streams(mark).has('1:5'), 'edited active pattern B2 streams CC 5');
press('pattern');
press('B1');
mark = midi.length;
run(6000000);
check(streams(mark).has('1:7') && !streams(mark).has('1:5'),
  'returning to pattern B1 restored its CC 7 (non-selected preserved)');
press('pattern'); // exit pattern mode
run(400000);
check(machine.board.panel.leds.pattern < 64, 'PATTERN LED off after leaving pattern mode');

// --- SHIFT-001: SHIFT is a modifier only -----------------------------------
const shiftWrites = machine.board.flash.writes;
press('shift'); // SHIFT alone: no effect
hold('shift', true);
mark = midi.length;
press('play'); // undeclared SHIFT combination: the transport must not start
machine.event({type: 'encoder', control: 'swing', delta: 4}); // undeclared
run(4000000);
hold('shift', false);
check(!midi.slice(mark).some(m => m[0] === 0xfa), 'SHIFT+PLAY started no transport');
run(4000000);
check(!streams(mark).has('1:11'), 'SHIFT+encoder changed no CC assignment');
check(machine.board.flash.writes === shiftWrites,
  'SHIFT alone and undeclared SHIFT combinations wrote nothing');

// --- Global settings (GLOBAL-001..002) --------------------------------------
// Browsing alone commits nothing.
const browseWrites = machine.board.flash.writes;
press('global'); // enter global settings mode
run(400000);
check(machine.board.panel.leds.global > 128, 'GLOBAL LED lit in global settings mode');
press('global'); // documented exit, no edits made
run(400000);
check(machine.board.flash.writes === browseWrites, 'browsing global settings committed nothing');

// Edit the MIDI channel base to 5..8: immediate behavioral effect, committed
// on exit, and visible in the exported global record.
press('global');
machine.event({type: 'encoder', control: 'param2', delta: 4});
run(4000000);
mark = midi.length;
run(6000000);
check(streams(mark).has('4:1'), 'CC streams moved to channel 5 while editing (LFO A)');
const globalWrites = machine.board.flash.writes;
press('global'); // exit commits the edit
check(machine.board.flash.writes > globalWrites, 'global edit committed on exit');
const glbRec = Array.from(machine.board.flash.data.slice(0x41000, 0x41008));
check(glbRec[0] === 0x4c && glbRec[1] === 0x46 && glbRec[2] === 0x47 && glbRec[3] === 0x31,
  'global record written (magic LFG1)');
check(glbRec[5] === 4, 'committed global record holds MIDI base channel 5');

// Commit the workspace settings with SHIFT+GLOBAL, then power off.
const writesBefore = machine.board.flash.writes;
hold('shift', true);
press('global');
hold('shift', false);
check(machine.board.flash.writes > writesBefore,
  'SHIFT+GLOBAL committed workspace settings to SPI flash');

machine.event({type: 'power', down: true});
run(4000000);
check(machine.board.poweredOff, 'power button shutdown releases the hold');

// Cold restart with the same SPI flash contents: committed settings persist.
const savedSpi = machine.board.flash.data.slice();
const midi2 = [];
const machine2 = new Machine(engine, await loadImage(new Uint8Array(readFileSync(imagePath))), {onMidi: m => midi2.push(Array.from(m))});
machine2.board.flash.data.set(savedSpi);
const run2 = n => {
  while (n > 0) { const r = machine2.run(200000); if (r.error) throw Error(r.error); n -= 200000; }
};
run2(6000000);
check(machine2.board.panel.leds.trackB > 128, 'committed selection (LFO B) restored after restart');
check(midi2.some(m => (m[0] & 0xff) === 0xb5 && m[1] === 7),
  'committed CC assignment and global MIDI base (B on CC 7, channel 6) restored after restart');

// Saved projects persist across the cold restart: reset the workspace to the
// defaults (B back to CC 2), then load slot D16 to bring back CC 7.
const press2 = control => {
  machine2.event({type: 'button', control, down: true}); run2(400000);
  machine2.event({type: 'button', control, down: false}); run2(400000);
};
// Pattern contents persist too: B2 still holds the CC 5 edit.
press2('pattern');
press2('B2');
let markPat = midi2.length;
run2(6000000);
check(midi2.slice(markPat).some(m => (m[0] & 0xff) === 0xb5 && m[1] === 5),
  'pattern B2 content (CC 5) persisted across the cold restart');
press2('pattern');
machine2.event({type: 'button', control: 'shift', down: true}); run2(400000);
press2('project');
machine2.event({type: 'button', control: 'shift', down: false}); run2(400000);
let mark2 = midi2.length;
run2(6000000);
check(midi2.slice(mark2).some(m => (m[0] & 0xff) === 0xb5 && m[1] === 2),
  'new project after restart reset LFO B to CC 2 (globals untouched)');
press2('project');
press2('D16');
mark2 = midi2.length;
run2(6000000);
check(midi2.slice(mark2).some(m => (m[0] & 0xff) === 0xb5 && m[1] === 7),
  'project in last slot D16 reloads after the cold restart');
machine2.close();

// STORE-006: a v1.0 settings record (magic LFO1, one config per track) is a
// supported older format and loads migrated: its configs become pattern 1.
const v1 = new Uint8Array(36);
v1.set([0x4c, 0x46, 0x4f, 0x31]); // "LFO1"
v1[4] = 200 & 255; v1[5] = 200 >> 8; // bpm 200
v1[6] = 2; // selected LFO C
[[0, 7, 100, 0, 1, 1], [1, 5, 100, 4, 2, 1], [2, 3, 100, 0, 11, 1], [5, 10, 100, 0, 74, 0]]
  .flat().forEach((b, i) => { v1[8 + i] = b; });
let h = 0x811c9dc5;
for (let i = 0; i < 32; i++) h = Math.imul(h ^ v1[i], 0x01000193) >>> 0;
v1[32] = h & 255; v1[33] = h >> 8 & 255; v1[34] = h >> 16 & 255; v1[35] = h >> 24 & 255;
const midi3 = [];
const machine3 = new Machine(engine, await loadImage(new Uint8Array(readFileSync(imagePath))), {onMidi: m => midi3.push(Array.from(m))});
machine3.board.flash.data.set(v1, 0x40000);
let n3 = 6000000;
while (n3 > 0) { const r = machine3.run(200000); if (r.error) throw Error(r.error); n3 -= 200000; }
check(machine3.board.panel.leds.trackC > 128, 'v1 settings record migrated: committed selection (LFO C) restored');
check(midi3.some(m => (m[0] & 0xff) === 0xb1 && m[1] === 2),
  'v1 settings record migrated: configs load as pattern 1 of each track');
machine3.close();

machine.close();
if (failures.length) { console.error('FAILED: ' + failures.join('; ')); process.exit(1); }
console.log('browser-engine panel test passed');
