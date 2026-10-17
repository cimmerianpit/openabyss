/* SPDX-License-Identifier: MIT */
/* See uw_automap.h. */
#include "uw_automap.h"
#include "uw_motion_int.h"

#include <stdio.h>
#include <string.h>

enum {
    AM_TILES    = 0x3820,   /* automap_tiles: a byte a tile, y * 0x40 + x */
    AM_PATTERNS = 0x0aca,   /* automap_tile_patterns: five 3 x 3 cells */
    AM_DIAGONAL = 0x0af9,   /* automap_diagonal_wall_dir, for types 2..5 */
    AM_DOOR_A   = 0x0afd,   /* automap_door_dy: the tile step and the pixel's x */
    AM_DOOR_B   = 0x0b01,   /* automap_door_dx: the tile step and the pixel's y */
    AM_FLAGS    = 0x1d8a,   /* tile_type_flags: bit 0 a diagonal */
    AM_ORIGIN_X = 10,       /* the tile grid, three pixels a tile */
    AM_ORIGIN_Y = 7,
    AM_SLOT     = 26,       /* the automap block: level + 26 */
    AM_NOTES    = 0x23,     /* the note block: level + 0x23 */
    AM_NOTE_LEN = 0x36,     /* 50 bytes of text, then x and y */
    AM_MARKER   = 0x103f    /* the player's marker */
};

/* ---- the one primitive ------------------------------------------------- */

static uint8_t get_pixel(uw_automap *a, int x, int y) {
    int row = 199 - y;
    if (!a->m->screen || x < 0 || x >= 320 || row < 0 || row >= 200) return 0;
    return a->m->screen[row * 320 + x];
}

static void put_pixel(uw_automap *a, int x, int y, uint8_t c) {
    uw_motion_fill_rect(a->m, x, y, x, y, c);
}

/* automap_tint_pixel(x, y, add, spread): the pixel already on
 * the screen darkened by `add` plus rand() / (0x7fff / spread). TWO random
 * draws are made and only the second reaches the screen -- the first, with
 * the pixel it was added to, goes to a byte nothing reads -- so both are
 * taken here to leave the sequence where the original leaves it. */
static void tint_pixel(uw_automap *a, int x, int y, int add, int spread) {
    int step = (int)(0x7fffL / spread), roll;
    (void)(rt_rand(a->m) / step);                       /* the dead draw */
    roll = (int)(rt_rand(a->m) / step);
    put_pixel(a, x, y, (uint8_t)(get_pixel(a, x, y) + add + roll));
}

/* ---- a tile ------------------------------------------------------------ */

static uint8_t tile_byte(uw_automap *a, int x, int y) {
    if (x < 0 || x > 63 || y < 0 || y > 63) return 0;
    return a->m->ds[(uint16_t)(AM_TILES + y * 0x40 + x)];
}

/* automap_draw_door(x, y, px, py): a blot at the cell's centre
 * and, for the first of the four orientations with a mapped-floor tile on
 * either side of it, two more along that axis -- the mark laid across the
 * corridor the door sits in. */
static void draw_door(uw_automap *a, int x, int y, int px, int py) {
    const uint8_t *ds = a->m->ds;
    int k;
    px += 1;
    py += 1;
    tint_pixel(a, px, py, 6, 3);
    for (k = 0; k < 4; k++) {
        int da = (int8_t)ds[(uint16_t)(AM_DOOR_A + k)], db = (int8_t)ds[(uint16_t)(AM_DOOR_B + k)];
        if ((tile_byte(a, x + db, y + da) & 0xf) != 1 && (tile_byte(a, x - db, y - da) & 0xf) != 1)
            continue;
        tint_pixel(a, px + da, py + db, 6, 3);
        tint_pixel(a, px - da, py - db, 6, 3);
        return;
    }
}

/* automap_draw_tile(code, x, y): the tile's 3 x 3 cell from
 * automap_tile_patterns[min(code, 6) - 1] -- a 1 cell is floor, a 2 cell an
 * edge -- with the byte's high nibble choosing the floor's fill (bits 4..5:
 * the parchment stipple, two palette fills, or nothing) and an overlay over
 * the whole cell (bits 6..7: a door, a palette wash, a dark wash). */
