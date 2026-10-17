/* SPDX-License-Identifier: MIT */
/* THE RIGHT-HAND PANEL'S OTHER TWO VIEWS: the rune bag and the character's
 * stats, and the flip that turns the panel over to them --
 * panel_view_switch_step on the path it takes when the three EMS scratch
 * pages could not be had, which is the path a port without them is on.
 *
 * WHICH VIEW IS SHOWN is panel_values[6], the element whose drawer the
 * flip is: opening the rune bag (a container whose id nibble is 0xf) asks
 * for view 1, and the click between the flasks asks for view 2 or back to
 * 0 (panel_set_mode_icon). panel_mode then says whose click it is --
 * inventory_click_dispatch, rune_bag_click or stats_skill_scroll_click --
 * which is what panel_inventory_click switches on.
 *
 * THE FLIP is eight frames, one a redraw, a card turning about its
 * vertical axis. With the three EMS scratch pages (the host's
 * uw_motion.panel_ems) the begin draws the new view off the screen and
 * keeps it, and each frame draws the card scaled: the old panel narrowing
 * over frames 1..3, edge-on at 4, the new one widening over 5..7, whole at
 * 8. Without them it is three of the eight: 3 paints the panel over in
 * 0xf1 with the two hinge pictures and the edge-on card, 6 draws the new
 * view's background and runs its drawer, 8 ends it. */
#include "uw_motion_int.h"

#include <stdio.h>
#include <string.h>

enum {
    SKILL_SCROLL   = 0x1d5c,   /* stats_skill_scroll: the six-row window's top */
    ORDINALS       = 0x1d5d,   /* "ST", "ND", "RD", "TH", three bytes each */
    SKILL_BACKDROP = 0x1d69,   /* stats_skill_backdrop: the rows' saved strip */
    SMALL_BACKDROP = 0x1d6b,
    FLIP_FRAME     = 0x08a2,   /* panel_flip_frame, 1..8 */
    FLIP_FLAGS     = 0x08a8,   /* bit 0: the EMS pages were allocated */
    FLIP_TARGET    = 0x35a8,   /* panel_flip_target_mode */
    /* the flip's rectangle as panel_view_switch_begin keeps it -- "h" is
     * the width in columns, "w" the rows */
    PANEL_X        = 0x35a2, PANEL_Y = 0x35a6, PANEL_H = 0x3632, PANEL_W = 0x35a4,
    FLIP_W         = 0x35a0,   /* panel_flip_w: a column's rows as the card scales */
    FLIP_H         = 0x3626,   /* panel_flip_h: the scaled card's columns */
    FLIP_COLUMN    = 0x3634,   /* panel_flip_column: the rows left blank above and below */
    WIDTH_PCT      = 0x087c,   /* panel_flip_width_pct, eight words */
    HEIGHT_PCT     = 0x088c,   /* panel_flip_height_pct, eight words */
    EMS_PAGES      = 0x089c,   /* the three handles: work, new, old */
    PAGES_TRIED    = 0x094a,   /* panel_ems_pages_tried */
    SWITCH_DIR     = 0x18a4,   /* paperdoll_flip_page */
    SCRATCH_CURSOR = 0x010c,   /* ems_scratch_page_cursor */
    STATS_RIGHT    = 0x138,    /* every number on the stats panel is right-aligned here */
    STATS_LEFT     = 0xf2
};

/* ---- the stats panel -------------------------------------------------- */

static void panel_text(uw_motion *m, const uint8_t *font, size_t size, const char *s, int x, int y,
                       uint8_t colour) {
    if (!font) { UW_NOT_CARRIED(m->not_carried); return; }
    uw_motion_draw_string(m, font, size, s, x, y, colour);
}

static int panel_width(uw_motion *m, const uint8_t *font, size_t size, const char *s) {
    if (!font) { UW_NOT_CARRIED(m->not_carried); return 0; }
    return uw_motion_string_width(font, size, s);
}

static void upper(char *s) {
    for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 0x20);
}

static void ds_string(uw_motion *m, uint16_t id, char *out, size_t cap) {
    out[0] = 0;
    if (m->strings) uw_strings_by_id(m->strings, id, out, (int)cap);
    else UW_NOT_CARRIED(m->not_carried);
}

/* stats_draw_name_class_level: the player's name upper-cased and
 * centred over a field of 0x48 from 0xf2, the class -- block 2 string 0x17 +
 * the record's class field -- upper-cased at 0xf2, and the level with its
 * ordinal suffix right-aligned, all in the italic font. */
