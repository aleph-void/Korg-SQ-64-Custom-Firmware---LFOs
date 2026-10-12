/* Polled SPI NOR driver (chip select on PB9) and settings persistence.
 * Commands used: 0x06 WREN, 0x03 read, 0x02 page program, 0x20 4K erase,
 * 0xd8 64K erase. Settings occupy one 4K sector well above the installed
 * firmware region, inside this firmware's declared user-data range. */
#include <stddef.h>

#include "storage.h"
#include "board.h"

int memcmp(const void *a, const void *b, size_t n); /* startup.c */

#define SETTINGS_MAGIC 0x314f464cu /* "LFO1" */
#define PROJECT_MAGIC  0x3150464cu /* "LFP1" */

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

/* Detected SPI capacity, from the JEDEC density byte at storage_init. The
 * settings sector always fits; the project catalog needs a 16 MiB part, so
 * slots beyond the detected capacity report PROJECT_UNAVAIL instead of
 * issuing out-of-range transactions (the native model is only 1 MiB). */
static uint32_t spi_capacity = 0x100000u;

void storage_init(void)
{
    uint8_t id[3];
    cs(true);
    xfer(0x9f);
    for (int i = 0; i < 3; i++) id[i] = xfer(0);
    cs(false);
    spi_capacity = id[2] >= 20 && id[2] <= 24 ? 1u << id[2] : 0x100000u;
}

int project_slots_available(void)
{
    int n = 0;
    while (n < PROJECT_COUNT
           && PROJECT_BASE + (uint32_t)n * PROJECT_STRIDE + 4096u <= spi_capacity)
        n++;
    return n;
}

static uint32_t fnv1a(const void *data, uint32_t length)
{
    const uint8_t *bytes = data;
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < length; i++)
        hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

#define RECORD_HASH(r) fnv1a(r, sizeof(*(r)) - sizeof(uint32_t))

static const lfo_cfg_t LFO_DEFAULTS[4] = {
    /* wave       div         depth phase cc  dest */
    {WAVE_SINE, 7 /* 1/4  */, 100, 0, 1,  DEST_MOD}, /* A: mod wheel  */
    {WAVE_TRI,  5 /* 1/2  */, 100, 4, 2,  DEST_MOD}, /* B: breath, 90 degrees later */
    {WAVE_RISE, 3 /* 1BAR */, 100, 0, 11, DEST_MOD}, /* C: expression */
    {WAVE_SNH,  10 /* 1/8 */, 100, 0, 74, 0},        /* D: MIDI only  */
};

static bool lfo_cfg_valid(const lfo_cfg_t *c)
{
    return c->wave < WAVE_COUNT && c->div < DIV_COUNT && c->depth <= 100
        && c->phase <= 15 && c->cc <= 127 && c->dest <= 7;
}

void settings_defaults(settings_t *s)
{
    s->magic = SETTINGS_MAGIC;
    s->bpm = 120;
    s->selected = 0;
    s->reserved = 0;
    for (int t = 0; t < 4; t++) s->lfo[t] = LFO_DEFAULTS[t];
    s->hash = RECORD_HASH(s);
}

static bool settings_valid(const settings_t *s)
{
    if (s->magic != SETTINGS_MAGIC || s->hash != RECORD_HASH(s)) return false;
    if (s->bpm < 20 || s->bpm > 300 || s->selected > 3) return false;
    for (int t = 0; t < 4; t++)
        if (!lfo_cfg_valid(&s->lfo[t])) return false;
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
    copy.hash = RECORD_HASH(&copy);
    /* Skip the erase/program cycle when the stored bytes already match. */
    settings_t stored;
    spi_flash_read(SETTINGS_ADDR, (uint8_t *)&stored, sizeof(stored));
    if (memcmp(&stored, &copy, sizeof(copy)) == 0) return;
    spi_flash_erase_4k(SETTINGS_ADDR);
    spi_flash_program(SETTINGS_ADDR, (const uint8_t *)&copy, sizeof(copy));
}

/* --- Saved-project catalog (PROJECT-001..004) --------------------------- */

static uint32_t slot_addr(int slot)
{
    return PROJECT_BASE + (uint32_t)slot * PROJECT_STRIDE;
}

void project_defaults(project_t *p)
{
    p->magic = PROJECT_MAGIC;
    p->bpm = 120;
    p->reserved[0] = p->reserved[1] = 0;
    for (int t = 0; t < 4; t++) p->lfo[t] = LFO_DEFAULTS[t];
    p->hash = RECORD_HASH(p);
}

static bool project_valid(const project_t *p)
{
    if (p->magic != PROJECT_MAGIC || p->hash != RECORD_HASH(p)) return false;
    if (p->bpm < 20 || p->bpm > 300) return false;
    for (int t = 0; t < 4; t++)
        if (!lfo_cfg_valid(&p->lfo[t])) return false;
    return true;
}

static project_status_t classify(const project_t *p)
{
    const uint8_t *bytes = (const uint8_t *)p;
    bool blank = true;
    for (uint32_t i = 0; i < sizeof(*p) && blank; i++) blank = bytes[i] == 0xff;
    if (blank) return PROJECT_EMPTY;
    return project_valid(p) ? PROJECT_OK : PROJECT_INVALID;
}

project_status_t project_load(int slot, project_t *p)
{
    project_t record;
    if (slot < 0 || slot >= project_slots_available()) return PROJECT_UNAVAIL;
    spi_flash_read(slot_addr(slot), (uint8_t *)&record, sizeof(record));
    project_status_t status = classify(&record);
    if (status == PROJECT_OK) *p = record;
    return status;
}

project_status_t project_peek(int slot)
{
    project_t record;
    return project_load(slot, &record);
}

project_status_t project_save(int slot, const project_t *p)
{
    if (slot < 0 || slot >= project_slots_available()) return PROJECT_UNAVAIL;
    project_t copy = *p;
    copy.magic = PROJECT_MAGIC;
    copy.reserved[0] = copy.reserved[1] = 0;
    copy.hash = RECORD_HASH(&copy);
    uint32_t address = slot_addr(slot);
    project_t stored;
    spi_flash_read(address, (uint8_t *)&stored, sizeof(stored));
    if (memcmp(&stored, &copy, sizeof(copy)) == 0) return PROJECT_OK;
    spi_flash_erase_4k(address);
    spi_flash_program(address, (const uint8_t *)&copy, sizeof(copy));
    /* Completion is reported only once the committed bytes read back. */
    spi_flash_read(address, (uint8_t *)&stored, sizeof(stored));
    return memcmp(&stored, &copy, sizeof(copy)) == 0 ? PROJECT_OK : PROJECT_INVALID;
}

project_status_t project_delete(int slot)
{
    project_t record;
    if (slot < 0 || slot >= project_slots_available()) return PROJECT_UNAVAIL;
    spi_flash_read(slot_addr(slot), (uint8_t *)&record, sizeof(record));
    if (classify(&record) == PROJECT_EMPTY) return PROJECT_OK; /* already empty */
    spi_flash_erase_4k(slot_addr(slot));
    return PROJECT_OK;
}
