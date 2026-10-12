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

// Commit with GLOBAL, then power off and verify the hold release.
const writesBefore = machine.board.flash.writes;
press('global');
run(2000000);
check(machine.board.flash.writes > writesBefore, 'GLOBAL committed settings to SPI flash');

machine.event({type: 'power', down: true});
run(4000000);
check(machine.board.poweredOff, 'power button shutdown releases the hold');

// Cold restart with the same SPI flash contents: committed settings persist.
const savedSpi = machine.board.flash.data.slice();
const midi2 = [];
const machine2 = new Machine(engine, await loadImage(new Uint8Array(readFileSync(imagePath))), {onMidi: m => midi2.push(Array.from(m))});
machine2.board.flash.data.set(savedSpi);
let n = 6000000;
while (n > 0) { const r = machine2.run(200000); if (r.error) throw Error(r.error); n -= 200000; }
check(machine2.board.panel.leds.trackB > 128, 'committed selection (LFO B) restored after restart');
check(midi2.some(m => (m[0] & 0xff) === 0xb1 && m[1] === 7),
  'committed CC assignment (B on CC 7) restored after restart');
machine2.close();

machine.close();
if (failures.length) { console.error('FAILED: ' + failures.join('; ')); process.exit(1); }
console.log('browser-engine panel test passed');