static void stats_draw_name_class_level(uw_motion *m, uint8_t pen) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    const uint8_t *f = m->font_italic;
    size_t fs = m->font_italic_size;
    char buf[0x28], part[0x28];
    int lvl, k;
    memcpy(buf, ds + rec, 0xf);
    buf[0xf] = 0;
    upper(buf);
    panel_text(m, f, fs, buf, STATS_LEFT + (0x48 - panel_width(m, f, fs, buf)) / 2, 0xb9, pen);
    ds_string(m, (uint16_t)(0x400 | (0x17 + ((ds[(uint16_t)(rec + 0x64)] >> 5) & 7))), buf, sizeof buf);
    upper(buf);
    panel_text(m, f, fs, buf, STATS_LEFT, 0xb2, pen);
    lvl = ds[(uint16_t)(rec + 0x3d)];
    itoa10(lvl, buf);
    k = lvl < 4 ? lvl - 1 : 3;
    if (k < 0) k = 0;
    snprintf(part, sizeof part, "%s", (const char *)(ds + ORDINALS + k * 3));
    strncat(buf, part, sizeof buf - strlen(buf) - 1);
    panel_text(m, f, fs, buf, STATS_RIGHT - panel_width(m, f, fs, buf), 0xb2, pen);
}

/* stats_draw_attribute(n): strength, dexterity and intelligence
 * from the critter row's +5, +6 and +7, seven pixels apart from 0x9d
 * upward. */
static void stats_draw_attribute(uw_motion *m, int n, uint8_t pen) {
    uint8_t *ds = m->ds;
    char buf[0x10];
    itoa10(ds[(uint16_t)(rw(ds, CRITTER_ROW_PTR) + 5 + n)], buf);
    panel_text(m, m->font_italic, m->font_italic_size, buf,
               STATS_RIGHT - panel_width(m, m->font_italic, m->font_italic_size, buf),
               (2 - n) * 7 + 0x9d, pen);
}

/* stats_draw_hits and _mana: current over maximum with a
 * '/' between -- the hit points from the player OBJECT's +8 against the
 * critter row's +4, the mana from the record's +0x37 and +0x38. */
static void stats_draw_two(uw_motion *m, int cur, int max, int y, uint8_t pen) {
    char buf[0x18], part[0x10];
    itoa10(cur, buf);
    itoa10(max, part);
    strncat(buf, "/", sizeof buf - strlen(buf) - 1);
    strncat(buf, part, sizeof buf - strlen(buf) - 1);
    panel_text(m, m->font_italic, m->font_italic_size, buf,
               STATS_RIGHT - panel_width(m, m->font_italic, m->font_italic_size, buf), y, pen);
}

/* stats_draw_experience: the 32-bit total at +0x4e divided by
 * ten -- the game keeps a decimal place of headroom. */
static void stats_draw_experience(uw_motion *m, uint8_t pen) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    uint32_t xp = (uint32_t)rw(ds, (uint16_t)(rec + 0x4e)) | ((uint32_t)rw(ds, (uint16_t)(rec + 0x50)) << 16);
    char buf[0x18];
    snprintf(buf, sizeof buf, "%lu", (unsigned long)(xp / 10));
    panel_text(m, m->font_italic, m->font_italic_size, buf,
               STATS_RIGHT - panel_width(m, m->font_italic, m->font_italic_size, buf), 0x88, pen);
}

/* stats_draw_skill_row(row): the row's strip of backdrop put
 * back -- gfx_blit_planar's source rows row * 7 up to (row + 1) * 7, the
 * fifth argument the end and not a count, as the flip's new page in flip2
 * shows -- then the skill's name (block 2 string 0x1f + the scroll + the
 * row, upper-cased) at 0xf2 and its value from the record's +0x21 array,
 * seven pixels a row down from 0x80, in the italic font: stats_panel_draw
 * and stats_skill_scroll_click open font5x6i.sys before the rows and put
 * font5x6p.sys back only after them. */
static void stats_draw_skill_row(uw_motion *m, int row, uint8_t pen) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), h = rw(ds, SKILL_BACKDROP);
    int scroll = ds[SKILL_SCROLL], y = 0x80 - row * 7;
    char buf[0x28];
    if (h) imgbuf_restore_rows(m, h, 0xf0, y + 1, 0x4c, (row + 1) * 7, row * 7);
    else UW_NOT_CARRIED(m->not_carried);
    ds_string(m, (uint16_t)(0x400 | (0x1f + scroll + row)), buf, sizeof buf);
    upper(buf);
    panel_text(m, m->font_italic, m->font_italic_size, buf, STATS_LEFT, y, pen);
    itoa10(ds[(uint16_t)(rec + 0x21 + scroll + row)], buf);
    panel_text(m, m->font_italic, m->font_italic_size, buf,
               STATS_RIGHT - panel_width(m, m->font_italic, m->font_italic_size, buf), y, pen);
}

/* stats_panel_draw: the two saved images taken once, then the
 * header lines in the italic font with the pen at 0xf1 and the six skill
 * rows with it at 0x68. */
