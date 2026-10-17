/* SPDX-License-Identifier: MIT */
/* saving and restoring: PLAYER.DAT packed and unpacked, level_save and level_load, savegame_restore_progress, and
 * changing level through perform_pending_teleport.
 *
 * One of src/uw_motion*.c: see uw_motion.h and uw_motion_int.h. */
#include "uw_motion_int.h"
#include "uw_scroll.h"

/* ==== saving the player ==== */

enum {
    SAVE_BUF_CAP      = 0x5b + 8 * 0x401,
    SAVE_SLOTS_PTR    = 0x5a82,   /* far: the frame's packed slot table */
    SAVE_OBJECTS_PTR  = 0x5a86,   /* far: the frame's records, index 0 at +0 */
    SAVE_OBJECT_COUNT = 0x5a8a    /* savegame_obj_count */
};

/* savegame_pack_chain(live_link, buf_link), depth first: each
 * object the live chain reaches takes the next buffer index
 * (savegame_alloc_slot), its 8 bytes copied there and the index
 * written into the buffer-side link's bits 6..15 over its own low six;
 * savegame_slot_to_buffer gives any of the first 19 inventory slots
 * holding that object the buffer index; then +4 is followed and, for a
 * non-quantity with contents, +6 recursed into. */
static void savegame_pack_chain(uw_motion *m, uint16_t live, uint16_t buf_link, uint8_t *buf, uint8_t *known,
                                uint16_t *count) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t o;
    int i;
    while ((o = deref_link(m, live)) != 0 && *count < 0x400) {
        uint16_t slot;
        (*count)++;
        slot = (uint16_t)(0x5b + *count * 8);
        memcpy(buf + slot, ls + o, 8);
        memset(known + slot, 1, 8);
        ww(buf, buf_link, (uint16_t)((rw(buf, buf_link) & 0x3f) | (*count << 6)));
        for (i = 0; i < 0x13; i++)
            if ((rw(ds, (uint16_t)(INVENTORY_SLOTS + i * 2)) >> 6) == (rw(ls, live) >> 6))
                ww(buf, (uint16_t)(0x23 + i * 2), (uint16_t)((rw(buf, (uint16_t)(0x23 + i * 2)) & 0x3f) | (rw(buf, buf_link) & 0xffc0)));
        live = (uint16_t)(o + 4);
        buf_link = (uint16_t)(slot + 4);
        if (!(rw(ls, o) & 0x8000) && (rw(ls, (uint16_t)(o + 6)) >> 6))
            savegame_pack_chain(m, (uint16_t)(o + 6), (uint16_t)(slot + 6), buf, known, count);
    }
}

/* savegame_pack into `buf`, the EMS frame: the Avatar's record
 * with its next link masked, the first 19 packed slots cleared -- 0x13..0x1b
 * are left as the frame held them -- and the inventory chain; the frame's
 * two pointers and the count (savegame_obj_count) in the data segment, and
 * savegame_write's count one past the objects. With something on the
 * cursor (action_state 1) its eight bytes at +0x1b, a non-quantity's
 * contents packed from its +6, and its chain cleared out of the pool
 * through a link word on the stack (object_chain_clear) -- savegame_unpack
 * makes it again; a level changed with a thing held carries it so. */
static int savegame_pack(uw_motion *m, uint8_t *buf, uint8_t *known, uint16_t *count) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t tracked = rw(ds, TRACKED_OBJECT);
    int i;
    *count = 0;
    container_panel_close(m);
    memcpy(buf, ls + tracked, 0x1b);
    memset(known, 1, 0x1b);
    ww(buf, 4, (uint16_t)(rw(buf, 4) & 0x3f));
    ww(ds, SAVE_SLOTS_PTR, 0x23);
    ww(ds, SAVE_OBJECTS_PTR, 0x5b);
    for (i = 0; i < 0x13; i++) {
        ww(buf, (uint16_t)(0x23 + i * 2), 0);
        known[0x23 + i * 2] = known[0x24 + i * 2] = 1;
    }
    savegame_pack_chain(m, (uint16_t)(tracked + 6), 6, buf, known, count);
    if (rw(ds, ACTION_STATE_WORD) == 1) {
        uint16_t held = rw(ds, CURSOR_OBJECT);
        memcpy(buf + 0x1b, ls + held, 8);
        memset(known + 0x1b, 1, 8);
        if (!(rw(ls, held) & 0x8000))
            savegame_pack_chain(m, (uint16_t)(held + 6), 0x1b + 6, buf, known, count);
        object_chain_clear_local(m, held);
    }
    (*count)++;
    ww(ds, SAVE_OBJECT_COUNT, *count);
    return 1;
}

size_t uw_motion_save_player_dat(uw_motion *m, uint8_t *out, size_t cap, uint8_t *known_out) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    static uint8_t buf[SAVE_BUF_CAP], known[SAVE_BUF_CAP];
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), tracked = rw(ds, TRACKED_OBJECT), count = 0, len, k;
    uint8_t seed, key[0x50];
    size_t n = 0;
    int i;
    memset(buf, 0, sizeof buf);
    memset(known, 0, sizeof known);
    if (!savegame_pack(m, buf, known, &count)) return 0;
    len = (uint16_t)(0x5b + count * 8);
    /* player_save_record: the seed from the record's first byte,
     * then the live values into the record */
    seed = (uint8_t)(ds[rec] ^ 0xaa);
    ds[(uint16_t)(rec + 0x1e)] = ds[(uint16_t)(rw(ds, CRITTER_ROW_PTR) + 5)];
    ds[(uint16_t)(rec + 0x1f)] = ds[(uint16_t)(rw(ds, CRITTER_ROW_PTR) + 6)];
    ds[(uint16_t)(rec + 0x20)] = ds[(uint16_t)(rw(ds, CRITTER_ROW_PTR) + 7)];
    ds[(uint16_t)(rec + 0x35)] = ls[(uint16_t)(tracked + 8)];
    ds[(uint16_t)(rec + 0x36)] = ds[(uint16_t)(rw(ds, CRITTER_ROW_PTR) + 4)];
    ww(ds, (uint16_t)(rec + 0x54), rw(ds, PLAYER_X));
    ww(ds, (uint16_t)(rec + 0x56), rw(ds, PLAYER_Y));
    ww(ds, (uint16_t)(rec + 0x58), rw(ds, PLAYER_Z));
    ww(ds, (uint16_t)(rec + 0x5a), rw(ds, PLAYER_HEADING));
    ww(ds, (uint16_t)(rec + 0x5c), rw(ds, CURRENT_LEVEL_WORD));
    ds[(uint16_t)(rec + 0xb5)] = (uint8_t)((ds[(uint16_t)(rec + 0xb5)] & 0xfc) | ((ds[SFX_AVAILABLE] ? ds[SFX_ENABLED] : 0) & 3));
    ds[(uint16_t)(rec + 0xb5)] = (uint8_t)((ds[(uint16_t)(rec + 0xb5)] & 0xf3) | (((ds[MUSIC_AVAILABLE] ? ds[MUSIC_ENABLED] : 0) & 3) << 2));
    ww(ds, (uint16_t)(rec + 0xb6), (uint16_t)((rw(ds, (uint16_t)(rec + 0xb6)) & 0xf807) | (ds[0x27a5] << 3)));
    /* savedata_xor_encode: the key seed + 3, + 6, ... over each 80-byte run */
    for (i = 0; i < 0x50; i++) key[i] = (uint8_t)(seed + 3 * (i + 1));
    if (cap < 1u + 0xd2u + 2u + len) return 0;
    out[n] = seed;
    known_out[n++] = 1;
    for (k = 0; k < 0xd2; k++) {
        out[n] = (uint8_t)(ds[(uint16_t)(rec + k)] ^ key[k % 0x50]);
        known_out[n++] = 1;
    }
    out[n] = (uint8_t)count;
    out[n + 1] = (uint8_t)(count >> 8);
    known_out[n] = known_out[n + 1] = 1;
    n += 2;
    memcpy(out + n, buf, len);
    memcpy(known_out + n, known, len);
    return n + len;
}

/* ==== saving the level: level_save ==== */

/* inventory_chain_free: a non-quantity's contents through +6,
 * then the rest of the chain through +4, then the object out of `link` and
 * back to its pool -- every object, traps included, since the chain is the
 * player's own. */
void inventory_chain_free(uw_motion *m, uint16_t link) {
    uint8_t *ls = m->lseg;
    uw_objpool pool;
    uint16_t o = deref_link(m, link);
    if (!o) return;
    if (!(rw(ls, o) & 0x8000) && (rw(ls, (uint16_t)(o + 6)) >> 6))
        inventory_chain_free(m, (uint16_t)(o + 6));
    if (rw(ls, (uint16_t)(o + 4)) >> 6)
        inventory_chain_free(m, (uint16_t)(o + 4));
    pool_from_ds(m, &pool);
    uw_object_list_remove(&pool, link, o);
    uw_obj_free(&pool, o);
    pool_to_ds(m, &pool);
}

enum {
    ACTIVE_MOBILE_LIST = 0x273c, ACTIVE_MOBILE_END = 0x2732,
    MOBILE_FREE_FLOOR  = 0x2752, MOBILE_FREE_SP = 0x2756,
    STATIC_FREE_FLOOR  = 0x2746, STATIC_FREE_SP = 0x274a,
    LEVEL_DIRTY        = 0x19b8,
    LEVEL_EFFECT_COUNT = 0x3656, LEVEL_EFFECT_LIST = 0x369c,
    TEXTURES_WALL      = 0x71aa, TEXTURES_FLOOR = 0x717c, DOOR_TEXTURES = 0x3148,
    AUTOMAP_TILES      = 0x3820
};

static uint8_t save_frame[SAVE_BUF_CAP];
static int savegame_read(uw_motion *m, const uint8_t *file, size_t size);

