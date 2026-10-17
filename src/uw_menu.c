/* SPDX-License-Identifier: MIT */
/* See uw_menu.h. */
#include "uw_menu.h"
#include "uw_gamedir.h"
#include "uw_image.h"
#include "uw_strings.h"

#include <stdio.h>
#include <string.h>

enum {
    MENU_TABLE = 0x1b48,      /* the four records main_menu copies */
    MENU_ROWS = 4,
    DESC_ROW = 0x28,
    LINE_STEP = 0x16,         /* the text list's line spacing */
    LINE_BASE = 100,          /* line 0's baseline, bottom-up */
    COLOUR_HIGHLIGHT = 0xa2,
    COLOUR_PLAIN = 0xaa,
    MSG_BAD_SAVE = 0x2a9,     /* block 1, 0xa9: "Error: Bad save file" */
    MSG_REENTER = 0x301,      /* block 1, 0x101: "You reenter the Abyss . . ." */
    MSG_Y = 0x5a,
    IDLE_TICKS = 0xd
};

static uint16_t rw(const uint8_t *m, uint16_t at) { return (uint16_t)(m[at] | m[(uint16_t)(at + 1)] << 8); }

/* ---- drawing on the title's own page ------------------------------------- */

/* gfx_blit of an opaque image, its top row at 199 - y. */
static void blit(uw_menu *u, const uint8_t *px, int w, int h, int x, int y) {
    int r, c, top = 199 - y;
    if (!px) { UW_NOT_CARRIED(u->not_carried); return; }
    for (r = 0; r < h; r++) {
        int sy = top + r;
        if (sy < 0 || sy >= 200) continue;
        for (c = 0; c < w; c++) {
            int sx = x + c;
            if (sx < 0 || sx >= 320) continue;
            u->screen[sy * 320 + sx] = px[r * w + c];
        }
    }
}

static int string_width(const uw_menu *u, const char *s) {
    int w = 0, k;
    if (!u->have_font) return 0;
    for (k = 0; s[k] && k < 54; k++) w += uw_font_width(&u->font, (uint8_t)s[k]);
    return w;
}

/* gfx_draw_string: the glyphs' set bits in `colour`, the top
 * row at 199 - y. */
static void draw_string(uw_menu *u, const char *s, int x, int y, uint8_t colour) {
    int k, top = 199 - y;
    if (!u->have_font) { UW_NOT_CARRIED(u->not_carried); return; }
    for (k = 0; s[k] && k < 0x84; k++) {
        int g = (uint8_t)s[k], w = uw_font_width(&u->font, g), r, b;
        for (r = 0; r < (int)u->font.height; r++)
            for (b = 0; b < w; b++) {
                int sx = x + b, sy = top + r;
                if (!uw_font_pixel(&u->font, g, b, r)) continue;
                if (sx < 0 || sx >= 320 || sy < 0 || sy >= 200) continue;
                u->screen[sy * 320 + sx] = colour;
            }
        x += w;
    }
}

/* A message centred two ways: main_menu's error at 0xa0 - w / 2, the save
 * list's reentry at (0x140 - w) / 2 + 10 -- a pixel apart for an odd
 * width */
static void draw_message(uw_menu *u, uint16_t id, int y, int reentry) {
    char text[256];
    int w;
    if (!u->m->strings || uw_strings_by_id(u->m->strings, id, text, (int)sizeof text) < 0) {
        UW_NOT_CARRIED(u->not_carried);
        return;
    }
    w = string_width(u, text);
    draw_string(u, text, reentry ? (0x140 - w) / 2 + 10 : 0xa0 - w / 2, y, COLOUR_HIGHLIGHT);
}

/* ---- the saves ------------------------------------------------------------- */

/* enumerate_save_games: each slot's DESC read with fgets(0x27) and
 * nothing more -- a newline stays in the text, and a file fgets reads
 * nothing from leaves the row as it was */