void stats_panel_draw(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), row = rw(ds, CRITTER_ROW_PTR), tracked = rw(ds, TRACKED_OBJECT);
    int n;
    if (!rw(ds, SKILL_BACKDROP)) {
        uint16_t h = imgbuf_alloc(m, 0x4b, 0x2a);
        ww(ds, SKILL_BACKDROP, h);
        if (h) imgbuf_capture_rect(m, h, 0xf0, 0x81, 0x4b, 0x2b);
        h = imgbuf_alloc(m, 0x23, 0x15);
        ww(ds, SMALL_BACKDROP, h);
        if (h) imgbuf_capture_rect(m, h, 0x115, 0x96, 0x23, 0x15);
    }
    cursor_hide(m);
    stats_draw_name_class_level(m, 0xf1);
    for (n = 0; n < 3; n++) stats_draw_attribute(m, n, 0xf1);
    stats_draw_two(m, m->lseg[(uint16_t)(tracked + 8)], ds[(uint16_t)(row + 4)], 0x96, 0xf1);
    stats_draw_two(m, ds[(uint16_t)(rec + 0x37)], ds[(uint16_t)(rec + 0x38)], 0x8f, 0xf1);
    stats_draw_experience(m, 0xf1);
    for (n = 0; n < 6; n++) stats_draw_skill_row(m, n, 0x68);
    cursor_show(m);
}

/* panels_refresh: the hits, mana and experience drawn again
 * when the stats panel is the one shown -- the small saved image behind
 * them put back first, which is the one stats_panel_draw takes and nothing
 * in its own overlay restores. */
void panels_refresh(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), row = rw(ds, CRITTER_ROW_PTR);
    uint16_t tracked = rw(ds, TRACKED_OBJECT);
    if (ds[PANEL_MODE] != 2) return;
    cursor_hide(m);
    if (rw(ds, SMALL_BACKDROP)) imgbuf_restore(m, rw(ds, SMALL_BACKDROP));
    else UW_NOT_CARRIED(m->not_carried);
    stats_draw_two(m, m->lseg[(uint16_t)(tracked + 8)], ds[(uint16_t)(row + 4)], 0x96, 0xf1);
    stats_draw_two(m, ds[(uint16_t)(rec + 0x37)], ds[(uint16_t)(rec + 0x38)], 0x8f, 0xf1);
    stats_draw_experience(m, 0xf1);
    cursor_show(m);
}

/* stats_skill_scroll_click: only the top eight rows of the
 * panel count -- x below 0x25 scrolls back, anything else forward, the
 * offset clamped to 0..0xe -- and a change redraws the six rows. It ends
 * on the release wait whatever it did, its click included, so one press
 * scrolls the list once however long it is held. */
void stats_skill_scroll_click(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t ev = rw(ds, 0x00e2);
    int dir, was = ds[SKILL_SCROLL], n;
    if (rs(ds, (uint16_t)(ev + 2)) < 8) {
        dir = rs(ds, ev) < 0x25 ? -1 : 1;
        if (dir < 0) { if (was > 0) ds[SKILL_SCROLL] = (uint8_t)(was - 1); }
        else if (was < 0xe) ds[SKILL_SCROLL] = (uint8_t)(was + 1);
        if (ds[SKILL_SCROLL] != was) {
            cursor_hide(m);
            for (n = 0; n < 6; n++) stats_draw_skill_row(m, n, 0x68);
            cursor_show(m);
        }
    }
    input_wait_button_release(m, 1);
}

/* ---- the rune bag ----------------------------------------------------- */

/* rune_bag_draw_one(n): rune n's stone, art 0xe8 + n, in a grid
 * of four columns 0x12 apart from 0xf4 and rows 0xf apart down from 0xbb. */
static void rune_bag_draw_one(uw_motion *m, int n) {
    cursor_hide(m);
    m->span_variant = 1;
    uw_motion_gr_draw_art(m, (uint16_t)(0xe8 + n), (n & 3) * 0x12 + 0xf4, 0xbb - (n >> 2) * 0xf);
    m->span_variant = 0;
    cursor_show(m);
}

/* rune_bag_draw: every rune the bag holds -- the record's
 * +0x44..+0x46, whose bits run most significant first. */
void rune_bag_draw(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    int n;
    cursor_hide(m);
    for (n = 0; n < 24; n++)
        if ((ds[(uint16_t)(rec + 0x44 + (n >> 3))] >> (7 - (n & 7))) & 1) rune_bag_draw_one(m, n);
    cursor_show(m);
}

/* rune_shelf_clear: the three shelf slots set to 0x18 -- past
 * the twenty-four runes, which is how a slot says empty -- the count in the
 * record's +0x60 bits 2..3 cleared, and the shelf drawn. */
