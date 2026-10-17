/* SPDX-License-Identifier: MIT */
/* See uw_boot.h. Each block below names the original routine whose writes
 * it makes; the values a routine takes
 * from a table in the executable are read from the image, and the ones it
 * computes are computed. */
#include "uw_boot.h"
#include "uw_motion_int.h"
#include "uw_image.h"
#include "uw_objprops.h"
#include "uw_chargen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    RUNTIME_LOAD_SEG = 0x0824,   /* the load segment the runtime's far pointers carry */
    LEVEL_SEG        = 0x7f5e,   /* the level buffer's runtime segment: farmalloc(0x7c08)'s
                                  * block past the runtime's heap base 0x6c2c, its data at +4
                                  * past the block header, the break at 0x871f after it */
    DS_FILE_SEG      = 0x6aac - 0x1000,
    HOTSPOT_TABLE    = 0x80c0,   /* input_tables_init's malloc, grown by each bind */
    KEY_TABLE        = 0x81ae
};

static uw_blob file_at(const char *dir, const char *sub, const char *name) {
    char path[768];
    snprintf(path, sizeof path, "%s/%s%s", dir, sub, name);
    return uw_read_file(path);
}

/* ---- the data segment from the executable ------------------------------- */

/* The image of 6aac, and the linker's relocations in it applied at the
 * runtime load segment: 153 far pointers, the handler tables among them
 * (event_handlers at 0x12ec, the panel drawers at 0x08c3, the motion
 * filters' callbacks), which the port matches by their runtime spelling. */
static bool ds_image(uint8_t *ds, const char *dir) {
    uw_blob exe = file_at(dir, "", "UW.EXE");
    size_t hdr, base = (size_t)DS_FILE_SEG * 16u;
    uint16_t nrel, rtab, i;
    if (!exe.data || exe.size < 0x20) { uw_free(&exe); return false; }
    hdr = (size_t)(exe.data[8] | exe.data[9] << 8) * 16u;
    if (hdr + base + 0x10000 > exe.size) { uw_free(&exe); return false; }
    memcpy(ds, exe.data + hdr + base, 0x10000);
    nrel = (uint16_t)(exe.data[6] | exe.data[7] << 8);
    rtab = (uint16_t)(exe.data[0x18] | exe.data[0x19] << 8);
    for (i = 0; i < nrel && (size_t)rtab + 4u * i + 4 <= exe.size; i++) {
        const uint8_t *e = exe.data + rtab + 4u * i;
        uint32_t lin = (uint32_t)(e[2] | e[3] << 8) * 16u + (uint32_t)(e[0] | e[1] << 8);
        if (lin >= base && lin + 1 < base + 0x10000) {
            uint16_t at = (uint16_t)(lin - base);
            ww(ds, at, (uint16_t)(rw(ds, at) + RUNTIME_LOAD_SEG));
        }
    }
    uw_free(&exe);
    return true;
}

/* obj_properties_load: OBJECTS.DAT's sections through the
 * loader table at 0x19ca -- weapon_props_load (melee, missile, armour),
 * critter_properties_load, class2_props_load, trigger_props_load,
 * animation_props_load -- each into its home in the data segment. */
static bool obj_properties_load(uint8_t *ds, const char *dir) {
    static const struct { uw_objprop_section s; uint16_t at; } homes[] = {
        { UW_SEC_MELEE, 0x5972 },     { UW_SEC_MISSILE, 0x5942 }, { UW_SEC_ARMOUR, 0x59f2 },
        { UW_SEC_CRITTER, 0x4a52 },   { UW_SEC_CONTAINER, 0x5b0a }, { UW_SEC_LIGHT, 0x5b3a },
        { UW_SEC_FOOD, 0x5b5a },      { UW_SEC_TRIGGER, 0x737e },   { UW_SEC_ANIMATION, 0x3658 },
    };
    uw_objprops op;
    char path[768];
    size_t i;
    snprintf(path, sizeof path, "%s/DATA/OBJECTS.DAT", dir);
    if (!uw_objprops_open(&op, path)) return false;
    for (i = 0; i < sizeof homes / sizeof homes[0]; i++) {
        size_t n;
        const uint8_t *d = uw_objprop_section_data(&op, homes[i].s, &n);
        if (d && n && (size_t)homes[i].at + n <= 0x10000) memcpy(ds + homes[i].at, d, n);
    }
    uw_objprops_close(&op);
    return true;
}

/* ---- the input tables ----------------------------------------------------- */

typedef struct { uint16_t key, param, mask, off, seg; } key_bind;
typedef struct { uint16_t x1, y1, x2, y2, param, mask, off, seg; } hotspot_bind;

/* input_bind_key's records: debug_bind_key's, dungeon_mode_setup's
 * in its order, parse_command_line's two. The segments are the
 * runtime's: movement_set_mode's 35c0, view_pitch_step's 6277, 2b13, 629a,
 * 622f, 6261, combat_swing's 2945, cursor_key_move's 2300, 61c0, 61a2. */
static const key_bind key_binds[] = {
    { 0x283, 0, 0xff, 0x0020, 0x6259 },
    { 0x77, 0xe, 1, 0x4e, 0x35c0 }, { 0x73, 5, 1, 0x4e, 0x35c0 }, { 0x61, 3, 1, 0x4e, 0x35c0 },
    { 0x64, 4, 1, 0x4e, 0x35c0 },   { 0x7a, 9, 1, 0x4e, 0x35c0 }, { 0x63, 10, 1, 0x4e, 0x35c0 },
    { 0x78, 8, 1, 0x4e, 0x35c0 },   { 0x65, 0xc, 0x1b, 0x4e, 0x35c0 }, { 0x71, 0xd, 0x1b, 0x4e, 0x35c0 },
    { 0x41, 0xffff, 1, 0x334, 0x35c0 }, { 0x44, 1, 1, 0x334, 0x35c0 }, { 0x53, 0, 1, 0x334, 0x35c0 },
    { 0x58, 0xfffe, 1, 0x334, 0x35c0 }, { 0x57, 2, 1, 0x334, 0x35c0 },
    { 0x33, 1, 0x11, 0x52, 0x6277 }, { 0x31, 0xffff, 0x11, 0x52, 0x6277 }, { 0x32, 0, 0x11, 0x52, 0x6277 },
    { 0x6a, 7, 0x1b, 0x4e, 0x35c0 }, { 0x4a, 6, 0x1b, 0x4e, 0x35c0 },
    { 0x86, 0, 0x1b, 0xf, 0x2b13 },  { 0x89, 0, 0x1b, 0x8e, 0x629a }, { 0x88, 2, 0x1b, 0x93, 0x629a },
    { 0x87, 1, 0x1b, 0x20, 0x622f },
    { 0x173, 0x173, 1, 0x7f, 0x6261 }, { 0x172, 0x172, 1, 0x7f, 0x6261 }, { 0x16d, 0x16d, 1, 0x7f, 0x6261 },
    { 0x166, 0x166, 1, 0x7f, 0x6261 }, { 0x164, 0x164, 1, 0x7f, 0x6261 }, { 0x171, 0x171, 1, 0x7f, 0x6261 },
    { 0x80, 5, 1, 0x13d5, 0x2b13 }, { 0x81, 4, 1, 0x13d5, 0x2b13 }, { 0x82, 3, 1, 0x13d5, 0x2b13 },
    { 0x83, 2, 1, 0x13d5, 0x2b13 }, { 0x84, 1, 1, 0x13d5, 0x2b13 }, { 0x85, 0, 1, 0x13d5, 0x2b13 },
    { 0x70, 9, 1, 0x11ba, 0x2945 }, { 0x2e, 3, 1, 0x11ba, 0x2945 }, { 0x3b, 6, 1, 0x11ba, 0x2945 },
    { 0x4a3, 0x4a3, 7, 0xe29, 0x2300 }, { 9, 9, 7, 0xe29, 0x2300 }, { 0x8d, 0x8d, 7, 0xe29, 0x2300 },
    { 0x93, 0x93, 7, 0xe29, 0x2300 }, { 0x8f, 0x8f, 7, 0xe29, 0x2300 }, { 0x91, 0x91, 7, 0xe29, 0x2300 },
    { 0x8c, 0x8c, 7, 0xe29, 0x2300 }, { 0x8e, 0x8e, 7, 0xe29, 0x2300 }, { 0x92, 0x92, 7, 0xe29, 0x2300 },
    { 0x94, 0x94, 7, 0xe29, 0x2300 }, { 0x95, 0x95, 7, 0xe29, 0x2300 }, { 0x96, 0x96, 7, 0xe29, 0x2300 },
    { 0x1b, 4, 4, 0x5c, 0x61c0 }, { 0x31, 1, 4, 0x75, 0x61c0 }, { 0x32, 2, 4, 0x75, 0x61c0 },
    { 0x33, 3, 4, 0x75, 0x61c0 }, { 0x34, 4, 4, 0x75, 0x61c0 },
    { 0x286, 0, 0x1b, 0x61, 0x6277 }, { 0x287, 0, 0x1b, 0x66, 0x6277 },
    { 0x278, 0, 0xbd, 0x3e, 0x61f7 }, { 0x271, 0x4f4c, 0xff, 0x20, 0x6205 },
};

