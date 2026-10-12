/* Polled SPI NOR driver (chip select on PB9) and settings persistence.
 * Commands used: 0x06 WREN, 0x03 read, 0x02 page program, 0x20 4K erase,
 * 0xd8 64K erase. Settings occupy one 4K sector well above the installed
 * firmware region, inside this firmware's declared user-data range. */
#include <stddef.h>

#include "storage.h"
#include "board.h"

int memcmp(const void *a, const void *b, size_t n); /* startup.c */

#define SETTINGS_MAGIC 0x334f464cu /* "LFO3" */
#define PROJECT_MAGIC  0x3350464cu /* "LFP3" */
#define GLOBALS_MAGIC  0x3147464cu /* "LFG1" */

/* Supported older record formats (STORE-006). v2 has no step masks or swing:
 * it loads with every step on and neutral swing. v1 additionally has one
 * configuration per track, loading as pattern 1. Saves write v3 only. */
#define SETTINGS_V2_MAGIC 0x324f464cu /* "LFO2" */
#define PROJECT_V2_MAGIC  0x3250464cu /* "LFP2" */
#define SETTINGS_V1_MAGIC 0x314f464cu /* "LFO1" */
#define PROJECT_V1_MAGIC  0x3150464cu /* "LFP1" */

/* The v1/v2 per-pattern configuration: no step mask. */
typedef struct {
    uint8_t wave;
    uint8_t div;
    uint8_t depth;
    uint8_t phase;
    uint8_t cc;
    uint8_t dest;
} lfo_cfg_v2_t;

typedef struct {
    uint32_t magic;
    uint16_t bpm;
    uint8_t selected;
    uint8_t reserved;
    lfo_cfg_v2_t lfo[4];
    uint32_t hash;
} settings_v1_t;

typedef struct {
    uint32_t magic;
    uint16_t bpm;
    uint8_t reserved[2];
    lfo_cfg_v2_t lfo[4];
    uint32_t hash;
} project_v1_t;

typedef struct {
    uint32_t magic;
    uint16_t bpm;
    uint8_t selected;
    uint8_t reserved;
    uint8_t active[4];
    uint16_t present[4];
    lfo_cfg_v2_t pattern[4][PATTERN_COUNT];
    uint32_t hash;
} settings_v2_t;

typedef struct {
    uint32_t magic;
    uint16_t bpm;
    uint8_t reserved[2];
    uint8_t active[4];
    uint16_t present[4];
    lfo_cfg_v2_t pattern[4][PATTERN_COUNT];
    uint32_t hash;
} project_v2_t;

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

#define STEPS_ALL_ON {0xffff, 0xffff, 0xffff, 0xffff}
static const lfo_cfg_t LFO_DEFAULTS[4] = {
    /* wave       div         depth phase cc  dest      steps (all on) */
    {WAVE_SINE, 7 /* 1/4  */, 100, 0, 1,  DEST_MOD, STEPS_ALL_ON}, /* A: mod wheel  */
    {WAVE_TRI,  5 /* 1/2  */, 100, 4, 2,  DEST_MOD, STEPS_ALL_ON}, /* B: breath, 90 degrees later */
    {WAVE_RISE, 3 /* 1BAR */, 100, 0, 11, DEST_MOD, STEPS_ALL_ON}, /* C: expression */
    {WAVE_SNH,  10 /* 1/8 */, 100, 0, 74, 0,        STEPS_ALL_ON}, /* D: MIDI only  */
};

/* Every value of the step masks is meaningful, so only the scalar fields
 * constrain validity. */
static bool lfo_cfg_valid(const lfo_cfg_t *c)
{
    return c->wave < WAVE_COUNT && c->div < DIV_COUNT && c->depth <= 100
        && c->phase <= 15 && c->cc <= 127 && c->dest <= 7;
}

static bool lfo_cfg_v2_valid(const lfo_cfg_v2_t *c)
{
    return c->wave < WAVE_COUNT && c->div < DIV_COUNT && c->depth <= 100
        && c->phase <= 15 && c->cc <= 127 && c->dest <= 7;
}

/* Migrate a v1/v2 configuration: every step on. */
static void lfo_cfg_from_v2(lfo_cfg_t *c, const lfo_cfg_v2_t *v)
{
    c->wave = v->wave;
    c->div = v->div;
    c->depth = v->depth;
    c->phase = v->phase;
    c->cc = v->cc;
    c->dest = v->dest;
    for (int b = 0; b < BAR_COUNT; b++) c->steps[b] = 0xffff;
}

/* Every pattern of a fresh workspace holds its track's default config;
 * pattern 1 is active and the only one marked present. */
static void patterns_defaults(uint8_t active[4], uint16_t present[4],
                              lfo_cfg_t pattern[4][PATTERN_COUNT])
{
    for (int t = 0; t < 4; t++) {
        active[t] = 0;
        present[t] = 1;
        for (int p = 0; p < PATTERN_COUNT; p++) pattern[t][p] = LFO_DEFAULTS[t];
    }
}

