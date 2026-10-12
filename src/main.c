/* Synced LFOs for the SQ-64: boot mode selection and normal mode.
 *
 * Four low-frequency oscillators run phase-locked to a shared 24 PPQN
 * transport position. The position advances from the internal tempo clock,
 * or follows incoming MIDI clock whenever one is present (auto-switching),
 * so every LFO stays in sync with the beat and with the other LFOs. Each
 * LFO has a waveform, a tempo-synced rate division, depth, a phase offset
 * and a MIDI CC number. LFOs A/B/C can drive their mod CV, pitch CV and
 * gate jacks; LFO D pulses trigger jack 1 once per cycle. All four stream
 * MIDI CCs. SHIFT+GLOBAL commits workspace settings to SPI flash and the
 * power button saves and shuts down cleanly.
 *
 * Every pattern additionally carries a 64-step gate mask (four bars of
 * sixteen sixteenth-note steps, BAR-001) that mutes the LFO on cleared
 * steps, a workspace swing amount that delays the second sixteenth of every
 * pair (50..75 %, SWING-001), and the LOOP key loops step playback over the
 * selected bar (LOOP-001).
 *
 * Mode keys (each key returns to normal mode; SHIFT is a declared modifier
 * only, SHIFT-001):
 * - PITCH, GATE, MOD and CONTROL each toggle their edit mode over the
 *   normal surface (MODE-001). Entering or leaving changes no data; inside
 *   a mode the pads and the parameter encoders edit only that mode's
 *   documented parameters on the active track (PITCH: pitch-CV routing and
 *   phase offset; GATE: gate/trigger routing and the selected bar's step
 *   mask; MOD: mod-CV routing and depth; CONTROL: the MIDI CC number).
 * - PROJECT manages the 64 documented saved-project slots: each pad is a
 *   slot (load; SHIFT+pad saves; RECORD+pad deletes) and SHIFT+PROJECT
 *   starts a new default project (PROJECT-001..004).
 * - PATTERN selects each track's active pattern from its 16 pattern slots
 *   on the pads (PATTERN-001..002).
 * - GLOBAL opens the global settings mode: the parameter encoders edit the
 *   four global fields and GLOBAL exits, committing any edits
 *   (GLOBAL-001..002). Browsing alone commits nothing.
 */
#include <stdbool.h>

#include "board.h"
#include "display.h"
#include "midi.h"
#include "outputs.h"
#include "panel.h"
#include "storage.h"
#include "updater.h"

volatile uint32_t g_ms;

void tick_irq_handler(void)
{
    REG32(TICK_TIM + 0x10) = 0; /* clear update flag */
    g_ms++;
}

/* Synced rate divisions in 24 PPQN clock ticks, slowest first. */
static const uint16_t DIV_TICKS[DIV_COUNT] = {
    768, 384, 192, 96, 72, 48, 36, 24, 18, 16, 12, 9, 8, 6, 4, 3,
};
static const char *const DIV_NAMES[DIV_COUNT] = {
    "8BAR", "4BAR", "2BAR", "1BAR", "1/2D", "1/2", "1/4D", "1/4",
    "1/8D", "1/4T", "1/8", "1/16D", "1/8T", "1/16", "1/16T", "1/32",
};
static const char *const WAVE_NAMES[WAVE_COUNT] = {
    "SINE", "TRI", "RISE", "FALL", "SQR", "S+H",
};

static settings_t cfg;
static globals_t glb;       /* committed global settings, live copy */
static globals_t glb_entry; /* snapshot at global-mode entry, for commit-on-exit */
static bool playing;

/* UI modes. PROJECT, PATTERN and GLOBAL each toggle their mode; GLOBAL mode
 * is left only through GLOBAL (the documented exit, GLOBAL-001). */
typedef enum { MODE_NORMAL, MODE_PROJECT, MODE_PATTERN, MODE_GLOBAL } ui_mode_t;
static ui_mode_t mode;

/* Edit modes over the normal surface (MODE-001): the PITCH, GATE, MOD and
 * CONTROL keys each toggle their mode; a different mode key switches
 * directly. Entry and exit change no data. */
typedef enum { EDIT_NONE, EDIT_PITCH, EDIT_GATE, EDIT_MOD, EDIT_CONTROL } edit_mode_t;
static edit_mode_t edit;
static const char *const EDIT_NAMES[] = {"", "PITCH", "GATE", "MOD", "CONTROL"};

/* Bar selection (BAR-001) and the bar loop (LOOP-001): volatile performance
 * state, never persisted; selection and loop toggling alone mutate nothing. */
static uint8_t edit_bar; /* 0..BAR_COUNT-1, selected by the 1..4 keys */
static bool loop_on;     /* step playback loops over the selected bar */

/* Project manager state. */
#define SLOT_NONE 0xff
static uint8_t cur_slot = SLOT_NONE;      /* last loaded/saved slot */
static uint8_t slot_state[PROJECT_COUNT]; /* cached project_status_t per slot */
static const char *status_msg;
static int status_slot = -1;              /* -1: message without a slot name */
static uint32_t status_set_ms;

/* Transport position: integer 24 PPQN ticks plus a 1/65536-tick fraction.
 * LFO phases are derived from it, never free-running, so a position reset
 * or an external-clock snap re-syncs every LFO at once. */
static uint32_t pos_tick;
static uint32_t pos_frac;     /* 0..65535 */
static uint32_t last_ms;

/* External MIDI clock: measured tick spacing and arrival time. The clock
 * source switches to external automatically while ticks keep arriving. */
static uint32_t ext_last_ms;
static uint32_t ext_gap_ms;   /* 0 = no usable spacing measured yet */