void uw_motion_savegame_reload(uw_motion *m) { savegame_read(m, NULL, 0); }

int uw_motion_level_save(uw_motion *m, uw_ark *ark) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t tracked = rw(ds, TRACKED_OBJECT), tilemap = rw(ds, TILEMAP_PTR), count;
    int level = rw(ds, CURRENT_LEVEL_WORD), ok;
    uint8_t textures[0x7a];
    static uint8_t known[SAVE_BUF_CAP];
    uw_objpool pool;
    /* savegame_write(0) first: the inventory parked in the frame. Then it is
     * freed and the Avatar unlinked from its tile, so no chain in the block
     * names them. */
    if (!savegame_pack(m, save_frame, known, &count)) return 0;
    inventory_chain_free(m, (uint16_t)(tracked + 6));
    if ((int16_t)rw(ds, TRACKED_TILE) >= 0) {
        pool_from_ds(m, &pool);
        uw_object_list_remove(&pool, (uint16_t)(tilemap + rw(ds, TRACKED_TILE) * 4 + 2), tracked);
        pool_to_ds(m, &pool);
    }
    ww(ds, TRACKED_TILE, 0xffff);
    ww(ls, tracked, (uint16_t)(rw(ls, tracked) & 0xfe3f));
    /* level_block_save: the trailer -- the roster's length, both
     * stacks' depths as `rt_ldiv(sp - floor, 2)` on a signed long, the magic
     * -- and the block to slot level - 1 */
    ww(ls, (uint16_t)(tilemap + 0x7c00), (uint16_t)(rw(ds, ACTIVE_MOBILE_END) - rw(ds, ACTIVE_MOBILE_LIST)));
    ww(ls, (uint16_t)(tilemap + 0x7c02),
       (uint16_t)(((int32_t)rw(ds, MOBILE_FREE_SP) - rw(ds, MOBILE_FREE_FLOOR)) / 2));
    ww(ls, (uint16_t)(tilemap + 0x7c04),
       (uint16_t)(((int32_t)rw(ds, STATIC_FREE_SP) - rw(ds, STATIC_FREE_FLOOR)) / 2));
    ww(ls, (uint16_t)(tilemap + 0x7c06), 0x7577);
    ds[LEVEL_DIRTY] = 0;
    ok = uw_ark_write_block(ark, level - 1, ls + tilemap, 0x7c08);
    /* level_effects_save: the first free record's word 0 masked
     * to its low six bits, which is what stops the loader; slot level + 8 */
    if (ok) {
        if ((int8_t)ds[LEVEL_EFFECT_COUNT] < 0x40) {
            uint16_t at = (uint16_t)(LEVEL_EFFECT_LIST + ds[LEVEL_EFFECT_COUNT] * 6);
            ww(ds, at, (uint16_t)(rw(ds, at) & 0x3f));
        }
        ok = uw_ark_write_block(ark, level + 8, ds + LEVEL_EFFECT_LIST, 0x180);
    }
    /* lev_save_texture_map: 48 wall words, 10 floor words, 3 door
     * words; slot level + 17 */
    if (ok) {
        memcpy(textures, ds + TEXTURES_WALL, 0x60);
        memcpy(textures + 0x60, ds + TEXTURES_FLOOR, 0x14);
        memcpy(textures + 0x74, ds + DOOR_TEXTURES, 6);
        ok = uw_ark_write_block(ark, level + 17, textures, sizeof textures);
    }
    /* automap_block_save: automap_tiles whole; slot level + 26 */
    if (ok) ok = uw_ark_write_block(ark, level + 26, ds + AUTOMAP_TILES, 0x1000);
    /* savegame_read(0): the inventory unpacked from the frame again, into
     * the pools it was freed to */
    savegame_read(m, NULL, 0);
    return ok;
}

/* ==== restoring a game: savegame_restore_progress ==== */

enum {
    DRAGON_ELEMS        = 0x078f,   /* dragon_elem, two words */
    SFX_ENABLED_BYTE    = 0x0136, SFX_AVAILABLE_BYTE = 0x0137,
    MUSIC_ON            = 0x0135, MUSIC_AVAILABLE_BYTE = 0x0138, MUSIC_SEQUENCE = 0x2646,
    RENDER_DETAIL_FLAG  = 0x054b, DRAWLIST_LIGHT_SCALE = 0x054d,
    SHAPE_EMITTER       = 0x0555, QUAD_EMITTER = 0x055d,
    TEXTURE_BASE        = 0x7178, WALL_TEXTURE_SEG = 0x720a, FLOOR_TEXTURE_SEG = 0x7190,
    FLOOR_TEXTURE_COUNT = 0x71a6, WALL16_TEXTURE_SEG = 0x71a8, FLOOR16_TEXTURE_SEG = 0x717a,
    WALL_TERRAIN        = 0x720c, FLOOR_TERRAIN = 0x7192
};

/* save_frame, above uw_motion_level_save: the EMS frame savegame_write(0)
 * parks the inventory in across a block load or save and savegame_read
 * unpacks -- one, as the original claims one. */

/* inventory_reset: the slots' object indices, the paperdoll's
 * loaded art, both container-stack pointers, the cursor's object, the two
 * paging flags and the weight left's cache. */
void inventory_reset(uw_motion *m) {
    uint8_t *ds = m->ds;
    int i;
    for (i = 0; i < 0x1c; i++)
        ww(ds, (uint16_t)(INVENTORY_SLOTS + i * 2), (uint16_t)(rw(ds, (uint16_t)(INVENTORY_SLOTS + i * 2)) & 0x3f));
    for (i = 1; i < 6; i++) ds[(uint16_t)(PAPERDOLL_ART_ITEM + i)] = 0;
    ww(ds, 0x1722, 0);
    ww(ds, 0x1724, 0);
    ww(ds, CONTAINER_STACK_TOP, 0);
    ww(ds, (uint16_t)(CONTAINER_STACK_TOP + 2), 0);
    ds[0x18a2] = 0;
    ds[0x18a1] = 0;
    ww(ds, CURSOR_OBJECT, 0);
    ww(ds, (uint16_t)(CURSOR_OBJECT + 2), 0);
    ww(ds, 0x189f, 0xffff);
}

/* inventory_discard_all: container_panel_close, the inventory
 * freed, inventory_reset. */
void inventory_discard_all(uw_motion *m) {
    uint8_t *ds = m->ds;
    container_panel_close(m);
    inventory_chain_free(m, (uint16_t)(rw(ds, TRACKED_OBJECT) + 6));
    inventory_reset(m);
}

/* savegame_slot_to_live: any of the 19 packed slots naming the
 * buffer object at `buf_link` names the live object at `live_link` now. */
static void savegame_slot_to_live(uw_motion *m, const uint8_t *buf, uint16_t live_link, uint16_t buf_link) {
    uint8_t *ds = m->ds;
    int i;
    for (i = 0; i < 0x13; i++)
        if ((rw(buf, (uint16_t)(0x23 + i * 2)) >> 6) == (rw(buf, buf_link) >> 6))
            ww(ds, (uint16_t)(INVENTORY_SLOTS + i * 2), (uint16_t)((rw(ds, (uint16_t)(INVENTORY_SLOTS + i * 2)) & 0x3f)
                                                                  | (rw(m->lseg, live_link) & 0xffc0)));
}

/* savegame_unpack_chain: each buffer object along the chain a
 * static from the pool, its 8 bytes copied, the live link naming it, the
 * slots remapped; then +4, and a non-quantity's contents through +6. */
static void savegame_unpack_chain(uw_motion *m, const uint8_t *buf, uint16_t live_link, uint16_t buf_link) {
    uint8_t *ls = m->lseg;
    uw_objpool pool;
    uint16_t index, rec, o;
    while ((index = (uint16_t)(rw(buf, buf_link) >> 6)) != 0) {
        rec = (uint16_t)(0x5b + index * 8);            /* savegame_slot_addr */
        if (rec + 8 > SAVE_BUF_CAP) { UW_NOT_CARRIED(m->not_carried); return; }
        pool_from_ds(m, &pool);
        o = uw_obj_alloc(&pool, 0);
        pool_to_ds(m, &pool);
        if (!o) return;
        memcpy(ls + o, buf + rec, 8);
        ww(ls, live_link, (uint16_t)((rw(ls, live_link) & 0x3f) | (uw_obj_index(o) << 6)));
        savegame_slot_to_live(m, buf, live_link, buf_link);
        live_link = (uint16_t)(o + 4);
        buf_link = (uint16_t)(rec + 4);
        if (!(rw(buf, rec) & 0x8000) && (rw(buf, (uint16_t)(rec + 6)) >> 6))
            savegame_unpack_chain(m, buf, (uint16_t)(o + 6), (uint16_t)(rec + 6));
    }
}

/* savegame_unpack: the frame's pointers, the Avatar's record
 * over the tracked object, its inventory; with action_state 1 the object on
 * the cursor a static from the pool (obj_alloc(0)), +0x1b's eight bytes
 * copied over it and a non-quantity's contents unpacked from its +6. */
static void savegame_unpack(uw_motion *m, const uint8_t *buf) {
    uint8_t *ds = m->ds;
    uint16_t tracked = rw(ds, TRACKED_OBJECT);
    ww(ds, SAVE_SLOTS_PTR, 0x23);
    ww(ds, SAVE_OBJECTS_PTR, 0x5b);
    memcpy(m->lseg + tracked, buf, 0x1b);
    savegame_unpack_chain(m, buf, (uint16_t)(tracked + 6), 6);
    if (rw(ds, ACTION_STATE_WORD) == 1) {
        uw_objpool pool;
        uint16_t held;
        pool_from_ds(m, &pool);
        held = uw_obj_alloc(&pool, 0);
        pool_to_ds(m, &pool);
        ww(ds, CURSOR_OBJECT, held);
        ww(ds, (uint16_t)(CURSOR_OBJECT + 2), held ? rw(ds, (uint16_t)(MOBILE_BASE + 2)) : 0);
        if (!held) {
            UW_NOT_CARRIED(m->not_carried);
            return;
        }
        memcpy(m->lseg + held, buf + 0x1b, 8);
        if (!(rw(buf, 0x1b) & 0x8000))
            savegame_unpack_chain(m, buf, (uint16_t)(held + 6), 0x1b + 6);
    }
}