void rune_shelf_clear(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    memset(ds + rec + 0x47, 0x18, 3);
    ds[(uint16_t)(rec + 0x60)] &= 0xf3;
    panel_draw_runes_at(m, (uint16_t)(rec + 0x47));
}

/* rune_bag_add(obj): a rune stone put into the bag -- item ids
 * 0xe8 through 0xe8 + 0x18 by the range test, twenty-five of them, one past
 * the twenty-four runes (the original's off-by-one) -- sets its bit in the
 * record's +0x44..+0x46, most significant first, and frees the object. 0
 * for anything else, which is what makes the caller say only runes go in
 * the rune bag. */
int rune_bag_add(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    int n = (rw(m->lseg, obj) & 0x1ff) - 0xe8;
    if (n < 0 || n > 0x18) return 0;
    uw_motion_obj_free(m, obj);
    ds[(uint16_t)(rec + 0x44 + (n >> 3))] |= (uint8_t)(1 << (7 - (n & 7)));
    return 1;
}

/* rune_bag_click: nothing at all while
 * action_state is set. Otherwise the click's position in the panel turned
 * into a rune -- four columns of 0x12 from 3, rows of 0xf from 0x12,
 * counting UP the panel, so the grid's bottom row is 20..23 -- and, for a
 * rune the bag holds, pushed onto the shelf, the oldest dropped when three
 * are already on it. THE SHELF IS EMPTIED FIRST IF A SPELL WAS CAST OFF IT
 * (a flag spell_cast_from_shelf sets), so a cast ends the
 * combination and the next rune begins a new one. A click below the grid
 * clears the shelf; with the event's second button the rune is looked at
 * instead. Either way the release wait, which is what keeps one press from
 * pushing a rune once a pass.
 *
 * The rune's index is not bounded: the rect the hotspot allows reaches 24,
 * whose bit is read out of the shelf's own first slot (+0x47) and is never
 * set, because a slot holds 0..0x18. The port reads it where the original
 * reads it rather than guarding, so the two agree by the same accident.
 *
 * Not carried: the look. The original builds its object on the frame --
 * word 0's item id over whatever the high bits held, +6 zeroed and
 * everything from +8 up, the quality word among it, left as the stack found
 * it -- and passes that to look_at, so what it prints past "a Lor stone"
 * is the caller's stack. There is nothing to reproduce. */
void rune_bag_click(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t ev = rw(ds, 0x00e2), rec = rw(ds, PLAYER_RECORD_PTR);
    int16_t x = rs(ds, ev), y = rs(ds, (uint16_t)(ev + 2));
    int n, count;
    if (rw(ds, ACTION_STATE_WORD)) return;
    if (y < 0x12) {
        rune_shelf_clear(m);
        input_wait_button_release(m, 1);
        return;
    }
    n = 0x14 - ((y - 0x12) / 0xf) * 4 + (x - 3) / 0x12;
    if (!((ds[(uint16_t)(rec + 0x44 + (n >> 3))] >> (7 - (n & 7))) & 1)) {
        input_wait_button_release(m, 1);
        return;
    }
    if (rw(ds, (uint16_t)(ev + 6)) & 2) {
        /* look_at of a rune stone the original builds on its stack: word 0
         * item 0xe8 + n over whatever the stack held, word 3's owner bits
         * cleared, and lore 0. The port makes it a free static object
         * with nothing else set -- the printed name, not the stack's
         * garbage -- and gives the slot back after. */
        uw_objpool pool;
        uint16_t o;
        pool_from_ds(m, &pool);
        o = uw_obj_alloc(&pool, 0);
        pool_to_ds(m, &pool);
        if (!o) UW_NOT_CARRIED(m->not_carried);
        else {
            uint8_t *ls = m->lseg;
            ww(ls, o, (uint16_t)((0xe8 + n) & 0x1ff));
            ww(ls, (uint16_t)(o + 2), 0);
            ww(ls, (uint16_t)(o + 4), 0);
            ww(ls, (uint16_t)(o + 6), 0);
            look_at(m, o, 0);
            pool_from_ds(m, &pool);
            uw_obj_free(&pool, o);
            pool_to_ds(m, &pool);
        }
        input_wait_button_release(m, 1);
        return;
    }
    if (ds[SHELF_SPELL_CAST]) rune_shelf_clear(m);
    ds[SHELF_SPELL_CAST] = 0;
    count = (ds[(uint16_t)(rec + 0x60)] >> 2) & 3;
    if (count >= 3) {
        ds[(uint16_t)(rec + 0x47)] = ds[(uint16_t)(rec + 0x48)];
        ds[(uint16_t)(rec + 0x48)] = ds[(uint16_t)(rec + 0x49)];
        count = 2;
    }
    ds[(uint16_t)(rec + 0x47 + count)] = (uint8_t)n;
    ds[(uint16_t)(rec + 0x60)] = (uint8_t)((ds[(uint16_t)(rec + 0x60)] & 0xf3) | ((count + 1) << 2));
    panel_draw_runes_at(m, (uint16_t)(rec + 0x47));
    input_wait_button_release(m, 1);
}