static uint16_t lfo_phase_raw[4]; /* pre-offset phase, for wrap detection */
static uint16_t lfo_rand[4] = {0x5a21, 0xc3f1, 0x90b7, 0x1e65};
static uint16_t lfo_out[4];       /* post-depth level, for LEDs and UI */
static uint8_t cc_last[4] = {0xff, 0xff, 0xff, 0xff};
static uint32_t cc_sent_ms[4];
static uint32_t trigger_off_ms;
static uint32_t tap_last_ms;
static volatile bool rt_start, rt_stop;

/* A track's sound is its active pattern's configuration (PATTERN-001). */
static lfo_cfg_t *lfo_cur(int t)
{
    return &cfg.pattern[t][cfg.active[t]];
}

static bool ext_active(void)
{
    return ext_gap_ms != 0 && g_ms - ext_last_ms < 1000;
}

static void realtime_in(uint8_t byte)
{
    if (byte == 0xfa) {
        rt_start = true;
    } else if (byte == 0xfc) {
        rt_stop = true;
    } else if (byte == 0xf8) {
        uint32_t now = g_ms;
        uint32_t gap = now - ext_last_ms;
        ext_last_ms = now;
        if (gap >= 1 && gap <= 1000) {
            /* Snap to the next tick boundary: external clock owns the beat. */
            ext_gap_ms = gap;
            pos_tick++;
            pos_frac = 0;
        } else {
            ext_gap_ms = 0; /* first tick after silence only arms the sync */
        }
    }
}

static void clock_advance_ms(void)
{
    if (ext_active()) {
        /* Interpolate between external ticks but never cross the boundary
         * before the real tick arrives. */
        uint32_t frac = pos_frac + 65536u / ext_gap_ms;
        pos_frac = frac > 65535u ? 65535u : frac;
        return;
    }
    pos_frac += (uint32_t)cfg.bpm * 65536u / 2500u; /* bpm*24/60000 ticks/ms */
    if (pos_frac >= 65536u) {
        pos_frac -= 65536u;
        pos_tick++;
        if (playing && glb.clock_out) midi_send((const uint8_t[]){0xf8}, 1);
    }
}

static uint16_t wave_value(int i, uint16_t phase)
{
    uint32_t v;
    switch (lfo_cur(i)->wave) {
    case WAVE_SINE: { /* smoothstep of the triangle: a round LFO sine */
        uint32_t t = phase < 32768 ? phase * 2u : (65535u - phase) * 2u;
        uint32_t w = (t * t) >> 16;
        v = 3u * w - 2u * ((w * t) >> 16);
        break;
    }
    case WAVE_TRI: v = phase < 32768 ? phase * 2u : (65535u - phase) * 2u; break;
    case WAVE_RISE: v = phase; break;
    case WAVE_FALL: v = 65535u - phase; break;
    case WAVE_SQR: v = phase < 32768 ? 65535u : 0u; break;
    default: v = lfo_rand[i]; break; /* sample & hold */
    }
    return v > 65535u ? 65535u : (uint16_t)v;
}

static void prng_step(int i)
{
    uint16_t r = lfo_rand[i];
    r ^= (uint16_t)(r << 7);
    r ^= (uint16_t)(r >> 9);
    r ^= (uint16_t)(r << 8);
    lfo_rand[i] = r ? r : 0x7321;
}

/* Current step of the 64-step sequence (BAR-001), derived from the shared
 * transport position. A sixteenth is 6 ticks at 24 PPQN; swing moves the
 * boundary inside each 12-tick pair from 50 % up to 75 % (SWING-001). With
 * the loop engaged, playback stays inside the selected bar and the position
 * within it still follows the transport, so clearing the loop resumes the
 * declared normal (never-looped) playback at once (LOOP-001). */
static int step_now(void)
{
    uint32_t in_pair = (pos_tick % 12u) * 65536u + pos_frac;
    uint32_t boundary = 786432u /* 12 ticks << 16 */ * cfg.swing / 100u;
    int step = (int)((pos_tick / 12u % 32u) * 2u) + (in_pair >= boundary ? 1 : 0);
    if (loop_on) step = edit_bar * BAR_STEPS + (step & (BAR_STEPS - 1));
    return step;
}

__attribute__((noinline)) static void lfo_update(void)
{
    uint32_t now = g_ms;
    int step = step_now();
    for (int i = 0; i < 4; i++) {
        const lfo_cfg_t *c = lfo_cur(i);
        bool step_on = (c->steps[step >> 4] >> (step & 15)) & 1;
        uint32_t period = DIV_TICKS[c->div];
        uint16_t raw = (uint16_t)(((pos_tick % period) << 16 | pos_frac) / period);
        if (raw < lfo_phase_raw[i]) { /* cycle wrapped */
            prng_step(i);
            if (i == 3 && (c->dest & DEST_GATE) && step_on) {
                panel_triggers(1);
                trigger_off_ms = now + glb.trig_ms;
            }
        }
        lfo_phase_raw[i] = raw;
        /* A cleared step mutes the LFO: the raw value (and with it the gate
         * jack) and the depth-scaled output both read zero. */
        uint16_t value = step_on ? wave_value(i, (uint16_t)(raw + c->phase * 4096u)) : 0;
        uint16_t out = (uint16_t)((uint32_t)value * c->depth / 100u);
        lfo_out[i] = out;
        if (i < 3) {
            if (c->dest & DEST_MOD) cv_mod16(i, out);
            if (c->dest & DEST_PITCH) cv_pitch16(i, out);
            if (c->dest & DEST_GATE) gate_set(i, value >= 32768);
        }
        uint8_t cc_value = (uint8_t)(out >> 9);
        if (cc_value != cc_last[i] && now - cc_sent_ms[i] >= glb.cc_ms) {
            uint8_t msg[3] = {(uint8_t)(0xb0 | (glb.midi_base + i)), c->cc, cc_value};
            midi_send(msg, 3);
            cc_last[i] = cc_value;
            cc_sent_ms[i] = now;
        }
    }
    if (trigger_off_ms && (int32_t)(now - trigger_off_ms) >= 0) {
        panel_triggers(0);
        trigger_off_ms = 0;
    }
}