static void enumerate_saves(uw_menu *u) {
    int slot;
    u->saves_mask = 0;
    for (slot = 0; slot < MENU_ROWS; slot++) {
        char path[1100];
        FILE *f;
        snprintf(path, sizeof path, "%s/SAVE%d/DESC", u->saves, slot + 1);
        f = uw_fopen(path, "r");
        if (f) {
            if (!fgets(u->desc[slot], 0x27, f)) { /* the row as it was */ }
            u->saves_mask |= 1u << slot;
            fclose(f);
        }
        if (!(u->saves_mask & (1u << slot))) snprintf(u->desc[slot], DESC_ROW, "%s", "<not used yet>");
    }
}

int uw_menu_no_saves(const uw_menu *u) { return u->saves_mask == 0; }

/* ---- main_menu's entry ------------------------------------------------------ */

bool uw_menu_open(uw_menu *u, uw_motion *m, const char *dir, const char *saves) {
    char path[600];
    uw_blob pals;
    int i;
    memset(u, 0, sizeof *u);
    u->m = m;
    snprintf(u->dir, sizeof u->dir, "%s", dir);
    snprintf(u->saves, sizeof u->saves, "%s", saves);
    snprintf(path, sizeof path, "%s/DATA/OPSCR.BYT", dir);
    u->opscr = uw_read_file(path);
    if (!u->opscr.data || u->opscr.size < 64000) return false;
    snprintf(path, sizeof path, "%s/DATA/FONTBIG.SYS", dir);
    u->have_font = uw_font_open(&u->font, path);
    if (!u->have_font) UW_NOT_CARRIED(u->not_carried);
    /* palette_read(2) */
    snprintf(path, sizeof path, "%s/DATA/PALS.DAT", dir);
    pals = uw_read_file(path);
    if (pals.data && pals.size >= 3 * 768) memcpy(u->title_palette, pals.data + 2 * 768, 768);
    else UW_NOT_CARRIED(u->not_carried);
    uw_free(&pals);
    /* the menu's records, and OPBTN.GR through menu_set_item: image
     * 2i is item i's normal face and measures the record, 2i + 1 the
     * highlighted one */
    snprintf(path, sizeof path, "%s/DATA", dir);
    for (i = 0; i < MENU_ROWS; i++) {
        uint16_t rec = (uint16_t)(MENU_TABLE + 16 * i);
        int w = 0, h = 0, w1, h1;
        u->item[i].x = (int16_t)rw(m->ds, (uint16_t)(rec + 8));
        u->item[i].y = (int16_t)rw(m->ds, (uint16_t)(rec + 10));
        u->item[i].face[0] = uw_gr_file_image(path, "OPBTN", 2 * i, &w, &h);
        u->item[i].face[1] = uw_gr_file_image(path, "OPBTN", 2 * i + 1, &w1, &h1);
        u->item[i].w = w;
        u->item[i].h = h;
        if (!u->item[i].face[0] || !u->item[i].face[1]) UW_NOT_CARRIED(u->not_carried);
    }
    enumerate_saves(u);
    u->count = u->saves_mask ? 4 : 3;
    u->cursor = u->saves_mask ? 3 : 1;
    u->hover = -1;
    u->last = -2;
    u->choice = UW_MENU_NONE;
    return true;
}

void uw_menu_close(uw_menu *u) {
    uw_free(&u->opscr);
    if (u->have_font) uw_font_close(&u->font);
    u->have_font = 0;
}

/* ---- menu_draw_items ---------------------------------------------------------- */

static void draw_items(uw_menu *u, int highlight) {
    int i;
    memcpy(u->screen, u->opscr.data, 64000);
    for (i = 0; i < u->count; i++)
        blit(u, u->item[i].face[i == highlight], u->item[i].w, u->item[i].h, u->item[i].x, u->item[i].y);
}

/* the lines at 0xa0 - w / 2, a signed halving -- a pixel right of menu_run's
 * (0x140 - w) / 2 hit box for an odd width -- each first cut, in place and
 * a character at a time, to at most 0x13e pixels, so the hit box takes the
 * cut width too */