static bool patterns_valid(const uint8_t active[4], const uint16_t present[4],
                           const lfo_cfg_t pattern[4][PATTERN_COUNT])
{
    for (int t = 0; t < 4; t++) {
        /* The active pattern must be marked present (PATTERN-002). */
        if (active[t] >= PATTERN_COUNT || !(present[t] >> active[t] & 1))
            return false;
        for (int p = 0; p < PATTERN_COUNT; p++)
            if (!lfo_cfg_valid(&pattern[t][p])) return false;
    }
    return true;
}

static bool patterns_v2_valid(const uint8_t active[4], const uint16_t present[4],
                              const lfo_cfg_v2_t pattern[4][PATTERN_COUNT])
{
    for (int t = 0; t < 4; t++) {
        if (active[t] >= PATTERN_COUNT || !(present[t] >> active[t] & 1))
            return false;
        for (int p = 0; p < PATTERN_COUNT; p++)
            if (!lfo_cfg_v2_valid(&pattern[t][p])) return false;
    }
    return true;
}

void settings_defaults(settings_t *s)
{
    s->magic = SETTINGS_MAGIC;
    s->bpm = 120;
    s->selected = 0;
    s->swing = SWING_MIN;
    patterns_defaults(s->active, s->present, s->pattern);
    s->hash = RECORD_HASH(s);
}

static bool settings_valid(const settings_t *s)
{
    if (s->magic != SETTINGS_MAGIC || s->hash != RECORD_HASH(s)) return false;
    if (s->bpm < 20 || s->bpm > 300 || s->selected > 3) return false;
    if (s->swing < SWING_MIN || s->swing > SWING_MAX) return false;
    return patterns_valid(s->active, s->present, s->pattern);
}

static bool lfo4_valid(const lfo_cfg_v2_t lfo[4], uint16_t bpm)
{
    if (bpm < 20 || bpm > 300) return false;
    for (int t = 0; t < 4; t++)
        if (!lfo_cfg_v2_valid(&lfo[t])) return false;
    return true;
}