static void position_reset(void)
{
    pos_tick = 0;
    pos_frac = 0;
    for (int i = 0; i < 4; i++) lfo_phase_raw[i] = 0;
}

static void transport_start(void)
{
    playing = true;
    position_reset();
    midi_send((const uint8_t[]){0xfa}, 1);
}

static void transport_stop(void)
{
    playing = false;
    midi_send((const uint8_t[]){0xfc}, 1);
}

static void dest_cleared(int track, uint8_t bit)
{
    if (track < 3) {
        if (bit == DEST_MOD) cv_mod16(track, 0);
        if (bit == DEST_PITCH) cv_pitch16(track, 0);
        if (bit == DEST_GATE) gate_set(track, false);
    } else if (bit == DEST_GATE && !trigger_off_ms) {
        panel_triggers(0);
    }
}

static void set_status(const char *msg, int slot)
{
    status_msg = msg;
    status_slot = slot;
    status_set_ms = g_ms;
}

static bool status_active(void)
{
    return status_msg && g_ms - status_set_ms < 2500;
}

static void slots_rescan(void)
{
    for (int slot = 0; slot < PROJECT_COUNT; slot++) {
        slot_state[slot] = (uint8_t)project_peek(slot);
        panel_poll(); /* a full catalog scan outlasts several scan buffers */
    }
}

/* Silence every output once; the next lfo_update rewrites whatever the new
 * destinations drive. */
static void outputs_resync(void)
{
    outputs_all_off();
    panel_triggers(0);
    trigger_off_ms = 0;
}

/* Replace the workspace content with a project's. */
static void apply_project(const project_t *p)
{
    cfg.bpm = p->bpm;
    cfg.swing = p->swing;
    for (int t = 0; t < 4; t++) {
        cfg.active[t] = p->active[t];
        cfg.present[t] = p->present[t];
        for (int i = 0; i < PATTERN_COUNT; i++)
            cfg.pattern[t][i] = p->pattern[t][i];
    }
    outputs_resync();
}

static void workspace_to_project(project_t *p)
{
    project_defaults(p);
    p->bpm = cfg.bpm;
    p->swing = cfg.swing;
    for (int t = 0; t < 4; t++) {
        p->active[t] = cfg.active[t];
        p->present[t] = cfg.present[t];
        for (int i = 0; i < PATTERN_COUNT; i++)
            p->pattern[t][i] = cfg.pattern[t][i];
    }
}

/* PATTERN-001: make pattern p the track's active pattern. Selecting a slot
 * marks it present; a never-used slot activates with the track's default
 * configuration. Other patterns are untouched (PATTERN-002). */
static void pattern_select(int t, int p)
{
    if (cfg.active[t] != p) {
        cfg.active[t] = (uint8_t)p;
        outputs_resync();
    }
    cfg.present[t] |= (uint16_t)(1u << p);
    cfg.selected = (uint8_t)t;
    set_status("PATTERN", t * 16 + p);
}

/* PROJECT-001: new project = declared defaults in the workspace; no slot is
 * touched and nothing is persisted until an explicit save. */
static void project_new(void)
{
    project_t p;
    project_defaults(&p);
    apply_project(&p);
    cur_slot = SLOT_NONE;
    set_status("NEW", -1);
}

/* PROJECT-003: the explicit save operation (SHIFT+pad in the manager). */
static void project_pad_save(int slot)
{
    project_t p;
    workspace_to_project(&p);
    project_status_t status = project_save(slot, &p);
    slot_state[slot] = (uint8_t)project_peek(slot);
    if (status == PROJECT_OK) {
        cur_slot = (uint8_t)slot;
        set_status("SAVED", slot);
    } else {
        set_status(status == PROJECT_UNAVAIL ? "NO SLOT" : "SAVE ERR", slot);
    }
}

static void project_pad(int slot)
{
    project_t p;
    project_status_t status;
    if (panel_down(SW_RECORD)) { /* PROJECT-004: explicit delete */
        status = project_delete(slot);
        if (status == PROJECT_OK) {
            slot_state[slot] = PROJECT_EMPTY;
            if (cur_slot == slot) cur_slot = SLOT_NONE;
            set_status("DELETED", slot);
        } else {
            set_status("NO SLOT", slot);
        }
    } else { /* PROJECT-002: load; the workspace is untouched on rejection */
        status = project_load(slot, &p);
        slot_state[slot] = (uint8_t)status;
        if (status == PROJECT_OK) {
            apply_project(&p);
            cur_slot = (uint8_t)slot;
            set_status("LOADED", slot);
        } else if (status == PROJECT_EMPTY) {
            set_status("EMPTY", slot);
        } else if (status == PROJECT_INVALID) {
            set_status("INVALID", slot);
        } else {
            set_status("NO SLOT", slot);
        }
    }
}

/* Routing edits inside the PITCH/GATE/MOD edit modes (MODE-001): the Param 1
 * encoder turns the mode's destination on (right) or off (left) for the
 * active track. Tracks A..C own their jacks; on LFO D only the GATE mode's
 * cycle-trigger routing exists. */
