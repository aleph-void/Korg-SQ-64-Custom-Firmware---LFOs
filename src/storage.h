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

#define SETTINGS_ADDR 0x40000u

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

typedef struct {
    uint8_t wave;  /* 0..WAVE_COUNT-1 */
    uint8_t div;   /* 0..DIV_COUNT-1 synced rate division */
    uint8_t depth; /* 0..100 % */
    uint8_t phase; /* 0..15 sixteenths of a cycle */
    uint8_t cc;    /* 0..127 MIDI controller number */
    uint8_t dest;  /* DEST_* bit mask */
} lfo_cfg_t;

typedef struct {
    uint32_t magic;
    uint16_t bpm;      /* 20..300, internal clock */
    uint8_t selected;  /* edited LFO 0..3 */
    uint8_t reserved;
    lfo_cfg_t lfo[4];
    uint32_t hash;
} settings_t;

void settings_defaults(settings_t *s);
bool settings_load(settings_t *s);
void settings_save(const settings_t *s);

#endif