/* input_bind_hotspot's records, in binding order: the three
 * movement arrows and the three options hotspots of dungeon_mode_setup, the
 * 3-D view's (view_set_viewport's active area, view_action_dispatch), the
 * inventory panel's (inventory_panel_init, panel_inventory_click), and
 * dungeon_bind_hotspots' five: the action buttons, the rune shelf, the
 * active spells, the compass and the flasks. */
static const hotspot_bind hotspot_binds[] = {
    { 0x6b, 0x21, 0x7b, 0x2f, 0xffff, 1, 0x334, 0x35c0 },
    { 0x82, 0x1f, 0x92, 0x2c, 0, 1, 0x334, 0x35c0 },
    { 0x9b, 0x21, 0xaa, 0x2f, 1, 1, 0x334, 0x35c0 },
    { 0x52, 0x98, 0x88, 0xbe, 4, 4, 0x70, 0x61a2 },
    { 0x8b, 0x98, 0xc1, 0xbe, 4, 4, 0x84, 0x61a2 },
    { 0x0f, 0x01, 0x131, 0x1e, 0, 4, 0x75, 0x61c0 },
    { 0x34, 0x44, 0xdf, 0xb4, 0, 0x1b, 0xeff, 0x2b13 },
    { 0xf0, 0x52, 0x13b, 0xbd, 0, 5, 0x131d, 0x2b13 },
    { 0x08, 0x54, 0x20, 0xce, 0xffff, 1, 0x13d5, 0x2b13 },
    { 0xb0, 0x2d, 0xde, 0x3d, 0, 1, 0x20, 0x622f },
    { 0x34, 0x2f, 0x66, 0x3f, 0, 1, 0x52, 0x622f },
    { 0x7a, 0x31, 0x98, 0x40, 0, 1, 0x32, 0x2b13 },
    { 0xf4, 0x2c, 0x135, 0x50, 0, 1, 0x10f, 0x2b13 },
};

static void input_tables(uint8_t *ds) {
    uint16_t nk = sizeof key_binds / sizeof key_binds[0], nh = sizeof hotspot_binds / sizeof hotspot_binds[0], i;
    /* input_tables_init: the tables, the counts, the ids in
     * opposite directions, the event's +6 cleared */
    ww(ds, 0x24a4, HOTSPOT_TABLE);
    ww(ds, 0x24b4, KEY_TABLE);
    ww(ds, (uint16_t)(rw(ds, 0x00e2) + 6), 0);
    for (i = 0; i < nk; i++) {
        uint16_t r = (uint16_t)(KEY_TABLE + i * 0xc);
        ww(ds, r, (uint16_t)(0xffff - i));
        ww(ds, (uint16_t)(r + 2), key_binds[i].key);
        ww(ds, (uint16_t)(r + 4), key_binds[i].param);
        ww(ds, (uint16_t)(r + 6), key_binds[i].mask);
        ww(ds, (uint16_t)(r + 8), key_binds[i].off);
        ww(ds, (uint16_t)(r + 10), key_binds[i].seg);
    }
    ww(ds, 0x24a6, nk);
    ww(ds, 0x00e4, (uint16_t)(0xffff - nk));
    for (i = 0; i < nh; i++) {
        uint16_t r = (uint16_t)(HOTSPOT_TABLE + i * 0x12);
        ww(ds, r, (uint16_t)(i + 1));
        ww(ds, (uint16_t)(r + 2), hotspot_binds[i].x2);
        ww(ds, (uint16_t)(r + 4), hotspot_binds[i].y2);
        ww(ds, (uint16_t)(r + 6), hotspot_binds[i].x1);
        ww(ds, (uint16_t)(r + 8), hotspot_binds[i].y1);
        ww(ds, (uint16_t)(r + 0xa), hotspot_binds[i].param);
        ww(ds, (uint16_t)(r + 0xc), hotspot_binds[i].mask);
        ww(ds, (uint16_t)(r + 0xe), hotspot_binds[i].off);
        ww(ds, (uint16_t)(r + 0x10), hotspot_binds[i].seg);
    }
    ww(ds, 0x24a8, nh);
    ww(ds, 0x24a2, (uint16_t)(nh + 1));
    /* the ids the binders keep for the leave: dungeon_bind_hotspots'
     * five (the view's, viewport_hotspot_id, below) */
    ww(ds, 0x2694, 9);  ww(ds, 0x26a4, 10); ww(ds, 0x2692, 11);
    ww(ds, 0x268e, 12); ww(ds, 0x26a2, 13);
}

/* ---- the cursor ------------------------------------------------------------ */

/* cursor_init: the clamp box the whole screen and the arrow
 * shape; and the nine cursor regions the panels register
 * (cursor_region_x1/y1/x2/y2 at 0x24fc, 0x24d0, 0x2528, 0x2554, the shapes
 * at 0x258e): the inventory panel's eight slots with the hand cursors
 * 0x106d..0x1074, and the panel itself with the arrow -- read from the
 * running game (stand1's state), as inventory_panel_init's registrations
 * leave them. Unused entries of x1 hold 10000. */
