/*
 * chunk_sender.c
 *
 * Builds the Chunk Data and Update Light packet (0x2C) from a raw
 * decompressed chunk NBT buffer as produced by world_loader.
 *
 * Protocol: Java 1.21.x (protocol 774)
 * World format: 1.18+  (Y = -64 … +319, sections Y = -4 … +19, 24 sections)
 */

#include "chunk_sender.h"
#include "packet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* =========================================================================
 * Dynamic byte buffer
 * ========================================================================= */

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} dynbuf_t;

static int dynbuf_grow(dynbuf_t *b, size_t extra) {
    if (b->len + extra <= b->cap) return 1;
    size_t nc = b->cap ? b->cap : 4096;
    while (nc < b->len + extra) nc *= 2;
    uint8_t *p = (uint8_t *)realloc(b->data, nc);
    if (!p) return 0;
    b->data = p;
    b->cap  = nc;
    return 1;
}

static int db_bytes(dynbuf_t *b, const uint8_t *src, size_t n) {
    if (!dynbuf_grow(b, n)) return 0;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 1;
}

static int db_u8(dynbuf_t *b, uint8_t v) {
    return db_bytes(b, &v, 1);
}

static int db_be16(dynbuf_t *b, uint16_t v) {
    uint8_t buf[2] = {(uint8_t)(v >> 8), (uint8_t)(v & 0xFF)};
    return db_bytes(b, buf, 2);
}

static int db_be32(dynbuf_t *b, uint32_t v) {
    uint8_t buf[4] = {
        (uint8_t)(v >> 24), (uint8_t)(v >> 16),
        (uint8_t)(v >>  8), (uint8_t)(v & 0xFF)
    };
    return db_bytes(b, buf, 4);
}

static int db_be64(dynbuf_t *b, uint64_t v) {
    uint8_t buf[8] = {
        (uint8_t)(v >> 56), (uint8_t)(v >> 48),
        (uint8_t)(v >> 40), (uint8_t)(v >> 32),
        (uint8_t)(v >> 24), (uint8_t)(v >> 16),
        (uint8_t)(v >>  8), (uint8_t)(v & 0xFF)
    };
    return db_bytes(b, buf, 8);
}

static int db_varint(dynbuf_t *b, int32_t v) {
    uint8_t buf[5];
    size_t n = write_varint(buf, v);
    return db_bytes(b, buf, n);
}

/* Write a size_t position as a VarInt back into the buffer at pos (patch). */
static void db_patch_varint(dynbuf_t *b, size_t pos, int32_t v) {
    uint8_t buf[5];
    size_t n = write_varint(buf, v);
    /* The caller reserved exactly the right number of bytes */
    memcpy(b->data + pos, buf, n);
}

/* Reserve `n` bytes at current position and return the offset. */
static size_t db_reserve(dynbuf_t *b, size_t n) {
    size_t off = b->len;
    dynbuf_grow(b, n);
    memset(b->data + b->len, 0, n);
    b->len += n;
    return off;
}

/* =========================================================================
 * Block state global ID lookup table (1.21.x best-effort)
 *
 * Unknown blocks default to stone (1) — solid, safe fallback.
 * Non-solid decorative blocks are explicitly mapped to air (0).
 * ========================================================================= */

typedef struct { const char *name; int32_t id; } block_entry_t;

static const block_entry_t BLOCK_TABLE[] = {
    /* --- air variants -------------------------------------------------- */
    {"minecraft:air",                   0},
    {"minecraft:cave_air",              0},
    {"minecraft:void_air",              0},

    /* --- stone family -------------------------------------------------- */
    {"minecraft:stone",                 1},
    {"minecraft:granite",               2},
    {"minecraft:polished_granite",      3},
    {"minecraft:diorite",               4},
    {"minecraft:polished_diorite",      5},
    {"minecraft:andesite",              6},
    {"minecraft:polished_andesite",     7},

    /* --- dirt family --------------------------------------------------- */
    {"minecraft:grass_block",           9},
    {"minecraft:dirt",                 10},
    {"minecraft:coarse_dirt",          11},
    {"minecraft:podzol",               12},   /* snowy=false state */
    {"minecraft:rooted_dirt",          14},
    {"minecraft:mud",                  15},

    /* --- overworld terrain extras ------------------------------------- */
    {"minecraft:water",                86},
    {"minecraft:sand",               118},
    {"minecraft:oak_log",             137},
    {"minecraft:oak_leaves",          279},  /* default oak_leaves state for 1.21.11 */

    /* --- cobblestone / planks ------------------------------------------ */
    {"minecraft:cobblestone",          14},
    {"minecraft:oak_planks",           15},
    {"minecraft:spruce_planks",        16},
    {"minecraft:birch_planks",         17},
    {"minecraft:jungle_planks",        18},
    {"minecraft:acacia_planks",        19},
    {"minecraft:cherry_planks",        20},
    {"minecraft:dark_oak_planks",      21},
    {"minecraft:mangrove_planks",      26},
    {"minecraft:bamboo_planks",        27},
    {"minecraft:bamboo_mosaic",        28},

    /* --- non-solid decorative plants → air so player can walk through -- */
    {"minecraft:grass",                 0},
    {"minecraft:tall_grass",            0},
    {"minecraft:fern",                  0},
    {"minecraft:large_fern",            0},
    {"minecraft:dandelion",             0},
    {"minecraft:poppy",                 0},
    {"minecraft:blue_orchid",           0},
    {"minecraft:allium",                0},
    {"minecraft:azure_bluet",           0},
    {"minecraft:red_tulip",             0},
    {"minecraft:orange_tulip",          0},
    {"minecraft:white_tulip",           0},
    {"minecraft:pink_tulip",            0},
    {"minecraft:oxeye_daisy",           0},
    {"minecraft:cornflower",            0},
    {"minecraft:lily_of_the_valley",    0},
    {"minecraft:wither_rose",           0},
    {"minecraft:sunflower",             0},
    {"minecraft:lilac",                 0},
    {"minecraft:rose_bush",             0},
    {"minecraft:peony",                 0},
    {"minecraft:dead_bush",             0},
    {"minecraft:vine",                  0},
    {"minecraft:glow_lichen",           0},
    {"minecraft:wheat",                 0},
    {"minecraft:sugar_cane",            0},
    {"minecraft:bamboo",                0},
    {"minecraft:seagrass",              0},
    {"minecraft:tall_seagrass",         0},
    {"minecraft:kelp",                  0},
    {"minecraft:kelp_plant",            0},
    {"minecraft:snow",                  0},
    {"minecraft:nether_sprouts",        0},
    {"minecraft:twisting_vines",        0},
    {"minecraft:twisting_vines_plant",  0},
    {"minecraft:weeping_vines",         0},
    {"minecraft:weeping_vines_plant",   0},
    {"minecraft:cave_vines",            0},
    {"minecraft:cave_vines_plant",      0},
    {"minecraft:spore_blossom",         0},
    {"minecraft:azalea",                0},
    {"minecraft:flowering_azalea",      0},
    {"minecraft:moss_carpet",           0},
    {"minecraft:big_dripleaf",          0},
    {"minecraft:small_dripleaf",        0},
    {"minecraft:hanging_roots",         0},
    {"minecraft:sculk_vein",            0},
};

static int32_t lookup_block_id(const char *name, size_t name_len) {
    size_t n = sizeof(BLOCK_TABLE) / sizeof(BLOCK_TABLE[0]);
    for (size_t i = 0; i < n; i++) {
        const char *t = BLOCK_TABLE[i].name;
        size_t tlen = strlen(t);
        if (tlen == name_len && memcmp(t, name, name_len) == 0)
            return BLOCK_TABLE[i].id;
    }
    return 1; /* default: stone */
}

