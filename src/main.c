/* Synced LFOs for the SQ-64: boot mode selection and normal mode.
 *
 * Four low-frequency oscillators run phase-locked to a shared 24 PPQN
 * transport position. The position advances from the internal tempo clock,
 * or follows incoming MIDI clock whenever one is present (auto-switching),
 * so every LFO stays in sync with the beat and with the other LFOs. Each
 * LFO has a waveform, a tempo-synced rate division, depth, a phase offset
 * and a MIDI CC number. LFOs A/B/C can drive their mod CV, pitch CV and
 * gate jacks; LFO D pulses trigger jack 1 once per cycle. All four stream
 * MIDI CCs on channels 1..4. GLOBAL commits settings to SPI flash and the
 * power button saves and shuts down cleanly.
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
static bool playing;

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
        if (playing) midi_send((const uint8_t[]){0xf8}, 1);
    }
}

static uint16_t wave_value(int i, uint16_t phase)
{
    uint32_t v;
    switch (cfg.lfo[i].wave) {
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

static void lfo_update(void)
{
    uint32_t now = g_ms;
    for (int i = 0; i < 4; i++) {
        const lfo_cfg_t *c = &cfg.lfo[i];
        uint32_t period = DIV_TICKS[c->div];
        uint16_t raw = (uint16_t)(((pos_tick % period) << 16 | pos_frac) / period);
        if (raw < lfo_phase_raw[i]) { /* cycle wrapped */
            prng_step(i);
            if (i == 3 && (c->dest & DEST_GATE)) {
                panel_triggers(1);
                trigger_off_ms = now + 20;
            }
        }
        lfo_phase_raw[i] = raw;
        uint16_t value = wave_value(i, (uint16_t)(raw + c->phase * 4096u));
        uint16_t out = (uint16_t)((uint32_t)value * c->depth / 100u);
        lfo_out[i] = out;
        if (i < 3) {
            if (c->dest & DEST_MOD) cv_mod16(i, out);
            if (c->dest & DEST_PITCH) cv_pitch16(i, out);
            if (c->dest & DEST_GATE) gate_set(i, value >= 32768);
        }
        uint8_t cc_value = (uint8_t)(out >> 9);
        if (cc_value != cc_last[i] && now - cc_sent_ms[i] >= 10) {
            uint8_t msg[3] = {(uint8_t)(0xb0 | i), c->cc, cc_value};
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

static void toggle_dest(uint8_t bit)
{
    lfo_cfg_t *c = &cfg.lfo[cfg.selected];
    if (cfg.selected == 3 && bit != DEST_GATE) return; /* D is MIDI + trigger */
    c->dest ^= bit;
    if (!(c->dest & bit)) dest_cleared(cfg.selected, bit);
}

static void handle_events(void)
{
    panel_event_t ev;
    while (panel_event_next(&ev)) {
        if (!ev.down) continue;
        switch (ev.sw) {
        case SW_PLAY: transport_start(); continue;
        case SW_STOP: transport_stop(); continue;
        case SW_TAP: {
            uint32_t now = g_ms;
            uint32_t gap = now - tap_last_ms;
            if (gap >= 200 && gap <= 3000) {
                uint32_t bpm = 60000 / gap;
                cfg.bpm = (uint16_t)(bpm < 20 ? 20 : bpm > 300 ? 300 : bpm);
            }
            tap_last_ms = now;
            continue;
        }
        case SW_TRACK_A: cfg.selected = 0; continue;
        case SW_TRACK_B: cfg.selected = 1; continue;
        case SW_TRACK_C: cfg.selected = 2; continue;
        case SW_TRACK_D: cfg.selected = 3; continue;
        case SW_MOD: toggle_dest(DEST_MOD); continue;
        case SW_PITCH: toggle_dest(DEST_PITCH); continue;
        case SW_GATE: toggle_dest(DEST_GATE); continue;
        case SW_GLOBAL: settings_save(&cfg); continue;
        default: break;
        }
        for (int t = 0; t < 4; t++)
            for (int p = 0; p < 16; p++)
                if (pad_switch(t, p) == ev.sw) {
                    cfg.selected = (uint8_t)t;
                    if (panel_down(SW_SHIFT)) {
                        if (p < WAVE_COUNT) cfg.lfo[t].wave = (uint8_t)p;
                    } else {
                        cfg.lfo[t].div = (uint8_t)p;
                    }
                }
    }
    lfo_cfg_t *c = &cfg.lfo[cfg.selected];
    int d;
    if ((d = panel_encoder_take(ENC_TEMPO))) {
        int bpm = cfg.bpm + d;
        cfg.bpm = (uint16_t)(bpm < 20 ? 20 : bpm > 300 ? 300 : bpm);
    }
    if ((d = panel_encoder_take(ENC_SWING))) {
        int cc = c->cc + d;
        c->cc = (uint8_t)(cc < 0 ? 0 : cc > 127 ? 127 : cc);
    }
    if ((d = panel_encoder_take(ENC_PARAM1))) {
        int wave = c->wave + d;
        c->wave = (uint8_t)(wave < 0 ? 0 : wave >= WAVE_COUNT ? WAVE_COUNT - 1 : wave);
    }
    if ((d = panel_encoder_take(ENC_PARAM2))) {
        int div = c->div + d;
        c->div = (uint8_t)(div < 0 ? 0 : div >= DIV_COUNT ? DIV_COUNT - 1 : div);
    }
    if ((d = panel_encoder_take(ENC_PARAM3))) {
        int depth = c->depth + d;
        c->depth = (uint8_t)(depth < 0 ? 0 : depth > 100 ? 100 : depth);
    }
    if ((d = panel_encoder_take(ENC_PARAM4))) {
        c->phase = (uint8_t)((c->phase + d) & 15);
    }
}

static void update_leds(void)
{
    uint32_t rows[8];
    for (int i = 0; i < 4; i++) {
        /* Each LFO's row of pads is a live level bar. */
        int lit = (lfo_out[i] >> 12) + 1; /* 1..16 pads */
        uint32_t cols = ((1u << lit) - 1) << 1;
        if (i == cfg.selected) cols |= 1; /* track select LED, column 0 */
        rows[i * 2] = cols;
        rows[i * 2 + 1] = 0;
    }
    /* Destination LEDs for the edited LFO (row 0 columns 18..20). */
    const lfo_cfg_t *c = &cfg.lfo[cfg.selected];
    if (c->dest & DEST_GATE) rows[0] |= 1u << 18;
    if (c->dest & DEST_PITCH) rows[0] |= 1u << 19;
    if (c->dest & DEST_MOD) rows[0] |= 1u << 20;
    panel_led_columns(rows);
    bool beat = pos_tick % 24 < 2;
    panel_transport_leds(beat, !playing, ext_active(), playing);
}

static void draw_wave_preview(int x, int y, int w, int h, int lfo)
{
    for (int i = 0; i < w; i++) {
        uint16_t phase = (uint16_t)((i << 16) / w + cfg.lfo[lfo].phase * 4096u);
        int v = wave_value(lfo, phase) * (h - 1) / 65535;
        fb_pixel(x + i, y + h - 1 - v, 9);
    }
    int cursor = lfo_phase_raw[lfo] * w / 65536;
    fb_fill(x + cursor, y, 1, h, 15);
}

static void draw_ui(void)
{
    const lfo_cfg_t *c = &cfg.lfo[cfg.selected];
    uint16_t bpm_shown = cfg.bpm;
    if (ext_active()) bpm_shown = (uint16_t)(2500 / ext_gap_ms);
    fb_clear();
    fb_text(2, 1, "LFOS", 15);
    fb_number(60, 1, bpm_shown, 12);
    fb_text(84, 1, "BPM", 8);
    fb_text(112, 1, ext_active() ? "EXT" : "INT", ext_active() ? 15 : 8);
    fb_text(216, 1, playing ? "PLAY" : "STOP", playing ? 15 : 6);

    fb_text(2, 12, "LFO", 8);
    fb_text(26, 12, (const char[]){(char)('A' + cfg.selected), 0}, 15);
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

    /* One lane per LFO: name, wave, division, CC and a live level bar. */
    for (int i = 0; i < 4; i++) {
        const lfo_cfg_t *lc = &cfg.lfo[i];
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
    }
    draw_wave_preview(222, 26, 32, 28, cfg.selected);
    fb_text(2, 57, "GLOBAL SAVES", 5);
    display_flush();
}

static void power_off(void)
{
    if (playing) transport_stop();
    outputs_all_off();
    panel_triggers(0);
    settings_save(&cfg);
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
    settings_load(&cfg);
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