/* cursor_region_add(x1, y1, x2, y2, shape) -> slot: five
 * PARALLEL arrays of twenty words, a free slot marked by an x1 of 10000
 * rather than by the count, which is only a high-water mark. The boot's
 * is the motion layer's (src/uw_motion_cursor.c) without its last step,
 * the shape looked up again, which here, before the cursor is set up,
 * would read a cursor there is not yet. */
static int boot_region_add(uw_motion *m, uint16_t x1, uint16_t y1,
                           uint16_t x2, uint16_t y2, uint16_t shape) {
    uint8_t *ds = m->ds;
    int i;
    for (i = 0; i < 20 && rw(ds, (uint16_t)(0x24fc + i * 2)) != 10000; i++) { }
    if (i == 20) return -1;
    ww(ds, (uint16_t)(0x24fc + i * 2), x1);
    ww(ds, (uint16_t)(0x24d0 + i * 2), y1);
    ww(ds, (uint16_t)(0x2528 + i * 2), x2);
    ww(ds, (uint16_t)(0x2554 + i * 2), y2);
    ww(ds, (uint16_t)(0x258e + i * 2), shape);
    if ((int)rw(ds, CURSOR_REGIONS) <= i) ww(ds, CURSOR_REGIONS, (uint16_t)(i + 1));
    return i;
}

/* viewport_bind_hotspots(x, y, w, h), its cursor half: the view
 * tiled with EIGHT arrow cursors as a 3 x 3 grid without its centre -- the
 * columns the outer five fifteenths each side, the rows three, three and
 * nine -- which is the movement-cursor map, and the same quantisation
 * action_combat picks a swing from; these are the two calls that make
 * them. The hotspot half is in
 * the static table; the slots are kept where the teardown reads them
 * (viewport_unbind_hotspots, src/uw_motion_save.c), and the way back into the
 * dungeon makes all of it again with the motion layer's own
 * viewport_bind_hotspots. */
static void viewport_bind_cursor_regions(uw_motion *m, int x, int y, int w, int h) {
    uint8_t *ds = m->ds;
    int fifth = (w * 5) / 15, third = (h * 3) / 15, sixth = (h * 6) / 15;
    ww(ds, 0x7284, (uint16_t)boot_region_add(m, (uint16_t)x, (uint16_t)y,
                                               (uint16_t)(x + fifth), (uint16_t)(y + third), 0x106f));
    ww(ds, 0x7366, (uint16_t)boot_region_add(m, (uint16_t)(x + w - fifth), (uint16_t)y,
                                               (uint16_t)(x + w - 1), (uint16_t)(y + third), 0x1070));
    ww(ds, 0x7362, (uint16_t)boot_region_add(m, (uint16_t)(x + fifth), (uint16_t)y,
                                               (uint16_t)(x + w - fifth), (uint16_t)(y + third), 0x106e));
    ww(ds, 0x7286, (uint16_t)boot_region_add(m, (uint16_t)x, (uint16_t)(y + third),
                                               (uint16_t)(x + fifth), (uint16_t)(y + sixth), 0x1071));
    ww(ds, 0x7368, (uint16_t)boot_region_add(m, (uint16_t)(x + w - fifth), (uint16_t)(y + third),
                                               (uint16_t)(x + w - 1), (uint16_t)(y + sixth), 0x1072));
    ww(ds, 0x727e, (uint16_t)boot_region_add(m, (uint16_t)(x + fifth), (uint16_t)(y + sixth),
                                               (uint16_t)(x + w - fifth), (uint16_t)(y + h - 1), 0x106d));
    ww(ds, 0x7360, (uint16_t)boot_region_add(m, (uint16_t)x, (uint16_t)(y + sixth),
                                               (uint16_t)(x + fifth), (uint16_t)(y + h - 1), 0x1073));
    ww(ds, 0x735c, (uint16_t)boot_region_add(m, (uint16_t)(x + w - fifth), (uint16_t)(y + sixth),
                                               (uint16_t)(x + w - 1), (uint16_t)(y + h - 1), 0x1074));
    /* and the rectangle (0x7282, 0x735e, 0x7364, 0x7280) */
    ww(ds, 0x7282, (uint16_t)x); ww(ds, 0x735e, (uint16_t)y);
    ww(ds, 0x7364, (uint16_t)w); ww(ds, 0x7280, (uint16_t)h);
}

static void cursor_init(uw_motion *m) {
    uint8_t *ds = m->ds;
    int i;
    /* ems_init's page frame (ems_page_frame_seg): INT 67h's answer,
     * 0xE000. ems_frame_claim returns it, and converse_draw_screen keeps
     * that in the word converse_refresh tests to take the table down
     * (end_barter) */
    ww(ds, 0x24ba, 0xe000);
    /* the clamp box the whole screen, and the twenty region slots free --
     * twenty: two more would run over the box's x0 and y0 at 0x2524 */
    ww(ds, 0x2524, 0);
    ww(ds, 0x2526, 0);
    ww(ds, 0x257e, 0x13f);
    ww(ds, 0x2580, 199);
    for (i = 0; i < 20; i++) ww(ds, (uint16_t)(0x24fc + i * 2), 10000);
    ww(ds, CURSOR_REGIONS, 0);
    viewport_bind_cursor_regions(m, 0x34, 0x44, 0xac, 0x71);
    /* inventory_panel_init's one registration, the ninth */
    boot_region_add(m, 0xf0, 0x52, 0x13b, 0xbd, 0x106c);
    ww(ds, CURSOR_CACHE_X1, 0xffff);
    ww(ds, CURSOR_X, 160);
    ww(ds, CURSOR_Y, 100);
    ww(ds, CURSOR_VISIBLE, 0);
    ds[CURSOR_ON_SCREEN] = 0;
    ds[CURSOR_DEPTH] = 0;
    ww(ds, MOUSE_PENDING, 0xffff);
    cursor_set_shape(m, 0x106c);
}

/* ---- the player ------------------------------------------------------------ */

/* player_init(blank): the new character's
 * record, then the skills 3d4 and the attributes 2d10 + 10 when not blank,
 * player_recompute_maxima(1), +0x4a cleared, the Avatar's hit points the
 * row's maximum less 6 less rand() % 6, level 1, player_state_recalc. */
