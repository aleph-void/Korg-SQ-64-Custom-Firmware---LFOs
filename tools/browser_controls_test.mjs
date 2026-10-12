#!/usr/bin/env node
// Front-panel control tests against the browser (WASM) engine, covering the
// spec 0.6.0 panel requirements: track selection (TRACK-001), the
// PITCH/GATE/MOD/CONTROL edit modes (MODE-001), the bar loop (LOOP-001),
// bar selection and step editing (BAR-001), tap tempo (TAP-001), the tempo
// encoder (TEMPO-001) and the swing encoder (SWING-001) — including
// measured output intervals for the three quantitative requirements.
//
// Usage: node tools/browser_controls_test.mjs [path/to/emulator/web] [image.vsb]
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
const hold = (control, down) => { machine.event({type: 'button', control, down}); run(400000); };
// Precise turns send one detent per event right after a display frame
// completes: between draws the firmware's poll loop keeps up with the scan,
// so the detent's quadrature edge pair never straddles a dropped
// half-buffer. Bulk turns (sweeping into a clamp) send batches and rely on
// overshoot being absorbed by the clamp.
const afterFrame = () => {
  const seen = machine.board.panel.displayFrames;
  for (let i = 0; i < 60 && machine.board.panel.displayFrames === seen; i++) run(20000);
};
const turn = (control, delta) => {
  const step = Math.sign(delta);
  for (let i = 0; i < Math.abs(delta); i++) {
    afterFrame();
    machine.event({type: 'encoder', control, delta: step});
    run(100000);
  }
  run(400000);
};
// Virtual time: 216 MHz, 16 cycles per instruction -> 13500 instructions/ms.
const IPMS = 13500;
const virtualMs = () => machine.board.cycles / 216000;
const streams = from => new Set(midi.slice(from).filter(m => (m[0] & 0xf0) === 0xb0)
  .map(m => (m[0] & 15) + ':' + m[1]));

// Average interval between MIDI clock (F8) bytes on the DIN port, in
// virtual ms (216 MHz, 16 cycles per instruction tick).
const DIN_PORT = 0x40004800;
let clockTick = 0;
const clockIntervalMs = (count = 24) => {
  // Discard clock ticks already buffered before this measurement window.
  for (const m of machine.board.midiOut)
    if (m.byte === 0xf8 && m.port === DIN_PORT && m.tick > clockTick) clockTick = m.tick;
  const ticks = [];
  for (let guard = 0; ticks.length <= count && guard < 60000; guard++) {
    for (const m of machine.board.midiOut) {
      if (m.byte === 0xf8 && m.port === DIN_PORT && m.tick > clockTick) {
        clockTick = m.tick;
        ticks.push(m.tick);
      }
    }
    run(50000);
  }
  if (ticks.length < 2) return NaN;
  return (ticks[ticks.length - 1] - ticks[0]) * 16 / 216000 / (ticks.length - 1);
};
const near = (value, expected, tolerance) => Math.abs(value - expected) <= tolerance;

run(1500000); // boot into normal mode
const leds = () => machine.board.panel.leds;

// --- TRACK-001: track selection --------------------------------------------
let mark = midi.length;
for (const [key, led] of [['trackB', 'trackB'], ['trackC', 'trackC'], ['trackD', 'trackD'], ['trackA', 'trackA']]) {
  press(key);
  check(leds()[led] > 128, `track key ${key} reported selected`);
}
run(6000000);
const defaultStreams = ['0:1', '1:2', '2:11', '3:74'];
check(defaultStreams.every(s => streams(mark).has(s)),
  'selection-only sequence left every track streaming its own default CC');
check(machine.board.flash.writes === 0, 'track selection alone wrote nothing');

// Edits follow the selection: CONTROL-mode edit on track C only.
press('trackC');
press('control');
turn('param1', 2); // C: CC 11 -> 13
press('control');
mark = midi.length;
run(6000000);
check(streams(mark).has('2:13'), 'edit followed the selection (C now CC 13)');
check(!streams(mark).has('2:11') && streams(mark).has('0:1') && streams(mark).has('1:2'),
  'edit touched only the selected track');
press('trackC'); press('control'); turn('param1', -2); press('control'); // restore

// While sequencing: selection still reported, still selection-only.
press('play');
press('trackB');
check(leds().trackB > 128, 'track selection reported while sequencing');
mark = midi.length;
run(4000000);
check(defaultStreams.every(s => streams(mark).has(s)),
  'selection while sequencing mutated nothing');

// --- MODE-001: the four edit-mode keys -------------------------------------
press('stop');
press('trackD'); // D routes nothing, so no destination LED masks the mode LEDs
for (const key of ['pitch', 'gate', 'mod', 'control']) {
  mark = midi.length;
  press(key);
  check(leds()[key] > 128, `${key} key entered its edit mode (mode report)`);
  press(key);
  check(leds()[key] < 64, `${key} key exited its edit mode`);
  run(4000000);
  check([...streams(mark)].every(s => defaultStreams.includes(s)) && machine.board.flash.writes === 0,
    `${key} mode entry and exit alone mutated nothing`);
}

