#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stdint.h>

/* External SPI NOR primitives. Addresses below 0x30000 hold the installed
 * firmware image and are written only by the updater during an installation;
 * all application data lives at or above SETTINGS_ADDR. */
void spi_flash_read(uint32_t address, uint8_t *data, uint32_t length);
void spi_flash_erase_4k(uint32_t address);
void spi_flash_erase_64k(uint32_t address);
void spi_flash_program(uint32_t address, const uint8_t *data, uint32_t length);

#define SETTINGS_ADDR 0x40000u /* workspace settings, one 4 KiB sector  */
#define GLOBALS_ADDR  0x41000u /* global settings record, one 4 KiB sector */

/* Saved-project catalog: the 64 documented slot regions at
 * 0xC0000 + n * 0x3D000 (spec board contract). Each record occupies the
 * first 4 KiB sector of its slot region; the rest of the region is never
 * written. Slots beyond the detected flash capacity are unavailable. */
#define PROJECT_BASE   0x000c0000u
#define PROJECT_STRIDE 0x0003d000u
#define PROJECT_COUNT  64

/* LFO waveforms */
#define WAVE_SINE 0
#define WAVE_TRI  1
#define WAVE_RISE 2
#define WAVE_FALL 3
#define WAVE_SQR  4
#define WAVE_SNH  5
#define WAVE_COUNT 6

/* Output destinations (A/B/C: their jacks; D's gate bit pulses trigger 1). */
#define DEST_MOD   1u
#define DEST_PITCH 2u
#define DEST_GATE  4u

#define DIV_COUNT 16

/* Patterns: each track owns 16 of them; one is active per track and the
 * presence bitmap records which slots hold user content (PATTERN-001/002). */
#define PATTERN_COUNT 16

typedef struct {
    uint8_t wave;  /* 0..WAVE_COUNT-1 */
    uint8_t div;   /* 0..DIV_COUNT-1 synced rate division */
    uint8_t depth; /* 0..100 % */
    uint8_t phase; /* 0..15 sixteenths of a cycle */
    uint8_t cc;    /* 0..127 MIDI controller number */
    uint8_t dest;  /* DEST_* bit mask */
} lfo_cfg_t;

/* Workspace settings record ("LFO2"). The v1 record ("LFO1", one config per
 * track) is a supported older format and loads as pattern 1 of each track. */
typedef struct {
    uint32_t magic;
    uint16_t bpm;       /* 20..300, internal clock */
    uint8_t selected;   /* edited LFO 0..3 */
    uint8_t reserved;
    uint8_t active[4];  /* active pattern per track, 0..15 */
    uint16_t present[4]; /* presence bitmap per track; active bit always set */
    lfo_cfg_t pattern[4][PATTERN_COUNT];
    uint32_t hash;
} settings_t;

/* A project is the workspace's musical content: every pattern of every
 * track, the active/presence maps and the tempo ("LFP2"; v1 "LFP1" records
 * are a supported older format). The edited-LFO selection stays in the
 * settings record. */
typedef struct {
    uint32_t magic;
    uint16_t bpm;
    uint8_t reserved[2]; /* zero */
    uint8_t active[4];
    uint16_t present[4];
    lfo_cfg_t pattern[4][PATTERN_COUNT];
    uint32_t hash;
} project_t;

/* Global settings record ("LFG1"): device-wide behavior, separate from the
 * workspace and the saved-project catalog (GLOBAL-001/002). */
typedef struct {
    uint32_t magic;
    uint8_t clock_out; /* 0/1: emit MIDI clock while playing */
    uint8_t midi_base; /* 0..12: LFO n streams CC on channel midi_base+n */
    uint8_t trig_ms;   /* 5..100: LFO D trigger pulse length, ms */
    uint8_t cc_ms;     /* 1..50: minimum spacing between CC sends, ms */
    uint32_t hash;
} globals_t;

typedef enum {
    PROJECT_OK = 0,
    PROJECT_EMPTY,   /* slot is erased: no record */
    PROJECT_INVALID, /* record present but fails validation */
    PROJECT_UNAVAIL, /* slot lies beyond the detected flash capacity */
} project_status_t;

void storage_init(void); /* detect SPI capacity before any slot access */
int project_slots_available(void); /* 0..PROJECT_COUNT addressable slots */

void settings_defaults(settings_t *s);
bool settings_load(settings_t *s);
void settings_save(const settings_t *s);

void globals_defaults(globals_t *g);
bool globals_load(globals_t *g);
void globals_save(const globals_t *g);

void project_defaults(project_t *p);
project_status_t project_peek(int slot); /* fast header-only occupancy probe */
project_status_t project_load(int slot, project_t *p); /* fills p on OK only */
project_status_t project_save(int slot, const project_t *p);
project_status_t project_delete(int slot); /* deleting an empty slot is OK */

#endif