static void player_init(uw_motion *m, int blank) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), row = rw(ds, CRITTER_ROW_PTR), tracked = rw(ds, TRACKED_OBJECT);
    int i, r;
    ds[rec + 0x64] |= 1;
    ww(ds, (uint16_t)(rec + 0x4e), 0);
    ww(ds, (uint16_t)(rec + 0x50), 0);
    ds[rec + 0x52] = 1;
    ds[rec + 0x53] = 0;
    ds[rec + 0x3d] = 1;
    ww(ds, (uint16_t)(rec + 0xce), 0x3000);
    ww(ds, (uint16_t)(rec + 0xd0), 0x10b);
    ds[rec + 0x5e] = 2;
    ds[rec + 0x5f] &= 0xc3;
    ww(ds, (uint16_t)(rec + 0x5f), (uint16_t)(rw(ds, (uint16_t)(rec + 0x5f)) & 0xfc3f));
    ds[rec + 0x60] &= 0x03;
    ds[rec + 0x62] &= 0xe3;
    ds[rec + 0x61] &= 0xfc;
    ww(ds, (uint16_t)(rec + 0x61), (uint16_t)(rw(ds, (uint16_t)(rec + 0x61)) & 0xfc0f));
    ds[rec + 0x61] &= 0xf3;
    ds[rec + 0xb5] = (uint8_t)((ds[rec + 0xb5] & 0xf) | 0x30);
    /* set_render_detail for detail 3 */
    ww(ds, 0x54b, 1);
    ww(ds, 0x555, 0x0a4a);
    ww(ds, 0x55d, 0x0a4a);
    ww(ds, 0x54d, 1);
    ds[rec + 0x6d] = 8;
    ww(ds, (uint16_t)(rec + 0x65), 0);
    ww(ds, (uint16_t)(rec + 0x67), 0);
    ww(ds, (uint16_t)(rec + 0x6e), 0);
    ds[rec + 0xb6] &= 0xf8;
    ds[rec + 0xb8] = 0;
    ds[rec + 0xb9] = 0;
    ds[rec + 0x3a] = 0x40;
    ds[rec + 0x3b] = 0x40;
    ds[rec + 0x3c] = 0;
    memset(ds + rec + 0x69, 0, 4);
    memset(ds + rec + 0x47, 0x18, 3);
    memset(ds + rec + 0x44, 0, 3);
    memset(ds + rec + 0x70, 0, 0x40);
    memset(ds + rec + 0xc2, 0, 8);
    ds[rec + 0x8a] = 0x35;
    ds[rec + 0x39] = 0xc0;
    r = rt_rand(m);
    ds[rec + 0x64] = (uint8_t)((ds[rec + 0x64] & 0xe3) | ((r % 5 & 7) << 2));
    r = rt_rand(m);
    ds[rec + 0x64] = (uint8_t)((ds[rec + 0x64] & 0xfd) | ((r & 1) << 1));
    for (i = 0; i < 0x14; i++) ds[rec + 0x21 + i] = (uint8_t)(blank ? 0 : roll_dice(m, 3, 4));
    for (i = 0; i < 3; i++) ds[row + 5 + i] = (uint8_t)(blank ? 0 : roll_dice(m, 2, 10) + 10);
    player_recompute_maxima(m, 1);
    ww(ds, (uint16_t)(rec + 0x4a), 0);      /* the carried weight, a word */
    m->lseg[(uint16_t)(tracked + 8)] = (uint8_t)(ds[row + 4] - 6 - rt_rand(m) % 6);
    ww(ds, CURRENT_LEVEL_WORD, 1);
    player_state_recalc(m);
}

/* ---- the character generation's result ------------------------------------ */

/* skill_governing_attribute: strength for skills under 7,
 * intelligence under 10, dexterity for the rest. */
static int skill_governing_attribute(int skill) {
    return skill < 7 ? 0 : skill < 10 ? 2 : 1;
}

/* player_raise_skill(skill), from the instructions: a skill at
 * zero gains 3, its attribute / 9, rand() * 3 / 0x8000 and three rolls of
 * check_skill_roll(attribute, 20); one above zero gains 1, the attribute
 * / 13, rand() * 2 / 0x8000 and two rolls; capped at 30. */
static void player_raise_skill(uw_motion *m, int skill) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), row = rw(ds, CRITTER_ROW_PTR), at = (uint16_t)(rec + 0x21 + skill);
    int zero = ds[at] == 0, add = zero ? 3 : 1, div = zero ? 9 : 13, n = zero ? 3 : 2, i;
    int attr = ds[(uint16_t)(row + 5 + skill_governing_attribute(skill))];
    ds[at] = (uint8_t)(ds[at] + add);
    ds[at] = (uint8_t)(ds[at] + attr / div);
    ds[at] = (uint8_t)(ds[at] + (int32_t)rt_rand(m) * n / 0x8000);
    for (i = 0; i < n; i++) ds[at] = (uint8_t)(ds[at] + check_skill_roll(m, (int16_t)attr, 0x14));
    if (ds[at] > 0x1e) ds[at] = 0x1e;
}

/* chargen_roll_attributes, from the instructions: the class's
 * row of SKILLS.DAT's first table into the critter row's strength,
 * dexterity and intelligence, the twenty skills cleared, then the row's
 * pool spent one to four points at a time (rand() & 3 + 1, no more than is
 * left) into an attribute chosen at random (rand() % 3), each capped at
 * 30; player_recompute_maxima(1) and the vitality into the Avatar. */
void uw_chargen_roll_attributes(uw_motion *m, const uw_skills *sk) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), row = rw(ds, CRITTER_ROW_PTR), tracked = rw(ds, TRACKED_OBJECT);
    int cls = (ds[rec + 0x64] >> 5) & 7, pool, i;
    for (i = 0; i < 3; i++) ds[row + 5 + i] = (uint8_t)uw_skills_attribute(sk, cls, i);
    memset(ds + rec + 0x21, 0, 0x14);
    pool = uw_skills_attribute(sk, cls, 3);
    while (pool > 0) {
        int k = (rt_rand(m) & 3) + 1, a;
        if (k > pool) k = pool;
        a = rt_rand(m) % 3;
        if (ds[row + 5 + a] + k > 0x1e) k = 0x1e - ds[row + 5 + a];
        ds[row + 5 + a] = (uint8_t)(ds[row + 5 + a] + k);
        pool -= k;
    }
    player_recompute_maxima(m, 1);
    m->lseg[(uint16_t)(tracked + 8)] = ds[row + 4];
}

void uw_chargen_keep(uw_motion *m) {
    player_recompute_maxima(m, 1);
}

/* chargen_skill_choices(&pos, list, step, skills): from the
 * class's record `pos` of the five SKILLS.DAT holds for it -- an empty
 * record puts the sentinel 0x14 in the list, a record of one its skill,
 * and a longer one is a choice, its skills offered as the step's list and
 * 1 returned with `pos` past it; 0 once the five are walked. */
int uw_chargen_skill_choices(uw_motion *m, const uw_skills *sk, int *pos, uint8_t *list, const uint8_t **offer,
                                 int *offered) {
    int cls = (m->ds[rw(m->ds, PLAYER_RECORD_PTR) + 0x64] >> 5) & 7;
    while (*pos < 5) {
        int n = 0;
        const uint8_t *r = uw_skills_record(sk, cls, *pos, &n);
        if (!r || n == 0) list[*pos] = 0x14;
        else if (n == 1) list[*pos] = r[0];
        else {
            *offer = r;
            *offered = n;
            (*pos)++;
            return 1;
        }
        (*pos)++;
    }
    return 0;
}

/* chargen_apply_skills(from, list): player_raise_skill for every
 * entry from `from` under 0x14; how many. */
int uw_chargen_apply_skills(uw_motion *m, int from, const uint8_t *list) {
    int i, n = 0;
    for (i = from; i < 6; i++)
        if (list[i] < 0x14) { player_raise_skill(m, list[i]); n++; }
    return n;
}

/* chargen_run_steps' record writes (its eight cases): sex into +0x64 bit 1, handedness into bit 0, the class
 * into bits 5..7 with chargen_roll_attributes and the skills -- the given
 * ones granted, each choice's skill into the list and granted in turn --
 * the portrait into bits 2..4, the difficulty into +0xb4, the name copied
 * to +0 (29 bytes) and +0x1d cleared. */