static void draw_tile(uw_automap *a, int code, int x, int y) {
    const uint8_t *ds = a->m->ds;
    int b = tile_byte(a, x, y) >> 4, fill = b & 3, over = b & 0xc;
    int px = (x - 1) * 3 + AM_ORIGIN_X, py = (y - 1) * 3 + AM_ORIGIN_Y, di, si;
    if (!code) return;
    if (code >= 6) code = 1;
    code--;
    for (di = 0; di < 3; di++) {
        for (si = 0; si < 3; si++) {
            int cell = ds[(uint16_t)(AM_PATTERNS + code * 9 + si * 3 + di)];
            if (cell == 1) {
                if (fill == 0) tint_pixel(a, px + di, py + si, 2, 3);
                else if (fill == 1) put_pixel(a, px + di, py + si, (uint8_t)(0xb1 + rt_rand(a->m) % 2));
                else if (fill == 2) put_pixel(a, px + di, py + si, (uint8_t)(0xb5 + rt_rand(a->m) % 2));
            } else if (cell == 2) {
                tint_pixel(a, px + di, py + si, 6, 2);
            }
        }
    }
    if (over == 4) {
        draw_door(a, x, y, px, py);
    } else if (over == 8) {
        for (si = 0; si < 3; si++)
            for (di = 0; di < 3; di++)
                put_pixel(a, px + di, py + si, (uint8_t)(0xe9 + (int32_t)rt_rand(a->m) * 3 / 0x8000));
    } else if (over == 12) {
        for (di = 0; di < 3; di++)
            for (si = 0; si < 3; si++)
                tint_pixel(a, px + di, py + si, 6, 3);
    }
}

/* automap_draw_tile_wall(dir, x, y) -> whether it drew. The
 * neighbour in that direction -- 0 y+1, 1 x+1, 2 y-1, 3 x-1 -- mapped floor
 * (1..9) draws nothing; otherwise three pixels along that edge of the cell,
 * light where the neighbour is 0x0b (seen but not walked) and dark else. */
static int draw_wall(uw_automap *a, int dir, int x, int y) {
    int px = (x - 1) * 3 + AM_ORIGIN_X, py = (y - 1) * 3 + AM_ORIGIN_Y, code, add, spread, i;
    switch (dir) {
    case 0: y++; break;
    case 1: x++; break;
    case 2: y--; break;
    default: x--; break;
    }
    code = tile_byte(a, x, y) & 0xf;
    if (code > 0 && code < 10) return 0;
    if (code == 0x0b) { add = 0; spread = 4; }
    else              { add = 6; spread = 2; }
    switch (dir) {
    case 0: py += 3; for (i = 0; i < 3; i++) tint_pixel(a, px + i, py, add, spread); break;
    case 2: py -= 1; for (i = 0; i < 3; i++) tint_pixel(a, px + i, py, add, spread); break;
    case 1: px += 3; for (i = 0; i < 3; i++) tint_pixel(a, px, py + i, add, spread); break;
    default: px -= 1; for (i = 0; i < 3; i++) tint_pixel(a, px, py + i, add, spread); break;
    }
    return 1;
}

/* automap_draw_tiles: every tile the player has stood in -- a
 * low nibble of 1..9, the raw tile type -- with its floor, its walls (a
 * diagonal gets only the two automap_diagonal_wall_dir names), and a corner
 * dot wherever two adjacent walls both drew. */