/* set_render_detail: the record's detail nibble (+0xb5 bits
 * 4..7) into the renderer's flag and its two emitters; the emitters'
 * segments are already the ones the table holds. */
void uw_motion_set_render_detail(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint8_t detail = (uint8_t)(ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0xb5)] >> 4);
    ds[RENDER_DETAIL_FLAG] = detail != 0;
    ww(ds, QUAD_EMITTER, detail > 2 ? 0x0a4a : 0x079a);
    ww(ds, SHAPE_EMITTER, detail > 1 ? 0x0a4a : 0x079a);
    ds[DRAWLIST_LIGHT_SCALE] = 1;
}

/* player_load_record: the record decoded after its seed, and
 * the live values out of it -- the attributes and maximum hit points into
 * the critter row, hit points into the Avatar, the position, heading,
 * level and collision face -- then the sound and music settings, the
 * render detail and the movement mode. */
static void player_load_record(uw_motion *m, const uint8_t *file) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), row = rw(ds, CRITTER_ROW_PTR), k;
    uint8_t seed = file[0], v;
    /* savedata_xor_decode: key[i] = seed + 3 * (i + 1), a run of 80 */
    for (k = 0; k < 0xd2; k++) ds[(uint16_t)(rec + k)] = (uint8_t)(file[1 + k] ^ (uint8_t)(seed + 3 * (k % 0x50 + 1)));
    ds[(uint16_t)(row + 5)] = ds[(uint16_t)(rec + 0x1e)];
    ds[(uint16_t)(row + 6)] = ds[(uint16_t)(rec + 0x1f)];
    ds[(uint16_t)(row + 7)] = ds[(uint16_t)(rec + 0x20)];
    m->lseg[(uint16_t)(rw(ds, TRACKED_OBJECT) + 8)] = ds[(uint16_t)(rec + 0x35)];
    ds[(uint16_t)(row + 4)] = ds[(uint16_t)(rec + 0x36)];
    ww(ds, PLAYER_X, rw(ds, (uint16_t)(rec + 0x54)));
    ww(ds, PLAYER_Y, rw(ds, (uint16_t)(rec + 0x56)));
    ww(ds, PLAYER_Z, rw(ds, (uint16_t)(rec + 0x58)));
    ww(ds, PLAYER_HEADING, rw(ds, (uint16_t)(rec + 0x5a)));
    ww(ds, CURRENT_LEVEL_WORD, rw(ds, (uint16_t)(rec + 0x5c)));
    ds[COLLISION_FACE] = (uint8_t)(rw(ds, (uint16_t)(rec + 0xb6)) >> 3);
    /* the settings the record carries, bits 0..1 and 2..3 of +0xb5 */
    sfx_set_enabled(m, ds[(uint16_t)(rec + 0xb5)] & 3);
    v = (uint8_t)((ds[(uint16_t)(rec + 0xb5)] & 0xc) >> 2);
    music_set_enabled(m, v);
    uw_motion_set_render_detail(m);
    apply_movement_mode(m, (int8_t)(ds[(uint16_t)(rec + 0xb6)] & 7));
}

/* savegame_read: with a file, the Avatar out of its tile, the
 * inventory discarded, the record and the frame read, the body art; without
 * one, the frame savegame_write(0) left. Then the inventory unpacked,
 * player_state_recalc, and with a file the Avatar back in its tile. */
static int savegame_read(uw_motion *m, const uint8_t *file, size_t size) {
    uint8_t *ds = m->ds;
    uw_objpool pool;
    uint16_t count;
    if (file && size < 0xd5) return 0;
    if (file && rs(ds, TRACKED_TILE) >= 0) {
        memset(&pool, 0, sizeof pool);
        pool.seg = m->lseg;
        uw_object_list_remove(&pool, (uint16_t)(rw(ds, TILEMAP_PTR) + rw(ds, TRACKED_TILE) * 4 + 2),
                              rw(ds, TRACKED_OBJECT));
    }
    inventory_discard_all(m);
    if (file) {
        player_load_record(m, file);
        count = rw(file, 0xd3);
        ww(ds, SAVE_OBJECT_COUNT, count);
        if ((size_t)0x5b + (size_t)count * 8 > SAVE_BUF_CAP) return 0;
        memcpy(save_frame, file + 0xd5, size - 0xd5 < (size_t)0x5b + count * 8 ? size - 0xd5 : (size_t)0x5b + count * 8);
        paperdoll_load_body_art(m);
    }
    savegame_unpack(m, save_frame);
    player_state_recalc(m);
    if (file && rs(ds, TRACKED_TILE) >= 0) {
        memset(&pool, 0, sizeof pool);
        pool.seg = m->lseg;
        uw_object_list_insert(&pool, (uint16_t)(rw(ds, TILEMAP_PTR) + rw(ds, TRACKED_TILE) * 4 + 2),
                              rw(ds, TRACKED_OBJECT));
    }
    return 1;
}

/* level_block_load: the block over the level segment, the pools'
 * pointers from its trailer, then level_effects_load -- slot
 * level + 8, exactly 0x180 bytes or no effects, the count walked. 1, or 0
 * when the effects block is wrong. */
static int level_block_load(uw_motion *m, const uw_ark *ark, int level) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t tm = rw(ds, TILEMAP_PTR);
    const uint8_t *blk;
    size_t n;
    int i;
    ww(ls, (uint16_t)(tm + 0x7c06), 0);
    n = uw_ark_block(ark, level - 1, &blk);
    if (n > 0x10000u - tm) n = 0x10000u - tm;
    if (n) memcpy(ls + tm, blk, n);
    if (rw(ls, (uint16_t)(tm + 0x7c06)) == 0x7577) {
        ww(ds, MOBILE_FREE_SP, (uint16_t)(rw(ds, MOBILE_FREE_FLOOR) + rw(ls, (uint16_t)(tm + 0x7c02)) * 2));
        ww(ds, STATIC_FREE_SP, (uint16_t)(rw(ds, STATIC_FREE_FLOOR) + rw(ls, (uint16_t)(tm + 0x7c04)) * 2));
        ww(ds, ACTIVE_MOBILE_END, (uint16_t)(rw(ds, ACTIVE_MOBILE_LIST) + rw(ls, (uint16_t)(tm + 0x7c00))));
        ds[LEVEL_DIRTY] = 0;
    } else {
        UW_NOT_CARRIED(m->not_carried);
    }
    n = uw_ark_block(ark, level + 8, &blk);
    if (n > 0x180) n = 0x180;
    if (n) memcpy(ds + LEVEL_EFFECT_LIST, blk, n);
    if (n != 0x180) {
        ds[LEVEL_EFFECT_COUNT] = 0;
        return 0;
    }
    for (i = 0; i < 0x40 && (rw(ds, (uint16_t)(LEVEL_EFFECT_LIST + i * 6)) >> 6); i++) ;
    ds[LEVEL_EFFECT_COUNT] = (uint8_t)i;
    return 1;
}

/* lev_load_texture_map: slot level + 17's 48 wall and 10 floor
 * texture words, their terrain words out of TERRAIN.DAT (lev_load_terrain)
 * or zero, the 3 door words; then load_textures_and_doors, of which the
 * texture counts and their pages' segments -- sums over a base segment --
 * are carried and the textures' pixels
 * and the level's GR files are not. */