static bool chargen_result(uw_motion *m, const char *dir, const uw_character *ch) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    uw_skills sk;
    char path[768];
    uint8_t list[6];
    const uint8_t *offer = NULL;
    int pos = 0, applied = 0, offered = 0, choice = 0;
    snprintf(path, sizeof path, "%s/DATA/SKILLS.DAT", dir);
    if (!uw_skills_open(&sk, path)) return false;
    memset(list, 0x14, sizeof list);
    ds[rec + 0x64] = (uint8_t)((ds[rec + 0x64] & 0xfd) | ((ch->sex & 1) << 1));
    ds[rec + 0x64] = (uint8_t)((ds[rec + 0x64] & 0xfe) | (ch->handedness & 1));
    ds[rec + 0x64] = (uint8_t)((ds[rec + 0x64] & 0x1f) | ((ch->cls & 7) << 5));
    uw_chargen_roll_attributes(m, &sk);
    while (uw_chargen_skill_choices(m, &sk, &pos, list, &offer, &offered)) {
        int c = choice < 5 ? ch->skill[choice] : 0;
        if (c < 0 || c >= offered) c = 0;
        list[pos - 1] = offer[c];
        choice++;
        applied += uw_chargen_apply_skills(m, applied, list);
    }
    applied += uw_chargen_apply_skills(m, applied, list);
    ds[rec + 0x64] = (uint8_t)((ds[rec + 0x64] & 0xe3) | ((ch->portrait & 7) << 2));
    ds[rec + 0xb4] = (uint8_t)ch->difficulty;
    if (ch->name[0]) {
        size_t n = strlen(ch->name);
        memset(ds + rec, 0, 0x1d);
        memcpy(ds + rec, ch->name, n < 0x1d ? n : 0x1d);
    }
    ds[rec + 0x1d] = 0;                     /* whether or not a name was typed */
    uw_chargen_keep(m);
    uw_skills_close(&sk);
    return true;
}

/* ---- the boot --------------------------------------------------------------- */