static void route_turn(uint8_t bit, int detents)
{
    lfo_cfg_t *c = lfo_cur(cfg.selected);
    if (cfg.selected == 3 && bit != DEST_GATE) return; /* D is MIDI + trigger */
    uint8_t want = detents > 0 ? bit : 0;
    if ((c->dest & bit) == want) return;
    c->dest ^= bit;
    if (!want) dest_cleared(cfg.selected, bit);
}

/* MODE-001 entry/exit: the same key leaves, another mode key switches.
 * Toggling alone changes no pattern, workspace or settings data. */
static void edit_toggle(edit_mode_t which)
{
    edit = edit == which ? EDIT_NONE : which;
}

static bool pad_lookup(uint8_t sw, int *track, int *pad)
{
    for (int t = 0; t < 4; t++)
        for (int p = 0; p < 16; p++)
            if (pad_switch(t, p) == sw) {
                *track = t;
                *pad = p;
                return true;
            }
    return false;
}

static bool globals_edited(void)
{
    return glb.clock_out != glb_entry.clock_out || glb.midi_base != glb_entry.midi_base
        || glb.trig_ms != glb_entry.trig_ms || glb.cc_ms != glb_entry.cc_ms;
}

/* Kept out of line (like the other main-loop stages below): fully inlined
 * into normal_main they fuse into straight-line blocks beyond what the
 * browser engine's current TCI build translates reliably (its limit sits
 * near a hundred instructions per block; see the emulator's engine notes). */