/* rune_bag_clear: eight bytes from the record's +0x44 zeroed --
 * the bag's three, the shelf's three and two more. */
void rune_bag_clear(uw_motion *m) {
    memset(m->ds + (uint16_t)(rw(m->ds, PLAYER_RECORD_PTR) + 0x44), 0, 8);
}

static void panel_background(uw_motion *m, int view);

/* panel_redraw: the panel's background
 * for panel_mode (PANELS.GR) and the mode's drawer through its table
 * -- the inventory, the rune bag or the stats -- on the back page,
 * then a fill in colour 0x106, which copies the drawn page to the shown
 * one; the port draws on the screen and has no copy to make. */
void panel_redraw(uw_motion *m) {
    uint8_t mode = m->ds[PANEL_MODE];
    cursor_hide(m);
    panel_background(m, mode);
    if (mode == 1) rune_bag_draw(m);
    else if (mode == 2) stats_panel_draw(m);
    else uw_motion_dungeon_refresh_inventory(m);
    cursor_show(m);
}

/* stats_panel_refresh: the WHOLE panel redrawn (panel_redraw)
 * when it is the stats panel -- what a level-up, a mantra and a night's
 * sleep call, the level line and the skill rows with the rest; the three
 * numbers alone are panels_refresh's. */
void stats_panel_refresh(uw_motion *m) {
    if (m->ds[PANEL_MODE] == 2) panel_redraw(m);
}

/* ---- the flip ----------------------------------------------------------- */

/* The panel's background: PANELS.GR's entry `view`, 83 x 114 raw pixels
 * with no header, at (0xec, 0xc0) -- what panel_build_elements and
 * panel_redraw blit for the view on show. */
static void panel_background(uw_motion *m, int view) {
    const uint8_t *px = m->gr_entry ? m->gr_entry(m->art_user, "PANELS", view, 0x53 * 0x72) : NULL;
    if (px) uw_motion_blit(m, px, 0x53, 0xec, 0xc0, 0x72, 0x53);
    else m->pixels_not_drawn++;
}

/* the drawer panel_redraw's table holds for a panel mode */
static void panel_mode_drawer(uw_motion *m, uint8_t mode) {
    if (mode == 1) rune_bag_draw(m);
    else if (mode == 2) stats_panel_draw(m);
    else uw_motion_dungeon_refresh_inventory(m);
}

/* ems_map_scratch_page(handle): the handle's page mapped into
 * the next of the frame's four physical pages -- the page cursor
 * turned -- and its address. The renderer's three window bytes it also
 * invalidates are the scene's, not here. `which` is 0, 1 or 2 for
 * the handle at 0x89c, 0x89e or 0x8a0. */
static uint8_t *ems_map_scratch_page(uw_motion *m, int which) {
    m->ds[SCRATCH_CURSOR] = (uint8_t)((m->ds[SCRATCH_CURSOR] + 1) & 3);
    return m->panel_ems + which * 0x4000;
}

/* vga_read_rect(dst, x, y, columns, rows): the screen's
 * rectangle into `dst` a row after another, the top row first. */
static void vga_read_rect(uw_motion *m, uint8_t *dst, int x, int y, int cols, int rows) {
    int i, j;
    for (j = 0; j < rows; j++)
        for (i = 0; i < cols; i++) {
            int sy = 199 - y + j, sx = x + i;
            dst[j * cols + i] = sy >= 0 && sy < 200 && sx >= 0 && sx < 320 ? m->screen[sy * 320 + sx] : 0;
        }
}

/* The pages are 0x4000 bytes and every offset the flip reaches is inside
 * one; a byte past it would be the next physical page's, and is dropped. */
#define PAGE_AT(p, o) ((unsigned)(o) < 0x4000u ? (p)[o] : 0)
#define PAGE_PUT(p, o, v) do { if ((unsigned)(o) < 0x4000u) (p)[o] = (uint8_t)(v); } while (0)

/* panel_blit_column_scaled(src, dst), from the instructions:
 * one column of the card. panel_flip_column blank rows, then the source's
 * panel_w rows as panel_flip_w: straight when they are equal; grown by
 * panel_flip_w - panel_w repeated rows, one after each run of panel_w /
 * (d + 1); shrunk by d skipped rows after runs of -(panel_w / (d - 1)) + 1
 * -- then the rest straight, the blank rows again and one more blank. The
 * source's rows are panel_h apart, the card's panel_flip_h. */