// Edit scoping: a MOD-mode depth edit on the active track changes only that
// track's depth; the PITCH-mode routing edit drives only its pitch jack.
const peakLevel = name => {
  let peak = 0;
  for (let i = 0; i < 24; i++) { run(400000); peak = Math.max(peak, outputLevels(machine.board)[name]); }
  return peak;
};
press('trackA');
press('mod');
const fullPeak = peakLevel('Amod');
turn('param2', -30); // depth 100 -> 70: the A level bar shrinks to match
const cutPeak = peakLevel('Amod');
check(fullPeak > 220 && cutPeak < fullPeak * 0.8 && cutPeak > fullPeak * 0.6,
  `MOD-mode depth edit scoped to depth (peak ${fullPeak} -> ${cutPeak})`);
let bMoves = new Set();
for (let i = 0; i < 6; i++) { bMoves.add(outputLevels(machine.board).Bmod); run(800000); }
check(bMoves.size > 2, 'MOD-mode edit left track B untouched');
turn('param2', 30); // restore depth 100
press('mod');
press('pitch');
turn('param1', 1); // pitch routing on
let pMoves = new Set();
for (let i = 0; i < 6; i++) { pMoves.add(outputLevels(machine.board).Apitch); run(800000); }
check(pMoves.size > 2, 'PITCH-mode routing edit drives the pitch jack');
turn('param1', -1); // routing off again
run(1000000);
check(outputLevels(machine.board).Apitch === 0, 'pitch jack released when routing cleared');
press('pitch');

// --- BAR-001 + LOOP-001: bar selection, step edits and the bar loop --------
for (const bar of ['bar2', 'bar3', 'bar4', 'bar1']) {
  press(bar);
  check(leds()[bar] > 128, `${bar} selection reported on its key LED`);
}
check(machine.board.flash.writes === 0, 'bar selection alone wrote nothing');

// Clear every step of bar 2 on track A (GATE mode step edits).
press('gate');
press('bar2');
for (let p = 1; p <= 16; p++) press('A' + p, 200000);
press('gate');

// Loop over the muted bar 2: track A's output is silenced; the untouched
// bars keep playing when the loop moves (boundary bars 1 and 4), and
// clearing the loop resumes normal playback.
// Sample across more than one full bar (2 s at 120 BPM), so a window that
// starts inside the muted bar still reaches live bars before judging.
const animates = () => {
  const seen = new Set();
  for (let i = 0; i < 16; i++) { seen.add(outputLevels(machine.board).Amod); run(2200000); }
  return seen.size > 2;
};
press('loop');
check(leds().loop > 128, 'LOOP engaged reported on the LOOP key LED');
run(2000000);
check(outputLevels(machine.board).Amod === 0 && !animates(),
  'loop over the all-cleared bar 2 mutes track A (playback honors the loop)');
press('bar1');
check(animates(), 'moving the loop to untouched bar 1 resumes output (boundary bar)');
press('bar4');
check(animates(), 'loop over untouched bar 4 keeps playing (boundary bar)');
press('bar2');
run(2000000);
check(!animates(), 'loop follows the selected bar back to the muted bar');
press('loop');
check(leds().loop < 64, 'LOOP cleared reported on the LOOP key LED');
check(animates(), 'clearing the loop resumes normal four-bar playback');
check(machine.board.flash.writes === 0, 'loop and step edits left saved data untouched');

// Step edits landed only in the selected bar: bars 1, 3 and 4 still play.
press('gate');
press('bar1');
// (visual check of the step row happens in the workbench; behaviorally the
// loop checks above already proved bars 1 and 4 kept their steps)
press('gate');

// --- TAP-001 + TEMPO-001: tap tempo and the tempo encoder, measured --------
const tap = () => {
  machine.event({type: 'button', control: 'tap', down: true});
  run(270000); // ~20 ms held
  machine.event({type: 'button', control: 'tap', down: false});
};
const tapPair = gapMs => {
  run(Math.round(3050 * IPMS)); // isolate the pair: the first tap only arms
  tap();
  run(Math.round(gapMs * IPMS) - 270000);
  tap();
  run(2000000);
};
press('play'); // MIDI clock out carries the measured tempo (24 PPQN)
run(2000000);
let interval = clockIntervalMs();
check(near(interval, 2500 / 120, 2), `nominal tempo measured (${interval.toFixed(2)} ms/tick ~ 120 BPM)`);
tapPair(2900); // the declared minimum: 60000/2900 floors to 20 BPM
interval = clockIntervalMs(8);
check(near(interval, 125, 6), `tapped minimum tempo measured (${interval.toFixed(1)} ms/tick ~ 20 BPM)`);
turn('tempo', -6); // repeated detents at the limit must clamp, not wrap
run(2000000);
interval = clockIntervalMs(8);
check(near(interval, 125, 6), `minimum tempo clamped without wraparound (${interval.toFixed(1)} ms/tick ~ 20 BPM)`);
turn('tempo', 1); // one documented increment per detent
run(2000000);
interval = clockIntervalMs(12);
check(near(interval, 2500 / 21, 2.5), `one detent raised the tempo one BPM (${interval.toFixed(2)} ms/tick ~ 21 BPM)`);
turn('tempo', -1); // and one documented decrement in the other direction
run(2000000);
interval = clockIntervalMs(8);
check(near(interval, 125, 6), `one detent back lowered it one BPM (${interval.toFixed(1)} ms/tick ~ 20 BPM)`);
tapPair(205); // near the top of the range: 60000/205 floors to 292 BPM
turn('tempo', 14); // sweep across the declared maximum: clamp at 300
run(2000000);
interval = clockIntervalMs();
check(near(interval, 2500 / 300, 1), `maximum tempo clamped (${interval.toFixed(2)} ms/tick ~ 300 BPM)`);
check(machine.board.flash.writes === 0, 'tempo adjustments alone wrote nothing');