__attribute__((noinline)) static void handle_events(void)
{
    panel_event_t ev;
    int t, p;
    while (panel_event_next(&ev)) {
        if (!ev.down) continue;
        if (panel_down(SW_SHIFT)) {
            /* SHIFT-001: SHIFT is a modifier only. Declared combinations:
             * SHIFT+PROJECT (new project), SHIFT+GLOBAL (commit settings),
             * SHIFT+pad 1..6 (waveform, normal mode) and SHIFT+pad (save,
             * project manager). Everything else, including SHIFT chords
             * with RECORD, causes no state change. */
            if (panel_down(SW_RECORD)) continue;
            if (ev.sw == SW_PROJECT) {
                project_new();
            } else if (ev.sw == SW_GLOBAL) {
                settings_save(&cfg);
                set_status("SETTINGS", -1);
            } else if (pad_lookup(ev.sw, &t, &p)) {
                if (mode == MODE_PROJECT) {
                    project_pad_save(t * 16 + p);
                } else if (mode == MODE_NORMAL && edit == EDIT_NONE && p < WAVE_COUNT) {
                    cfg.selected = (uint8_t)t;
                    lfo_cur(t)->wave = (uint8_t)p;
                }
            }
            continue;
        }
        switch (ev.sw) {
        case SW_PLAY: transport_start(); continue;
        case SW_STOP: transport_stop(); continue;
        case SW_TAP: {
            /* TAP-001: two taps 200..3000 ms apart (quarter notes at the
             * declared 20..300 BPM range) set the internal tempo. A single
             * tap only arms the reference; an out-of-range gap re-arms it
             * and leaves the tempo unchanged, so the tempo is never
             * undefined. Taps alone persist nothing. */
            uint32_t now = g_ms;
            uint32_t gap = now - tap_last_ms;
            if (gap >= 200 && gap <= 3000) {
                uint32_t bpm = 60000 / gap;
                cfg.bpm = (uint16_t)(bpm < 20 ? 20 : bpm > 300 ? 300 : bpm);
            }
            tap_last_ms = now;
            continue;
        }
        case SW_GLOBAL:
            if (mode == MODE_GLOBAL) { /* documented exit commits any edits */
                if (globals_edited()) {
                    globals_save(&glb);
                    set_status("GLB SAVED", -1);
                }
                mode = MODE_NORMAL;
            } else {
                glb_entry = glb;
                mode = MODE_GLOBAL;
            }
            continue;
        case SW_PROJECT:
            if (mode == MODE_GLOBAL) continue; /* GLOBAL is the only exit */
            if (mode == MODE_PROJECT) {
                mode = MODE_NORMAL;
            } else {
                slots_rescan();
                mode = MODE_PROJECT;
            }
            continue;
        case SW_PATTERN:
            if (mode == MODE_GLOBAL) continue; /* GLOBAL is the only exit */
            mode = mode == MODE_PATTERN ? MODE_NORMAL : MODE_PATTERN;
            continue;
        default: break;
        }
        if (mode == MODE_NORMAL) {
            switch (ev.sw) {
            /* TRACK-001: A..D select the active edit track; selection alone
             * changes no pattern, workspace or settings data. */
            case SW_TRACK_A: cfg.selected = 0; continue;
            case SW_TRACK_B: cfg.selected = 1; continue;
            case SW_TRACK_C: cfg.selected = 2; continue;
            case SW_TRACK_D: cfg.selected = 3; continue;
            /* MODE-001: the four edit-mode keys. */
            case SW_PITCH: edit_toggle(EDIT_PITCH); continue;
            case SW_GATE: edit_toggle(EDIT_GATE); continue;
            case SW_MOD: edit_toggle(EDIT_MOD); continue;
            case SW_CONTROL: edit_toggle(EDIT_CONTROL); continue;
            /* BAR-001: the 1..4 keys select the edit bar; LOOP-001: LOOP
             * toggles the bar loop. Both are selection-only state. */
            case SW_BAR1: edit_bar = 0; continue;
            case SW_BAR2: edit_bar = 1; continue;
            case SW_BAR3: edit_bar = 2; continue;
            case SW_BAR4: edit_bar = 3; continue;
            case SW_LOOP: loop_on = !loop_on; continue;
            default: break;
            }
            if (pad_lookup(ev.sw, &t, &p)) {
                if (edit == EDIT_NONE) {
                    cfg.selected = (uint8_t)t;
                    lfo_cur(t)->div = (uint8_t)p;
                } else if (edit == EDIT_PITCH) {
                    /* Any bank's pad n: phase offset n-1 on the active track. */
                    lfo_cur(cfg.selected)->phase = (uint8_t)p;
                } else if (edit == EDIT_GATE) {
                    /* Step edit: toggle step n of the selected bar on the
                     * active track (BAR-001). */
                    lfo_cur(cfg.selected)->steps[edit_bar] ^= (uint16_t)(1u << p);
                }
                /* MOD and CONTROL modes: the pads are documented inert. */
            }
        } else if (mode == MODE_PROJECT) {
            if (pad_lookup(ev.sw, &t, &p)) project_pad(t * 16 + p);
        } else if (mode == MODE_PATTERN) {
            if (pad_lookup(ev.sw, &t, &p)) pattern_select(t, p);
        }
        /* MODE_GLOBAL: pads, track and destination keys are inert. */
    }
    /* Encoders: always drain the detent counters, then route by mode. With
     * SHIFT held nothing is edited (SHIFT+encoder is undeclared). */
    int d_tempo = panel_encoder_take(ENC_TEMPO);
    int d_swing = panel_encoder_take(ENC_SWING);
    int d1 = panel_encoder_take(ENC_PARAM1);
    int d2 = panel_encoder_take(ENC_PARAM2);
    int d3 = panel_encoder_take(ENC_PARAM3);
    int d4 = panel_encoder_take(ENC_PARAM4);
    if (panel_down(SW_SHIFT)) return;
    if (mode == MODE_GLOBAL) {
        /* GLOBAL-002: edits apply to behavior immediately and are committed
         * by the GLOBAL exit. */
        if (d1) glb.clock_out = d1 > 0 ? 1 : 0;
        if (d2) {
            int v = glb.midi_base + d2;
            glb.midi_base = (uint8_t)(v < 0 ? 0 : v > 12 ? 12 : v);
        }
        if (d3) {
            int v = glb.trig_ms + d3;
            glb.trig_ms = (uint8_t)(v < 5 ? 5 : v > 100 ? 100 : v);
        }
        if (d4) {
            int v = glb.cc_ms + d4;
            glb.cc_ms = (uint8_t)(v < 1 ? 1 : v > 50 ? 50 : v);
        }
        return;
    }
    if (mode != MODE_NORMAL) return; /* detents drained, nothing edited */
    lfo_cfg_t *c = lfo_cur(cfg.selected);
    if (d_tempo) {
        /* TEMPO-001: one BPM per detent, clamped at the declared 20..300
         * limits with no wraparound. */
        int bpm = cfg.bpm + d_tempo;
        cfg.bpm = (uint16_t)(bpm < 20 ? 20 : bpm > 300 ? 300 : bpm);
    }
    if (d_swing) {
        /* SWING-001: one percent per detent across the declared 50..75 %
         * range; 50 % is the unswung neutral setting. */
        int swing = cfg.swing + d_swing;
        cfg.swing = (uint8_t)(swing < SWING_MIN ? SWING_MIN
                              : swing > SWING_MAX ? SWING_MAX : swing);
    }
    switch (edit) {
    case EDIT_PITCH: /* MODE-001: pitch-CV routing and phase offset only */
        if (d1) route_turn(DEST_PITCH, d1);
        if (d2) c->phase = (uint8_t)((c->phase + d2) & 15);
        return;
    case EDIT_GATE: /* gate/trigger routing; the pads edit the step mask */
        if (d1) route_turn(DEST_GATE, d1);
        return;
    case EDIT_MOD: /* mod-CV routing and depth only */
        if (d1) route_turn(DEST_MOD, d1);
        if (d2) {
            int depth = c->depth + d2;
            c->depth = (uint8_t)(depth < 0 ? 0 : depth > 100 ? 100 : depth);
        }
        return;
    case EDIT_CONTROL: { /* the MIDI CC number only */
        if (d1) {
            int cc = c->cc + d1;
            c->cc = (uint8_t)(cc < 0 ? 0 : cc > 127 ? 127 : cc);
        }
        return;
    }
    default: break;
    }
    if (d1) {
        int wave = c->wave + d1;
        c->wave = (uint8_t)(wave < 0 ? 0 : wave >= WAVE_COUNT ? WAVE_COUNT - 1 : wave);
    }
    if (d2) {
        int div = c->div + d2;
        c->div = (uint8_t)(div < 0 ? 0 : div >= DIV_COUNT ? DIV_COUNT - 1 : div);
    }
    if (d3) {
        int depth = c->depth + d3;
        c->depth = (uint8_t)(depth < 0 ? 0 : depth > 100 ? 100 : depth);
    }
    if (d4) {
        c->phase = (uint8_t)((c->phase + d4) & 15);
    }
}