static void draw_list(uw_menu *u, int highlight) {
    int i;
    memcpy(u->screen, u->opscr.data, 64000);
    for (i = 0; i < u->list_count; i++) {
        int w;
        while ((w = string_width(u, u->list_text[i])) > 0x13e && u->list_text[i][0])
            u->list_text[i][strlen(u->list_text[i]) - 1] = 0;
        draw_string(u, u->list_text[i], 0xa0 - w / 2, LINE_BASE - LINE_STEP * i,
                    (uint8_t)(i == highlight ? COLOUR_HIGHLIGHT : COLOUR_PLAIN));
    }
}

void uw_menu_draw(uw_menu *u) {
    if (u->list_up) draw_list(u, u->list_cursor);
    else draw_items(u, u->cursor);
}

/* ---- menu_idle_tick ------------------------------------------------------------ */

int uw_menu_idle(uw_menu *u, uint32_t clock) {
    uint8_t *working;
    if ((uint16_t)(clock - u->idle_stamp) <= IDLE_TICKS) return 0;
    /* palette_rotate_range(0x40, 0x40, 1) over THE WORKING PALETTE, which
     * while the title is up is palette 2 -- main_menu's palette_read(2),
     * which this player keeps as `title_palette`. The game's own working
     * palette is palette 0, whose 0x40..0x7f are blues, not the title
     * letters' golds. */
    working = u->m->palette;
    u->m->palette = u->title_palette;
    uw_motion_palette_rotate_range(u->m, 0x40, 0x40, 1);
    u->m->palette = working;
    u->idle_stamp = clock;
    return 1;
}

/* ---- menu_select's keys ----------------------------------------------------------- */

static int *cursor_of(uw_menu *u) { return u->list_up ? &u->list_cursor : &u->cursor; }
static int count_of(const uw_menu *u) { return u->list_up ? u->list_count : u->count; }

static void picked(uw_menu *u, int index) {
    if (u->list_up) {
        u->list_cursor = index;
        u->choice = u->list_slot[index];
    } else {
        u->cursor = index;
        u->choice = index;
    }
}

void uw_menu_key(uw_menu *u, uint16_t code) {
    static const int table[6] = { 0, -1, +1, 2, +1, 2 };   /* 0xa7..0xac: first, up, down, last, down, last */
    int *cur = cursor_of(u), n = count_of(u);
    int move = 0;                     /* -1 up, +1 down, 0 none, 2 last, 3 first */
    switch (code) {
    case 0x8d: case 0x8f: case 0xa6: case 0x162: case 0x170: move = -1; break;
    case 0x91: case 0x93: case 0x166: case 0x16e: move = +1; break;
    case 0x8c: case 0x8e: case 0xa5: case 0x23c: move = 3; break;
    case 0x92: case 0x94: case 0x23e: move = 2; break;
    case 0x0d: picked(u, *cur); return;
    /* menu_select: Alt-x answers -1 always, Escape only
     * with the flag main_menu_save_list passes (1) -- and the list's -1
     * is main_menu's loop again, not the end */
    case 0x1b: if (u->list_up) u->choice = UW_MENU_QUIT; return;
    case 0x278: u->choice = UW_MENU_QUIT; return;
    default:
        if (code >= 0xa7 && code <= 0xac) {
            int t = table[code - 0xa7];
            move = t == 0 ? 3 : t;
        }
        break;
    }
    if (move == -1) (*cur)--;
    else if (move == +1) (*cur)++;
    else if (move == 2) *cur = n - 1;
    else if (move == 3) *cur = 0;
    if (*cur < 0) *cur = 0;
    if (*cur > n - 1) *cur = n - 1;
    if (move) uw_menu_draw(u);
}

/* ---- menu_run ----------------------------------------------------------------------- */

static int item_at(const uw_menu *u, int x, int y) {
    int i;
    if (u->list_up) {
        int fh = u->have_font ? (int)u->font.height : 0;
        for (i = 0; i < u->list_count; i++) {
            int w = string_width(u, u->list_text[i]), x0 = (0x140 - w) / 2, base = LINE_BASE - LINE_STEP * i;
            if (x >= x0 && x < x0 + w && y <= base && y > base - fh) return i;
        }
        return -1;
    }
    for (i = 0; i < u->count; i++) {
        const uw_menu_item *it = &u->item[i];
        if (x >= it->x && x <= it->x + it->w - 1 && y >= it->y - it->h + 1 && y <= it->y) return i;
    }
    return -1;
}