// Out-of-range taps: faster than the declared range changes nothing.
tapPair(100);
interval = clockIntervalMs();
check(near(interval, 2500 / 300, 1), 'too-fast taps left the tempo unchanged');
tapPair(500); // quarter notes at 120 BPM
interval = clockIntervalMs();
check(near(interval, 2500 / 120, 2), `tapped nominal tempo measured (${interval.toFixed(2)} ms/tick ~ 120 BPM)`);
// A single tap only arms the reference: the tempo stays defined and unchanged.
run(Math.round(3200 * IPMS));
tap();
run(4000000);
interval = clockIntervalMs();
check(near(interval, 2500 / 120, 2), 'single tap left the tempo unchanged');
// A gap beyond the declared range re-arms instead of setting a tempo.
run(Math.round(3200 * IPMS));
tap();
run(2000000);
interval = clockIntervalMs();
check(near(interval, 2500 / 120, 2), 'too-slow tap pair left the tempo unchanged');
check(machine.board.flash.writes === 0, 'taps alone wrote nothing');

// --- SWING-001: the swing encoder, with measured step timing ---------------
// Track A: FALL wave at 8 bars keeps the raw value above the gate threshold
// for two whole bars after a resync, so with gate routing on, the gate jack
// becomes a direct image of the step mask. Alternate the steps of bar 1 and
// loop over it: the gate's high/low durations are the step durations.
tapPair(500); // back to 120 BPM: a sixteenth is 125 ms
press('trackA');
press('A1'); // rate 8BAR
hold('shift', true);
press('A4'); // wave FALL
hold('shift', false);
press('gate');
turn('param1', 1); // gate routing on
press('bar1');
for (let p = 2; p <= 16; p += 2) press('A' + p, 200000); // steps 2,4..16 off
press('gate');
press('loop');

// Sample the gate at ~2 ms and collect high/low phase durations.
const gatePhases = (windowMs) => {
  const durations = {high: [], low: []};
  let level = outputLevels(machine.board).Agate > 128;
  let since = virtualMs();
  const until = virtualMs() + windowMs;
  while (virtualMs() < until) {
    run(27000);
    const now = outputLevels(machine.board).Agate > 128;
    if (now !== level) {
      const t = virtualMs();
      durations[level ? 'high' : 'low'].push(t - since);
      level = now;
      since = t;
    }
  }
  const mid = a => a.length ? a.sort((x, y) => x - y)[a.length >> 1] : NaN;
  return {high: mid(durations.high), low: mid(durations.low)};
};
const resync = () => { press('play'); run(2700000); }; // restart the FALL cycle

resync();
let phases = gatePhases(1600);
check(near(phases.high, 125, 8) && near(phases.low, 125, 8),
  `neutral swing is unswung (high ${phases.high?.toFixed(1)} / low ${phases.low?.toFixed(1)} ms)`);
turn('swing', 25); // the declared maximum, 75 %
resync();
phases = gatePhases(1600);
check(near(phases.high, 187.5, 9) && near(phases.low, 62.5, 9),
  `maximum swing delays the second sixteenth to 75 % (high ${phases.high?.toFixed(1)} / low ${phases.low?.toFixed(1)} ms)`);
check(near(phases.high + phases.low, 250, 10),
  'the swung pair keeps its nominal length (odd steps stay on the grid)');
turn('swing', 10); // clamp at the maximum
resync();
phases = gatePhases(1200);
check(near(phases.high, 187.5, 9), 'swing clamps at the declared 75 % maximum');
turn('swing', -60); // sweep back down, clamping at the neutral minimum
resync();
phases = gatePhases(1200);
check(near(phases.high, 125, 8) && near(phases.low, 125, 8),
  'swing clamps at the 50 % minimum and timing returns to nominal');
check(machine.board.flash.writes === 0, 'swing adjustments alone wrote nothing');

machine.close();
if (failures.length) { console.error('FAILED: ' + failures.join('; ')); process.exit(1); }
console.log('browser-engine controls test passed');