static void lev_load_texture_map(uw_motion *m, const uw_ark *ark, int level, const uint8_t *terrain, size_t terrain_size) {
    uint8_t *ds = m->ds;
    const uint8_t *blk;
    uint8_t local[0x7a];
    size_t n = uw_ark_block(ark, level + 17, &blk);
    int16_t walls, i;
    memset(local, 0, sizeof local);
    if (n != 0x7a) UW_NOT_CARRIED(m->not_carried);
    if (n) memcpy(local, blk, n < sizeof local ? n : sizeof local);
    for (i = 0; i < 0x30; i++) {
        ww(ds, (uint16_t)(TEXTURES_WALL + i * 2), rw(local, (uint16_t)(i * 2)));
        ww(ds, (uint16_t)(WALL_TERRAIN + i * 2), 0);
    }
    for (i = 0; i < 10; i++) {
        ww(ds, (uint16_t)(FLOOR_TERRAIN + i * 2), 0);
        ww(ds, (uint16_t)(TEXTURES_FLOOR + i * 2), rw(local, (uint16_t)(0x60 + i * 2)));
    }
    if (terrain) {
        for (i = 0; i < 0x30; i++) {
            long at = (long)(int16_t)rw(ds, (uint16_t)(TEXTURES_WALL + i * 2)) * 2;
            if (at >= 0 && (size_t)at + 2 <= terrain_size) ww(ds, (uint16_t)(WALL_TERRAIN + i * 2), rw(terrain, (uint16_t)at));
        }
        for (i = 0; i < 10; i++) {
            long at = (long)(int16_t)rw(ds, (uint16_t)(TEXTURES_FLOOR + i * 2)) * 2 + 0x200;
            if (at >= 0 && (size_t)at + 2 <= terrain_size) ww(ds, (uint16_t)(FLOOR_TERRAIN + i * 2), rw(terrain, (uint16_t)at));
        }
    } else {
        UW_NOT_CARRIED(m->not_carried);
    }
    memcpy(ds + DOOR_TEXTURES, local + 0x74, 6);
    /* load_textures_and_doors: load_texture_file stops a list at 12 or its
     * first negative index, and says how many it took */
    ww(ds, WALL_TEXTURE_SEG, rw(ds, TEXTURE_BASE));
    for (walls = 0; walls < 12 && (int16_t)rw(ds, (uint16_t)(TEXTURES_WALL + walls * 2)) >= 0; walls++) ;
    ww(ds, FLOOR_TEXTURE_SEG, (uint16_t)(rw(ds, WALL_TEXTURE_SEG) + walls * 0x100));
    if ((int16_t)rw(ds, FLOOR_TEXTURE_COUNT) + walls > 0x3c)
        ww(ds, FLOOR_TEXTURE_COUNT, (uint16_t)(0x3c - rw(ds, 0x726c)));
    for (i = 0; i < (int16_t)rw(ds, FLOOR_TEXTURE_COUNT) && (int16_t)rw(ds, (uint16_t)(TEXTURES_FLOOR + i * 2)) >= 0; i++) ;
    ww(ds, FLOOR_TEXTURE_COUNT, (uint16_t)i);
    ww(ds, WALL16_TEXTURE_SEG, (uint16_t)(rw(ds, FLOOR_TEXTURE_SEG) + (uint16_t)((int16_t)(uint16_t)(i << 10) >> 4)));
    for (walls = 0; walls < 12 && (int16_t)rw(ds, (uint16_t)(TEXTURES_WALL + walls * 2)) >= 0; walls++) ;
    ww(ds, FLOOR16_TEXTURE_SEG, (uint16_t)(rw(ds, WALL16_TEXTURE_SEG) + (uint16_t)((int16_t)(uint16_t)(walls << 8) >> 4)));
    for (i = 0; i < (int16_t)rw(ds, FLOOR_TEXTURE_COUNT) && (int16_t)rw(ds, (uint16_t)(TEXTURES_FLOOR + i * 2)) >= 0; i++) ;
    ww(ds, FLOOR_TEXTURE_COUNT, (uint16_t)i);
    /* the four .tr files' pixels, gr_load_level_textures and
     * gr_load_door_textures, are the host scene's: it loads the level's
     * textures by this same map when it first draws the level
     * (uw_scene_level) */
}

/* creature_clear_paths: mobiles 2..0xff's +0x15 bit 7,
 * and npc_path_slot_mask to -1. */
static void creature_clear_paths(uw_motion *m) {
    uint16_t i;
    for (i = 2; i < 0x100; i++) m->lseg[(uint16_t)(obj_at(m, i) + 0x15)] &= 0x7f;
    ww(m->ds, NPC_PATH_SLOT_MASK, 0xffff);
}

int uw_motion_level_load(uw_motion *m, const uw_ark *ark, int level, const uint8_t *terrain, size_t terrain_size) {
    uint8_t *ds = m->ds;
    static uint8_t known[SAVE_BUF_CAP];
    const uint8_t *blk;
    uint16_t count;
    size_t n;
    int r;
    /* savegame_write(0): the inventory parked in the frame */
    if (!savegame_pack(m, save_frame, known, &count)) return 0;
    if (rs(ds, TRACKED_TILE) >= 0) ww(ds, TRACKED_TILE, 0xffff);
    r = level_block_load(m, ark, level);
    savegame_read(m, NULL, 0);
    if (r > 0) {
        lev_load_texture_map(m, ark, level, terrain, terrain_size);
        memset(ds + AUTOMAP_TILES, 0, 0x1000);                          /* automap_clear */
        creature_clear_paths(m);
        /* combat_state_reset */
        ds[ASSAULT_VICTIM_INDEX] = 0;
        ds[ASSAULT_VICTIM_RACE] = 0xff;
        creature_clear_paths(m);
        if (r == 1) {
            /* automap_block_load: slot level + 26, empty for a
             * level never visited */
            n = uw_ark_block(ark, level + 26, &blk);
            if (n) memcpy(ds + AUTOMAP_TILES, blk, n < 0x1000 ? n : 0x1000);
        }
    }
    return r;
}

/* panel_values_reset: the nine panel values and the flasks' shown
 * levels zeroed, both dragons hidden and their phase cleared, the panel
 * mode 0, element 7's step 0, the redraw mask's bit 7 off, and the
 * weapon's states put back (4, 6, 6, 6, frame 0). */
static void panel_values_reset(uw_motion *m) {
    uint8_t *ds = m->ds;
    int i;
    for (i = 0; i < 9; i++) {
        ds[(uint16_t)(PANEL_VALUES + i)] = 0;
        ds[(uint16_t)(PANEL_FLASK_SHOWN + i)] = 0;
    }
    elem_hide(m, rw(ds, DRAGON_ELEMS));
    elem_hide(m, rw(ds, (uint16_t)(DRAGON_ELEMS + 2)));
    ww(ds, 0x078d, 0);
    ww(ds, 0x078b, 0);
    ds[0x784] = 0;
    ww(ds, 0x08b5, 0);
    ww(ds, 0x0785, (uint16_t)(rw(ds, 0x0785) & 0xff7f));
    ds[0x362f] = 4;
    ds[0x3630] = 6;
    ds[0x35ea] = 6;
    ds[0x795] = 6;
    ww(ds, 0x0796, 0);
    elem_flush(m);
}

int uw_motion_restore(uw_motion *m, const uint8_t *player_dat, size_t size, const uw_ark *ark,
                      const uint8_t *terrain, size_t terrain_size) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t tracked = rw(ds, TRACKED_OBJECT), rec;
    int level;
    /* game_reset_player_state */
    inventory_discard_all(m);
    player_state_recalc(m);
    ww(ls, (uint16_t)(tracked + 2), (uint16_t)(rw(ls, (uint16_t)(tracked + 2)) & 0xfc7f));
    ls[(uint16_t)(tracked + 0x18)] &= 0xe0;
    ww(ds, PLAYER_HEADING, 0);
    ww(ds, MOVE_HEADING, 0);
    ds[0x546] = 1;
    panel_values_reset(m);
    ww(ds, 0x5702, 0);
    weapon_stow(m);
    ww(ds, 0x1a30, 2);
    if (rw(ds, 0x268c) == 1 || rw(ds, 0x268c) == 3 || rw(ds, 0x268c) == 4) cursor_shape_pop(m, 3);
    ww(ds, 0x268c, 0);
    ww(ds, 0x09f4, 0);
    ww(ds, 0x09f6, 0);
    ww(ds, 0x16e3, 0);
    ww(ds, 0x16e5, 0);
    if (rw(ds, ACTION_STATE_WORD)) {
        if (rs(ds, ACTION_STATE_WORD) < 4) {
            cursor_shape_pop(m, 3);
            ww(ds, CURSOR_OBJECT, 0);
            ww(ds, (uint16_t)(CURSOR_OBJECT + 2), 0);
        } else {
            debug_camera_set_target(m, 1);
        }
        ww(ds, ACTION_STATE_WORD, 0);
    }
    if (!savegame_read(m, player_dat, size)) return 0;
    if (m->scroll) print_message(m, 0xaa);  /* savegame_restore_progress's "..." */
    level = rw(ds, CURRENT_LEVEL_WORD);
    if (!uw_motion_level_load(m, ark, level, terrain, terrain_size)) return 0;
    /* level_transition_effects(level, 3): level 9's byte cleared */
    if (level == 9) ds[0x546] = 0;
    if (m->scroll) print_message(m, 0xaa);
    /* combat_state_restore */
    rec = rw(ds, PLAYER_RECORD_PTR);
    ds[ASSAULT_VICTIM_INDEX] = ds[(uint16_t)(rec + 0xba)];
    ds[ASSAULT_VICTIM_RACE] = ds[(uint16_t)(rec + 0xbb)];
    ww(ds, 0x2486, rw(ds, (uint16_t)(rec + 0xbc)));
    ww(ds, 0x2488, rw(ds, (uint16_t)(rec + 0xbe)));
    ds[0x248a] = ds[(uint16_t)(rec + 0xc0)];
    ds[0x248b] = ds[(uint16_t)(rec + 0xc1)];
    return 1;
}

/* weapons_load_colourmap: WEAPONS.CM's sixteen entries into
 * weapon_colourmap (0x591e), the second map for appearance 1 (the
 * record's +0x64 bits 2..4). 1 when read. */
static int weapons_load_colourmap(uw_motion *m) {
    uint8_t *ds = m->ds;
    size_t at = ((ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x64)] >> 2) & 7) == 1 ? 0x10 : 0;
    if (!m->weapons_cm || at + 0x10 > m->weapons_cm_size) {
        UW_NOT_CARRIED(m->not_carried);
        return 0;
    }
    memcpy(ds + 0x591e, m->weapons_cm + at, 0x10);
    return 1;
}

void uw_motion_menu_restore_tail(uw_motion *m) {
    weapons_load_colourmap(m);
    weapon_stow(m);
}

void uw_motion_restore_tail(uw_motion *m, uint32_t clock) {
    uint8_t *ds = m->ds;
    weapons_load_colourmap(m);
    uw_motion_tick_update(m, clock, 0);
    ds[0x12b3] = 1;
    /* panel_build_elements, its elements long built: of its
     * drawing, panel_flask_fill's store of each flask's value as shown */
    ds[PANEL_FLASK_SHOWN] = ds[PANEL_VALUES];
    ds[(uint16_t)(PANEL_FLASK_SHOWN + 1)] = ds[(uint16_t)(PANEL_VALUES + 1)];
    /* the rest of it -- the page copies, the flasks', compass's, runes' and
     * panel's drawing -- is the host's page redraw after a restore
     * (uw_boot_draw_main_screen, which runs panel_build_elements whole) */
    apply_movement_mode(m, -1);
    ds[PLAYER_IN_LIQUID] = 1;
    ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 0x7ffe));   /* post_event(0x7ffe) */
}