__attribute__((noinline)) static void update_leds(void)
{
    uint32_t rows[8];
    bool blink = (g_ms >> 8) & 1;
    if (mode == MODE_PROJECT) {
        /* Pad LEDs become the slot grid: lit = populated, blink = current. */
        for (int t = 0; t < 4; t++) {
            uint32_t cols = 0;
            for (int p = 0; p < 16; p++) {
                int slot = t * 16 + p;
                bool on = slot_state[slot] == PROJECT_OK;
                if (slot == cur_slot) on = blink;
                if (on) cols |= 2u << p;
            }
            rows[t * 2] = cols;
            rows[t * 2 + 1] = 0;
        }
        rows[6] |= 1u << 17; /* PROJECT button LED */
    } else if (mode == MODE_PATTERN) {
        /* Pad LEDs become the pattern grid: lit = present, blink = active. */
        for (int t = 0; t < 4; t++) {
            uint32_t cols = 0;
            for (int p = 0; p < 16; p++) {
                bool on = (cfg.present[t] >> p) & 1;
                if (p == cfg.active[t]) on = blink;
                if (on) cols |= 2u << p;
            }
            rows[t * 2] = cols;
            rows[t * 2 + 1] = 0;
        }
        rows[6] |= 1u << 18; /* PATTERN button LED */
    } else {
        for (int i = 0; i < 4; i++) {
            uint32_t cols;
            if (edit == EDIT_GATE && i == cfg.selected) {
                /* Step editing: the active track's pad row shows the
                 * selected bar's step mask, current step blinking. */
                cols = (uint32_t)lfo_cur(i)->steps[edit_bar] << 1;
                int step = step_now();
                if (step >> 4 == edit_bar && blink) cols ^= 2u << (step & 15);
            } else {
                /* Each LFO's row of pads is a live level bar. */
                int lit = (lfo_out[i] >> 12) + 1; /* 1..16 pads */
                cols = ((1u << lit) - 1) << 1;
            }
            if (i == cfg.selected) cols |= 1; /* track select LED, column 0 */
            rows[i * 2] = cols;
            rows[i * 2 + 1] = 0;
        }
        if (edit == EDIT_NONE) {
            /* Destination LEDs for the edited LFO (row 0 columns 18..20). */
            const lfo_cfg_t *c = lfo_cur(cfg.selected);
            if (c->dest & DEST_GATE) rows[0] |= 1u << 18;
            if (c->dest & DEST_PITCH) rows[0] |= 1u << 19;
            if (c->dest & DEST_MOD) rows[0] |= 1u << 20;
        } else {
            /* The active edit mode's key LED (MODE-001 mode report). */
            if (edit == EDIT_CONTROL) rows[0] |= 1u << 17;
            if (edit == EDIT_GATE) rows[0] |= 1u << 18;
            if (edit == EDIT_PITCH) rows[0] |= 1u << 19;
            if (edit == EDIT_MOD) rows[0] |= 1u << 20;
        }
        /* Selected-bar and loop LEDs (BAR-001/LOOP-001 reports). */
        if (loop_on) rows[2] |= 1u << 17;
        if (edit_bar == 0) rows[2] |= 1u << 18;
        if (edit_bar == 1) rows[2] |= 1u << 19;
        if (edit_bar == 2) rows[4] |= 1u << 17;
        if (edit_bar == 3) rows[4] |= 1u << 18;
        if (mode == MODE_GLOBAL) rows[4] |= 1u << 19; /* GLOBAL button LED */
    }
    panel_led_columns(rows);
    bool beat = pos_tick % 24 < 2;
    panel_transport_leds(beat, !playing, ext_active(), playing);
}

static void draw_wave_preview(int x, int y, int w, int h, int lfo)
{
    for (int i = 0; i < w; i++) {
        uint16_t phase = (uint16_t)((i << 16) / w + lfo_cur(lfo)->phase * 4096u);
        int v = wave_value(lfo, phase) * (h - 1) / 65535;
        fb_pixel(x + i, y + h - 1 - v, 9);
    }
    int cursor = lfo_phase_raw[lfo] * w / 65536;
    fb_fill(x + cursor, y, 1, h, 15);
}

static void draw_status(int x, int y)
{
    fb_text(x, y, status_msg, 15);
    if (status_slot >= 0) {
        fb_text(x + 50, y, (const char[]){(char)('A' + status_slot / 16), 0}, 15);
        fb_number(x + 57, y, status_slot % 16 + 1, 15);
    }
}

static void draw_projects(void)
{
    fb_clear();
    fb_text(2, 1, "PROJECTS", 15);
    panel_poll(); /* redraws outlast the 16-frame scan buffer: keep decoding */
    if (status_active()) draw_status(120, 1);
    int avail = project_slots_available();
    for (int t = 0; t < 4; t++) {
        int y = 13 + t * 9;
        fb_text(2, y, (const char[]){(char)('A' + t), 0}, 8);
        for (int p = 0; p < 16; p++) {
            int slot = t * 16 + p;
            int x = 12 + p * 15;
            if (slot >= avail) continue; /* unavailable: blank cell */
            uint8_t st = slot_state[slot];
            if (st == PROJECT_OK) fb_fill(x, y, 12, 6, 9);
            else if (st == PROJECT_INVALID) fb_fill(x, y + 2, 12, 2, 4);
            else fb_fill(x, y + 5, 12, 1, 2); /* empty: a faint baseline */
            if (slot == cur_slot) fb_fill(x, y, 12, 1, 15);
        }
        panel_poll();
    }
    fb_text(2, 53, "PAD LOAD  SHIFT+PAD SAVE  REC+PAD DEL", 5);
    display_flush();
}