/* =========================================================================
 * Minimal streaming NBT reader
 * ========================================================================= */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} nbtr_t;

static int nbtr_can(const nbtr_t *r, size_t n)  { return r->pos + n <= r->len; }
static int nbtr_u8 (nbtr_t *r, uint8_t  *v)     {
    if (!nbtr_can(r, 1)) return 0;
    *v = r->data[r->pos++]; return 1;
}
static int nbtr_u16(nbtr_t *r, uint16_t *v)     {
    if (!nbtr_can(r, 2)) return 0;
    *v = (uint16_t)((r->data[r->pos] << 8) | r->data[r->pos+1]);
    r->pos += 2; return 1;
}
static int nbtr_u32(nbtr_t *r, uint32_t *v)     {
    if (!nbtr_can(r, 4)) return 0;
    *v = ((uint32_t)r->data[r->pos  ] << 24) |
         ((uint32_t)r->data[r->pos+1] << 16) |
         ((uint32_t)r->data[r->pos+2] <<  8) |
          (uint32_t)r->data[r->pos+3];
    r->pos += 4; return 1;
}
static int nbtr_name(nbtr_t *r, const uint8_t **name, uint16_t *nlen) {
    if (!nbtr_u16(r, nlen)) return 0;
    if (!nbtr_can(r, *nlen)) return 0;
    *name = r->data + r->pos;
    r->pos += *nlen; return 1;
}
static int nbtr_nameq(const uint8_t *name, uint16_t nlen, const char *lit) {
    size_t ll = strlen(lit);
    return nlen == (uint16_t)ll && memcmp(name, lit, ll) == 0;
}

static int nbtr_skip(nbtr_t *r, uint8_t type);

static int nbtr_skip_list(nbtr_t *r) {
    uint8_t etype; uint32_t cnt;
    if (!nbtr_u8(r, &etype) || !nbtr_u32(r, &cnt)) return 0;
    for (uint32_t i = 0; i < cnt; i++) {
        if (!nbtr_skip(r, etype)) return 0;
    }
    return 1;
}
static int nbtr_skip_compound(nbtr_t *r) {
    for (;;) {
        uint8_t t; const uint8_t *nm; uint16_t nl;
        if (!nbtr_u8(r, &t)) return 0;
        if (t == 0) return 1;
        if (!nbtr_name(r, &nm, &nl)) return 0;
        (void)nm; (void)nl;
        if (!nbtr_skip(r, t)) return 0;
    }
}
static int nbtr_skip(nbtr_t *r, uint8_t type) {
    uint16_t slen; uint32_t cnt;
    switch (type) {
    case 1: r->pos += 1; return r->pos <= r->len;
    case 2: r->pos += 2; return r->pos <= r->len;
    case 3: case 5: r->pos += 4; return r->pos <= r->len;
    case 4: case 6: r->pos += 8; return r->pos <= r->len;
    case 7:
        if (!nbtr_u32(r, &cnt)) return 0;
        r->pos += cnt; return r->pos <= r->len;
    case 8:
        if (!nbtr_u16(r, &slen)) return 0;
        r->pos += slen; return r->pos <= r->len;
    case 9: return nbtr_skip_list(r);
    case 10: return nbtr_skip_compound(r);
    case 11:
        if (!nbtr_u32(r, &cnt)) return 0;
        r->pos += cnt * 4; return r->pos <= r->len;
    case 12:
        if (!nbtr_u32(r, &cnt)) return 0;
        r->pos += cnt * 8; return r->pos <= r->len;
    default: return 0;
    }
}

/* =========================================================================
 * Chunk section data (parsed from NBT)
 * ========================================================================= */

#define MAX_PALETTE 256
#define NUM_SECTIONS 24           /* Y = -4 … +19 */
#define SECTION_Y_MIN (-4)

typedef struct {
    int32_t  global_ids[MAX_PALETTE]; /* mapped palette */
    int      palette_size;
    const uint8_t *data_ptr;          /* points to raw longs in nbt buffer */
    uint32_t       data_count;        /* number of 8-byte longs */
} section_bs_t;

typedef struct {
    int valid;
    section_bs_t bs;
} chunk_section_t;

typedef struct {
    const uint8_t *ptr;   /* pointer into nbt buffer */
    uint32_t count;       /* number of longs */
} hm_array_t;

/* =========================================================================
 * Parse a block_states palette list
 *
 * Reader is positioned at the start of the LIST payload
 * (element_type byte first).
 * ========================================================================= */
static int parse_bs_palette(nbtr_t *r, section_bs_t *bs) {
    uint8_t etype; uint32_t cnt;
    if (!nbtr_u8(r, &etype) || !nbtr_u32(r, &cnt)) return 0;
    if (etype != 10) {
        /* Not TAG_Compound list — skip and fail gracefully */
        for (uint32_t i = 0; i < cnt; i++) {
            if (!nbtr_skip(r, etype)) return 0;
        }
        return 0;
    }
    bs->palette_size = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        const char *block_name = NULL;
        size_t      block_name_len = 0;
        /* Walk the entry compound looking for "Name" TAG_String */
        for (;;) {
            uint8_t t; const uint8_t *nm; uint16_t nl;
            if (!nbtr_u8(r, &t)) return 0;
            if (t == 0) break; /* TAG_End */
            if (!nbtr_name(r, &nm, &nl)) return 0;
            if (t == 8 && nbtr_nameq(nm, nl, "Name")) {
                /* Read string length then string */
                uint16_t slen;
                if (!nbtr_u16(r, &slen)) return 0;
                if (!nbtr_can(r, slen)) return 0;
                block_name     = (const char *)(r->data + r->pos);
                block_name_len = slen;
                r->pos += slen;
            } else {
                if (!nbtr_skip(r, t)) return 0;
            }
        }
        if (bs->palette_size < MAX_PALETTE) {
            int32_t gid = block_name ?
                lookup_block_id(block_name, block_name_len) : 1;
            bs->global_ids[bs->palette_size++] = gid;
        }
    }
    return 1;
}

/* =========================================================================
 * Parse a single section compound (LIST element — no type/name prefix).
 * ========================================================================= */
