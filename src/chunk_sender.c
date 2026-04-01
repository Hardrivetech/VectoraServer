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
    {"minecraft:grass_block",           8},   /* snowy=false state */
    {"minecraft:dirt",                 10},
    {"minecraft:coarse_dirt",          11},
    {"minecraft:podzol",               12},   /* snowy=false state */
    {"minecraft:rooted_dirt",          14},
    {"minecraft:mud",                  15},

    /* --- cobblestone / planks ------------------------------------------ */
    {"minecraft:cobblestone",          16},
    {"minecraft:oak_planks",           17},
    {"minecraft:spruce_planks",        18},
    {"minecraft:birch_planks",         19},
    {"minecraft:jungle_planks",        20},
    {"minecraft:acacia_planks",        21},
    {"minecraft:cherry_planks",        22},
    {"minecraft:dark_oak_planks",      23},
    {"minecraft:mangrove_planks",      24},
    {"minecraft:bamboo_planks",        25},
    {"minecraft:bamboo_mosaic",        26},

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
        if (!db_varint(b, 0)) return 0;
        return db_varint(b, 0);
    }
    if (bs->palette_size == 1) {
        /* Single-value container */
        if (!db_u8(b, 0)) return 0;
        if (!db_varint(b, bs->global_ids[0])) return 0;
        return db_varint(b, 0);
    }
    /* Indirect palette */
    int bits = ceil_log2_i(bs->palette_size);
    if (bits < 4) bits = 4;
    if (bits > 8) {
        /* Direct format required (>256 palette entries — very rare):
         * fall back to single-value stone for safety */
        if (!db_u8(b, 0)) return 0;
        if (!db_varint(b, 1)) return 0; /* stone */
        return db_varint(b, 0);
    }
    if (!db_u8(b, (uint8_t)bits)) return 0;
    if (!db_varint(b, bs->palette_size)) return 0;
    for (int i = 0; i < bs->palette_size; i++) {
        if (!db_varint(b, bs->global_ids[i])) return 0;
    }
    if (!db_varint(b, (int32_t)bs->data_count)) return 0;
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

    for (int i = 0; i < 4096; i++) {
        uint32_t bit_index = (uint32_t)i * (uint32_t)bits;
        uint32_t long_index = bit_index >> 6;
        uint32_t start_bit = bit_index & 63u;

        if (long_index >= bs->data_count) {
            break;
        }

        uint64_t lo = read_u64_be_ptr(bs->data_ptr + (size_t)long_index * 8);
        uint64_t value = lo >> start_bit;

        if (start_bit + (uint32_t)bits > 64u) {
            uint32_t hi_index = long_index + 1u;
            if (hi_index < bs->data_count) {
                uint64_t hi = read_u64_be_ptr(bs->data_ptr + (size_t)hi_index * 8);
                value |= hi << (64u - start_bit);
            }
        }

        int palette_index = (int)(value & mask);
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
    if (!db_varint(b, 0)) return 0; /* biome ID 0 (plains) */
    return db_varint(b, 0);         /* data array length = 0 */
}

/* =========================================================================
 * Serialize the NBT Heightmaps field (network NBT format)
 * ========================================================================= */
static int write_heightmaps_nbt(dynbuf_t *b,
                                 const hm_array_t *motion,
                                 const hm_array_t *surface) {
    /* Network NBT: type byte + 2-byte empty name length + compound contents */
    if (!db_u8(b, 10)) return 0;  /* TAG_Compound */
    if (!db_be16(b, 0)) return 0; /* empty root name */

    /* Helper to write one TAG_Long_Array entry */
    #define WRITE_HM(nm, arr)                                              \
        do {                                                                \
            if ((arr)->ptr && (arr)->count > 0) {                          \
                uint16_t nlen = (uint16_t)strlen(nm);                      \
                if (!db_u8(b, 12)) return 0;   /* TAG_Long_Array */        \
                if (!db_be16(b, nlen)) return 0;                           \
                if (!db_bytes(b, (const uint8_t *)(nm), nlen)) return 0;   \
                if (!db_be32(b, (arr)->count)) return 0;                   \
                if (!db_bytes(b, (arr)->ptr, (arr)->count * 8)) return 0;  \
            }                                                               \
        } while (0)

    WRITE_HM("MOTION_BLOCKING", motion);
    WRITE_HM("WORLD_SURFACE",   surface);

    #undef WRITE_HM

    return db_u8(b, 0); /* TAG_End */
}

/* =========================================================================
 * Light data
 *
 * 1.18+ has 24 sections (Y=-4…+19) plus 2 border sections = 26 total.
 * We send full sky light (0xFF) for all 26 and empty block light for all 26.
 * ========================================================================= */
#define LIGHT_SECTIONS 26
#define LIGHT_MASK     ((uint64_t)0x3FFFFFF) /* bits 0-25 set */

static int write_light_data(dynbuf_t *b) {
    static const uint8_t FULL_SKY[2048] = {0}; /* initialised below */
    static uint8_t sky_init = 0;
    static uint8_t FULL_SKY_FF[2048];

    if (!sky_init) {
        memset(FULL_SKY_FF, 0xFF, 2048);
        sky_init = 1;
    }

    /* Sky Light Mask: all 26 sections have sky light */
    if (!db_varint(b, 1)) return 0; /* 1 long */
    if (!db_be64(b, LIGHT_MASK)) return 0;

    /* Block Light Mask: 0 (no block light arrays) */
    if (!db_varint(b, 1)) return 0;
    if (!db_be64(b, 0)) return 0;

    /* Empty Sky Light Mask: 0 (because we provide sky light for all) */
    if (!db_varint(b, 1)) return 0;
    if (!db_be64(b, 0)) return 0;

    /* Empty Block Light Mask: all 26 sections have empty block light */
    if (!db_varint(b, 1)) return 0;
    if (!db_be64(b, LIGHT_MASK)) return 0;

    /* Sky Light Arrays: 26 × 2048 bytes of 0xFF */
    if (!db_varint(b, LIGHT_SECTIONS)) return 0;
    for (int i = 0; i < LIGHT_SECTIONS; i++) {
        if (!db_varint(b, 2048)) return 0;
        if (!db_bytes(b, FULL_SKY_FF, 2048)) return 0;
    }

    /* Block Light Arrays: 0 */
    if (!db_varint(b, 0)) return 0;

    (void)FULL_SKY; /* suppress unused warning */
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

    chunk_section_t sections[NUM_SECTIONS];
    memset(sections, 0, sizeof(sections));

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

    /* Heightmaps NBT */
    if (!write_heightmaps_nbt(&b, &hm_motion, &hm_surface)) goto fail;

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
    return b.data;

fail:
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

    /* Empty Heightmaps compound */
    {
        hm_array_t none = {NULL, 0};
        if (!write_heightmaps_nbt(&b, &none, &none)) goto fail;
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
            /* single value container: stone */
            if (!db_u8(&b, 0)) goto fail;
            if (!db_varint(&b, 1)) goto fail;
            if (!db_varint(&b, 0)) goto fail;
        } else {
            if (!db_be16(&b, 0)) goto fail;
            /* single value container: air */
            if (!db_u8(&b, 0)) goto fail;
            if (!db_varint(&b, 0)) goto fail;
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