static void draw_tiles(uw_automap *a) {
    const uint8_t *ds = a->m->ds;
    int x, y, d;
    for (y = 1; y < 0x3f; y++) {
        for (x = 1; x < 0x3f; x++) {
            uint8_t walls[4];
            int code = tile_byte(a, x, y) & 0xf, cx, cy;
            if (code == 0 || code >= 10) continue;
            draw_tile(a, code, x, y);
            memset(walls, 0, sizeof walls);
            if (ds[(uint16_t)(AM_FLAGS + code)] & 1) {
                d = (int8_t)ds[(uint16_t)(AM_DIAGONAL + code - 2)];
                walls[d & 3] = (uint8_t)draw_wall(a, d & 3, x, y);
                d = (d + 1) & 3;
                walls[d] = (uint8_t)draw_wall(a, d, x, y);
            } else {
                for (d = 0; d < 4; d++) walls[d] = (uint8_t)draw_wall(a, d, x, y);
            }
            cx = x * 3 + AM_ORIGIN_X;
            cy = y * 3 + AM_ORIGIN_Y;
            for (d = 0; d < 4; d++) {
                int off = (d & 2) ? 4 : 0;
                if (!walls[d] || !walls[(d + 1) & 3]) continue;
                tint_pixel(a, cx - off, cy - off, 3, 2);
            }
        }
    }
}

/* ---- the screen -------------------------------------------------------- */

static void palette_load(uw_automap *a, int which) {
    char path[600];
    uw_blob bl;
    snprintf(path, sizeof path, "%s/DATA/PALS.DAT", a->dir);
    bl = uw_read_file(path);
    if (bl.data && bl.size >= (size_t)(which + 1) * 768 && a->m->palette)
        memcpy(a->m->palette, bl.data + (size_t)which * 768, 768);
    else UW_NOT_CARRIED(a->not_carried);
    uw_free(&bl);
}

/* A fill in colour mode 0x106 over the map: the rectangle back from
 * BLNKMAP.BYT, which is what the other page holds here. */
static void restore(uw_automap *a, int x0, int y0, int x1, int y1) {
    int sy, sx;
    if (!a->m->screen || !a->blank.data || a->blank.size < 64000) { UW_NOT_CARRIED(a->not_carried); return; }
    for (sy = 199 - y0; sy <= 199 - y1; sy++) {
        if (sy < 0 || sy >= 200) continue;
        for (sx = x0; sx <= x1; sx++) {
            if (sx < 0 || sx >= 320) continue;
            a->m->screen[sy * 320 + sx] = a->blank.data[sy * 320 + sx];
            if (a->m->screen_written) a->m->screen_written[sy * 320 + sx] = 1;
        }
    }
}

/* automap_notes_load: the level's note block read into the
 * array, its length over the record size the only count there is. */
static void notes_load(uw_automap *a, int level) {
    const uint8_t *blk = NULL;
    size_t n = a->ark ? uw_ark_block(a->ark, level + AM_NOTES, &blk) : 0, i;
    a->nnotes = 0;
    a->notes_dirty = 0;
    memset(a->note, 0, sizeof a->note);
    if (!blk) return;
    for (i = 0; i + AM_NOTE_LEN <= n && a->nnotes < UW_MAP_NOTES; i += AM_NOTE_LEN) {
        uw_map_note *r = &a->note[a->nnotes++];
        memcpy(r->text, blk + i, 0x32);
        r->text[0x31] = 0;
        r->x = (int16_t)(blk[i + 0x32] | (blk[i + 0x33] << 8));
        r->y = (int16_t)(blk[i + 0x34] | (blk[i + 0x35] << 8));
    }
}

/* automap_draw_notes: every live note at its own place in
 * colour 0x2d -- a record whose x is negative has been deleted. */
static void draw_notes(uw_automap *a) {
    int i;
    if (!a->font_small.data) { UW_NOT_CARRIED(a->not_carried); return; }
    for (i = 0; i < a->nnotes; i++)
        if (a->note[i].x >= 0)
            uw_motion_draw_string(a->m, a->font_small.data, a->font_small.size, a->note[i].text,
                                  a->note[i].x, a->note[i].y, 0x2d);
}

/* automap_notes_save: nothing unless a note changed and there
 * is one; the delete markers compacted away as its loop does -- each slides
 * the rest down one and the index moves on regardless, so a marker right
 * after another stays -- and the block written back whatever is left, none
 * included. */