static int parse_section(nbtr_t *r, chunk_section_t *sec) {
    memset(sec, 0, sizeof(*sec));
    int got_bs = 0;
    int y_set  = 0;
    int y_val  = 0;

    for (;;) {
        uint8_t t; const uint8_t *nm; uint16_t nl;
        if (!nbtr_u8(r, &t)) return 0;
        if (t == 0) break; /* TAG_End */
        if (!nbtr_name(r, &nm, &nl)) return 0;

        if (nbtr_nameq(nm, nl, "Y")) {
            if (t == 1) {           /* TAG_Byte */
                uint8_t bv;
                if (!nbtr_u8(r, &bv)) return 0;
                y_val = (int)(int8_t)bv;
            } else if (t == 2) {    /* TAG_Short */
                uint16_t sv;
                if (!nbtr_u16(r, &sv)) return 0;
                y_val = (int)(int16_t)sv;
            } else if (t == 3) {    /* TAG_Int */
                uint32_t iv;
                if (!nbtr_u32(r, &iv)) return 0;
                y_val = (int)(int32_t)iv;
            } else {
                if (!nbtr_skip(r, t)) return 0;
            }
            y_set = 1;

        } else if (nbtr_nameq(nm, nl, "block_states") && t == 10) {
            /* Parse block_states compound */
            for (;;) {
                uint8_t  bt; const uint8_t *bn; uint16_t bln;
                if (!nbtr_u8(r, &bt)) return 0;
                if (bt == 0) break;
                if (!nbtr_name(r, &bn, &bln)) return 0;

                if (nbtr_nameq(bn, bln, "palette") && bt == 9) {
                    if (!parse_bs_palette(r, &sec->bs)) {
                        /* gracefully skip rest of compound */
                        nbtr_skip_compound(r);
                        goto next_tag;
                    }
                } else if (nbtr_nameq(bn, bln, "data") && bt == 12) {
                    uint32_t lcount;
                    if (!nbtr_u32(r, &lcount)) return 0;
                    if (!nbtr_can(r, (size_t)lcount * 8)) return 0;
                    sec->bs.data_ptr   = r->data + r->pos;
                    sec->bs.data_count = lcount;
                    r->pos += (size_t)lcount * 8;
                } else {
                    if (!nbtr_skip(r, bt)) return 0;
                }
            }
            got_bs = 1;

        } else {
            if (!nbtr_skip(r, t)) return 0;
        }
        next_tag:;
    }

    if (y_set && got_bs) {
        sec->valid = 1;
        /* Store y into a helper field reachable outside */
        /* We'll use a global array indexed by y+4 in the caller */
        (void)y_val; /* stored via out-param below */
    }
    /* Return y through a side-channel — caller assigned via index */
    return y_set ? (y_val + 128) : -1; /* encode y for caller */
}

/* =========================================================================
 * Parse heightmap long array from within a compound.
 * Reader positioned AFTER the name of the long_array tag.
 * Returns pointer into nbt buffer and count.
 * ========================================================================= */
static int parse_hm_longarray(nbtr_t *r, hm_array_t *out) {
    uint32_t cnt;
    if (!nbtr_u32(r, &cnt)) return 0;
    if (!nbtr_can(r, (size_t)cnt * 8)) return 0;
    out->ptr   = r->data + r->pos;
    out->count = cnt;
    r->pos += (size_t)cnt * 8;
    return 1;
}

/* =========================================================================
 * ceil_log2
 * ========================================================================= */
static int ceil_log2_i(int n) {
    if (n <= 1) return 0;
    int bits = 0, v = n - 1;
    while (v > 0) { bits++; v >>= 1; }
    return bits;
}

/* =========================================================================
 * Write a paletted container for block states
 * ========================================================================= */
static int write_bs_container(dynbuf_t *b, const section_bs_t *bs) {
    if (bs->palette_size <= 0) {
        /* empty/invalid — single-value air */
        if (!db_u8(b, 0)) return 0;
        return db_varint(b, 0);
    }
    if (bs->palette_size == 1) {
        /* Single-value container */
        if (!db_u8(b, 0)) return 0;
        return db_varint(b, bs->global_ids[0]);
    }
    /* Indirect palette */
    int bits = ceil_log2_i(bs->palette_size);
    if (bits < 4) bits = 4;
    if (bits > 8) {
        /* Direct format required (>256 palette entries — very rare):
         * fall back to single-value stone for safety */
        if (!db_u8(b, 0)) return 0;
        return db_varint(b, 1); /* stone */
    }
    if (!db_u8(b, (uint8_t)bits)) return 0;
    if (!db_varint(b, bs->palette_size)) return 0;
    for (int i = 0; i < bs->palette_size; i++) {
        if (!db_varint(b, bs->global_ids[i])) return 0;
    }
    /* no size prefix: fixed data length is implied by bits-per-entry */
    if (bs->data_count > 0 && bs->data_ptr) {
        if (!db_bytes(b, bs->data_ptr, (size_t)bs->data_count * 8)) return 0;
    }
    return 1;
}

static uint64_t read_u64_be_ptr(const uint8_t *p) {
    return ((uint64_t)p[0] << 56) |
           ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) |
           ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) |
           ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] <<  8) |
            (uint64_t)p[7];
}

static void write_u64_be_ptr(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)(v & 0xFF);
}

/*
 * Count non-air blocks in a section from palette + packed data.
 * Returns 0..4096.
 */
static uint16_t section_non_air_count(const section_bs_t *bs) {
    if (bs->palette_size <= 0) return 0;

    if (bs->palette_size == 1) {
        return (bs->global_ids[0] == 0) ? 0 : 4096;
    }

    int bits = ceil_log2_i(bs->palette_size);
    if (bits < 4) bits = 4;
    if (bits > 8) {
        /* We don't currently emit direct-format containers; keep safe fallback. */
        return 4096;
    }

    if (bs->data_ptr == NULL || bs->data_count == 0) {
        return 0;
    }

    uint16_t count = 0;
    uint64_t mask = ((uint64_t)1u << bits) - 1u;
    uint32_t values_per_long = (uint32_t)(64 / bits);

    if (values_per_long == 0) {
        return 0;
    }

    for (int i = 0; i < 4096; i++) {
        uint32_t long_index = (uint32_t)i / values_per_long;
        uint32_t start_bit = ((uint32_t)i % values_per_long) * (uint32_t)bits;

        if (long_index >= bs->data_count) {
            break;
        }

        uint64_t lo = read_u64_be_ptr(bs->data_ptr + (size_t)long_index * 8);
        int palette_index = (int)((lo >> start_bit) & mask);
        if (palette_index >= 0 && palette_index < bs->palette_size) {
            if (bs->global_ids[palette_index] != 0) {
                count++;
            }
        }
    }

    return count;
}

/* Single-value biome container — always plains (ID 0) */
static int write_biome_container(dynbuf_t *b) {
    if (!db_u8(b, 0)) return 0;     /* bits per entry = 0 */
    return db_varint(b, 0);         /* biome ID 0 (plains) */
}

/* =========================================================================
 * Serialize Heightmaps (protocol 774):
 * VarInt count + repeated { VarInt type, VarInt long_count, i64[] }
 * type mapping: world_surface=1, motion_blocking=4
 * ========================================================================= */
static int write_heightmaps(dynbuf_t *b,
                            const hm_array_t *motion,
                            const hm_array_t *surface) {
    const hm_array_t *hm_motion = motion;
    const hm_array_t *hm_surface = surface;
    static uint8_t hm_zero[37 * 8] = {0};
    static hm_array_t hm_zero_arr = {hm_zero, 37};

    if (hm_surface == NULL || hm_surface->ptr == NULL || hm_surface->count == 0) {
        hm_surface = &hm_zero_arr;
    }
    if (hm_motion == NULL || hm_motion->ptr == NULL || hm_motion->count == 0) {
        hm_motion = hm_surface;
    }

    /* count = 2 entries: world_surface and motion_blocking */
    if (!db_varint(b, 2)) return 0;

    /* world_surface (id=1) */
    if (!db_varint(b, 1)) return 0;
    if (!db_varint(b, (int32_t)hm_surface->count)) return 0;
    if (!db_bytes(b, hm_surface->ptr, (size_t)hm_surface->count * 8)) return 0;

    /* motion_blocking (id=4) */
    if (!db_varint(b, 4)) return 0;
    if (!db_varint(b, (int32_t)hm_motion->count)) return 0;
    if (!db_bytes(b, hm_motion->ptr, (size_t)hm_motion->count * 8)) return 0;

    return 1;
}

/* =========================================================================
 * Light data
 *
 * 1.18+ has 24 sections (Y=-4…+19) plus 2 border sections = 26 total.
 * We send full sky light (0xFF) for all 26 and empty block light for all 26.
 * ========================================================================= */
#define LIGHT_SECTIONS 26
#define LIGHT_MASK     ((uint64_t)0x3FFFFFF) /* bits 0-25 set */