/* ==== changing level: perform_pending_teleport ==== */

enum {
    CRITTER_PROPS      = 0x4a52,   /* critter_properties: 0x30 bytes a creature type */
    FLOOR_HEIGHTS      = 0x1992,   /* a tile height's floor z, a word each */
    SETTLE_TILE_X      = 0x247a, SETTLE_TILE_Y = 0x247e,
    FADE_FLAGS         = 0x13b2,   /* teleport_fade_flags: 1 fade out, 2 fade in */
    ARRIVAL_CALLBACK   = 0x13ba,   /* far */
    LEVEL_ENTERED      = 0x12b3
};

/* tile_standing_spot(type): 0 for solid; else where a creature
 * stands in the tile, in eighths -- the centre, and a corner of three of the
 * four diagonals, type 5 repeating type 2's. */
int tile_standing_spot(uint8_t type, uint8_t *x, uint8_t *y) {
    switch (type) {
    case 0: return 0;
    case 2: case 5: *x = 6; *y = 1; break;
    case 3: *x = 1; *y = 1; break;
    case 4: *x = 6; *y = 6; break;
    default: *x = 4; *y = 4; break;
    }
    return 1;
}

/* npc_send_home(npc, race table): a creature marked for
 * deletion (+0x0d bit 8) removed; else +0x19's hunger, attitude override
 * and four more bits cleared, a heading at random, the wound halved (unless
 * +0x0d bit 9), its attitude 0 or 3 counted against its race (unless pinned,
 * +0x0a bit 7), and, away from home, moved to the standing spot of its home
 * tile if item_fits_in_tile allows -- a flier at half the floor plus 0x40. */
static void npc_send_home(uw_motion *m, uint16_t npc, int8_t *race_delta, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t row = (uint16_t)(CRITTER_PROPS + (rw(ls, npc) & 0x3f) * 0x30), tp, hp, w;
    uint8_t tx = (uint8_t)(rw(ls, (uint16_t)(npc + 0x16)) >> 10);
    uint8_t ty = (uint8_t)((rw(ls, (uint16_t)(npc + 0x16)) & 0x3f0) >> 4);
    uint8_t hx, hy, sx = 0, sy = 0, z, att;
    uw_objpool pool;
    tp = tile_ptr(m, tx, ty);
    if (rw(ls, (uint16_t)(npc + 0xd)) & 0x100) {
        object_chain_remove(m, (uint16_t)(tp + 2), npc);
        return;
    }
    ls[(uint16_t)(npc + 0x19)] &= 0x0c;
    ww(ls, (uint16_t)(npc + 2), (uint16_t)((rw(ls, (uint16_t)(npc + 2)) & 0xfc7f) | ((((int16_t)rt_rand(m) % 8) & 7) << 7)));
    if (ls[(uint16_t)(npc + 8)] < ds[(uint16_t)(row + 4)] && !(rw(ls, (uint16_t)(npc + 0xd)) & 0x200))
        ls[(uint16_t)(npc + 8)] = (uint8_t)((ls[(uint16_t)(npc + 8)] + ds[(uint16_t)(row + 4)]) / 2);
    if (!(ls[(uint16_t)(npc + 0xa)] & 0x80)) {
        att = (uint8_t)(rw(ls, (uint16_t)(npc + 0xd)) >> 14);
        if (att == 0) race_delta[ds[(uint16_t)(row + 9)]]--;
        else if (att == 3) race_delta[ds[(uint16_t)(row + 9)]]++;
    }
    hx = (uint8_t)(ls[(uint16_t)(npc + 4)] & 0x3f);
    hy = (uint8_t)(ls[(uint16_t)(npc + 6)] & 0x3f);
    hp = tile_ptr(m, hx, hy);
    if ((tx == hx && ty == hy) || !tile_standing_spot((uint8_t)(ls[hp] & 0xf), &sx, &sy)) return;
    if (ds[(uint16_t)(row + 0xa)] & 0x80)
        z = (uint8_t)(((uint16_t)(((ls[hp] >> 4) & 0xf) << 3) + 0x80) >> 1);
    else
        z = (uint8_t)(((ls[hp] >> 4) & 0xf) << 3);
    if (!item_fits_in_tile(m, (uint16_t)(rw(ls, npc) & 0x1ff), obj_index_of(m, npc), (int16_t)(hx * 8 + sx),
                           (int16_t)(hy * 8 + sy), z, (ds[(uint16_t)(row + 0xa)] >> 7) & 1, 8,
                           (uint16_t)(bp - 0x10 - 4 - 14 - 4 - 2)))
        return;
    memset(&pool, 0, sizeof pool);
    pool.seg = ls;
    uw_object_list_remove(&pool, (uint16_t)(tp + 2), npc);
    uw_object_list_insert(&pool, (uint16_t)(hp + 2), npc);
    w = rw(ls, (uint16_t)(npc + 0x16));
    w = (uint16_t)((w & 0x3ff) | ((hx & 0x3f) << 10));
    w = (uint16_t)((w & 0xfc0f) | ((hy & 0x3f) << 4));
    ww(ls, (uint16_t)(npc + 0x16), w);
    w = rw(ls, (uint16_t)(npc + 2));
    w = (uint16_t)((w & 0x1fff) | ((sx & 7) << 13));
    w = (uint16_t)((w & 0xe3ff) | ((sy & 7) << 10));
    w = (uint16_t)((w & 0xff80) | (z & 0x7f));
    ww(ls, (uint16_t)(npc + 2), w);
}

/* object_settle_to_floor(obj): a mobile thing that is not a
 * creature out of its tile (object_remove, unforced), what object_hits_floor
 * makes of it placed near where it was, scattered (object_place_near, 6),
 * or failing that relinked on the floor. Always 1. */
static int object_settle_to_floor(uw_motion *m, uint16_t obj, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w2 = rw(ls, (uint16_t)(obj + 2)), tx, ty, tp, o;
    int16_t fy;
    uw_objpool pool;
    tx = (uint16_t)(rw(ls, (uint16_t)(obj + 0x16)) >> 10);
    ww(ds, SETTLE_TILE_X, tx);
    ty = (uint16_t)((rw(ls, (uint16_t)(obj + 0x16)) & 0x3f0) >> 4);
    ww(ds, SETTLE_TILE_Y, ty);
    fy = (int16_t)(ty * 8 + ((w2 & 0x1c00) >> 10));
    tp = tile_ptr(m, tx, ty);
    o = object_remove(m, (uint16_t)(tp + 2), obj, 0);
    if (!o) return 1;
    o = object_hits_floor(m, o);
    if (!o) return 1;
    memset(&pool, 0, sizeof pool);
    pool.seg = ls;
    uw_object_list_remove(&pool, (uint16_t)(tp + 2), o);
    ds[PLACE_SCATTER] = 1;
    if (!object_place_near(m, o, (int16_t)(tx * 8 + (w2 >> 13)), fy, (int16_t)(((ls[tp] >> 4) & 0xf) << 3), 6,
                           (uint16_t)(bp - 8 - 4 - 12 - 4 - 2))) {
        ww(ls, (uint16_t)(o + 2), (uint16_t)((rw(ls, (uint16_t)(o + 2)) & 0xff80) | (((ls[tp] >> 4) & 0xf) << 3)));
        uw_object_list_insert(&pool, (uint16_t)(tp + 2), o);
    }
    return 1;
}

/* npc_settle_level: each thing on the active roster -- a
 * creature sent home, anything else settled to the floor and its entry
 * looked at again -- then every unpinned creature's attitude moved by its
 * race's total and held to 0..3. */
void npc_settle_level(uw_motion *m, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    int8_t race_delta[0x100];
    uint16_t at, o, row;
    int8_t d, a;
    memset(race_delta, 0, sizeof race_delta);   /* the original clears 0x40 and indexes by a byte */
    for (at = rw(ds, ACTIVE_MOBILE_LIST); at < rw(ds, ACTIVE_MOBILE_END); at++) {
        o = (uint16_t)(rw(ds, MOBILE_BASE) + ls[at] * 0x1b);
        if ((rw(ls, o) & 0x1c0) == 0x40)
            npc_send_home(m, o, race_delta, (uint16_t)(bp - 0x4e - 4 - 6 - 4 - 2));
        else if (object_settle_to_floor(m, o, (uint16_t)(bp - 0x4e - 4 - 4 - 4 - 2)))
            at--;
    }
    for (at = rw(ds, ACTIVE_MOBILE_LIST); at < rw(ds, ACTIVE_MOBILE_END); at++) {
        o = (uint16_t)(rw(ds, MOBILE_BASE) + ls[at] * 0x1b);
        if (ls[(uint16_t)(o + 0xa)] & 0x80) continue;
        row = (uint16_t)(CRITTER_PROPS + (rw(ls, o) & 0x3f) * 0x30);
        d = race_delta[ds[(uint16_t)(row + 9)]];
        if (!d) continue;
        a = (int8_t)((uint8_t)(rw(ls, (uint16_t)(o + 0xd)) >> 14) + d);
        if (d < 0) { if (a < 0) a = 0; }
        else if (a > 3) a = 3;
        ww(ls, (uint16_t)(o + 0xd), (uint16_t)((rw(ls, (uint16_t)(o + 0xd)) & 0x3fff) | ((uint16_t)(int16_t)a << 14)));
    }
}

/* level_transition_effects(level, phase): with record +0x60 bit
 * 4 set and arriving, game_world_reset(0) instead; arriving
 * resets the combat state and leaving settles the level; level 7 takes the
 * magic on the way in and gives back its maximum and a quarter on the way
 * out; level 9 swaps a byte with one in another segment, and phase 3
 * clears it. */