bool uw_boot_new_game_begin(uw_boot *b, uw_motion *m, uw_scene *scene, const char *dir,
                            uint32_t clock, const char *ark_path) {
    uint8_t *ds = b->ds;
    uw_blob bl;
    char path[768];
    uint16_t tracked;
    int i, program = b->program;

    /* the record cleared -- but not which boot it is, which the caller set */
    memset(b, 0, sizeof *b);
    b->program = program;
    snprintf(b->dir, sizeof b->dir, "%s", dir);
    memset(m, 0, sizeof *m);
    m->ds = ds;
    m->lseg = b->lseg;
    m->keys = b->keys;
    m->ext = b->ext;
    m->grid = b->grid;
    m->palette = b->palette;
    m->elem = b->elem;
    m->screen = b->screen;
    m->screen_written = b->written;
    m->vram = b->vram;
    m->imgheap = b->imgheap;
    m->clock = clock;

    /* ---- the executable's image and the loaders ---- */
    if (!ds_image(ds, dir)) return false;
    {
        /* the key segment: 6624's image, key_char_table at 0x10, the
         * driver's modifier flags at 0x1e2..0x1e8 and the state array at
         * 0x1eb clear */
        uw_blob exe = file_at(dir, "", "UW.EXE");
        size_t hdr = exe.data ? (size_t)(exe.data[8] | exe.data[9] << 8) * 16u : 0;
        if (!exe.data || hdr + (0x6624 - 0x1000) * 16u + 0x10000 > exe.size) { uw_free(&exe); return false; }
        memcpy(b->keys, exe.data + hdr + (0x6624 - 0x1000) * 16u, 0x10000);
        memset(b->keys + 0x1e2, 0, 0x1eb + 0x80 - 0x1e2);
        /* the pathfinder's segment, as far as the load image goes (BSS past
         * it): atk_charge's wind-up table at +0x3c0 (50..255, the scale a
         * creature's blow takes) */
        {
            size_t pages = (size_t)(exe.data[4] | exe.data[5] << 8), last = (size_t)(exe.data[2] | exe.data[3] << 8);
            size_t image = pages * 512u - (last ? 512u - last : 0) - hdr, off = (0x65dd - 0x1000) * 16u;
            size_t n = image > off ? image - off : 0;
            if (n > 0x10000) n = 0x10000;
            if (hdr + off + n > exe.size) { uw_free(&exe); return false; }
            memcpy(b->ext, exe.data + hdr + off, n);
        }
        uw_free(&exe);
    }
    if (!obj_properties_load(ds, dir)) return false;
    /* COMOBJ.DAT, ALLPALS.DAT, the animation properties, gr_load_all's
     * texture base and drawlist_reset's labels: uw_scene_open's writes */
    if (!scene || scene->level_no < -1) return false;
    memcpy(ds + 0x5b6e, scene->ds + 0x5b6e, 0x1600);
    memcpy(ds + 0x573e, scene->ds + 0x573e, 0x200);
    memcpy(ds + 0x3658, scene->ds + 0x3658, 0x40);
    memcpy(ds + 0x7856, scene->ds + 0x7856, 0x140);
    memcpy(ds + 0x15e2, scene->ds + 0x15e2, 2);
    /* cmb_load: CMB.DAT into the recipe table */
    bl = file_at(dir, "DATA/", "CMB.DAT");
    if (bl.data) memcpy(ds + 0x48c6, bl.data, bl.size < 0x60 ? bl.size : 0x60);
    uw_free(&bl);
    /* the art, its sizes, the fonts, the strings, the shades */
    snprintf(path, sizeof path, "%s/DATA", dir);
    uw_art_load(path);
    m->art = uw_art;
    m->gr_file = uw_gr_file;
    m->gr_entry = uw_gr_entry;
    {
        static const char *const f1000[3] = { "BUTTONS.GR", "CURSORS.GR", "3DWIN.GR" };
        static const char *const fobj[1] = { "OBJECTS.GR" };
        uint16_t n = 0, k;
        int f;
        for (f = 0; f < 4; f++) {
            uw_blob g = file_at(dir, "DATA/", f < 3 ? f1000[f] : fobj[0]);
            uint16_t count;
            if (!g.data || g.size < 3) { uw_free(&g); return false; }
            if (f == 3) { m->art_size_count = n; n = 0; }
            count = (uint16_t)(g.data[1] | g.data[2] << 8);
            for (k = 0; k < count && n < 0x200; k++, n++) {
                size_t at = 3 + (size_t)k * 4, off;
                uint8_t *out = f < 3 ? b->art_size : b->obj_art_size;
                if (at + 4 > g.size) break;
                off = (size_t)(g.data[at] | g.data[at + 1] << 8 | g.data[at + 2] << 16 | (uint32_t)g.data[at + 3] << 24);
                out[n * 2] = off + 3 <= g.size ? g.data[off + 1] : 0;
                out[n * 2 + 1] = off + 3 <= g.size ? g.data[off + 2] : 0;
            }
            uw_free(&g);
        }
        m->obj_art_size_count = n;
        m->art_size = b->art_size;
        m->obj_art_size = b->obj_art_size;
    }
    b->shades = file_at(dir, "DATA/", "SHADES.DAT");
    m->shades = b->shades.data && b->shades.size >= 96 ? b->shades.data : NULL;
    b->grave = file_at(dir, "DATA/", "GRAVE.DAT");
    m->grave = b->grave.data; m->grave_size = b->grave.size;
    b->weapons_dat = file_at(dir, "DATA/", "WEAPONS.DAT");
    m->weapons_dat = b->weapons_dat.data; m->weapons_dat_size = b->weapons_dat.size;
    b->weapons_gr = file_at(dir, "DATA/", "WEAPONS.GR");
    m->weapons_gr = b->weapons_gr.data; m->weapons_gr_size = b->weapons_gr.size;
    b->weapons_cm = file_at(dir, "DATA/", "WEAPONS.CM");
    m->weapons_cm = b->weapons_cm.data; m->weapons_cm_size = b->weapons_cm.size;
    b->font_small = file_at(dir, "DATA/", "FONT4X5P.SYS");
    m->font_small = b->font_small.data;
    m->font_small_size = b->font_small.size;
    b->font = file_at(dir, "DATA/", "FONT5X6P.SYS");
    m->font = b->font.data;
    m->font_size = b->font.size;
    b->font_italic = file_at(dir, "DATA/", "FONT5X6I.SYS");
    m->font_italic = b->font_italic.data;
    m->font_italic_size = b->font_italic.size;
    snprintf(path, sizeof path, "%s/DATA/STRINGS.PAK", dir);
    b->have_strings = uw_strings_open(&b->strings, path);
    if (b->have_strings) {
        m->strings = &b->strings;
        b->scroll.m = m;
        b->scroll.font = m->font;
        b->scroll.font_size = m->font_size;
        if (m->font) m->scroll = &b->scroll;
        /* scroll_reset: the message scroll current, its mode
         * cleared; and the escapes on, as the running game has them */
        uw_scroll_select_scroll(&b->scroll);
        ds[0x0a98] = 3;
    }
    b->terrain = file_at(dir, "DATA/", "TERRAIN.DAT");
    if (ark_path) snprintf(path, sizeof path, "%s", ark_path);
    else snprintf(path, sizeof path, "%s/DATA/LEV.ARK", dir);
    b->have_ark = uw_ark_open(&b->ark, path);
    if (!b->have_ark) return false;
    /* palette_load(0): PALS.DAT's first */
    bl = file_at(dir, "DATA/", "PALS.DAT");
    if (bl.data && bl.size >= 768) memcpy(b->palette, bl.data, 768);
    uw_free(&bl);

    /* ---- the runtime's state the loaders and the mode set ---- */
    /* the stack, above the near heap's last block: cleared, where the
     * image holds the overlays' bytes */
    memset(ds + 0x8480, 0, 0x9800 - 0x8480);
    input_tables(ds);
    /* imgheap_init: the save areas from plane address 0x7dd0 up
     * to the end of video memory, no records; cursor_init's two save areas
     * (gfx_colour_mode_areas 0x100 and 0x101) below them */
    ww(b->imgheap, 0x08, 0x100);
    ww(b->imgheap, 0x0c, 0x7dd0);
    ww(b->imgheap, 0x0e, 0xffff);
    ww(b->imgheap, 0x10, 0x7dd0);
    ww(b->imgheap, 0x12, 0x14);
    m->cursor_areas[0] = m->cursor_areas[1] = 0x7bd0;
    m->farheap[0] = 0x6c2c;
    m->farheap[1] = 0x7f5e;
    m->farheap[2] = 0;
    m->farheap_given = 1;
    /* the C runtime's words farmalloc's growth reads (rt_brk_far): heapbase
     * (the end of the loaded image), heaptop, the break past the level
     * buffer, the PSP at 0x814 and the 0x1fd 64-paragraph chunks DOS has
     * given -- a running game's */
    ww(ds, 0x00a0, 0); ww(ds, 0x00a2, 0x6c2c);      /* heapbase */
    ww(ds, 0x00a4, 0); ww(ds, 0x00a6, 0x871f);      /* brklvl */
    ww(ds, 0x00a8, 0); ww(ds, 0x00aa, 0x9fff);      /* heaptop */
    ww(ds, 0x0092, 0x0814);                          /* the PSP's segment */
    ww(ds, 0x1f0e, 0x01fd);                          /* DOS's chunks */
    /* game_set_mode(1), parse_command_line's mode state, game_main's run flag */
    ww(ds, (uint16_t)(rw(ds, 0x00e2) + 8), 1);
    ww(ds, 0x565e, 1);
    ww(ds, 0x5664, 0);
    ww(ds, 0x5666, 1);
    ww(ds, 0x1a30, 0);
    /* level_buffer_alloc and dungeon_mode_setup: the level
     * segment's pointers, the Avatar as the tracked object and the camera's,
     * the level, the player record and its critter row */
    ww(ds, TILEMAP_PTR, 0x0004); ww(ds, (uint16_t)(TILEMAP_PTR + 2), LEVEL_SEG);
    ww(ds, MOBILE_BASE, 0x4004); ww(ds, (uint16_t)(MOBILE_BASE + 2), LEVEL_SEG);
    ww(ds, STATIC_BASE, 0x5b04); ww(ds, (uint16_t)(STATIC_BASE + 2), LEVEL_SEG);
    ww(ds, TRACKED_OBJECT, 0x401f); ww(ds, (uint16_t)(TRACKED_OBJECT + 2), LEVEL_SEG);
    ww(ds, 0x2e10, 0x401f); ww(ds, 0x2e12, LEVEL_SEG);
    ww(ds, TRACKED_TILE, 0xffff);
    ww(ds, 0x727c, 0);
    ww(ds, PLAYER_HEADING, 0);
    ww(ds, 0x3588, 0);
    ww(ds, 0x358a, 0);
    ww(ds, CURRENT_LEVEL_WORD, 1);
    ds[0x27a4] = 8;
    ww(ds, 0x27a0, 1);
    ww(ds, 0x2856, 0x0bb3);
    ww(ds, 0x2858, 0x2161);
    ww(ds, 0x2850, 0x1100);
    ww(ds, 0x284e, 0);
    ds[0x1c50] = 0;                          /* shade_set_level(0) */
    ww(ds, 0x1b37, 0);
    ww(ds, 0x1b37, 7);                       /* and view_set_viewport's bind at the first
                                              * dungeon_draw_main_screen: the view's hotspot, 7 */
    ww(ds, PLAYER_RECORD_PTR, 0x7288);
    tracked = rw(ds, TRACKED_OBJECT);
    ww(ds, CRITTER_ROW_PTR, (uint16_t)((0x7f & 0x3f) * 0x30 + 0x4a52));
    /* motion_filters_init: the four descriptors -- masks and a
     * far callback each -- and the four motion blocks' +0xc..+0xf cleared
     * and +0x17 stamped */
    {
        static const uint8_t desc[4][12] = {
            { 0x00, 0x00, 0x30, 0x1f, 0x10, 0x10, 0x20, 0x00, 0x31, 0x04, 0xae, 0x1a },
            { 0x00, 0x10, 0x00, 0x07, 0x80, 0x00, 0x00, 0x00, 0xfe, 0x05, 0xae, 0x1a },
            { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf7, 0x05, 0xae, 0x1a },
            { 0x10, 0x00, 0x28, 0x17, 0xa8, 0x10, 0x00, 0x00, 0x5e, 0x06, 0xae, 0x1a },
        };
        for (i = 0; i < 4; i++) {
            uint16_t blk = (uint16_t)(0x27a8 + i * 0x28);
            memcpy(ds + 0x285c + i * 12, desc[i], 12);
            memset(ds + blk + 0xc, 0, 4);
            ds[blk + 0x17] = (uint8_t)(i == 2 ? 0 : 0x80);
        }
        ww(ds, 0x285a, 0x285c);              /* motion_filter: the walker's */
    }
    /* view_set_viewport(0x34, 0xb4, 0xac, 0x71): the cursor's
     * view rectangle, the view's pixel geometry and its corner regions */
    ww(ds, CURSOR_VIEW_X, 0x34); ww(ds, (uint16_t)(CURSOR_VIEW_X + 2), 0xb4);
    ww(ds, (uint16_t)(CURSOR_VIEW_X + 4), 0xac); ww(ds, (uint16_t)(CURSOR_VIEW_X + 6), 0x71);
    ww(ds, 0x7282, 0x34); ww(ds, 0x7280, 0x71); ww(ds, 0x7364, 0xac); ww(ds, 0x2e08, 0xac);
    ww(ds, 0x727e, 5); ww(ds, 0x7286, 3); ww(ds, 0x7360, 6); ww(ds, 0x7362, 2); ww(ds, 0x7366, 1); ww(ds, 0x7368, 4);
    /* and its tail: the dungeon's mode mask has bit 0 and not bit 3, so
     * view_viewport_dirty is 1 and view_projection_scale 25000 -- the flag
     * view_present composites the first-person weapon by */
    ds[VIEW_VIEWPORT_DIRTY] = 1;
    ww(ds, 0x726c, 0x30);                    /* wall_texture_count (texture_system_init) */
    cursor_init(m);

    /* ---- the level: SAVE0's LEV.ARK loaded, the Avatar placed ---- */
    /* obj_pool_init: the pools over the level block -- the
     * mobile free stack at tilemap + 0x7300..0x74fa, the static one at
     * 0x74fc..0x7afa, the active mobile list at the static stack's top + 2
     * (segment offset 0x7b00, a byte an index) -- whose stack pointers
     * level_block_load then sets from the block's trailer. */
    {
        static const uint16_t ptrs[][2] = {
            { 0x2752, 0x7304 }, { 0x274e, 0x74fe }, { 0x2746, 0x7500 }, { 0x2742, 0x7afe },
            { 0x273c, 0x7b00 }, { 0x2756, 0x7304 }, { 0x274a, 0x7500 }, { 0x2732, 0x7b00 },
        };
        for (i = 0; i < (int)(sizeof ptrs / sizeof ptrs[0]); i++) {
            ww(ds, ptrs[i][0], ptrs[i][1]);
            ww(ds, (uint16_t)(ptrs[i][0] + 2), LEVEL_SEG);
        }
    }
    ww(ds, ACTION_STATE_WORD, 0);
    /* load_uw_cfg: DATA/UW.CFG's two lines into the driver ids
     * and their hardware triples (snd_read_cfg) */
    {
        uw_config cfg;
        int k;
        uw_boot_read_config(dir, &cfg);
        ds[0x013a] = (uint8_t)cfg.music;
        ds[0x013b] = (uint8_t)cfg.digital;
        for (k = 0; k < 3; k++) {
            ww(ds, (uint16_t)(0x25d6 + k * 2), (uint16_t)cfg.music_hw[k]);
            ww(ds, (uint16_t)(0x2600 + k * 2), (uint16_t)cfg.digital_hw[k]);
        }
    }
    /* sound_init with no driver: nothing available, nothing
     * enabled -- play_sound_effect returns 0xff at its gate, and
     * sound_update returns at its first line. game_init's, so before
     * anything ticks (the title's cutscene re-enters the dungeon mode).
     * The host attaches its driver after (uw_motion_sound_attach). */
    memset(ds + 0x0135, 0, 4);
    if (!uw_motion_level_load(m, &b->ark, 1, b->terrain.data, b->terrain.size)) return false;
    /* player_object_reset: the Avatar's record cleared, its
     * whoami 0xfd, its id 0x7f */
    {
        uint8_t *ls = b->lseg;
        ww(ls, (uint16_t)(tracked + 6), (uint16_t)(rw(ls, (uint16_t)(tracked + 6)) & 0x3f));
        ls[tracked + 0x1a] = 0xfd;
        ww(ls, tracked, (uint16_t)((rw(ls, tracked) & 0x1fff) | 0x2000));
        ww(ls, (uint16_t)(tracked + 2), (uint16_t)(rw(ls, (uint16_t)(tracked + 2)) & 0xfc7f));
        ls[tracked + 0x18] &= 0xe0;
        ww(ls, (uint16_t)(tracked + 4), (uint16_t)(rw(ls, (uint16_t)(tracked + 4)) & 0x3f));
        ls[tracked + 0x11] = 0;
        ww(ls, tracked, (uint16_t)((rw(ls, tracked) & 0xfe00) | 0x7f));
        ls[tracked + 4] = ds[rw(ds, CRITTER_ROW_PTR) + 4];
    }
    /* new_game: player_init(1), and the character generation --
     * the screen (src/uw_chargen_ui.c) or an answer sheet -- between the
     * halves. The program's own boot is game_init's instead:
     * player_init(0), the skills and attributes rolled, some 69 draws of
     * rt_rand before the title -- new_game's player_init(1) comes later,
     * when the menu creates a character. Before them, init_world's first
     * call, init_mem, asks EMS for (rand() % 8) * 2 + 0x36 pages: one draw
     * (seventy in all; menu1's states hold the seventieth state) */
    if (b->program) (void)rt_rand(m);
    player_init(m, b->program ? 0 : 1);
    (void)clock;
    return true;
}