/* Pre-initialize light data at compile time to avoid race conditions */
static uint8_t FULL_SKY_FF[2048];
static int light_data_initialized = 0;

void init_light_data_once(void) {
    if (!light_data_initialized) {
        memset(FULL_SKY_FF, 0xFF, sizeof(FULL_SKY_FF));
        light_data_initialized = 1;
        fprintf(stderr, "[INIT] Light data pre-initialized (thread-safe)\n");
        fflush(stderr);
    }
}

static int write_light_data(dynbuf_t *b) {
    /* Light data should be pre-initialized by main() before threading starts */
    fprintf(stderr, "[LIGHT] write_light_data START (initialized=%d)\n", light_data_initialized);
    fflush(stderr);

    /* Sky Light Mask: all 26 sections have sky light arrays */
    fprintf(stderr, "[LIGHT] Writing sky light mask\n");
    fflush(stderr);
    if (!db_varint(b, 1)) {
        fprintf(stderr, "[LIGHT] ERROR: db_varint(1) failed\n");
        fflush(stderr);
        return 0;
    }
    if (!db_be64(b, LIGHT_MASK)) {
        fprintf(stderr, "[LIGHT] ERROR: db_be64(LIGHT_MASK) failed\n");
        fflush(stderr);
        return 0;
    }

    /* Block Light Mask: 0 (no block light arrays) */
    fprintf(stderr, "[LIGHT] Writing block light mask\n");
    fflush(stderr);
    if (!db_varint(b, 1)) {
        fprintf(stderr, "[LIGHT] ERROR: db_varint(1) failed (block)\n");
        fflush(stderr);
        return 0;
    }
    if (!db_be64(b, 0)) {
        fprintf(stderr, "[LIGHT] ERROR: db_be64(0) failed\n");
        fflush(stderr);
        return 0;
    }

    /* Empty Sky Light Mask: 0 (we provide data for all sections) */
    fprintf(stderr, "[LIGHT] Writing empty sky light mask\n");
    fflush(stderr);
    if (!db_varint(b, 1)) {
        fprintf(stderr, "[LIGHT] ERROR: db_varint(1) failed (empty sky)\n");
        fflush(stderr);
        return 0;
    }
    if (!db_be64(b, 0)) {
        fprintf(stderr, "[LIGHT] ERROR: db_be64(0) failed (empty sky)\n");
        fflush(stderr);
        return 0;
    }

    /* Empty Block Light Mask: all sections are empty block-light */
    fprintf(stderr, "[LIGHT] Writing empty block light mask\n");
    fflush(stderr);
    if (!db_varint(b, 1)) {
        fprintf(stderr, "[LIGHT] ERROR: db_varint(1) failed (empty block)\n");
        fflush(stderr);
        return 0;
    }
    if (!db_be64(b, LIGHT_MASK)) {
        fprintf(stderr, "[LIGHT] ERROR: db_be64(LIGHT_MASK) failed (empty block)\n");
        fflush(stderr);
        return 0;
    }

    /* Sky Light Arrays: 26 x 2048 bytes of 0xFF */
    fprintf(stderr, "[LIGHT] Writing %d sky light arrays\n", LIGHT_SECTIONS);
    fflush(stderr);
    if (!db_varint(b, LIGHT_SECTIONS)) {
        fprintf(stderr, "[LIGHT] ERROR: db_varint(LIGHT_SECTIONS=%d) failed\n", LIGHT_SECTIONS);
        fflush(stderr);
        return 0;
    }
    for (int i = 0; i < LIGHT_SECTIONS; i++) {
        fprintf(stderr, "[LIGHT] Writing sky array %d\n", i);
        fflush(stderr);
        if (!db_varint(b, 2048)) {
            fprintf(stderr, "[LIGHT] ERROR: db_varint(2048) failed at array %d\n", i);
            fflush(stderr);
            return 0;
        }
        if (!db_bytes(b, FULL_SKY_FF, 2048)) {
            fprintf(stderr, "[LIGHT] ERROR: db_bytes(2048) failed at array %d\n", i);
            fflush(stderr);
            return 0;
        }
    }

    /* Block Light Arrays: 0 entries */
    fprintf(stderr, "[LIGHT] Writing 0 block light arrays\n");
    fflush(stderr);
    if (!db_varint(b, 0)) {
        fprintf(stderr, "[LIGHT] ERROR: db_varint(0) failed (block arrays)\n");
        fflush(stderr);
        return 0;
    }

    fprintf(stderr, "[LIGHT] write_light_data SUCCESS\n");
    fflush(stderr);
    return 1;
}

/* =========================================================================
 * Main entry point
 * ========================================================================= */