static void notes_save(uw_automap *a, int level) {
    uint8_t blk[UW_MAP_NOTES * AM_NOTE_LEN];
    int i;
    if (!a->notes_dirty || !a->nnotes || !a->ark) return;
    for (i = 0; i < a->nnotes; i++) {
        if (a->note[i].x < 0) {
            memmove(&a->note[i], &a->note[i + 1], (size_t)(a->nnotes - i - 1) * sizeof a->note[0]);
            a->nnotes--;
        }
    }
    memset(blk, 0, sizeof blk);
    for (i = 0; i < a->nnotes; i++) {
        uint8_t *r = blk + (size_t)i * AM_NOTE_LEN;
        memcpy(r, a->note[i].text, 0x32);
        r[0x32] = (uint8_t)a->note[i].x; r[0x33] = (uint8_t)(a->note[i].x >> 8);
        r[0x34] = (uint8_t)a->note[i].y; r[0x35] = (uint8_t)(a->note[i].y >> 8);
    }
    if (!uw_ark_write_block(a->ark, level + AM_NOTES, blk, (size_t)a->nnotes * AM_NOTE_LEN))
        UW_NOT_CARRIED(a->not_carried);
    a->notes_dirty = 0;
}

/* automap_render(level): BLNKMAP.BYT as the whole screen, the
 * tiles over it, the player's marker on the level being played, the map's
 * palette, the notes, and the level's number in fontbig. */
static void render(uw_automap *a, int level) {
    uw_motion *m = a->m;
    uint8_t *ds = m->ds;
    char digits[0x10];
    if (!m->screen || !a->blank.data || a->blank.size < 64000) { UW_NOT_CARRIED(a->not_carried); return; }
    /* automap_render draws between cursor_hide and cursor_show: the page
     * goes out from under the cursor, whose saved background is the screen
     * it was last drawn over -- the dungeon's panel, put back on the
     * parchment at the first move without this */
    uw_motion_cursor_hide(m);
    memcpy(m->screen, a->blank.data, 64000);
    if (m->screen_written) memset(m->screen_written, 1, 64000);
    draw_tiles(a);
    if (level == (int)rw(ds, 0x7278) && level != 9) {
        uint16_t tracked = rw(ds, TRACKED_OBJECT), w = rw(m->lseg, (uint16_t)(tracked + 0x16));
        m->span_variant = 1;
        uw_motion_gr_draw_art(m, AM_MARKER, (int16_t)(((w >> 10) - 1) * 3 + 9),
                              (int16_t)((((w & 0x3f0) >> 4) - 1) * 3 + 0xc));
        m->span_variant = 0;
    }
    a->level_shown = level;
    ww(ds, 0x381e, (uint16_t)level);
    palette_load(a, 1);
    notes_load(a, level);
    draw_notes(a);
    snprintf(digits, sizeof digits, "%d", level);
    if (a->font_big.data)
        uw_motion_draw_string(m, a->font_big.data, a->font_big.size, digits,
                              0x121 - uw_motion_string_width(a->font_big.data, a->font_big.size, digits) / 2,
                              0xc2, 0x2d);
    else UW_NOT_CARRIED(a->not_carried);
    uw_motion_cursor_show(m);
}

/* automap_show_level(level): the level's own marks into
 * automap_tiles -- the archive's block, empty for a level never entered --
 * and the screen again, the notes of the level shown saved first
 * (automap_notes_save). */
/* automap_clear and automap_block_load(level): the level's marks from the
 * archive into the data segment's map. */
static void block_load(uw_automap *a, int level) {
    uint8_t *ds = a->m->ds;
    const uint8_t *blk = NULL;
    size_t n;
    memset(ds + AM_TILES, 0, 0x1000);
    if (level < 9 && a->ark) {
        n = uw_ark_block(a->ark, level + AM_SLOT, &blk);
        if (blk && n) memcpy(ds + AM_TILES, blk, n < 0x1000 ? n : 0x1000);
    }
}

static void show_level(uw_automap *a, int level) {
    notes_save(a, a->level_shown);
    if (level < 1) return;
    block_load(a, level);
    render(a, level);
}