void uw_boot_player_init(uw_motion *m) { player_init(m, 1); }

bool uw_boot_go_to_level(uw_boot *b, uw_motion *m, uw_scene *scene, int level, int x, int y) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    if (level < 1 || level > 9) return false;
    if (!uw_motion_level_load(m, &b->ark, level, b->terrain.data, b->terrain.size)) return false;
    ww(ds, CURRENT_LEVEL_WORD, (uint16_t)level);
    ww(ds, (uint16_t)(rec + 0x5c), (uint16_t)level);
    place_player_in_tile(m, (int16_t)x, (int16_t)y, 0x9500);
    ww(ds, (uint16_t)(rec + 0x54), rw(ds, PLAYER_X));
    ww(ds, (uint16_t)(rec + 0x56), rw(ds, PLAYER_Y));
    ww(ds, (uint16_t)(rec + 0x58), rw(ds, PLAYER_Z));
    uw_boot_level_scene(b, m, scene);
    return true;
}

void uw_boot_level_scene(uw_boot *b, uw_motion *m, uw_scene *scene) {
    int level = (int16_t)rw(m->ds, 0x7278) - 1;
    if (level < 0) level = 0;
    (void)uw_scene_level(scene, level, b->lseg + 4, 0x7c08);
    memcpy(m->ds + 0x7192, scene->ds + 0x7192, 0x16);
}