bool settings_load(settings_t *s)
{
    spi_flash_read(SETTINGS_ADDR, (uint8_t *)s, sizeof(*s));
    if (settings_valid(s)) return true;
    /* Supported older formats: migrate a valid v2 or v1 record (STORE-006). */
    settings_v2_t v2;
    spi_flash_read(SETTINGS_ADDR, (uint8_t *)&v2, sizeof(v2));
    if (v2.magic == SETTINGS_V2_MAGIC && v2.hash == RECORD_HASH(&v2)
        && v2.bpm >= 20 && v2.bpm <= 300 && v2.selected <= 3
        && patterns_v2_valid(v2.active, v2.present, v2.pattern)) {
        settings_defaults(s);
        s->bpm = v2.bpm;
        s->selected = v2.selected;
        for (int t = 0; t < 4; t++) {
            s->active[t] = v2.active[t];
            s->present[t] = v2.present[t];
            for (int p = 0; p < PATTERN_COUNT; p++)
                lfo_cfg_from_v2(&s->pattern[t][p], &v2.pattern[t][p]);
        }
        s->hash = RECORD_HASH(s);
        return true;
    }
    settings_v1_t v1;
    spi_flash_read(SETTINGS_ADDR, (uint8_t *)&v1, sizeof(v1));
    if (v1.magic == SETTINGS_V1_MAGIC && v1.hash == RECORD_HASH(&v1)
        && v1.selected <= 3 && lfo4_valid(v1.lfo, v1.bpm)) {
        settings_defaults(s);
        s->bpm = v1.bpm;
        s->selected = v1.selected;
        for (int t = 0; t < 4; t++) lfo_cfg_from_v2(&s->pattern[t][0], &v1.lfo[t]);
        s->hash = RECORD_HASH(s);
        return true;
    }
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

/* --- Global settings record (GLOBAL-001..002) ---------------------------- */

void globals_defaults(globals_t *g)
{
    g->magic = GLOBALS_MAGIC;
    g->clock_out = 1;
    g->midi_base = 0;
    g->trig_ms = 20;
    g->cc_ms = 10;
    g->hash = RECORD_HASH(g);
}

static bool globals_valid(const globals_t *g)
{
    return g->magic == GLOBALS_MAGIC && g->hash == RECORD_HASH(g)
        && g->clock_out <= 1 && g->midi_base <= 12
        && g->trig_ms >= 5 && g->trig_ms <= 100
        && g->cc_ms >= 1 && g->cc_ms <= 50;
}

bool globals_load(globals_t *g)
{
    spi_flash_read(GLOBALS_ADDR, (uint8_t *)g, sizeof(*g));
    if (globals_valid(g)) return true;
    globals_defaults(g);
    return false;
}

void globals_save(const globals_t *g)
{
    globals_t copy = *g;
    copy.magic = GLOBALS_MAGIC;
    copy.hash = RECORD_HASH(&copy);
    globals_t stored;
    spi_flash_read(GLOBALS_ADDR, (uint8_t *)&stored, sizeof(stored));
    if (memcmp(&stored, &copy, sizeof(copy)) == 0) return;
    spi_flash_erase_4k(GLOBALS_ADDR);
    spi_flash_program(GLOBALS_ADDR, (const uint8_t *)&copy, sizeof(copy));
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
    p->swing = SWING_MIN;
    p->reserved = 0;
    patterns_defaults(p->active, p->present, p->pattern);
    p->hash = RECORD_HASH(p);
}

static bool project_valid(const project_t *p)
{
    if (p->magic != PROJECT_MAGIC || p->hash != RECORD_HASH(p)) return false;
    if (p->bpm < 20 || p->bpm > 300) return false;
    if (p->swing < SWING_MIN || p->swing > SWING_MAX) return false;
    return patterns_valid(p->active, p->present, p->pattern);
}

project_status_t project_load(int slot, project_t *p)
{
    project_t record;
    if (slot < 0 || slot >= project_slots_available()) return PROJECT_UNAVAIL;
    spi_flash_read(slot_addr(slot), (uint8_t *)&record, sizeof(record));
    const uint8_t *bytes = (const uint8_t *)&record;
    bool blank = true;
    for (uint32_t i = 0; i < sizeof(record) && blank; i++) blank = bytes[i] == 0xff;
    if (blank) return PROJECT_EMPTY;
    if (project_valid(&record)) {
        *p = record;
        return PROJECT_OK;
    }
    /* Supported older formats: migrate a valid v2 or v1 record (STORE-006). */
    project_v2_t v2;
    spi_flash_read(slot_addr(slot), (uint8_t *)&v2, sizeof(v2));
    if (v2.magic == PROJECT_V2_MAGIC && v2.hash == RECORD_HASH(&v2)
        && v2.bpm >= 20 && v2.bpm <= 300
        && patterns_v2_valid(v2.active, v2.present, v2.pattern)) {
        project_defaults(p);
        p->bpm = v2.bpm;
        for (int t = 0; t < 4; t++) {
            p->active[t] = v2.active[t];
            p->present[t] = v2.present[t];
            for (int i = 0; i < PATTERN_COUNT; i++)
                lfo_cfg_from_v2(&p->pattern[t][i], &v2.pattern[t][i]);
        }
        p->hash = RECORD_HASH(p);
        return PROJECT_OK;
    }
    project_v1_t v1;
    spi_flash_read(slot_addr(slot), (uint8_t *)&v1, sizeof(v1));
    if (v1.magic == PROJECT_V1_MAGIC && v1.hash == RECORD_HASH(&v1)
        && lfo4_valid(v1.lfo, v1.bpm)) {
        project_defaults(p);
        p->bpm = v1.bpm;
        for (int t = 0; t < 4; t++) lfo_cfg_from_v2(&p->pattern[t][0], &v1.lfo[t]);
        p->hash = RECORD_HASH(p);
        return PROJECT_OK;
    }
    return PROJECT_INVALID;
}

/* Fast occupancy probe for the catalog display: header bytes only, so a
 * 64-slot rescan stays cheap. Full validation happens on project_load. */
project_status_t project_peek(int slot)
{
    uint8_t head[16];
    if (slot < 0 || slot >= project_slots_available()) return PROJECT_UNAVAIL;
    spi_flash_read(slot_addr(slot), head, sizeof(head));
    bool blank = true;
    for (uint32_t i = 0; i < sizeof(head) && blank; i++) blank = head[i] == 0xff;
    if (blank) return PROJECT_EMPTY;
    uint32_t magic = head[0] | (uint32_t)head[1] << 8 | (uint32_t)head[2] << 16
                   | (uint32_t)head[3] << 24;
    return magic == PROJECT_MAGIC || magic == PROJECT_V2_MAGIC
        || magic == PROJECT_V1_MAGIC ? PROJECT_OK : PROJECT_INVALID;
}

project_status_t project_save(int slot, const project_t *p)
{
    if (slot < 0 || slot >= project_slots_available()) return PROJECT_UNAVAIL;
    project_t copy = *p;
    copy.magic = PROJECT_MAGIC;
    copy.reserved = 0;
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
    const uint8_t *bytes = (const uint8_t *)&record;
    bool blank = true;
    for (uint32_t i = 0; i < sizeof(record) && blank; i++) blank = bytes[i] == 0xff;
    if (blank) return PROJECT_OK; /* already empty: no redundant erase */
    spi_flash_erase_4k(slot_addr(slot));
    return PROJECT_OK;
}