bool uw_automap_open(uw_automap *a, uw_motion *m, const char *dir, uw_ark *ark) {
    char path[600];
    memset(a, 0, sizeof *a);
    a->m = m;
    a->ark = ark;
    snprintf(a->dir, sizeof a->dir, "%s", dir);
    snprintf(path, sizeof path, "%s/DATA/BLNKMAP.BYT", dir);
    a->blank = uw_read_file(path);
    snprintf(path, sizeof path, "%s/DATA/FONTBIG.SYS", dir);
    a->font_big = uw_read_file(path);
    snprintf(path, sizeof path, "%s/DATA/FONT4X5P.SYS", dir);
    a->font_small = uw_read_file(path);
    snprintf(path, sizeof path, "%s/DATA/FONT5X6P.SYS", dir);
    a->font_cur = uw_read_file(path);
    if (!a->blank.data || a->blank.size < 64000) {
        uw_automap_close(a);
        return false;
    }
    a->active = 1;
    a->level_shown = (int)rw(m->ds, 0x7278);
    /* automap_block_save(0, level): the marks the walk has made go into the
     * archive before anything pages away from them */
    if (a->ark && a->level_shown >= 1 && a->level_shown < 9
        && !uw_ark_write_block(a->ark, a->level_shown + AM_SLOT, m->ds + AM_TILES, 0x1000))
        UW_NOT_CARRIED(a->not_carried);
    render(a, a->level_shown);
    /* automap_draw's pointer: kept off the map's left border and its foot,
     * and the map's own shape */
    uw_motion_cursor_bounds(m, 0x16, 7, 0x13f, 0xc7);
    uw_motion_cursor_hide(m);
    uw_motion_cursor_shape_push(m, 0x1078);
    uw_motion_cursor_show(m);
    return true;
}

void uw_automap_close(uw_automap *a) {
    /* automap_refresh_save: the cursor hidden and the map's shape popped,
     * the notes saved, ... the bounds the whole screen and the cursor
     * shown */
    uw_motion_cursor_hide(a->m);
    uw_motion_cursor_shape_pop(a->m, 0);
    notes_save(a, a->level_shown);        /* automap_refresh_save's persist */
    /* and the current level's marks back when another level's were shown,
     * or the walk goes on marking -- and the next save
     * writes -- the other level's map */
    if (a->level_shown >= 1 && a->level_shown != (int)rw(a->m->ds, 0x7278))
        block_load(a, (int)rw(a->m->ds, 0x7278));
    uw_free(&a->blank);
    uw_free(&a->font_big);
    uw_free(&a->font_small);
    uw_free(&a->font_cur);
    uw_motion_cursor_reset_bounds(a->m);
    uw_motion_cursor_show(a->m);
    a->active = 0;
}

/* The note editor's keys (automap_click's last region): a printable
 * character upper-cased and appended, refused when it would take the
 * note's right edge past 0x13b or its length past 0x2e (the last index
 * 0x2d); a backspace takes one back and puts the parchment over
 * what it drew; Return, Escape and a mouse button alike end it, which keeps
 * a note with a first character. */
/* The caret: the pointer put at the typed text's end -- curx, the note's
 * x less one plus the text's width -- ten right and 0x12 up from the click's
 * row, after every key. */
static void caret(uw_automap *a) {
    int w = a->font_small.data ? uw_motion_string_width(a->font_small.data, a->font_small.size, a->typed) : 0;
    uw_motion_cursor_move_to(a->m, (int16_t)(a->note_x - 1 + w + 10), (int16_t)(a->note_y - 4 + 0x12));
}

