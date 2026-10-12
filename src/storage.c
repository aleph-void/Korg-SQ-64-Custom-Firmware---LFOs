/* Polled SPI NOR driver (chip select on PB9) and settings persistence.
 * Commands used: 0x06 WREN, 0x03 read, 0x02 page program, 0x20 4K erase,
 * 0xd8 64K erase. Settings occupy one 4K sector well above the installed
 * firmware region, inside this firmware's declared user-data range. */
#include <stddef.h>

#include "storage.h"
#include "board.h"

int memcmp(const void *a, const void *b, size_t n); /* startup.c */

#define SETTINGS_MAGIC 0x314f464cu /* "LFO1" */

static void cs(bool selected)
{
    GPIO_BSRR(GPIOB) = selected ? 1u << (16 + PIN_FLASH_CS) : 1u << PIN_FLASH_CS;
}

static uint8_t xfer(uint8_t value)
{
    REG8(SPI_STORE_DR) = value;
    while (!(REG32(SPI_STORE_SR) & SPI_SR_RXNE)) { }
    return REG8(SPI_STORE_DR);
}

static void command_address(uint8_t command, uint32_t address)
{
    xfer(command);
    xfer((uint8_t)(address >> 16));
    xfer((uint8_t)(address >> 8));
    xfer((uint8_t)address);
}

static void write_enable(void)
{
    cs(true);
    xfer(0x06);
    cs(false);
}

void spi_flash_read(uint32_t address, uint8_t *data, uint32_t length)
{
    cs(true);
    command_address(0x03, address);
    while (length--) *data++ = xfer(0);
    cs(false);
}

void spi_flash_erase_4k(uint32_t address)
{
    write_enable();
    cs(true);
    command_address(0x20, address);
    cs(false);
}

void spi_flash_erase_64k(uint32_t address)
{
    write_enable();
    cs(true);
    command_address(0xd8, address);
    cs(false);
}

void spi_flash_program(uint32_t address, const uint8_t *data, uint32_t length)
{
    while (length) {
        uint32_t page_room = 256 - (address & 255);
        uint32_t chunk = length < page_room ? length : page_room;
        write_enable();
        cs(true);
        command_address(0x02, address);
        for (uint32_t i = 0; i < chunk; i++) xfer(data[i]);
        cs(false);
        address += chunk;
        data += chunk;
        length -= chunk;
    }
}

static uint32_t settings_hash(const settings_t *s)
{
    const uint8_t *bytes = (const uint8_t *)s;
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < sizeof(*s) - sizeof(uint32_t); i++)
        hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

void settings_defaults(settings_t *s)
{
    static const lfo_cfg_t defaults[4] = {
        /* wave       div         depth phase cc  dest */
        {WAVE_SINE, 7 /* 1/4  */, 100, 0, 1,  DEST_MOD}, /* A: mod wheel  */
        {WAVE_TRI,  5 /* 1/2  */, 100, 4, 2,  DEST_MOD}, /* B: breath, 90 degrees later */
        {WAVE_RISE, 3 /* 1BAR */, 100, 0, 11, DEST_MOD}, /* C: expression */
        {WAVE_SNH,  10 /* 1/8 */, 100, 0, 74, 0},        /* D: MIDI only  */
    };
    s->magic = SETTINGS_MAGIC;
    s->bpm = 120;
    s->selected = 0;
    s->reserved = 0;
    for (int t = 0; t < 4; t++) s->lfo[t] = defaults[t];
    s->hash = settings_hash(s);
}

static bool settings_valid(const settings_t *s)
{
    if (s->magic != SETTINGS_MAGIC || s->hash != settings_hash(s)) return false;
    if (s->bpm < 20 || s->bpm > 300 || s->selected > 3) return false;
    for (int t = 0; t < 4; t++) {
        const lfo_cfg_t *c = &s->lfo[t];
        if (c->wave >= WAVE_COUNT || c->div >= DIV_COUNT || c->depth > 100
            || c->phase > 15 || c->cc > 127 || c->dest > 7)
            return false;
    }
    return true;
}

bool settings_load(settings_t *s)
{
    spi_flash_read(SETTINGS_ADDR, (uint8_t *)s, sizeof(*s));
    if (settings_valid(s)) return true;
    settings_defaults(s);
    return false;
}

void settings_save(const settings_t *s)
{
    settings_t copy = *s;
    copy.magic = SETTINGS_MAGIC;
    copy.hash = settings_hash(&copy);
    /* Skip the erase/program cycle when the stored bytes already match. */
    settings_t stored;
    spi_flash_read(SETTINGS_ADDR, (uint8_t *)&stored, sizeof(stored));
    if (memcmp(&stored, &copy, sizeof(copy)) == 0) return;
    spi_flash_erase_4k(SETTINGS_ADDR);
    spi_flash_program(SETTINGS_ADDR, (const uint8_t *)&copy, sizeof(copy));
}