void uw_menu_button(uw_menu *u, int down, int x, int y) {
    int *cur = cursor_of(u), n = count_of(u);
    if (down) {
        int over = item_at(u, x, y);
        if (!u->held) { u->held = 1; u->hover = -1; }
        if (over >= 0 && over != u->hover) {
            u->hover = over;
            if (u->list_up) draw_list(u, over); else draw_items(u, over);
        }
        return;
    }
    if (!u->held) return;
    u->held = 0;
    {
        int over = item_at(u, x, y), answer;
        /* menu_run's answer: the item under the cursor, or -- when the
         * cursor left every item after being over one -- that item + count,
         * which menu_select takes as a cursor move; nothing hovered at all
         * answers -1 and changes nothing */
        if (over >= 0) answer = over;
        else if (u->hover >= 0) answer = u->hover + n;
        else answer = -1;
        if (answer >= 0 && answer < n) picked(u, answer);
        else if (answer >= n) {
            *cur = answer - n;
            if (*cur > n - 1) *cur = n - 1;
            uw_menu_draw(u);
        }
    }
}

/* ---- main_menu_save_list --------------------------------------------------------- */

void uw_menu_list_open(uw_menu *u) {
    int slot;
    enumerate_saves(u);
    u->list_count = 0;
    for (slot = 0; slot < MENU_ROWS; slot++) {
        char *row;
        size_t len;
        if (!(u->saves_mask & (1u << slot))) continue;
        row = u->list_text[u->list_count];
        snprintf(row, DESC_ROW, "%s", u->desc[slot]);
        len = strlen(row);
        while (len > 0 && row[len - 1] == ' ') row[--len] = 0;   /* the trailing spaces cut */
        u->list_slot[u->list_count] = slot + 1;
        u->list_count++;
    }
    u->list_up = 1;
    u->list_cursor = 0;
    u->list_hover = -1;
    u->held = 0;
    u->hover = -1;
    u->choice = UW_MENU_NONE;
    draw_list(u, u->list_cursor);
}

void uw_menu_list_picked(uw_menu *u) {
    memcpy(u->screen, u->opscr.data, 64000);
    draw_message(u, MSG_REENTER, MSG_Y, 1);
    u->list_up = 0;
}

void uw_menu_list_cancel(uw_menu *u) {
    memcpy(u->screen, u->opscr.data, 64000);
    u->list_up = 0;
    u->choice = UW_MENU_NONE;
    uw_menu_draw(u);
}

void uw_menu_draw_error(uw_menu *u) {
    memcpy(u->screen, u->opscr.data, 64000);
    draw_message(u, MSG_BAD_SAVE, MSG_Y, 0);
    u->list_up = 0;
}


/* ==== victory_screen ==== */

static int font_string_width(const uw_font *f, const char *s) {
    int w = 0, k;
    for (k = 0; s[k] && k < 54; k++) w += uw_font_width(f, (uint8_t)s[k]);
    return w;
}

static void page_draw_string(uint8_t *page, const uw_font *f, const char *s, int x, int y, uint8_t colour) {
    int k, top = 199 - y;
    for (k = 0; s[k] && k < 0x84; k++) {
        int g = (uint8_t)s[k], w = uw_font_width(f, g), r, b;
        for (r = 0; r < (int)f->height; r++)
            for (b = 0; b < w; b++) {
                int sx = x + b, sy = top + r;
                if (!uw_font_pixel(f, g, b, r)) continue;
                if (sx < 0 || sx >= 320 || sy < 0 || sy >= 200) continue;
                page[sy * 320 + sx] = colour;
            }
        x += w;
    }
}

static void page_draw_centred(uint8_t *page, const uw_font *f, const char *s, int y) {
    page_draw_string(page, f, s, 0xa0 - font_string_width(f, s) / 2, y, 0x5c);
}