static void automap_key_typed(uw_automap *a, uint16_t code) {
    int w;
    if (!a->editing) {
        if (code == 0x1b) a->leave = 1;   /* automap_draw's Escape: game_change_mode(1) */
        return;
    }
    if (code == 0x0d || code == 0x1b) {
        int16_t curx = (int16_t)(a->note_x - 1 + (a->font_small.data
                       ? uw_motion_string_width(a->font_small.data, a->font_small.size, a->typed) : 0));
        if (a->typed[0]) {
            uw_map_note *r = &a->note[a->nnotes++];
            snprintf(r->text, sizeof r->text, "%s", a->typed);
            r->x = a->note_x;
            r->y = a->note_y;
            a->notes_dirty = 1;
        }
        a->editing = 0;
        uw_motion_cursor_hide(a->m);       /* mouse_hide, which the pop's show ends */
        draw_notes(a);                     /* the notes again, not the map */
        /* the pointer left past the note's end, and the map's shape back */
        uw_motion_cursor_move_to(a->m, (int16_t)(curx + 0x16), (int16_t)(a->note_y - 4 + 7));
        uw_motion_cursor_shape_pop(a->m, 2);
        return;
    }
    if (code >= 0x20 && code <= 0x7a) {
        char one[2];
        one[0] = (char)(code >= 'a' && code <= 'z' ? code - 0x20 : code);
        one[1] = 0;
        if (!a->font_small.data) { UW_NOT_CARRIED(a->not_carried); return; }
        w = uw_motion_string_width(a->font_small.data, a->font_small.size, a->typed)
            + uw_motion_string_width(a->font_small.data, a->font_small.size, one);
        /* refused, do_beep(0x12c, 10): 300 Hz on the speaker for 10 ms */
        if (a->note_x + w > 0x13b || a->typed_len >= 0x2e) { a->beeps++; caret(a); return; }
        a->typed[a->typed_len++] = one[0];
        a->typed[a->typed_len] = 0;
    } else if (code == 8) {
        if (!a->typed_len) { caret(a); return; }
        w = a->font_small.data ? uw_motion_string_width(a->font_small.data, a->font_small.size, a->typed) : 0;
        a->typed[--a->typed_len] = 0;
        /* the fill in colour 0x106 over what was drawn: the parchment back */
        restore(a, a->note_x, a->note_y, a->note_x + w, a->note_y - 6);
    } else {
        caret(a);
        return;
    }
    caret(a);
    if (a->font_small.data)
        uw_motion_draw_string(a->m, a->font_small.data, a->font_small.size, a->typed,
                              a->note_x, a->note_y, 0x2d);
}

/* automap_nearest_note: whichever of two notes is closer to a
 * point, by |x + width/2 - px| (the width FONT5X6P's: delete_note_at) and |y - 2 - py| taken separately -- the
 * second wins when it is closer in y, or else when it is closer in x. */
static int nearer_note(uw_automap *a, int i, int j, int px, int py) {
    int wi, wj, dxi, dyi, dxj, dyj;
    if (i < 0) return j;
    if (j < 0) return i;
    wi = a->font_cur.data ? uw_motion_string_width(a->font_cur.data, a->font_cur.size, a->note[i].text) : 0;
    wj = a->font_cur.data ? uw_motion_string_width(a->font_cur.data, a->font_cur.size, a->note[j].text) : 0;
    dxi = a->note[i].x + wi / 2 - px; if (dxi < 0) dxi = -dxi;
    dyi = a->note[i].y - 2 - py;      if (dyi < 0) dyi = -dyi;
    dxj = a->note[j].x + wj / 2 - px; if (dxj < 0) dxj = -dxj;
    dyj = a->note[j].y - 2 - py;      if (dyj < 0) dyj = -dyj;
    return (dyj < dyi || dxj < dxi) ? j : i;
}

/* The delete's second click: every note whose text box holds the point
 * folded through automap_nearest_note, and the closest marked deleted --
 * its x set negative for automap_notes_save to compact away. The search
 * takes the records already marked too, and the text stays, so a click
 * at the left edge can erase a marked note again. The widths -- the hit
 * box, the nearest's middle, the parchment put back -- are FONT5X6P's,
 * the font every path through 808b leaves loaded (the editor's end 0ba1,
 * the notes' redisplay 0e2f, the render 10c2) and the erase (0bf7..0d44)
 * does not change, though the notes are drawn in FONT4X5P. */