static void draw_patterns(void)
{
    fb_clear();
    fb_text(2, 1, "PATTERNS", 15);
    panel_poll();
    if (status_active()) draw_status(120, 1);
    for (int t = 0; t < 4; t++) {
        int y = 13 + t * 9;
        fb_text(2, y, (const char[]){(char)('A' + t), 0},
                t == cfg.selected ? 15 : 8);
        for (int p = 0; p < 16; p++) {
            int x = 12 + p * 15;
            if ((cfg.present[t] >> p) & 1) fb_fill(x, y, 12, 6, 9);
            else fb_fill(x, y + 5, 12, 1, 2); /* unused: a faint baseline */
            if (p == cfg.active[t]) fb_fill(x, y, 12, 1, 15);
        }
        panel_poll();
    }
    fb_text(2, 53, "PAD SETS THE ACTIVE PATTERN", 5);
    display_flush();
}

static void draw_globals(void)
{
    fb_clear();
    fb_text(2, 1, "GLOBAL", 15);
    panel_poll();
    if (status_active()) draw_status(120, 1);
    fb_text(2, 14, "P1 CLOCK OUT", 8);
    fb_text(104, 14, glb.clock_out ? "ON" : "OFF", 15);
    fb_text(2, 24, "P2 MIDI CH", 8);
    fb_number(104, 24, glb.midi_base + 1, 15);
    fb_text(122, 24, "-", 8);
    fb_number(130, 24, glb.midi_base + 4, 15);
    fb_text(2, 34, "P3 TRIG MS", 8);
    fb_number(104, 34, glb.trig_ms, 15);
    fb_text(2, 44, "P4 CC MS", 8);
    fb_number(104, 44, glb.cc_ms, 15);
    fb_text(2, 56, "GLOBAL EXITS AND COMMITS EDITS", 5);
    display_flush();
}

__attribute__((noinline)) static void draw_ui(void)
{
    if (mode == MODE_PROJECT) {
        draw_projects();
        return;
    }
    if (mode == MODE_PATTERN) {
        draw_patterns();
        return;
    }
    if (mode == MODE_GLOBAL) {
        draw_globals();
        return;
    }
    const lfo_cfg_t *c = lfo_cur(cfg.selected);
    uint16_t bpm_shown = cfg.bpm;
    if (ext_active()) bpm_shown = (uint16_t)(2500 / ext_gap_ms);
    fb_clear();
    panel_poll(); /* redraws outlast the 16-frame scan buffer: keep decoding */
    fb_text(2, 1, "LFOS", 15);
    fb_number(60, 1, bpm_shown, 12);
    fb_text(84, 1, "BPM", 8);
    fb_text(112, 1, ext_active() ? "EXT" : "INT", ext_active() ? 15 : 8);
    fb_text(140, 1, "SW", 8);
    fb_number(158, 1, cfg.swing, cfg.swing == SWING_MIN ? 8 : 12);
    fb_text(216, 1, playing ? "PLAY" : "STOP", playing ? 15 : 6);

    fb_text(2, 12, "LFO", 8);
    fb_text(26, 12, (const char[]){(char)('A' + cfg.selected), 0}, 15);
    if (edit == EDIT_NONE) {
        fb_text(40, 12, WAVE_NAMES[c->wave], 15);
        fb_text(76, 12, DIV_NAMES[c->div], 15);
        fb_text(114, 12, "DEP", 8);
        fb_number(138, 12, c->depth, 15);
        fb_text(160, 12, "PHS", 8);
        fb_number(184, 12, c->phase, 15);
        fb_text(200, 12, "CC", 8);
        fb_number(216, 12, c->cc, 15);
        /* Destination flags: Mod / Pitch / Gate (trigger for LFO D). */
        fb_text(238, 12, "M", (c->dest & DEST_MOD) && cfg.selected < 3 ? 15 : 3);
        fb_text(244, 12, "P", (c->dest & DEST_PITCH) && cfg.selected < 3 ? 15 : 3);
        fb_text(250, 12, cfg.selected == 3 ? "T" : "G", c->dest & DEST_GATE ? 15 : 3);
    } else {
        /* MODE-001 mode report: the active edit mode and its scoped
         * parameters on the active track. */
        uint8_t bit = edit == EDIT_PITCH ? DEST_PITCH
                    : edit == EDIT_MOD ? DEST_MOD : DEST_GATE;
        fb_text(40, 12, EDIT_NAMES[edit], 15);
        if (edit == EDIT_CONTROL) {
            fb_text(96, 12, "CC", 8);
            fb_number(114, 12, c->cc, 15);
        } else if (cfg.selected == 3 && edit != EDIT_GATE) {
            fb_text(96, 12, "NO JACK ON D", 8);
        } else {
            fb_text(96, 12, c->dest & bit ? "ON" : "OFF", c->dest & bit ? 15 : 8);
        }
        if (edit == EDIT_PITCH) {
            fb_text(130, 12, "PHS", 8);
            fb_number(154, 12, c->phase, 15);
        } else if (edit == EDIT_MOD) {
            fb_text(130, 12, "DEP", 8);
            fb_number(154, 12, c->depth, 15);
        } else if (edit == EDIT_GATE) {
            /* The selected bar's sixteen steps (pads toggle them). */
            fb_text(126, 12, "BAR", 8);
            fb_number(150, 12, edit_bar + 1, 15);
            for (int s = 0; s < BAR_STEPS; s++) {
                bool on = (c->steps[edit_bar] >> s) & 1;
                if (on) fb_fill(160 + s * 6, 12, 5, 6, 12);
                else fb_fill(160 + s * 6, 17, 5, 1, 3);
            }
        }
    }

    /* One lane per LFO: name, wave, division, CC and a live level bar. */
    for (int i = 0; i < 4; i++) {
        const lfo_cfg_t *lc = lfo_cur(i);
        int y = 24 + i * 8;
        uint8_t bright = i == cfg.selected ? 15 : 6;
        fb_text(2, y, (const char[]){(char)('A' + i), 0}, bright);
        fb_text(12, y, WAVE_NAMES[lc->wave], bright);
        fb_text(44, y, DIV_NAMES[lc->div], bright);
        fb_text(80, y, "CC", 5);
        fb_number(94, y, lc->cc, bright);
        int width = lfo_out[i] * 100 / 65535;
        fb_fill(116, y + 1, 100, 1, 3);
        if (width) fb_fill(116, y, width, 4, i == cfg.selected ? 13 : 7);
        panel_poll();
    }
    draw_wave_preview(222, 26, 32, 28, cfg.selected);
    if (status_active()) {
        draw_status(2, 57);
    } else {
        fb_text(2, 57, "PAT", 5);
        for (int t = 0; t < 4; t++)
            fb_number(26 + t * 14, 57, cfg.active[t] + 1,
                      t == cfg.selected ? 12 : 5);
    }
    /* Selected bar and loop state (BAR-001/LOOP-001 reports). */
    fb_text(120, 57, "BAR", 5);
    fb_number(144, 57, edit_bar + 1, 12);
    fb_text(160, 57, "LOOP", loop_on ? 15 : 3);
    display_flush();
}