uint8_t *build_chunk_data_packet(const uint8_t *nbt, size_t nbt_len,
                                  int32_t chunk_x, int32_t chunk_z,
                                  size_t *out_len) {
    if (!nbt || nbt_len < 3) return NULL;

    /* ----------------------------------------------------------------
     * Phase 1: Parse the NBT to extract sections and heightmaps.
     * ---------------------------------------------------------------- */

    chunk_section_t *sections = (chunk_section_t *)malloc(NUM_SECTIONS * sizeof(chunk_section_t));
    if (!sections) return NULL;
    
    memset(sections, 0, NUM_SECTIONS * sizeof(chunk_section_t));

    hm_array_t hm_motion  = {NULL, 0};
    hm_array_t hm_surface = {NULL, 0};

    nbtr_t r;
    r.data = nbt;
    r.len  = nbt_len;
    r.pos  = 0;

    /* Skip root TAG_Compound type byte + name */
    {
        uint8_t root_type;
        if (!nbtr_u8(&r, &root_type)) goto build;
        if (root_type != 10) goto build;
        uint16_t root_name_len;
        if (!nbtr_u16(&r, &root_name_len)) goto build;
        r.pos += root_name_len;
    }

    /* Walk root compound looking for "sections" and "Heightmaps" */
    for (;;) {
        uint8_t t; const uint8_t *nm; uint16_t nl;
        if (!nbtr_u8(&r, &t) || t == 0) break;
        if (!nbtr_name(&r, &nm, &nl)) break;

        if (nbtr_nameq(nm, nl, "sections") && t == 9) {
            /* TAG_List of TAG_Compound */
            uint8_t etype; uint32_t cnt;
            if (!nbtr_u8(&r, &etype) || !nbtr_u32(&r, &cnt)) break;
            if (etype != 10) {
                /* Unexpected element type — skip */
                for (uint32_t i = 0; i < cnt; i++)
                    nbtr_skip(&r, etype);
                continue;
            }
            for (uint32_t si = 0; si < cnt; si++) {
                /* Parse one section compound (no type/name prefix in list) */
                int y_val = 0;
                int y_set = 0;
                chunk_section_t tmp;
                memset(&tmp, 0, sizeof(tmp));

                for (;;) {
                    uint8_t st; const uint8_t *sn; uint16_t snl;
                    if (!nbtr_u8(&r, &st) || st == 0) break;
                    if (!nbtr_name(&r, &sn, &snl)) goto done_sections;

                    if (nbtr_nameq(sn, snl, "Y")) {
                        if (st == 1) {
                            uint8_t bv; nbtr_u8(&r, &bv); y_val=(int)(int8_t)bv;
                        } else if (st == 2) {
                            uint16_t sv; nbtr_u16(&r, &sv); y_val=(int)(int16_t)sv;
                        } else if (st == 3) {
                            uint32_t iv; nbtr_u32(&r, &iv); y_val=(int)(int32_t)iv;
                        } else {
                            nbtr_skip(&r, st);
                        }
                        y_set = 1;

                    } else if (nbtr_nameq(sn, snl, "block_states") && st == 10) {
                        for (;;) {
                            uint8_t bt; const uint8_t *bn; uint16_t bnl;
                            if (!nbtr_u8(&r, &bt) || bt == 0) break;
                            if (!nbtr_name(&r, &bn, &bnl)) goto done_sections;

                            if (nbtr_nameq(bn, bnl, "palette") && bt == 9) {
                                parse_bs_palette(&r, &tmp.bs);
                            } else if (nbtr_nameq(bn, bnl, "data") && bt == 12) {
                                uint32_t lc;
                                if (!nbtr_u32(&r, &lc)) goto done_sections;
                                if (!nbtr_can(&r, (size_t)lc * 8)) goto done_sections;
                                tmp.bs.data_ptr   = r.data + r.pos;
                                tmp.bs.data_count = lc;
                                r.pos += (size_t)lc * 8;
                            } else {
                                if (!nbtr_skip(&r, bt)) goto done_sections;
                            }
                        }
                        tmp.valid = (tmp.bs.palette_size > 0);

                    } else {
                        if (!nbtr_skip(&r, st)) goto done_sections;
                    }
                }

                if (y_set) {
                    int idx = y_val - SECTION_Y_MIN;
                    if (idx >= 0 && idx < NUM_SECTIONS) {
                        sections[idx] = tmp;
                        sections[idx].valid = tmp.valid;
                    }
                }
            }
            done_sections:;

        } else if (nbtr_nameq(nm, nl, "Heightmaps") && t == 10) {
            for (;;) {
                uint8_t ht; const uint8_t *hn; uint16_t hnl;
                if (!nbtr_u8(&r, &ht) || ht == 0) break;
                if (!nbtr_name(&r, &hn, &hnl)) break;
                if (nbtr_nameq(hn, hnl, "MOTION_BLOCKING") && ht == 12) {
                    parse_hm_longarray(&r, &hm_motion);
                } else if (nbtr_nameq(hn, hnl, "WORLD_SURFACE") && ht == 12) {
                    parse_hm_longarray(&r, &hm_surface);
                } else {
                    nbtr_skip(&r, ht);
                }
            }
        } else {
            if (!nbtr_skip(&r, t)) break;
        }
    }

    /* ----------------------------------------------------------------
     * Phase 2: Serialize packet
     * ---------------------------------------------------------------- */
build:;
    dynbuf_t b;
    memset(&b, 0, sizeof(b));

    /* Packet ID */
    if (!db_varint(&b, 0x2C)) goto fail;

    /* Chunk X, Chunk Z (Int = 4 bytes big-endian) */
    if (!db_be32(&b, (uint32_t)chunk_x)) goto fail;
    if (!db_be32(&b, (uint32_t)chunk_z)) goto fail;

    /*
     * Use fixed-size zeroed heightmaps (37 longs each) to avoid malformed
     * heightmap extraction from chunk NBT causing client decode failures.
     */
    {
        static uint8_t hm_zero[37 * 8] = {0};
        hm_array_t hm_safe = {hm_zero, 37};
        if (!write_heightmaps(&b, &hm_safe, &hm_safe)) goto fail;
    }

    /* --- Sections data (VarInt-prefixed byte array) --- */
    /* Reserve space for the VarInt length prefix (up to 5 bytes) */
    size_t data_len_pos = b.len;
    if (!dynbuf_grow(&b, 5)) goto fail;
    b.len += 5; /* will patch below */

    size_t data_start = b.len;

    for (int i = 0; i < NUM_SECTIONS; i++) {
        const chunk_section_t *sec = &sections[i];

        uint16_t block_count = sec->valid ? section_non_air_count(&sec->bs) : 0;
        if (!db_be16(&b, block_count)) goto fail;

        if (sec->valid) {
            if (!write_bs_container(&b, &sec->bs)) goto fail;
        } else {
            /* empty section: single-value air */
            if (!db_u8(&b, 0)) goto fail;
            if (!db_varint(&b, 0)) goto fail;
        }
        /* biomes: single-value plains (0) */
        if (!write_biome_container(&b)) goto fail;
    }

    size_t data_end = b.len;
    size_t data_bytes = data_end - data_start;

    /* Patch the VarInt for data length */
    {
        uint8_t vi[5];
        size_t vi_len = write_varint(vi, (int32_t)data_bytes);
        /* We reserved 5 bytes; shift remaining data left if vi_len < 5 */
        size_t shift = 5 - vi_len;
        if (shift > 0) {
            memmove(b.data + data_len_pos + vi_len,
                    b.data + data_start,
                    data_bytes);
            b.len -= shift;
        }
        memcpy(b.data + data_len_pos, vi, vi_len);
    }

    /* Block entities: count = 0 */
    if (!db_varint(&b, 0)) goto fail;

    /* Light data */
    if (!write_light_data(&b)) goto fail;

    *out_len = b.len;
    free(sections);
    return b.data;

fail:
    free(sections);
    free(b.data);
    return NULL;
}

uint8_t *build_debug_flat_chunk_packet(int32_t chunk_x, int32_t chunk_z,
                                       size_t *out_len) {
    dynbuf_t b;
    memset(&b, 0, sizeof(b));

    if (!out_len) return NULL;

    /* Packet ID */
    if (!db_varint(&b, 0x2C)) goto fail;

    /* Chunk X, Chunk Z */
    if (!db_be32(&b, (uint32_t)chunk_x)) goto fail;
    if (!db_be32(&b, (uint32_t)chunk_z)) goto fail;

    /* Valid zeroed heightmaps (37 longs each). */
    {
        static uint8_t hm_zero[37 * 8] = {0};
        hm_array_t hm_safe = {hm_zero, 37};
        if (!write_heightmaps(&b, &hm_safe, &hm_safe)) goto fail;
    }

    /* Sections data */
    size_t data_len_pos = b.len;
    if (!dynbuf_grow(&b, 5)) goto fail;
    b.len += 5;
    size_t data_start = b.len;

    /* Y=64..79 corresponds to section y=4 in -64..319 world */
    const int stone_section_idx = 4 - SECTION_Y_MIN;

    for (int i = 0; i < NUM_SECTIONS; i++) {
        if (i == stone_section_idx) {
            if (!db_be16(&b, 4096)) goto fail;
            /* single-value container: stone */
            if (!db_u8(&b, 0)) goto fail;
            if (!db_varint(&b, 1)) goto fail;
        } else {
            if (!db_be16(&b, 0)) goto fail;
            /* single-value container: air */
            if (!db_u8(&b, 0)) goto fail;
            if (!db_varint(&b, 0)) goto fail;
        }

        if (!write_biome_container(&b)) goto fail;
    }

    {
        size_t data_bytes = b.len - data_start;
        uint8_t vi[5];
        size_t vi_len = write_varint(vi, (int32_t)data_bytes);
        size_t shift = 5 - vi_len;
        if (shift > 0) {
            memmove(b.data + data_len_pos + vi_len,
                    b.data + data_start,
                    data_bytes);
            b.len -= shift;
        }
        memcpy(b.data + data_len_pos, vi, vi_len);
    }

    /* Block entities = 0 */
    if (!db_varint(&b, 0)) goto fail;

    /* Light */
    if (!write_light_data(&b)) goto fail;

    *out_len = b.len;
    return b.data;

fail:
    free(b.data);
    return NULL;
}