bool uw_boot_apply_character(uw_motion *m, const char *dir, const uw_character *ch) {
    static const uw_character fighter = { 0, 1, 0, 0, 0, { 0, 0, 0, 0, 0 }, "AVATAR" };
    return chargen_result(m, dir, ch ? ch : &fighter);
}

/* weapons_load_anim for the ready weapon (weapon_anim_wanted,
 * 0x0793): WEAPONS.DAT's two 0x1c-byte halves for the hand and the weapon
 * into 0x35aa and 0x35c6, the frame group offsets, the weapon loaded; the
 * frames themselves the port takes from WEAPONS.GR as it draws. And
 * weapons_load_colourmap: WEAPONS.CM's sixteen entries at
 * 0x591e, the second map for appearance 1. */
void uw_boot_weapons_load(uw_motion *m, const char *dir) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    uint8_t weapon = ds[0x0793];
    uw_blob dat = file_at(dir, "DATA/", "WEAPONS.DAT"), cm = file_at(dir, "DATA/", "WEAPONS.CM");
    size_t at = (size_t)(1 - (ds[rec + 0x64] & 1)) * 0xe0 + (size_t)(weapon & 3) * 0x38;
    int i;
    if (dat.data && at + 0x38 <= dat.size) {
        memcpy(ds + 0x35aa, dat.data + at, 0x1c);
        memcpy(ds + 0x35c6, dat.data + at + 0x1c, 0x1c);
    }
    for (i = 0; i < 5; i++) ww(ds, (uint16_t)(0x35ec + i * 2), (uint16_t)(i * 8));
    ds[0x0794] = weapon;
    at = ((ds[rec + 0x64] >> 2) & 7) == 1 ? 0x10 : 0;
    if (cm.data && at + 0x10 <= cm.size) memcpy(ds + 0x591e, cm.data + at, 0x10);
    uw_free(&dat);
    uw_free(&cm);
}

bool uw_boot_new_game_finish(uw_boot *b, uw_motion *m, uw_scene *scene, const char *dir,
                             uint32_t clock, int start_x, int start_y) {
    uint8_t *ds = b->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    player_state_recalc(m);
    if (start_x < 0 || start_y < 0 || start_x > 63 || start_y > 63) { start_x = 32; start_y = 2; }
    place_player_in_tile(m, (int16_t)start_x, (int16_t)start_y, 0x9500);
    ww(ds, PLAYER_HEADING, 0);
    ww(ds, (uint16_t)(rec + 0x54), rw(ds, PLAYER_X));
    ww(ds, (uint16_t)(rec + 0x56), rw(ds, PLAYER_Y));
    ww(ds, (uint16_t)(rec + 0x58), rw(ds, PLAYER_Z));
    ww(ds, (uint16_t)(rec + 0x5a), 0);
    ww(ds, (uint16_t)(rec + 0x5c), 1);
    level_transition_effects(m, 1, 0, 0x9500);
    uw_boot_level_scene(b, m, scene);

    uw_boot_weapons_load(m, dir);

    /* ---- the clock, the mode's tail ---- */
    ww(ds, 0x0774, (uint16_t)clock);
    ww(ds, 0x0776, (uint16_t)(clock >> 16));
    ww(ds, LAST_TICK_TIME, (uint16_t)(clock >> 8));
    ww(ds, (uint16_t)(LAST_TICK_TIME + 2), (uint16_t)(clock >> 24));
    ds[TICK_ACCUMULATOR] = 0;
    apply_movement_mode(m, -1);
    ds[0x00d3] = 1;

    /* ---- the screen: dungeon_draw_main_screen and
     * dungeon_mode_enter ---- */
    m->elem_any_dirty = 0;
    uw_motion_tick_update(m, clock, 0);
    uw_motion_cursor_hide(m);               /* dungeon_draw_main_screen's */
    if (!uw_boot_draw_main_screen(m, dir)) return false;
    m->not_carried = 0;
    m->pixels_not_drawn = 0;
    return true;
}

bool uw_boot_new_game(uw_boot *b, uw_motion *m, uw_scene *scene, const char *dir, const uw_character *ch,
                      uint32_t clock, const char *ark_path, int start_x, int start_y) {
    if (!uw_boot_new_game_begin(b, m, scene, dir, clock, ark_path)) return false;
    if (!uw_boot_apply_character(m, dir, ch)) return false;
    return uw_boot_new_game_finish(b, m, scene, dir, clock, start_x, start_y);
}

bool uw_boot_read_config(const char *dir, uw_config *c) {
    uw_blob bl = file_at(dir, "DATA/", "UW.CFG");
    const char *p;
    int line;
    memset(c, 0, sizeof *c);
    c->music_hw[0] = c->music_hw[1] = c->music_hw[2] = -1;
    c->digital_hw[0] = c->digital_hw[1] = c->digital_hw[2] = -1;
    if (!bl.data) return false;
    p = (const char *)bl.data;
    for (line = 0; line < 2 && p < (const char *)bl.data + bl.size; line++) {
        char buf[100];
        size_t n = 0;
        while (p < (const char *)bl.data + bl.size && *p != '\n' && n < sizeof buf - 1) buf[n++] = *p++;
        buf[n] = 0;
        if (p < (const char *)bl.data + bl.size && *p == '\n') p++;
        if (line == 0) sscanf(buf, "%d %d %x %d", &c->music, &c->music_hw[0], (unsigned *)&c->music_hw[1], &c->music_hw[2]);
        else sscanf(buf, "%d %d %x %d", &c->digital, &c->digital_hw[0], (unsigned *)&c->digital_hw[1], &c->digital_hw[2]);
    }
    uw_free(&bl);
    return true;
}

bool uw_boot_draw_main_screen(uw_motion *m, const char *dir) {
    uint8_t *ds = m->ds;
    uw_blob bl = file_at(dir, "DATA/", "MAIN.BYT");
    if (!m->screen || !bl.data || bl.size < 64000) { uw_free(&bl); return false; }
    /* the cursor is off the screen while the page is replaced: the caller's
     * cursor_hide, dungeon_draw_main_screen's first call, balanced by the
     * cursor_show below */
    memcpy(m->screen, bl.data, 64000);
    uw_free(&bl);
    panel_build_elements(m);
    /* inventory_panel_init, which dungeon_mode_enter runs once
     * with the bare panel on the page: the elements' backgrounds, which
     * every restore over the panel puts back */
    uw_motion_inventory_panel_init(m);
    {
        /* the mode panel's image at (0xec, 0xc0), 0x72 rows of 0x53:
         * PANELS.GR's entries are RAW pixels with no header, so they come
         * through gr_load_to_buffer's reader and the caller says the size
         * (MAIN.BYT already carries the panel) */
        const uint8_t *px = uw_gr_entry(NULL, "PANELS", ds[PANEL_MODE], 0x53 * 0x72);
        if (px) uw_motion_blit(m, px, 0x53, 0xec, 0xc0, 0x72, 0x53);
        else m->pixels_not_drawn++;
    }
    uw_motion_dungeon_refresh_inventory(m);
    /* scroll_reset's text_window_fill: the scroll's parchment */
    if (m->scroll) {
        uw_scroll_select_scroll(m->scroll);
        uw_scroll_clear(m->scroll, 0);
    }
    elem_flush(m);
    uw_motion_cursor_show(m);
    ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 0x7dfe));
    return true;
}