static void panel_blit_column_scaled(uw_motion *m, const uint8_t *src, int s, uint8_t *dst, int d_) {
    uint8_t *ds = m->ds;
    int16_t d = (int16_t)(rs(ds, FLIP_W) - rs(ds, PANEL_W)), cx, di, si, rest;
    int16_t col = rs(ds, FLIP_COLUMN), fh = rs(ds, FLIP_H), ph = rs(ds, PANEL_H), pw = rs(ds, PANEL_W);
    for (di = 0; di < col; di++) { PAGE_PUT(dst, d_, 0); d_ += fh; }
    if (d == 0) {
        for (di = 0; di < pw; di++) { PAGE_PUT(dst, d_, PAGE_AT(src, s)); d_ += fh; s += ph; }
        rest = 0;
    } else if (d > 0) {
        cx = (int16_t)(pw / (d + 1));
        for (di = 0; di < d; di++) {
            for (si = 0; si < cx; si++) { PAGE_PUT(dst, d_, PAGE_AT(src, s)); d_ += fh; s += ph; }
            PAGE_PUT(dst, d_, PAGE_AT(src, s));
            d_ += fh;
        }
        rest = (int16_t)(pw - d * cx);
    } else {
        cx = (int16_t)(pw / (d - 1) + 1);
        for (di = 0; di > d; di--) {
            for (si = 0; si > cx; si--) { PAGE_PUT(dst, d_, PAGE_AT(src, s)); d_ += fh; s += ph; }
            s += ph;
        }
        rest = (int16_t)(rs(ds, FLIP_W) - d * cx - 1);
    }
    while (rest-- > 0) { PAGE_PUT(dst, d_, PAGE_AT(src, s)); d_ += fh; s += ph; }
    for (di = 0; di < col; di++) { PAGE_PUT(dst, d_, 0); d_ += fh; }
    PAGE_PUT(dst, d_, 0);
}

/* panel_flip_scale_blit(src, dst, frame), from the
 * instructions: the card at this frame's size into `dst`. Its columns are
 * panel_h * height_pct / 100 (panel_flip_h), its near edge's rows panel_w *
 * width_pct / 100; the source's columns go over in runs with one skipped
 * after each (100 / height_pct of them past 50%, else one run a column and
 * that many skipped), the rest one for one. Down the columns a Bresenham
 * term narrows the card toward its far edge -- the blank rows one more and
 * the rows two fewer a step -- the left edge near for frames under 4, the
 * right from 4 on. panel_flip_w ends as the near edge's rows. */
static void panel_flip_scale_blit(uw_motion *m, const uint8_t *src, uint8_t *dst, int frame) {
    uint8_t *ds = m->ds;
    int16_t ph = rs(ds, PANEL_H), pw = rs(ds, PANEL_W), fh, near, skip, runs, run, err, e_keep, e_step, k, j;
    int s = 0, d = 0;
    fh = (int16_t)(ph * rs(ds, (uint16_t)(HEIGHT_PCT + frame * 2)) / 100);
    ww(ds, FLIP_H, (uint16_t)fh);
    near = (int16_t)(pw * rs(ds, (uint16_t)(WIDTH_PCT + frame * 2)) / 100);
    skip = rs(ds, (uint16_t)(HEIGHT_PCT + frame * 2)) ? (int16_t)(100 / rs(ds, (uint16_t)(HEIGHT_PCT + frame * 2))) : 0;
    if (skip == 1) {
        runs = (int16_t)(ph - fh);
        run = (int16_t)(ph / (runs + 1) - 1);
    } else {
        run = 1;
        runs = fh;
    }
    if (frame < 4) {
        ww(ds, FLIP_W, (uint16_t)near);
        e_keep = (int16_t)((pw - near) * 2);
        e_step = (int16_t)((pw - near + fh) * 2);
        err = (int16_t)((pw - near) * 2 + fh);
        ww(ds, FLIP_COLUMN, 0);
        for (k = 0; k < runs; k++) {
            for (j = 0; j < run; j++) {
                if (err > 0) err = (int16_t)(err + e_keep);
                else {
                    ww(ds, FLIP_COLUMN, (uint16_t)(rs(ds, FLIP_COLUMN) + 1));
                    ww(ds, FLIP_W, (uint16_t)(rs(ds, FLIP_W) - 2));
                    err = (int16_t)(err + e_step);
                }
                panel_blit_column_scaled(m, src, s++, dst, d++);
            }
            s += skip;
        }
    } else {
        ww(ds, FLIP_W, (uint16_t)(pw * 2 - near));
        ww(ds, FLIP_COLUMN, (uint16_t)(near - pw));
        e_keep = (int16_t)((near - pw) * 2);
        e_step = (int16_t)((near - pw - fh) * 2);
        err = (int16_t)((near - pw) * 2 - fh);
        for (k = 0; k < runs; k++) {
            for (j = 0; j < run; j++) {
                if (err < 0) err = (int16_t)(err + e_keep);
                else {
                    ww(ds, FLIP_COLUMN, (uint16_t)(rs(ds, FLIP_COLUMN) - 1));
                    ww(ds, FLIP_W, (uint16_t)(rs(ds, FLIP_W) + 2));
                    err = (int16_t)(err + e_step);
                }
                panel_blit_column_scaled(m, src, s++, dst, d++);
            }
            s += skip;
        }
    }
    for (k = (int16_t)(fh - runs * run); k > 0; k--) panel_blit_column_scaled(m, src, s++, dst, d++);
    ww(ds, FLIP_W, (uint16_t)near);
}