int32_t generated_world_surface_y(int32_t block_x, int32_t block_z) {
    double x = (double)block_x;
    double z = (double)block_z;
    double continental = sin(x * 0.0028) * 14.0 + cos(z * 0.0026) * 11.0;
    double ridges = sin((x + z) * 0.0062) * 7.5 + cos((x - z) * 0.0054) * 6.0;
    double detail = sin(x * 0.045) * 4.5 + cos(z * 0.040) * 3.5;
    double height = 71.0 + continental + ridges + detail;
    double river_signal = fabs(sin(x * 0.0092) + cos(z * 0.0101) + sin((x - z) * 0.0068) * 0.6);

    if (river_signal < 0.26) {
        double river_t = (0.26 - river_signal) / 0.26;
        double carve = 4.0 + river_t * river_t * 14.0;
        height -= carve;
    }

    if (height < 44.0) {
        height = 44.0;
    }
    if (height > 114.0) {
        height = 114.0;
    }

    return (int32_t)floor(height);
}

#define GENERATED_SEA_LEVEL 64

static uint32_t generated_world_hash(int32_t block_x, int32_t block_z) {
    uint32_t x = (uint32_t)block_x;
    uint32_t z = (uint32_t)block_z;
    uint32_t hash = x * 374761393u;

    hash += z * 668265263u;
    hash = (hash ^ (hash >> 13)) * 1274126177u;
    return hash ^ (hash >> 16);
}

static int generated_world_chunk_local(int32_t value) {
    int local = value % 16;

    if (local < 0) {
        local += 16;
    }
    return local;
}

static int generated_world_has_tree(int32_t block_x, int32_t block_z, int32_t surface_y) {
    int32_t slope_x;
    int32_t slope_z;
    int local_x = generated_world_chunk_local(block_x);
    int local_z = generated_world_chunk_local(block_z);
    int chance_mod = 64;
    double moisture = sin((double)block_x * 0.0075) + cos((double)block_z * 0.0065);

    if (surface_y <= GENERATED_SEA_LEVEL + 1 || surface_y >= 100) {
        return 0;
    }
    if (local_x < 1 || local_x > 14 || local_z < 1 || local_z > 14) {
        return 0;
    }

    slope_x = abs(surface_y - generated_world_surface_y(block_x + 1, block_z));
    slope_x += abs(surface_y - generated_world_surface_y(block_x - 1, block_z));
    slope_z = abs(surface_y - generated_world_surface_y(block_x, block_z + 1));
    slope_z += abs(surface_y - generated_world_surface_y(block_x, block_z - 1));
    if (slope_x + slope_z > 3) {
        return 0;
    }

    if (moisture > 0.8) {
        chance_mod = 34;
    } else if (moisture > 0.2) {
        chance_mod = 46;
    } else if (moisture < -0.6) {
        chance_mod = 92;
    }

    if (surface_y > 86) {
        chance_mod += 24;
    }

    return (generated_world_hash(block_x, block_z) % (uint32_t)chance_mod) == 0u;
}

static int generated_world_tree_height(int32_t block_x, int32_t block_z) {
    return 4 + (int)((generated_world_hash(block_x, block_z) >> 8) % 3u);
}

static int32_t generated_world_tree_block(int32_t world_x,
                                          int32_t world_y,
                                          int32_t world_z,
                                          int32_t oak_log_id,
                                          int32_t oak_leaves_id) {
    for (int dz = -2; dz <= 2; ++dz) {
        for (int dx = -2; dx <= 2; ++dx) {
            int32_t trunk_x = world_x - dx;
            int32_t trunk_z = world_z - dz;
            int32_t trunk_surface_y = generated_world_surface_y(trunk_x, trunk_z);
            int trunk_height;
            int crown_base_y;
            int leaf_radius;
            int leaf_dx;
            int leaf_dz;
            int leaf_dy;

            if (!generated_world_has_tree(trunk_x, trunk_z, trunk_surface_y)) {
                continue;
            }

            trunk_height = generated_world_tree_height(trunk_x, trunk_z);
            if (world_x == trunk_x && world_z == trunk_z &&
                world_y > trunk_surface_y &&
                world_y <= trunk_surface_y + trunk_height) {
                return oak_log_id;
            }

            crown_base_y = trunk_surface_y + trunk_height - 2;
            leaf_dy = (int)(world_y - crown_base_y);
            if (leaf_dy < 0 || leaf_dy > 3) {
                continue;
            }

            leaf_radius = (leaf_dy == 3) ? 1 : 2;
            leaf_dx = abs(dx);
            leaf_dz = abs(dz);
            if (leaf_dx > leaf_radius || leaf_dz > leaf_radius) {
                continue;
            }
            if (leaf_dy == 3 && leaf_dx + leaf_dz > 1) {
                continue;
            }
            if (leaf_dy == 0 && leaf_dx == 2 && leaf_dz == 2) {
                continue;
            }
            if (world_x == trunk_x && world_z == trunk_z &&
                world_y <= trunk_surface_y + trunk_height) {
                continue;
            }

            return oak_leaves_id;
        }
    }

    return 0;
}