void level_transition_effects(uw_motion *m, int level, int phase, uint16_t bp) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    if ((ds[(uint16_t)(rec + 0x60)] & 0x10) && phase == 0) {
        game_world_reset(m);                /* after Armageddon every level is empty */
        return;
    }
    if (phase == 0) {
        ds[ASSAULT_VICTIM_INDEX] = 0;
        ds[ASSAULT_VICTIM_RACE] = 0xff;
        creature_clear_paths(m);
    } else if (phase == 1) {
        npc_settle_level(m, (uint16_t)(bp - 2 - 4 - 2));
    }
    if (level == 7) {
        if (ds[(uint16_t)(rec + 0x60)] & 0x20) return;
        if (phase == 0) {
            ds[(uint16_t)(rec + 0xb0)] = ds[(uint16_t)(rec + 0x38)];
            ds[(uint16_t)(rec + 0x38)] = 0;
            ds[(uint16_t)(rec + 0x37)] = 0;
            /* the floor the record remembers, again */
            apply_level7_floor_variant(m, (uint8_t)((ds[(uint16_t)(rec + 0x62)] >> 4) & 1));
        } else if (phase == 1) {
            ds[(uint16_t)(rec + 0x38)] = ds[(uint16_t)(rec + 0xb0)];
            ds[(uint16_t)(rec + 0x37)] = (uint8_t)(ds[(uint16_t)(rec + 0xb0)] >> 2);
        }
    } else if (level == 9) {
        if (phase == 0) {
            ds[(uint16_t)(rec + 0xb0)] = ds[0x546];
            ds[0x546] = 0;
        } else if (phase == 1) {
            ds[0x546] = ds[(uint16_t)(rec + 0xb0)];
        } else if (phase == 3) {
            ds[0x546] = 0;
        }
    }
}

/* combat_reset: a missile weapon aiming (its crosshair pushed)
 * put down, the weapon panel ready, the swing, its button and the attacker
 * cleared. */
void combat_reset(uw_motion *m) {
    uint8_t *ds = m->ds;
    if (ds[COMBAT_SWING_RANGED] && !rw(ds, COMBAT_SWING_ATTACK)) {
        ww(ds, ACTION_STATE_WORD, (uint16_t)(rw(ds, ACTION_STATE_WORD) - 4));
        cursor_shape_pop(m, 3);
    }
    combat_show_ready_weapon(m);
    ww(ds, SWING_STATE, 0);
    ww(ds, SWING_BUTTON, 0xffff);
    ww(ds, COMBAT_ATTACKER_OBJ, 0xffff);
}

/* place_player_in_tile(x, y), from the instructions: the Avatar
 * out of its tile, the motion reset -- the integrator's callback, flags,
 * speed and velocities -- at the tile's centre on its floor (raised 0x20
 * where the type's flags have bit 5), its record's position and fine
 * position (3, 3), +0x15's low six bits 0x2c and its link word cleared;
 * then the terrain under it through its own query (spatial_query_ptr left
 * on it), the collision face and the movement state from it, the vertical
 * sway stopped, view_apply_impairment, the liquid flag, and the Avatar back
 * in the tile. */
void place_player_in_tile(uw_motion *m, int16_t x, int16_t y, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t tracked = rw(ds, TRACKED_OBJECT), tp, q = (uint16_t)(bp - 0x18), w;
    uw_objpool pool;
    uint8_t face;
    memset(&pool, 0, sizeof pool);
    pool.seg = ls;
    if (rs(ds, TRACKED_TILE) >= 0)
        uw_object_list_remove(&pool, (uint16_t)(rw(ds, TILEMAP_PTR) + rw(ds, TRACKED_TILE) * 4 + 2), tracked);
    ww(ds, 0x2856, 0x0bb3);                 /* the callback's offset; its segment is the relocated one already there */
    ww(ds, 0x2850, 0x1100);
    ww(ds, RUN_FLAGS, 0);
    ww(ds, MOVEMENT_SPEED, 0);
    ww(ds, VERTICAL_GRAVITY, 0);
    ww(ds, MOVEMENT_INPUT_B, 0);
    ww(ds, MOVEMENT_INPUT_A, 0);
    ww(ds, VERTICAL_VELOCITY, 0);
    ww(ds, 0x2788, 0);
    ww(ds, 0x2786, 0);
    ww(ds, PLAYER_X, (uint16_t)((x << 8) + 0x80));
    ww(ds, PLAYER_Y, (uint16_t)((y << 8) + 0x80));
    ds[0x27a4] = 8;
    ww(ds, 0x27a0, 1);
    ww(ds, TRACKED_TILE, (uint16_t)((y << 6) + x));
    tp = (uint16_t)(rw(ds, TILEMAP_PTR) + rw(ds, TRACKED_TILE) * 4);
    ww(ds, PLAYER_Z, rw(ds, (uint16_t)(FLOOR_HEIGHTS + ((ls[tp] >> 4) & 0xf) * 2)));
    if (ds[(uint16_t)(TILE_TYPE_FLAGS + (ls[tp] & 0xf))] & 0x20)
        ww(ds, PLAYER_Z, (uint16_t)(rw(ds, PLAYER_Z) + 0x20));
    ww(ls, (uint16_t)(tracked + 2), (uint16_t)((rw(ls, (uint16_t)(tracked + 2)) & 0xff80) | ((rs(ds, PLAYER_Z) >> 3) & 0x7f)));
    w = rw(ls, (uint16_t)(tracked + 0x16));
    w = (uint16_t)((w & 0x3ff) | ((x & 0x3f) << 10));
    w = (uint16_t)((w & 0xfc0f) | ((y & 0x3f) << 4));
    ww(ls, (uint16_t)(tracked + 0x16), w);
    ww(ls, (uint16_t)(tracked + 2), (uint16_t)((rw(ls, (uint16_t)(tracked + 2)) & 0x1fff) | 0x6000));
    ww(ls, (uint16_t)(tracked + 2), (uint16_t)((rw(ls, (uint16_t)(tracked + 2)) & 0xe3ff) | 0x0c00));
    ls[(uint16_t)(tracked + 0x15)] = (uint8_t)((ls[(uint16_t)(tracked + 0x15)] & 0xc0) | 0x2c);
    ww(ls, (uint16_t)(tracked + 4), 0);
    ww(ds, SQ_PTR, q);
    ww(ds, (uint16_t)(q + 0xa), obj_index_of(m, tracked));
    ds[(uint16_t)(q + 8)] = (uint8_t)(prop(m, 0x7f, 1) & 7);
    ds[(uint16_t)(q + 9)] = prop(m, 0x7f, 0);
    ww(ds, q, (uint16_t)(x * 8 + 3));
    ww(ds, (uint16_t)(q + 2), (uint16_t)(y * 8 + 3));
    ww(ds, (uint16_t)(q + 4), (uint16_t)(rs(ds, PLAYER_Z) >> 3));
    sq_terrain(m, ds[0x27a4]);
    face = collision_face(m, (uint16_t)(rw(ds, (uint16_t)(q + 0xc)) | rw(ds, (uint16_t)(q + 0xe))));
    ds[COLLISION_FACE] = face;
    set_movement_state(m, face, 0);
    ww(ds, SWAY_VERTICAL, 0);
    view_apply_impairment(m);
    ds[PLAYER_IN_LIQUID] = 1;
    uw_object_list_insert(&pool, (uint16_t)(rw(ds, TILEMAP_PTR) + rw(ds, TRACKED_TILE) * 4 + 2), tracked);
}

/* level_change(from, to): combat_reset, an object on the cursor
 * dropped, leaving's effects, level_save, level_load and arriving's; the
 * level-entered flag. 0 when either half fails. */
static int level_change(uw_motion *m, uw_ark *ark, int from, int to, const uint8_t *terrain, size_t terrain_size,
                        uint16_t bp) {
    uint8_t *ds = m->ds;
    int ok;
    combat_reset(m);
    if (rw(ds, ACTION_STATE_WORD) == 2 && (rw(ds, CURSOR_OBJECT) || rw(ds, (uint16_t)(CURSOR_OBJECT + 2)))) {
        ww(ds, ACTION_STATE_WORD, 0);
        ww(ds, CURSOR_OBJECT, 0);
        ww(ds, (uint16_t)(CURSOR_OBJECT + 2), 0);
        cursor_shape_pop(m, 3);
    }
    level_transition_effects(m, from, 1, (uint16_t)(bp - 2 - 4 - 4 - 2));
    ok = uw_motion_level_save(m, ark);
    if (!ok) return 0;
    if (!uw_motion_level_load(m, ark, to, terrain, terrain_size)) return 0;
    level_transition_effects(m, to, 0, (uint16_t)(bp - 2 - 4 - 4 - 2));
    ds[LEVEL_ENTERED] = 1;
    return 1;
}