static void delete_note_at(uw_automap *a, int px, int py) {
    int i, best = -1;
    a->deleting = 0;
    for (i = 0; i < a->nnotes; i++) {
        int w;
        w = a->font_cur.data ? uw_motion_string_width(a->font_cur.data, a->font_cur.size, a->note[i].text) : 0;
        if (px < a->note[i].x || px > a->note[i].x + w) continue;
        if (py > a->note[i].y || py < a->note[i].y - 5) continue;
        best = nearer_note(a, best, i, px, py);
    }
    if (best < 0) return;
    a->notes_dirty = 1;
    {   /* the note's own rectangle back from the parchment */
        int w = a->font_cur.data
              ? uw_motion_string_width(a->font_cur.data, a->font_cur.size, a->note[best].text) : 0;
        restore(a, a->note[best].x, a->note[best].y, a->note[best].x + w, a->note[best].y - 5);
    }
    if (best == a->nnotes - 1) a->nnotes--;   /* the last record is simply dropped */
    else a->note[best].x = -1;                /* else the marker notes_save compacts away */
    draw_notes(a);
}

/* automap_click: the click's position, taken relative to the
 * map area's origin (0x16, 7), picked apart into five regions. CLOSE
 * leaves, the button under it begins a delete, the two corners page a
 * level, and anywhere else begins a note where the click landed. */
/* automap_click draws between cursor_hide and cursor_show, and
 * so does the note editor it runs: the note's text goes down where the map
 * was clicked, which is where the cursor stands, and a note deleted puts
 * the parchment back there. Drawn under a live cursor, its saved background
 * would go back over both at the next move. The two entries hold
 * the pair for everything they draw -- the notes, the parchment put back,
 * and render's own, which is nested and balanced. */
void uw_automap_key(uw_automap *a, uint16_t code) {
    if (!a->active) return;
    uw_motion_cursor_hide(a->m);
    automap_key_typed(a, code);
    uw_motion_cursor_show(a->m);
}

static void automap_click_at(uw_automap *a, int16_t x, int16_t y) {
    int ex = x - 0x16, ey = y - 7;
    /* the editor polls mouse_sample_buttons too: a button ends the note
     * as Return does */
    if (a->editing) { automap_key_typed(a, 0x0d); return; }
    /* the delete's click is read raw (cursor_get_pos_raw), and
     * the notes stand at their stored x and y on the screen */
    if (a->deleting) {
        /* the erase's wait over (a left click: the caller's): the bounds
         * the map's again, the eraser popped, the note under the click */
        uw_motion_cursor_bounds(a->m, 0x16, 7, 0x13f, 0xc7);
        uw_motion_cursor_shape_pop(a->m, 1);
        delete_note_at(a, x, y);
        uw_motion_cursor_show(a->m);
        return;
    }
    if (ex > 0x104 && ex < 0x140) {
        if (ey > 0x13 && ey < 0x33) { a->leave = 1; return; }
        if (ey > 0x33 && ey < 0x52) {
            /* ERASE: the whole screen, the eraser 0x1079, and a wait for a
             * left click alone */
            uw_motion_cursor_bounds(a->m, 0, 0, 0x13f, 0xc7);
            uw_motion_cursor_hide(a->m);
            uw_motion_cursor_shape_push(a->m, 0x1079);
            uw_motion_cursor_show(a->m);
            a->deleting = 1;
            return;
        }
    }
    if (ex > 0x113 && ex < 0x140) {
        if (ey > 0xb7 && ey < 0xc7) {
            if (a->level_shown > 1) show_level(a, a->level_shown - 1);
            return;
        }
        if (ey > 0 && ey < 0x12) {
            if (a->level_shown < 0x63) show_level(a, a->level_shown + 1);
            return;
        }
    }
    /* a new note, unless the hundred are used up -- then only the end's
     * pop, of the map's own shape, and its move to a caret never set */
    if (a->nnotes >= UW_MAP_NOTES) {
        UW_NOT_CARRIED(a->not_carried);
        uw_motion_cursor_hide(a->m);
        uw_motion_cursor_shape_pop(a->m, 2);
        return;
    }
    a->editing = 1;
    a->typed_len = 0;
    a->typed[0] = 0;
    a->note_x = (int16_t)ex;
    a->note_y = (int16_t)(ey + 4);
    uw_motion_cursor_shape_push(a->m, 0x107a);   /* the pen */
    caret(a);
}

void uw_automap_click(uw_automap *a, int16_t x, int16_t y) {
    if (!a->active) return;
    uw_motion_cursor_hide(a->m);
    automap_click_at(a, x, y);
    uw_motion_cursor_show(a->m);
}