static int build_generated_section(section_bs_t *bs,
                                   uint16_t *non_air_count,
                                   uint8_t *packed_out,
                                   int32_t chunk_x,
                                   int32_t chunk_z,
                                   int section_y) {
    int32_t palette_ids[8];
    uint8_t palette_indices[4096];
    uint64_t *longs = NULL;
    int palette_size = 0;
    int bits;
    uint32_t long_count;
    int y_base = (section_y + SECTION_Y_MIN) * 16;
    uint16_t non_air = 0;
    
    FILE *sec_log = fopen("section_generation.log", "a");
    if (sec_log) {
        fprintf(sec_log, "[SECTION_START] chunk (%d,%d) section %d\n", chunk_x, chunk_z, section_y);
        fflush(sec_log);
        fclose(sec_log);
    }
    int32_t air_id = 0;
    int32_t stone_id = 1;
    int32_t grass_id = 9;
    int32_t dirt_id = 10;
    int32_t water_id = 86;
    int32_t sand_id = 118;
    int32_t oak_log_id = 137;
    int32_t oak_leaves_id = 279;
    int32_t surface_y_cache[16 * 16];

    for (int lz = 0; lz < 16; ++lz) {
        for (int lx = 0; lx < 16; ++lx) {
            int32_t world_x = chunk_x * 16 + lx;
            int32_t world_z = chunk_z * 16 + lz;
            surface_y_cache[(lz << 4) | lx] = generated_world_surface_y(world_x, world_z);
        }
    }

    memset(bs, 0, sizeof(*bs));
    memset(palette_indices, 0, sizeof(palette_indices));
    /* Note: longs is NULL here, will be allocated later */

    for (int ly = 0; ly < 16; ++ly) {
        int32_t world_y = y_base + ly;
        for (int lz = 0; lz < 16; ++lz) {
            for (int lx = 0; lx < 16; ++lx) {
                int32_t world_x = chunk_x * 16 + lx;
                int32_t world_z = chunk_z * 16 + lz;
                int32_t surface_y = surface_y_cache[(lz << 4) | lx];
                int32_t block_id = air_id;
                int32_t tree_block_id;
                int idx = (ly << 8) | (lz << 4) | lx;
                int palette_index = 0;

                if (world_y <= surface_y) {
                    if (world_y == surface_y) {
                        block_id = (surface_y <= GENERATED_SEA_LEVEL + 1) ? sand_id : grass_id;
                    } else if (world_y >= surface_y - 3) {
                        block_id = (surface_y <= GENERATED_SEA_LEVEL + 3) ? sand_id : dirt_id;
                    } else {
                        block_id = stone_id;
                    }
                } else if (world_y <= GENERATED_SEA_LEVEL) {
                    block_id = water_id;
                } else {
                    if (world_y <= surface_y + 8) {
                        tree_block_id = generated_world_tree_block(world_x,
                                                                   world_y,
                                                                   world_z,
                                                                   oak_log_id,
                                                                   oak_leaves_id);
                        if (tree_block_id != air_id) {
                            block_id = tree_block_id;
                        }
                    }
                }

                for (palette_index = 0; palette_index < palette_size; ++palette_index) {
                    if (palette_ids[palette_index] == block_id) {
                        break;
                    }
                }
                if (palette_index == palette_size) {
                    if (palette_size >= 8) {
                        FILE *fail_log = fopen("section_generation.log", "a");
                        if (fail_log) {
                            fprintf(fail_log, "[SECTION_FAIL] chunk (%d,%d) section %d - palette overflow\n", chunk_x, chunk_z, section_y);
                            fflush(fail_log);
                            fclose(fail_log);
                        }
                        return 0;
                    }
                    palette_ids[palette_size++] = block_id;
                }

                palette_indices[idx] = (uint8_t)palette_index;
                if (block_id != air_id) {
                    non_air += 1;
                }
            }
        }
    }

    if (non_air_count != NULL) {
        *non_air_count = non_air;
    }

    bs->palette_size = palette_size;
    for (int i = 0; i < palette_size; ++i) {
        bs->global_ids[i] = palette_ids[i];
    }

    if (palette_size <= 1) {
        bs->data_ptr = NULL;
        bs->data_count = 0;
        return 1;
    }

    bits = ceil_log2_i(palette_size);
    if (bits < 4) {
        bits = 4;
    }

    {
        uint32_t values_per_long = (uint32_t)(64 / bits);
        long_count = (4096u + values_per_long - 1u) / values_per_long;
        
        /* Safety check: long_count should never exceed a reasonable size */
        if (long_count > 1024) {
            fprintf(stderr, "[ERROR] build_generated_section: long_count=%u exceeded max (bits=%d), chunk (%d,%d) section %d\n",
                    long_count, bits, chunk_x, chunk_z, section_y);
            FILE *fail_log = fopen("section_generation.log", "a");
            if (fail_log) {
                fprintf(fail_log, "[SECTION_FAIL] chunk (%d,%d) section %d - long_count %u exceeded 1024\n", chunk_x, chunk_z, section_y, long_count);
                fflush(fail_log);
                fclose(fail_log);
            }
            return 0;
        }
    }
    
    /* Allocate longs array with proper size */
    longs = (uint64_t *)malloc(long_count * sizeof(uint64_t));
    if (!longs) {
        FILE *fail_log = fopen("section_generation.log", "a");
        if (fail_log) {
            fprintf(fail_log, "[SECTION_FAIL] chunk (%d,%d) section %d - malloc longs failed\n", chunk_x, chunk_z, section_y);
            fflush(fail_log);
            fclose(fail_log);
        }
        return 0;
    }
    memset(longs, 0, long_count * sizeof(uint64_t));

    for (int i = 0; i < 4096; ++i) {
        uint32_t values_per_long = (uint32_t)(64 / bits);
        uint32_t long_index = (uint32_t)i / values_per_long;
        uint32_t start_bit = ((uint32_t)i % values_per_long) * (uint32_t)bits;
        uint64_t value = (uint64_t)palette_indices[i];
        
        /* Ensure value fits in the allocated bits */
        uint64_t mask = (1ULL << bits) - 1;
        value &= mask;

        if (start_bit + bits > 64) {
            fprintf(stderr, "[ERROR] build_generated_section: bit shift overflow at i=%d, start_bit=%u, bits=%d\n", 
                    i, start_bit, bits);
            free(longs);
            return 0;
        }

        longs[long_index] |= value << start_bit;
    }

    for (uint32_t i = 0; i < long_count; ++i) {
        if (!longs || i >= long_count) {
            fprintf(stderr, "[ERROR] build_generated_section: long_count overflow or null longs\n");
            free(longs);
            return 0;
        }
        /* Safety check: make sure we don't write beyond packed_out buffer (256 longs max = 2048 bytes) */
        if (i >= 256) {
            fprintf(stderr, "[ERROR] build_generated_section: writing beyond packed_out buffer (i=%u)\n", i);
            free(longs);
            return 0;
        }
        write_u64_be_ptr(packed_out + (size_t)i * 8u, longs[i]);
    }

    bs->data_ptr = packed_out;
    bs->data_count = long_count;
    free(longs);
    
    FILE *sec_end = fopen("section_generation.log", "a");
    if (sec_end) {
        fprintf(sec_end, "[SECTION_OK] chunk (%d,%d) section %d - success\n", chunk_x, chunk_z, section_y);
        fflush(sec_end);
        fclose(sec_end);
    }
    return 1;
}