static int victory_string(uw_motion *m, uint16_t id, char *out, int cap) {
    if (!m->strings || uw_strings_by_id(m->strings, id, out, cap) < 0) { out[0] = 0; UW_NOT_CARRIED(m->not_carried); return 0; }
    return 1;
}

bool uw_victory_draw(uw_motion *m, const char *dir, uint8_t *page) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, 0x7270), row = rw(ds, 0x7272);   /* player_record_ptr, the class row */
    uw_font f;
    char path[600], line[0x48], part[0x48], name[0x1f];
    int y, h, i;
    uint32_t clock = (uint32_t)rw(ds, (uint16_t)(rec + 0xce)) | (uint32_t)rw(ds, (uint16_t)(rec + 0xd0)) << 16;
    uint32_t exp = (uint32_t)rw(ds, (uint16_t)(rec + 0x4e)) | (uint32_t)rw(ds, (uint16_t)(rec + 0x50)) << 16;
    snprintf(path, sizeof path, "%s/DATA/FONTCHAR.SYS", dir);
    if (!uw_font_open(&f, path)) { UW_NOT_CARRIED(m->not_carried); return false; }
    h = (int)f.height;
    y = 0xb4;
    /* get_string(player_name_handle): the record's whole name */
    memcpy(name, ds + rec, 0x1e);
    name[0x1e] = 0;
    page_draw_centred(page, &f, name, y);
    y -= h;
    victory_string(m, 0x2bb, line, sizeof line);                   /* "A level " */
    snprintf(part, sizeof part, "%d ", ds[(uint16_t)(rec + 0x3d)]);
    strncat(line, part, sizeof line - strlen(line) - 1);
    victory_string(m, (uint16_t)(0x400 | (0x17 + ((ds[(uint16_t)(rec + 0x64)] >> 5) & 7))), part, sizeof part);
    strncat(line, part, sizeof line - strlen(line) - 1);
    page_draw_centred(page, &f, line, y);
    y -= h;
    victory_string(m, 0x2bc, line, sizeof line);                   /* "Banished the Slasher of Veils" */
    page_draw_centred(page, &f, line, y);
    y -= h;
    victory_string(m, 0x2bd, line, sizeof line);                   /* "after " */
    snprintf(part, sizeof part, "%u", (unsigned)((clock / 0x1c2000u) / 12));
    strncat(line, part, sizeof line - strlen(line) - 1);
    victory_string(m, 0x2be, part, sizeof part);                   /* " days in the Abyss" */
    strncat(line, part, sizeof line - strlen(line) - 1);
    page_draw_centred(page, &f, line, y);
    y -= h;
    for (i = 0; i < 6; i++) {
        int x = i / 3 == 0 ? 0x50 : 0xbe, ry = y - (i % 3) * h;
        victory_string(m, (uint16_t)(0x400 | (0x11 + i)), line, sizeof line);
        switch (i) {
        case 0: case 1: case 2: snprintf(part, sizeof part, "%d", ds[(uint16_t)(row + 5 + i)]); break;
        case 3: snprintf(part, sizeof part, "%d", ds[(uint16_t)(row + 4)]); break;
        case 4: snprintf(part, sizeof part, "%d", ds[(uint16_t)(rec + 0x38)]); break;
        default: snprintf(part, sizeof part, "%lu", (unsigned long)(exp / 10)); break;
        }
        page_draw_string(page, &f, line, x, ry, 0x5c);
        page_draw_string(page, &f, part, x + 0x2d, ry, 0x5c);
    }
    y -= 2 * h;
    for (i = 0; i < 20; i++) {
        int v = ds[(uint16_t)(rec + 0x21 + i)], x = (i % 3) * 0x4a + 0x32;
        victory_string(m, (uint16_t)(0x400 | (0x1f + i)), line, sizeof line);
        snprintf(part, sizeof part, "%d", v);
        if (i % 3 == 0) y -= h;
        page_draw_string(page, &f, line, x, y, 0x5c);
        page_draw_string(page, &f, part, x + 0x46 - font_string_width(&f, part), y, 0x5c);
    }
    uw_font_close(&f);
    return true;
}