static void power_off(void)
{
    if (playing) transport_stop();
    outputs_all_off();
    panel_triggers(0);
    settings_save(&cfg);
    globals_save(&glb);
    fb_clear();
    fb_text(2, 28, "POWERED OFF", 12);
    display_flush();
    for (int i = 0; i < 10000 && (REG32(DISPLAY_DMA_STREAM) & DMA_CR_EN); i++) { }
    display_pump();
    for (int i = 0; i < 10000 && (REG32(DISPLAY_DMA_STREAM) & DMA_CR_EN); i++) { }
    GPIO_BSRR(GPIOB) = 1u << (16 + PIN_POWER_HOLD); /* release the hold */
    for (;;) { }
}

static void normal_main(void)
{
    storage_init();
    settings_load(&cfg);
    globals_load(&glb);
    display_init();
    outputs_init();
    panel_init();
    midi_init(0, realtime_in);

    /* 1 ms tick: 216 MHz / (215+1) / (999+1). */
    REG32(TICK_TIM + 0x28) = 215;
    REG32(TICK_TIM + 0x2c) = 999;
    REG32(TICK_TIM + 0x0c) = 1;
    NVIC_ISER(TICK_IRQ / 32) = 1u << (TICK_IRQ % 32);
    REG32(TICK_TIM) = 1;

    bool power_seen_released = false;
    uint32_t last_draw_ms = 0;
    for (;;) {
        panel_poll();
        handle_events();
        midi_poll();
        if (rt_start) { rt_start = false; transport_start(); }
        if (rt_stop) { rt_stop = false; transport_stop(); }
        uint32_t now = g_ms;
        while ((int32_t)(now - last_ms) > 0) {
            last_ms++;
            clock_advance_ms();
        }
        lfo_update();
        update_leds();
        if ((int32_t)(g_ms - last_draw_ms) >= 40) {
            last_draw_ms = g_ms;
            draw_ui(); /* the lanes animate: redraw on a fixed cadence */
        }
        display_pump();
        bool pressed = !(GPIO_IDR(GPIOC) & (1u << PIN_POWER_BTN));
        if (power_seen_released && pressed)
            power_off();
        power_seen_released = power_seen_released || !pressed;
    }
}

void main_entry(void)
{
    /* Output pins: PB0 (panel latch), PB7 (power hold), PB9 (flash CS),
     * PB12/PB15/PC9 (gates). Everything idles high; gates are inverted by
     * their drivers, so high means inactive. */
    GPIO_BSRR(GPIOB) = (1u << PIN_PANEL_LATCH) | (1u << PIN_POWER_HOLD)
                     | (1u << PIN_FLASH_CS) | (1u << PIN_GATE_A) | (1u << PIN_GATE_B);
    GPIO_BSRR(GPIOC) = 1u << PIN_GATE_C;
    GPIO_MODER(GPIOB) |= (1u << (2 * PIN_PANEL_LATCH)) | (1u << (2 * PIN_POWER_HOLD))
                       | (1u << (2 * PIN_FLASH_CS)) | (1u << (2 * PIN_GATE_A))
                       | (1u << (2 * PIN_GATE_B));
    GPIO_MODER(GPIOC) |= 1u << (2 * PIN_GATE_C);

    /* Startup chords (BOOT-004): GLOBAL alone selects version mode,
     * GLOBAL+PATTERN selects update mode. Anything else boots normally. */
    uint32_t rows[8];
    panel_startup_scan(rows);
    bool global_held = (rows[SW_ROW(SW_GLOBAL)] >> SW_BIT(SW_GLOBAL)) & 1u;
    bool pattern_held = (rows[SW_ROW(SW_PATTERN)] >> SW_BIT(SW_PATTERN)) & 1u;
    uint32_t others = 0;
    for (int r = 0; r < 8; r++) {
        uint32_t buttons = rows[r] & (r == 0 ? 0x07ff0003u : r == 1 ? 0x07fe0003u : 0x07fe0000u);
        if (r == SW_ROW(SW_GLOBAL)) buttons &= ~(1u << SW_BIT(SW_GLOBAL));
        if (r == SW_ROW(SW_PATTERN)) buttons &= ~(1u << SW_BIT(SW_PATTERN));
        others |= buttons;
    }

    if (global_held && pattern_held && !others)
        updater_main();
    if (global_held && !pattern_held && !others)
        version_main();
    normal_main();
}