/* panel_view_switch_begin(target, x, y, h, w), from the
 * instructions: the rectangle kept; on the first flip ever the three EMS
 * pages asked for (a separate one-page handle each), bit 0 of the flags
 * only when all three came; then with them the new view drawn where the
 * screen does not show it -- its background out of PANELS.GR into the new
 * page, the edge-on card into the work page at 0x2800, the background
 * blitted onto the other page, the target's drawer run there with
 * panel_mode the target and paperdoll_flip_page 0, and the finished
 * panel read back into the new page -- and the target recorded. */
void panel_view_switch_begin(uw_motion *m, uint8_t target) {
    uint8_t *ds = m->ds;
    ww(ds, PANEL_X, 0xec);
    ww(ds, PANEL_Y, 0xc0);
    ww(ds, PANEL_H, 0x53);
    ww(ds, PANEL_W, 0x72);
    if (!ds[PAGES_TRIED]) {
        /* ems_alloc_one_page three times: handles 3, 4 and 5, as an EMM
         * hands them out */
        if (m->panel_ems) {
            int i;
            for (i = 0; i < 3; i++)
                if (!rw(ds, (uint16_t)(EMS_PAGES + i * 2))) ww(ds, (uint16_t)(EMS_PAGES + i * 2), (uint16_t)(3 + i));
            ds[FLIP_FLAGS] |= 1;
        }
        ds[PAGES_TRIED] = 1;
    }
    if ((ds[FLIP_FLAGS] & 1) && m->panel_ems) {
        static uint8_t back[64000];
        uint8_t *page, *screen = m->screen, *written = m->screen_written, was;
        const uint8_t *bg = m->gr_entry ? m->gr_entry(m->art_user, "PANELS", target, 0x53 * 0x72) : NULL;
        const uint8_t *edge = m->gr_entry ? m->gr_entry(m->art_user, "PANELS", 3, 0x78 * 3) : NULL;
        page = ems_map_scratch_page(m, 1);
        if (bg) memcpy(page, bg, 0x53 * 0x72);
        page = ems_map_scratch_page(m, 0);
        if (edge) memcpy(page + 0x2800, edge, 0x78 * 3);
        if (!bg || !edge) UW_NOT_CARRIED(m->not_carried);
        cursor_hide(m);
        if (screen) {
            /* gfx_swap_page_tables: the drawing goes to the other page */
            memcpy(back, screen, sizeof back);
            m->screen = back;
            m->screen_written = NULL;
            page = ems_map_scratch_page(m, 1);
            uw_motion_blit(m, page, 0x53, 0xec, 0xc0, 0x72, 0x53);
            was = ds[PANEL_MODE];
            ds[PANEL_MODE] = target;
            ds[SWITCH_DIR] = 0;
            panel_mode_drawer(m, target);
            ds[SWITCH_DIR] = 1;
            ds[PANEL_MODE] = was;
            page = ems_map_scratch_page(m, 1);
            vga_read_rect(m, page, 0xec, 0xc0, 0x53, 0x72);
            m->screen = screen;
            m->screen_written = written;
        }
        cursor_show(m);
    } else if (ds[FLIP_FLAGS] & 1) {
        UW_NOT_CARRIED(m->not_carried);
    }
    ds[FLIP_TARGET] = target;
}

/* panel_view_switch_step, a frame a call, 1 when the flip is
 * over. With the EMS pages, from the instructions: under a hidden cursor,
 * frame 1 reads the panel off the screen into the work page, and 1..3
 * scale it into the old page, fill the margins beside the card in 0xf1 and
 * blit it centred; 4 fills the last card's place and blits the edge-on
 * card, 0x78 rows by 3; 5..7 fill above and below the card (the last
 * frame's size) and scale and blit the new page's panel; 8 fills the same
 * and blits the new panel whole, the counter zeroed. Every frame then the
 * two hinge pictures, 0x20b8 and 0x20b0 plus the frame. Without them:
 * frame 3 the panel painted over in 0xf1 with the hinges and the edge-on
 * card, frame 6 the new view's background and its drawer, frame 8 the
 * end. */