uint8_t *build_generated_overworld_chunk_packet(int32_t chunk_x, int32_t chunk_z,
                                                size_t *out_len) {
    dynbuf_t b;
    chunk_section_t *sections = NULL;
    uint8_t **packed_data = NULL;
    uint16_t *non_air_counts = NULL;
    int i;

    fprintf(stderr, "[DEBUG] build_generated_overworld_chunk_packet START: chunk (%d,%d)\n", chunk_x, chunk_z);
    fflush(stderr);
    
    /* Also log to file for crash debugging */
    FILE *debug_log = fopen("chunk_debug.log", "a");
    if (debug_log) {
        fprintf(debug_log, "[DEBUG] build_generated_overworld_chunk_packet START: chunk (%d,%d)\n", chunk_x, chunk_z);
        fflush(debug_log);
        fclose(debug_log);
    }

    if (!out_len) {
        fprintf(stderr, "[ERROR] build_generated_overworld_chunk_packet: out_len is NULL\n");
        return NULL;
    }

    /* Allocate large buffers on heap instead of stack to avoid stack overflow */
    fprintf(stderr, "[DEBUG] Allocating sections: %zu bytes\n", NUM_SECTIONS * sizeof(chunk_section_t));
    fflush(stderr);
    sections = (chunk_section_t *)malloc(NUM_SECTIONS * sizeof(chunk_section_t));
    if (!sections) {
        fprintf(stderr, "[ERROR] Failed to malloc sections: %zu bytes\n", NUM_SECTIONS * sizeof(chunk_section_t));
        fflush(stderr);
        goto fail;
    }

    fprintf(stderr, "[DEBUG] Allocating packed_data array: %zu bytes\n", NUM_SECTIONS * sizeof(uint8_t *));
    fflush(stderr);
    packed_data = (uint8_t **)malloc(NUM_SECTIONS * sizeof(uint8_t *));
    if (!packed_data) {
        fprintf(stderr, "[ERROR] Failed to malloc packed_data\n");
        fflush(stderr);
        free(sections);
        goto fail;
    }

    fprintf(stderr, "[DEBUG] Allocating packed_data buffers (%d x 2048 bytes)\n", NUM_SECTIONS);
    fflush(stderr);
    for (i = 0; i < NUM_SECTIONS; ++i) {
        packed_data[i] = (uint8_t *)malloc(256 * 8);
        if (!packed_data[i]) {
            fprintf(stderr, "[ERROR] Failed to malloc packed_data[%d]\n", i);
            fflush(stderr);
            for (int j = 0; j < i; ++j) {
                free(packed_data[j]);
            }
            free(packed_data);
            free(sections);
            goto fail;
        }
    }

    fprintf(stderr, "[DEBUG] Allocating non_air_counts: %zu bytes\n", NUM_SECTIONS * sizeof(uint16_t));
    fflush(stderr);
    non_air_counts = (uint16_t *)malloc(NUM_SECTIONS * sizeof(uint16_t));
    if (!non_air_counts) {
        fprintf(stderr, "[ERROR] Failed to malloc non_air_counts\n");
        fflush(stderr);
        for (i = 0; i < NUM_SECTIONS; ++i) {
            free(packed_data[i]);
        }
        free(packed_data);
        free(sections);
        goto fail;
    }

    memset(&b, 0, sizeof(b));
    memset(sections, 0, NUM_SECTIONS * sizeof(chunk_section_t));
    for (i = 0; i < NUM_SECTIONS; ++i) {
        memset(packed_data[i], 0, 256 * 8);
    }
    memset(non_air_counts, 0, NUM_SECTIONS * sizeof(uint16_t));

    for (int i = 0; i < NUM_SECTIONS; ++i) {
        sections[i].valid = 1;
        fprintf(stderr, "[DEBUG] build_generated_overworld_chunk_packet: generating section %d for chunk (%d,%d)\n", i, chunk_x, chunk_z);
        fflush(stderr);
        
        FILE *debug_log = fopen("chunk_debug.log", "a");
        if (debug_log) {
            fprintf(debug_log, "[DEBUG] Section %d for chunk (%d,%d) - generating...\n", i, chunk_x, chunk_z);
            fflush(debug_log);
            fclose(debug_log);
        }
        
        if (!build_generated_section(&sections[i].bs,
                                     &non_air_counts[i],
                                     packed_data[i],
                                     chunk_x,
                                     chunk_z,
                                     i)) {
            fprintf(stderr, "[ERROR] build_generated_section failed for chunk (%d,%d) section %d\n", chunk_x, chunk_z, i);
            
            FILE *err_log = fopen("chunk_debug.log", "a");
            if (err_log) {
                fprintf(err_log, "[ERROR] build_generated_section FAILED for chunk (%d,%d) section %d\n", chunk_x, chunk_z, i);
                fflush(err_log);
                fclose(err_log);
            }
            goto fail;
        }
        
        FILE *success_log = fopen("chunk_debug.log", "a");
        if (success_log) {
            fprintf(success_log, "[OK] Section %d for chunk (%d,%d) - generated\n", i, chunk_x, chunk_z);
            fflush(success_log);
            fclose(success_log);
        }
    }

    if (!db_varint(&b, 0x2C)) {
        fprintf(stderr, "[ERROR] Failed to write packet ID\n");
        fflush(stderr);
        goto fail;
    }
    if (!db_be32(&b, (uint32_t)chunk_x)) {
        fprintf(stderr, "[ERROR] Failed to write chunk_x\n");
        fflush(stderr);
        goto fail;
    }
    if (!db_be32(&b, (uint32_t)chunk_z)) {
        fprintf(stderr, "[ERROR] Failed to write chunk_z\n");
        fflush(stderr);
        goto fail;
    }

    fprintf(stderr, "[DEBUG] Writing heightmaps for chunk (%d,%d)\n", chunk_x, chunk_z);
    fflush(stderr);
    {
        static uint8_t hm_zero[37 * 8] = {0};
        hm_array_t hm_safe = {hm_zero, 37};
        if (!write_heightmaps(&b, &hm_safe, &hm_safe)) {
            fprintf(stderr, "[ERROR] Failed to write_heightmaps\n");
            fflush(stderr);
            goto fail;
        }
    }

    {
        size_t data_len_pos = b.len;
        size_t data_start;
        fprintf(stderr, "[DEBUG] Growing dynbuf for data length varint\n");
        fflush(stderr);
        if (!dynbuf_grow(&b, 5)) {
            fprintf(stderr, "[ERROR] Failed to grow dynbuf\n");
            fflush(stderr);
            goto fail;
        }
        b.len += 5;
        data_start = b.len;

        fprintf(stderr, "[DEBUG] Writing %d sections to dynbuf\n", NUM_SECTIONS);
        fflush(stderr);
        for (int i = 0; i < NUM_SECTIONS; ++i) {
            if (!db_be16(&b, non_air_counts[i])) {
                fprintf(stderr, "[ERROR] Failed to write non_air_count[%d]\n", i);
                fflush(stderr);
                goto fail;
            }
            if (!write_bs_container(&b, &sections[i].bs)) {
                fprintf(stderr, "[ERROR] Failed to write_bs_container for section %d\n", i);
                fflush(stderr);
                goto fail;
            }
            if (!write_biome_container(&b)) {
                fprintf(stderr, "[ERROR] Failed to write_biome_container for section %d\n", i);
                fflush(stderr);
                goto fail;
            }
        }

        {
            size_t data_bytes = b.len - data_start;
            uint8_t vi[5];
            size_t vi_len = write_varint(vi, (int32_t)data_bytes);
            size_t shift = 5 - vi_len;
            fprintf(stderr, "[DEBUG] Data bytes: %zu, varint_len: %zu, shift: %zu\n", data_bytes, vi_len, shift);
            fflush(stderr);
            if (shift > 0) {
                if (!b.data) {
                    fprintf(stderr, "[ERROR] b.data is NULL before memmove\n");
                    fflush(stderr);
                    goto fail;
                }
                fprintf(stderr, "[DEBUG] Performing memmove: src=%p, dst=%p, size=%zu\n", 
                        b.data + data_start, b.data + data_len_pos + vi_len, data_bytes);
                fflush(stderr);
                memmove(b.data + data_len_pos + vi_len,
                        b.data + data_start,
                        data_bytes);
                b.len -= shift;
            }
            memcpy(b.data + data_len_pos, vi, vi_len);
        }
    }

    fprintf(stderr, "[DEBUG] Writing biome count\n");
    fflush(stderr);
    if (!db_varint(&b, 0)) {
        fprintf(stderr, "[ERROR] Failed to write biome count\n");
        fflush(stderr);
        goto fail;
    }
    
    fprintf(stderr, "[DEBUG] Writing light data\n");
    fflush(stderr);
    if (!write_light_data(&b)) {
        fprintf(stderr, "[ERROR] Failed to write_light_data\n");
        fflush(stderr);
        goto fail;
    }

    *out_len = b.len;
    /* Free heap allocations before returning success */
    fprintf(stderr, "[DEBUG] build_generated_overworld_chunk_packet SUCCESS: chunk (%d,%d), packet_len=%zu\n", chunk_x, chunk_z, b.len);
    fflush(stderr);
    
    FILE *success_log = fopen("chunk_debug.log", "a");
    if (success_log) {
        fprintf(success_log, "[SUCCESS] Chunk (%d,%d) generated successfully, size=%zu\n", chunk_x, chunk_z, b.len);
        fflush(success_log);
        fclose(success_log);
    }
    for (int j = 0; j < NUM_SECTIONS; ++j) {
        free(packed_data[j]);
    }
    free(packed_data);
    free(sections);
    free(non_air_counts);
    return b.data;

fail:
    fprintf(stderr, "[ERROR] build_generated_overworld_chunk_packet FAILED: chunk (%d,%d)\n", chunk_x, chunk_z);
    fflush(stderr);
    
    FILE *fail_log = fopen("chunk_debug.log", "a");
    if (fail_log) {
        fprintf(fail_log, "[FAILED] Chunk (%d,%d) generation failed\n", chunk_x, chunk_z);
        fflush(fail_log);
        fclose(fail_log);
    }
    if (packed_data) {
        for (int j = 0; j < NUM_SECTIONS; ++j) {
            if (packed_data[j]) {
                free(packed_data[j]);
            }
        }
        free(packed_data);
    }
    if (sections) {
        free(sections);
    }
    if (non_air_counts) {
        free(non_air_counts);
    }
    free(b.data);
    return NULL;
}