int uw_motion_pending_teleport(uw_motion *m, uw_ark *ark, const uint8_t *terrain, size_t terrain_size, uint16_t bp) {
    uint8_t *ds = m->ds;
    uint16_t lx = (uint16_t)(bp - 2), ly = (uint16_t)(bp - 4);
    if (rs(ds, TELEPORT_DEST_X) <= 0) return 1;
    if (ds[FADE_FLAGS] & 1) m->screen_fade |= 1;   /* view_rebuild_and_draw, then screen_fade_out: the host renders the view the fade runs over */
    if (rw(ds, CURRENT_LEVEL_WORD) != rw(ds, TELEPORT_DEST_LEVEL)) {
        if (!level_change(m, ark, rw(ds, CURRENT_LEVEL_WORD), rw(ds, TELEPORT_DEST_LEVEL), terrain, terrain_size,
                          (uint16_t)(bp - 4 - 4 - 4 - 2)))
            UW_NOT_CARRIED(m->not_carried);
        ww(ds, CURRENT_LEVEL_WORD, rw(ds, TELEPORT_DEST_LEVEL));
    }
    if (rw(ds, ARRIVAL_CALLBACK) || rw(ds, (uint16_t)(ARRIVAL_CALLBACK + 2))) {
        /* the arrival callback, by its runtime address: the silver tree's
         * (player_resurrect) and the moonstone's (teleport_to_moonstone) are
         * the two the game installs */
        uint32_t cb = (uint32_t)rw(ds, (uint16_t)(ARRIVAL_CALLBACK + 2)) << 16 | rw(ds, ARRIVAL_CALLBACK);
        if (cb == 0x629a0043u) player_resurrect(m);
        else if (cb == 0x629a0034u) teleport_to_moonstone(m);
        else UW_NOT_CARRIED(m->not_carried);
    }
    if (!find_landing_spot(m, rw(ds, TRACKED_OBJECT), rs(ds, TELEPORT_DEST_X), rs(ds, TELEPORT_DEST_Y), lx, ly, 0,
                           (uint16_t)(bp - 4 - 14 - 4 - 2))
        && !find_landing_spot(m, rw(ds, TRACKED_OBJECT), rs(ds, TELEPORT_DEST_X), rs(ds, TELEPORT_DEST_Y), lx, ly, 1,
                              (uint16_t)(bp - 4 - 14 - 4 - 2))) {
        ww(ds, TELEPORT_DEST_X, 0);
        m->lseg[(uint16_t)(rw(ds, TRACKED_OBJECT) + 8)] = 0;
        return 0;
    }
    ww(ds, TELEPORT_DEST_X, rw(ds, lx));
    ww(ds, TELEPORT_DEST_Y, rw(ds, ly));
    place_player_in_tile(m, rs(ds, TELEPORT_DEST_X), rs(ds, TELEPORT_DEST_Y), (uint16_t)(bp - 4 - 6 - 4 - 2));
    if (ds[FADE_FLAGS] & 2) m->screen_fade |= 2;   /* view_rebuild_and_draw, then screen_fade_in: the host's, as above */
    ww(ds, TELEPORT_DEST_X, 0);
    if (ds[FADE_FLAGS] & 2) ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 0x7ffe));
    return 1;
}


/* ==== leaving the dungeon and coming back: game_change_mode's pair for
 * row 0 of event_handlers -- dungeon_leave_handler, the
 * leave, and dungeon_draw_main_screen, the enter -- with
 * viewport_unbind_hotspots, viewport_bind_hotspots,
 * dungeon_mode_leave, dungeon_mode_enter and
 * panel_mode_restore; and leaving the game,
 * game_return_to_menu and game_reset_player_state,
 * which call the same pair ==== */

enum {
    VIEWPORT_HOTSPOT_ID = 0x1b37,
    HOTSPOT_PANEL_BUTTONS = 0x2694, HOTSPOT_RUNE_SHELF = 0x26a4, HOTSPOT_ACTIVE_SPELLS = 0x2692,
    HOTSPOT_COMPASS = 0x268e, HOTSPOT_FLASKS = 0x26a2
};
/* the eight arrow regions' slots, in the order viewport_bind_hotspots
 * stores them: shapes 0x106f, 0x1070, 0x106e, 0x1071, 0x1072, 0x106d,
 * 0x1073, 0x1074 */
static const uint16_t view_region_slots[8] = { 0x7284, 0x7366, 0x7362, 0x7286, 0x7368, 0x727e, 0x7360, 0x735c };

/* viewport_bind_hotspots(x, y, w, h), which view_set_viewport
 * runs: the last view hotspot unbound, the rectangle kept (0x7282, 0x735e,
 * 0x7364, 0x7280), the view's hotspot bound over it -- view_action_dispatch,
 * modes 0x1b, its id at 0x1b37 -- and the view tiled with the eight arrow
 * cursors, the columns the outer five fifteenths each side, the rows
 * three, three and nine fifteenths, each slot kept. */
static void viewport_bind_hotspots(uw_motion *m, int16_t x, int16_t y, int16_t w, int16_t h) {
    static const struct { uint8_t col, row; uint16_t shape; } grid[8] = {
        { 0, 0, 0x106f }, { 2, 0, 0x1070 }, { 1, 0, 0x106e }, { 0, 1, 0x1071 },
        { 2, 1, 0x1072 }, { 1, 2, 0x106d }, { 0, 2, 0x1073 }, { 2, 2, 0x1074 }
    };
    uint8_t *ds = m->ds;
    int16_t fifth = (int16_t)(w * 5 / 15), third = (int16_t)(h * 3 / 15), sixth = (int16_t)(h * 6 / 15);
    int16_t cx1[3] = { x, (int16_t)(x + fifth), (int16_t)(x + w - fifth) };
    int16_t cx2[3] = { (int16_t)(x + fifth), (int16_t)(x + w - fifth), (int16_t)(x + w - 1) };
    int16_t ry1[3] = { y, (int16_t)(y + third), (int16_t)(y + sixth) };
    int16_t ry2[3] = { (int16_t)(y + third), (int16_t)(y + sixth), (int16_t)(y + h - 1) };
    int i;
    input_unbind(m, rs(ds, VIEWPORT_HOTSPOT_ID));
    ww(ds, 0x7282, (uint16_t)x);
    ww(ds, 0x735e, (uint16_t)y);
    ww(ds, 0x7364, (uint16_t)w);
    ww(ds, 0x7280, (uint16_t)h);
    ww(ds, VIEWPORT_HOTSPOT_ID, (uint16_t)input_bind_hotspot(m, x, y, (int16_t)(x + w - 1), (int16_t)(y + h - 1),
                                                              0, 0x1b, 0x0eff, 0x2b13));
    for (i = 0; i < 8; i++)
        ww(ds, view_region_slots[i], (uint16_t)cursor_region_add(m, cx1[grid[i].col], ry1[grid[i].row],
                                                                cx2[grid[i].col], ry2[grid[i].row], grid[i].shape));
}

/* viewport_unbind_hotspots: the view's hotspot unbound, its id cleared, and
 * the eight arrow regions removed by their slots -- what view_set_viewport
 * registered, given back; dungeon_draw_main_screen runs it too, before it
 * registers them again. */
static void viewport_unbind_hotspots(uw_motion *m) {
    uint8_t *ds = m->ds;
    static const uint16_t order[8] = { 0x7284, 0x7366, 0x7286, 0x7368, 0x7362, 0x727e, 0x7360, 0x735c };
    int i;
    input_unbind(m, rs(ds, VIEWPORT_HOTSPOT_ID));
    ww(ds, VIEWPORT_HOTSPOT_ID, 0);
    for (i = 0; i < 8; i++) cursor_region_remove(m, rs(ds, order[i]));
}

/* dungeon_bind_hotspots: options_panel_active cleared and the
 * five hotspots bound, each id kept -- the action buttons (panel_button_click
 * with -1), the rune shelf and the active spells, the compass and
 * the flasks (their status lines); dungeon_unbind_hotspots
 * gives them back, the flasks before the compass. */
static void dungeon_bind_hotspots(uw_motion *m) {
    uint8_t *ds = m->ds;
    ww(ds, 0x1a30, 0);
    ww(ds, HOTSPOT_PANEL_BUTTONS, (uint16_t)input_bind_hotspot(m, 8, 0x54, 0x20, 0xce, -1, 1, 0x13d5, 0x2b13));
    ww(ds, HOTSPOT_RUNE_SHELF, (uint16_t)input_bind_hotspot(m, 0xb0, 0x2d, 0xde, 0x3d, 0, 1, 0x0020, 0x622f));
    ww(ds, HOTSPOT_ACTIVE_SPELLS, (uint16_t)input_bind_hotspot(m, 0x34, 0x2f, 0x66, 0x3f, 0, 1, 0x0052, 0x622f));
    ww(ds, HOTSPOT_COMPASS, (uint16_t)input_bind_hotspot(m, 0x7a, 0x31, 0x98, 0x40, 0, 1, 0x0032, 0x2b13));
    ww(ds, HOTSPOT_FLASKS, (uint16_t)input_bind_hotspot(m, 0xf4, 0x2c, 0x135, 0x50, 0, 1, 0x010f, 0x2b13));
}

static void dungeon_unbind_hotspots(uw_motion *m) {
    uint8_t *ds = m->ds;
    input_unbind(m, rs(ds, HOTSPOT_PANEL_BUTTONS));
    input_unbind(m, rs(ds, HOTSPOT_RUNE_SHELF));
    input_unbind(m, rs(ds, HOTSPOT_ACTIVE_SPELLS));
    input_unbind(m, rs(ds, HOTSPOT_FLASKS));
    input_unbind(m, rs(ds, HOTSPOT_COMPASS));
}

/* dungeon_mode_leave: dungeon_unbind_hotspots, inventory_noop, music_stop. */
static void dungeon_mode_leave(uw_motion *m) {
    dungeon_unbind_hotspots(m);
    music_stop(m);
}

/* dungeon_mode_enter: inventory_panel_init and panel_build_elements are
 * each guarded to run once (0x18a3, 0x0907) and have; scroll_reset (the
 * message scroll current, its mode cleared, the parchment repainted and
 * the end caps) is the page's, uw_boot_draw_main_screen's, which every way
 * in draws first -- a second clear would turn the end caps a frame past
 * the original's; music_resume; dungeon_bind_hotspots; the action button
 * pressed again -- the options loop's arm is dead, dungeon_bind_hotspots
 * having just cleared options_panel_active; dungeon_tick_update. */
static void dungeon_mode_enter(uw_motion *m, uint32_t clock) {
    uint8_t *ds = m->ds;
    music_resume(m);
    dungeon_bind_hotspots(m);
    if (rw(ds, 0x268c)) panel_button_draw(m, (int16_t)rw(ds, 0x268c), 1);
    uw_motion_tick_update(m, clock, 0);
}