int panel_view_switch_step(uw_motion *m) {
    uint8_t *ds = m->ds;
    int frame;
    ds[FLIP_FRAME]++;
    frame = ds[FLIP_FRAME];
    if ((ds[FLIP_FLAGS] & 1) && m->panel_ems) {
        int16_t px = rs(ds, PANEL_X), py = rs(ds, PANEL_Y), ph = rs(ds, PANEL_H), pw = rs(ds, PANEL_W);
        uint8_t *old, *page;
        cursor_hide(m);
        old = ems_map_scratch_page(m, 2);
#define FR rs(ds, FLIP_W)
#define FC rs(ds, FLIP_H)
        if (frame >= 1 && frame <= 3) {
            page = ems_map_scratch_page(m, 0);
            if (frame == 1 && m->screen) vga_read_rect(m, page, px, py, ph, pw);
            panel_flip_scale_blit(m, page, old, frame);
            uw_motion_fill_rect(m, px, py + (FR - pw) / 2, px + (ph - FC) / 2, py - (FR + pw) / 2, 0xf1);
            uw_motion_fill_rect(m, px + (ph + FC) / 2, py + (FR - pw) / 2, px + ph, py - (FR + pw) / 2, 0xf1);
            uw_motion_blit(m, old, FC, px + (ph - FC) / 2, py + (FR - pw) / 2, FR, FC);
        } else if (frame == 4) {
            page = ems_map_scratch_page(m, 0) + 0x2800;
            uw_motion_fill_rect(m, px + (ph - FC) / 2, py + (FR - pw) / 2, px + (ph + FC) / 2, py - (FR + pw) / 2, 0xf1);
            uw_motion_blit(m, page, 3, px + (ph - 3) / 2, py + (0x78 - pw) / 2, 0x78, 3);
        } else if (frame >= 5 && frame <= 7) {
            page = ems_map_scratch_page(m, 1);
            uw_motion_fill_rect(m, px, py + (FR - pw) / 2, px + ph, py, 0xf1);
            uw_motion_fill_rect(m, px, py - pw, px + ph, py - (FR + pw) / 2, 0xf1);
            panel_flip_scale_blit(m, page, old, frame);
            uw_motion_blit(m, old, FC, px + (ph - FC) / 2, py + (FR - pw) / 2, FR, FC);
        } else if (frame == 8) {
            page = ems_map_scratch_page(m, 1);
            uw_motion_fill_rect(m, px, py + (FR - pw) / 2, px + ph, py, 0xf1);
            uw_motion_fill_rect(m, px, py - pw, px + ph, py - (FR + pw) / 2, 0xf1);
            uw_motion_blit(m, page, ph, px, py, pw, ph);
            ds[FLIP_FRAME] = 0;
        }
#undef FR
#undef FC
        uw_motion_gr_draw_art(m, (uint16_t)(0x20b8 + ds[FLIP_FRAME]), 0x110, 0xc4);
        uw_motion_gr_draw_art(m, (uint16_t)(0x20b0 + ds[FLIP_FRAME]), 0x110, 0x4e);
        cursor_show(m);
        return ds[FLIP_FRAME] == 0;
    }
    if (ds[FLIP_FLAGS] & 1) {
        UW_NOT_CARRIED(m->not_carried);
        ds[FLIP_FRAME] = 0;
        return 1;
    }
    if (frame == 3) {
        /* the card seen edge-on: PANELS.GR's fourth entry, three columns of
         * 0x78 rows down the panel's middle */
        const uint8_t *px = m->gr_entry ? m->gr_entry(m->art_user, "PANELS", 3, 0x78 * 3) : NULL;
        cursor_hide(m);
        uw_motion_fill_rect(m, 0xec, 0xc0, 0x13f, 0x4e, 0xf1);
        uw_motion_gr_draw_art(m, 0x20bc, 0x110, 0xc4);
        uw_motion_gr_draw_art(m, 0x20b4, 0x110, 0x4e);
        if (px) uw_motion_blit(m, px, 3, 0x114, 0xc3, 0x78, 3);
        else m->pixels_not_drawn++;
        cursor_show(m);
    } else if (frame == 6) {
        uint8_t was = ds[PANEL_MODE], target = ds[FLIP_TARGET];
        cursor_hide(m);
        uw_motion_fill_rect(m, 0x114, 0xc3, 0x117, 0x4b, 0xf1);
        ds[PANEL_MODE] = target;
        panel_background(m, target);
        panel_mode_drawer(m, target);
        ds[PANEL_MODE] = was;
        uw_motion_gr_draw_art(m, 0x20b8, 0x110, 0xc4);
        uw_motion_gr_draw_art(m, 0x20b0, 0x110, 0x4e);
        cursor_show(m);
    } else if (frame == 8) {
        ds[FLIP_FRAME] = 0;
    }
    return ds[FLIP_FRAME] == 0;
}