void uw_motion_dungeon_leave(uw_motion *m) {
    uint8_t *ds = m->ds;
    viewport_unbind_hotspots(m);
    dungeon_mode_leave(m);
    /* panel_mode_restore: the panel shown made the panel wanted and up,
     * the flip's frame and the view switch cleared */
    ds[PANEL_MODE_WANTED] = ds[PANEL_MODE_SHOWN];
    ds[PANEL_MODE] = ds[PANEL_MODE_SHOWN];
    ds[PANEL_FLIP_FRAME_N] = 0;
    ds[VIEW_SWITCHING] = 0;
}

/* viewport_unbind_hotspots alone: the view's hotspot
 * and the eight arrow cursor regions given back. view_set_viewport is what
 * registers them, from dungeon_draw_main_screen, so a program that has not
 * been in the dungeon yet -- the title, the character generation screen --
 * has none, and the cursor keeps the shape the menu pushed. The port's boot
 * makes a running game's state, this takes them off it. */
void uw_motion_view_regions_unbind(uw_motion *m) {
    viewport_unbind_hotspots(m);
    /* and view_set_viewport's own first act, which the port's boot has
     * done for a running game: the title's captured states hold 0 here */
    m->ds[VIEW_VIEWPORT_DIRTY] = 0;
}

void uw_motion_dungeon_viewport(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t mask = rw(ds, (uint16_t)(rw(ds, 0x00e2) + 8));
    viewport_unbind_hotspots(m);
    viewport_bind_hotspots(m, 0x34, 0xb4 - 0x71 + 1, 0xac, 0x71);
    /* view_set_viewport's tail: the flag cleared and set again
     * for an event whose mode mask has bit 0 and not bit 3 -- the
     * dungeon's, which is also the mask that takes projection scale 25000.
     * view_present and screen_present composite the first-person weapon
     * over the view only while it is set, so without it no fist and no
     * blade is ever drawn; every captured dungeon state holds 1 and the
     * title's hold 0. */
    ds[VIEW_VIEWPORT_DIRTY] = (uint8_t)(!(mask & 8) && (mask & 1));
}

void uw_motion_dungeon_enter(uw_motion *m, uint32_t clock) {
    dungeon_mode_enter(m, clock);
}

/* game_reset_player_state: the inventory freed, the recalc, the player
 * object's word 2 bits 7..9 and +0x18 bits 0..4 cleared, the heading and
 * pitch zeroed, level 9's byte set, panel_values_reset (the dragons, the
 * eyes, the weapon's states),
 * game_prev_mode cleared, the weapon stowed, options_panel_active 2, the
 * action mode's cursor popped, the mode and its two pairs cleared, and a
 * held thing let go of -- or, past state 3, the camera put back on the
 * Avatar (debug_camera_set_target(1)). */
static void game_reset_player_state(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t pl = rw(ds, TRACKED_OBJECT);
    int16_t mode;
    inventory_discard_all(m);
    player_state_recalc(m);
    ww(ls, (uint16_t)(pl + 2), (uint16_t)(rw(ls, (uint16_t)(pl + 2)) & 0xfc7f));
    ls[(uint16_t)(pl + 0x18)] &= 0xe0;
    ww(ds, 0x727a, 0);
    ww(ds, 0x727c, 0);
    ds[0x0546] = 1;
    panel_values_reset(m);
    ww(ds, 0x5702, 0);
    weapon_stow(m);
    ww(ds, 0x1a30, 2);
    mode = rs(ds, 0x268c);
    if (mode == 1 || mode == 3 || mode == 4) cursor_shape_pop(m, 3);
    ww(ds, 0x268c, 0);
    ww(ds, 0x09f4, 0); ww(ds, 0x09f6, 0);
    ww(ds, 0x16e3, 0); ww(ds, 0x16e5, 0);
    if (rw(ds, ACTION_STATE_WORD)) {
        if (rs(ds, ACTION_STATE_WORD) < 4) {
            cursor_shape_pop(m, 3);
            ww(ds, CURSOR_OBJECT, 0);
            ww(ds, (uint16_t)(CURSOR_OBJECT + 2), 0);
            ww(ds, ACTION_STATE_WORD, 0);
        } else {
            ww(ds, ACTION_STATE_WORD, 0);
            debug_camera_set_target(m, 1);
        }
    }
}

void uw_motion_leave_game(uw_motion *m, int show) {
    uint8_t *ds = m->ds;
    ds[0x0726] = 0;                         /* cursor_reset_bounds' countdown */
    cursor_reset_bounds(m);
    /* input_poll until nothing is pending: the host's (the shell's
     * leave_game_begin) */
    if (show) {
        panel_set_value(m, 2, 0);
        panel_draw_compass(m);
        panel_flask_fill(m, 0);
    }
}

void uw_motion_leave_game_finish(uw_motion *m, int show) {
    uint8_t *ds = m->ds;
    if (show) {
        if (m->scroll) uw_scroll_clear(m->scroll, 1);
        else UW_NOT_CARRIED(m->not_carried);
    }
    uw_motion_dungeon_leave(m);             /* the mode's leave handler, row 0's */
    ww(ds, (uint16_t)(rw(ds, 0x00e2) + 8), 0);
    ww(ds, 0x565e, 0);
    ww(ds, 0x5664, 0xffff);
    ds[0x13b3] = 0;
    if (show) cursor_hide(m);
    game_reset_player_state(m);
}

void uw_motion_enter_game(uw_motion *m, uint32_t clock) {
    uint8_t *ds = m->ds;
    ds[0x13b3] = 1;
    ww(ds, 0x5664, 0);
    ww(ds, 0x565e, 1);
    ww(ds, (uint16_t)(rw(ds, 0x00e2) + 8), 1);
    /* the mode's enter handler, row 0's: the page is the host's */
    uw_motion_dungeon_viewport(m);
    dungeon_mode_enter(m, clock);
}


/* The view spun into the moongate at tile (32, 32): the camera on the
 * vortex's eye (attach_eye(3)), the distance to the gate's corner -- the
 * square root of the squared differences, >> 6 -- and its bearing from the
 * differences scaled by it, then 64 frames, the angle stepped by 0xccb
 * each, the eye drawing in as the timer runs; the camera back on the
 * Avatar. The frames are the host's (view_hook). */
static void vortex(uw_motion *m) {
    uint8_t *ds = m->ds;
    int32_t xd, yd;
    int16_t x, y;
    uint16_t rad;
    ds[0x3598] = ds[0x3599] = 0x20;
    debug_camera_set_target(m, 3);
    xd = (int16_t)(ds[0x3598] * 0x100 - rw(ds, 0x2780));
    yd = (int16_t)(ds[0x3599] * 0x100 - rw(ds, 0x2782));
    rad = (uint16_t)((int16_t)uw_isqrt32((uint32_t)(xd * xd + yd * yd)) >> 6);
    ww(ds, 0x357a, rad);
    if (!rad) {
        UW_NOT_CARRIED(m->not_carried);
        rad = 1;
    }
    x = (int16_t)((int32_t)((uint32_t)xd << 15) / ((int32_t)rad << 6));
    y = (int16_t)((int32_t)((uint32_t)yd << 15) / ((int32_t)rad << 6));
    ww(ds, 0x357e, (uint16_t)uw_atan2(y, x));
    for (ww(ds, 0x357c, 0); rw(ds, 0x357c) < 0x40; ww(ds, 0x357c, (uint16_t)(rw(ds, 0x357c) + 1))) {
        if (m->view_hook) m->view_hook(m->view_user, m, UW_VIEW_VORTEX_FRAME);
        else UW_NOT_CARRIED(m->not_carried);
        ww(ds, 0x357e, (uint16_t)(rw(ds, 0x357e) + 0xccb));
    }
    debug_camera_set_target(m, 1);
}

/* ==== game_ending_sequence ==== */

void uw_motion_ending(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), pl = rw(ds, TRACKED_OBJECT);
    enum { BP = 0x9568 };
    if (ds[0x1c8f] == 0) {
        uint16_t o;
        uw_objpool pool;
        if (ds[(uint16_t)(rec + 0x6d)] != 0) return;
        /* a moongate, item 0x15a -- the gate the Slasher is dragged
         * through -- made in tile (32, 32) for the moment of the fade: a quantity object with word 3's high bits 0xb */
        o = create_object(m, 0x15a, 0);
        if (o) {
            ww(ls, o, (uint16_t)(rw(ls, o) | 0x8000));
            ww(ls, (uint16_t)(o + 6), (uint16_t)((rw(ls, (uint16_t)(o + 6)) & 0x3f) | 0xb000));
            pool_from_ds(m, &pool);
            uw_object_list_append(&pool, (uint16_t)(tile_ptr(m, 0x20, 0x20) + 2), o);
            pool_to_ds(m, &pool);
        }
        print_message(m, 0x117);            /* "The Slasher of Veils is dragged from this world." */
        vortex(m);
        m->screen_fade |= 1;                /* screen_fade_out(5): the view wiped */
        if (o) {
            pool_from_ds(m, &pool);
            uw_object_list_remove(&pool, (uint16_t)(tile_ptr(m, 0x20, 0x20) + 2), o);
            uw_obj_free(&pool, o);
            pool_to_ds(m, &pool);
        }
        trap_teleport(m, pl, 0x1b, 0x17, 9, BP);
        ds[(uint16_t)(rec + 0x6d)] = 0xff;
        print_message(m, 0x118);            /* "You are sucked through the moongate . . . . ." */
        ds[0x13b2] &= 0xfe;
        m->ending = 1;                      /* perform_pending_teleport: the host's */
        return;
    }
    ww(ds, (uint16_t)(rw(ds, 0x00e2) + 8), 0);
    ww(ds, 0x1a30, 2);
    m->ending_show = 1;
}

void uw_motion_ending_teleported(uw_motion *m) {
    m->ds[0x13b2] |= 1;
    m->ending = 0;
}
