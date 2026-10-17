/* SPDX-License-Identifier: MIT */
/* See uw_options.h. */
#include "uw_options.h"
#include "uw_motion_int.h"
#include "uw_image.h"
#include "uw_scroll.h"
#include "uw_gamedir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum {
    PANEL_ACTIVE     = 0x1a30,
    PAGE             = 0x1a32,
    CLOSE_AFTER      = 0x1a34,
    ROW_IMAGES       = 0x1a6d,   /* options_row_images[7 pages][7 rows] */
    EXIT_FLAG        = 0x7172,
    HIGHLIGHT_IMAGE  = 0x7174,   /* the image + 1, -1 for none */
    HIGHLIGHT_ROW    = 0x7176,
    SLOT_LABELS      = 0x1bd8,   /* four near pointers: "I- " .. "IV- " */
    LIST_HEADER      = 0x1c1f,   /* "\6    Save Game Descriptions" */
    LIST_FOOTER      = 0x1c3e,   /* "\0" */
    PAGE_SAVE = 0, PAGE_RESTORE = 1, PAGE_MUSIC = 2, PAGE_SOUND = 3, PAGE_DETAIL = 4, PAGE_QUIT = 5, PAGE_MAIN = 6,
    PAGE_NONE = 7
};

/* ---- drawing ---------------------------------------------------------------- */

static void draw_image(uw_options *o, int image, int x, int y) {
    int w, h;
    const uint8_t *px = uw_gr_file(NULL, "OPTBTNS", image, &w, &h);
    if (!px) { UW_NOT_CARRIED(o->not_carried); return; }
    uw_motion_blit(o->m, px, w, x, y, h, w);
}

/* options_draw_page_bg(image) and options_draw_row(row, image) */
static void draw_page_bg(uw_options *o, int image) { draw_image(o, image, 4, 0xbd); }
static void draw_row(uw_options *o, int row, int image) { draw_image(o, image, 5, 0xbb - (6 - row) * 0xf); }

/* options_highlight(row, image): the row highlighted with
 * image + 1, the one before put back plain -- none when the image word is
 * negative, which is what a page's drawer sets */
static void highlight(uw_options *o, int row, int image) {
    uint8_t *ds = o->m->ds;
    if ((int16_t)rw(ds, HIGHLIGHT_IMAGE) >= 0)
        draw_row(o, (int16_t)rw(ds, HIGHLIGHT_ROW), (int16_t)rw(ds, HIGHLIGHT_IMAGE) - 1);
    draw_row(o, row, image + 1);
    ww(ds, HIGHLIGHT_ROW, (uint16_t)row);
    ww(ds, HIGHLIGHT_IMAGE, (uint16_t)(image + 1));
}

/* The list area (4, 0xbd, 0x26 x 0x52) put back from MAIN.BYT: the
 * original's refill in colour 0x106 draws on the back page, so on the
 * screen it shows only where the quit's YES clears the panel. */
static void clear_list_area(uw_options *o) {
    char path[768];
    uw_blob bl;
    int r, c;
    snprintf(path, sizeof path, "%s/DATA/MAIN.BYT", o->dir);
    bl = uw_read_file(path);
    if (!bl.data || bl.size < 64000 || !o->m->screen) { uw_free(&bl); return; }
    for (r = 199 - 0xbd; r < 199 - 0xbd + 0x52; r++)
        for (c = 4; c < 4 + 0x26; c++) {
            o->m->screen[r * 320 + c] = bl.data[r * 320 + c];
            if (o->m->screen_written) o->m->screen_written[r * 320 + c] = 1;
        }
    uw_free(&bl);
}

/* ---- the save slots ------------------------------------------------------- */

static void slot_dir(const uw_options *o, int slot, char *out, size_t cap) {
    snprintf(out, cap, "%s/SAVE%d", o->saves, slot);
}

/* enumerate_save_games: each slot's DESC, and which exist; a
 * slot with none reads "<not used yet>", in the list and as
 * the description field's first text. */
static unsigned enumerate(const uw_options *o, char desc[4][0x28]) {
    unsigned mask = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char path[800];
        uw_blob b;
        desc[i][0] = '\0';
        slot_dir(o, i + 1, path, sizeof path);
        strncat(path, "/DESC", sizeof path - strlen(path) - 1);
        b = uw_read_file(path);
        if (b.data) {
            size_t n = b.size < 0x27 ? b.size : 0x27;
            memcpy(desc[i], b.data, n);
            desc[i][n] = '\0';
            mask |= 1u << i;
        }
        uw_free(&b);
        if (!(mask & (1u << i))) snprintf(desc[i], 0x28, "%s", (const char *)o->m->ds + 0x1bfc);
    }
    return mask;
}

static const char *ds_string(const uw_motion *m, uint16_t at) { return (const char *)m->ds + at; }

/* savegame_list_to_scroll: the header, each slot's label and
 * description, the footer; the escapes off across the four lines. */
static void list_to_scroll(uw_options *o) {
    uw_motion *m = o->m;
    char desc[4][0x28], line[0x200];
    int i;
    if (!m->scroll) return;
    uw_scroll_clear(m->scroll, 1);
    enumerate(o, desc);
    uw_scroll_print(m->scroll, ds_string(m, LIST_HEADER));
    m->ds[0x0a98] = 0;
    for (i = 0; i < 4; i++) {
        snprintf(line, sizeof line, "\n%s%s", ds_string(m, rw(m->ds, (uint16_t)(SLOT_LABELS + i * 2))), desc[i]);
        uw_scroll_print(m->scroll, line);
    }
    m->ds[0x0a98] = 1;
    uw_scroll_print(m->scroll, ds_string(m, LIST_FOOTER));
}

/* ---- the pages ----------------------------------------------------------------- */

static void show_page(uw_options *o, int page);

static void page_main(uw_options *o) {
    draw_page_bg(o, 1);
    ww(o->m->ds, HIGHLIGHT_IMAGE, 0xffff);
    highlight(o, 6, 6);
}

static void page_slots(uw_options *o) {
    draw_page_bg(o, 2);
    ww(o->m->ds, HIGHLIGHT_IMAGE, 0xffff);
    highlight(o, 5, 0x1e);
    if (rw(o->m->ds, PAGE) == PAGE_RESTORE) draw_row(o, 6, 0x2e);
    list_to_scroll(o);
}

static void page_quit(uw_options *o) {
    draw_page_bg(o, 3);
    ww(o->m->ds, HIGHLIGHT_IMAGE, 0xffff);
    highlight(o, 3, 0x3b);
}

static void page_audio(uw_options *o) {
    uint8_t *ds = o->m->ds;
    /* music_is_enabled and sfx_is_enabled: 0 without a driver */
    int music = rw(ds, PAGE) == PAGE_MUSIC,
        on = music ? ds[MUSIC_AVAILABLE] && ds[MUSIC_ENABLED] : ds[SFX_AVAILABLE] && ds[SFX_ENABLED];
    draw_page_bg(o, 4);
    ww(ds, HIGHLIGHT_IMAGE, 0xffff);
    draw_row(o, 5, music ? 0x33 : 0x34);
    draw_row(o, 6, (music ? 0x2f : 0x31) + (on ? 0 : 1));
    if (on) highlight(o, 4, 0x14);
    else highlight(o, 3, 0x16);
}

static void page_detail(uw_options *o) {
    uint8_t *ds = o->m->ds;
    int level = ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0xb5)] >> 4;
    ww(ds, HIGHLIGHT_IMAGE, 0xffff);
    draw_page_bg(o, 5);
    draw_image(o, 0x35 + level, 5, 0xbc);
    highlight(o, 4 - level, 0x26 + level * 2);
}

static void show_page(uw_options *o, int page) {
    ww(o->m->ds, PAGE, (uint16_t)page);
    switch (page) {
    case PAGE_SAVE: case PAGE_RESTORE: page_slots(o); break;
    case PAGE_MUSIC: case PAGE_SOUND: page_audio(o); break;
    case PAGE_DETAIL: page_detail(o); break;
    case PAGE_QUIT: page_quit(o); break;
    default: page_main(o); break;
    }
}

/* options_close */
static void close_panel(uw_options *o) {
    uw_motion *m = o->m;
    uint8_t *ds = m->ds;
    uw_motion_cursor_hide(m);
    ww(ds, PANEL_ACTIVE, 0);
    ww(ds, PAGE, PAGE_NONE);
    draw_page_bg(o, 0);
    if ((int16_t)rw(ds, 0x268c) > 0) panel_button_draw(m, (int16_t)rw(ds, 0x268c), 1);   /* the action mode's button */
    ds[EXIT_FLAG] = 1;
    uw_motion_cursor_show(m);
    o->active = 0;
    o->typing = 0;
    ds[CLOSE_AFTER] = 0;                     /* options_open_from_key clears it after the loop */
}

/* The exit rule the music, sound and detail pages share. */
static void leave_setting(uw_options *o) {
    if (o->m->ds[CLOSE_AFTER]) close_panel(o);
    else show_page(o, PAGE_MAIN);
}

/* ---- saving and restoring ------------------------------------------------------- */

static int write_file(const char *path, const void *data, size_t n) {
    FILE *f = uw_fopen(path, "wb");
    if (!f) return 0;
    if (n && fwrite(data, 1, n, f) != n) { fclose(f); return 0; }
    fclose(f);
    return 1;
}

/* combat_state_save, save_game's first act: the assault
 * record into the player record. */
static void combat_state_save(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    ds[rec + 0xba] = ds[0x00c5];
    ds[rec + 0xbb] = ds[0x00c6];
    ww(ds, (uint16_t)(rec + 0xbc), rw(ds, 0x2486));
    ww(ds, (uint16_t)(rec + 0xbe), rw(ds, 0x2488));
    ds[rec + 0xc0] = ds[0x248a];
    ds[rec + 0xc1] = ds[0x248b];
}

/* save_game's rest once the description is typed: "\n" and
 * "Saving Game " (0xa7), then savegame_write's PLAYER.DAT, level_save into
 * SAVE0's archive, DESC, SAVE0 copied into the slot and the player
 * reloaded, a "..." (0xaa) for each of the three stages passed; the
 * footer and the window cleared, and save_or_restore_slot's 0xa5, "Game
 * saved" -- or at the first failure the window cleared and 0xa4. */
static void save_to_slot(uw_options *o, int slot, const char *desc) {
    uw_motion *m = o->m;
    static uint8_t pd[0x3000], known[0x3000];
    char dir[800], path[900];
    size_t n;
    int ok;
    uw_motion_print_ds_string(m, 0x1c3c);           /* "\n" */
    print_message(m, 0xa7);                          /* "Saving Game " */
    combat_state_save(m);
    n = uw_motion_save_player_dat(m, pd, sizeof pd, known);
    if (n > 0) print_message(m, 0xaa);
    ok = n > 0 && o->ark && uw_motion_level_save(m, o->ark);
    if (ok) print_message(m, 0xaa);
    uw_motion_savegame_reload(m);
    slot_dir(o, slot, dir, sizeof dir);
    uw_mkdir(o->saves);
    uw_mkdir(dir);
    if (ok) {
        snprintf(path, sizeof path, "%s/PLAYER.DAT", dir);
        ok = write_file(path, pd, n);
        snprintf(path, sizeof path, "%s/LEV.ARK", dir);
        ok = ok && write_file(path, o->ark->file.data, o->ark->file.size);
        snprintf(path, sizeof path, "%s/BGLOBALS.DAT", dir);
        ok = ok && write_file(path, o->bglobals && *o->bglobals ? *o->bglobals : (const uint8_t *)"", o->bglobals && *o->bglobals ? *o->bglobals_size : 0);
        snprintf(path, sizeof path, "%s/DESC", dir);
        ok = ok && write_file(path, desc, strlen(desc));
        if (ok) print_message(m, 0xaa);
    }
    if (ok) uw_motion_print_ds_string(m, LIST_FOOTER);
    if (m->scroll) uw_scroll_clear(m->scroll, 1);
    print_message(m, ok ? 0xa5 : 0xa4);
}

/* savegame_restore_progress and, from the panel (`in_game`),
 * save_or_restore_slot's tail: the slot's files over SAVE0's, the game
 * rebuilt, the panels and the mode. The title's Journey Onward has none of
 * the tail and prints neither of its messages. */
static void restore_from_slot(uw_options *o, int slot, uint32_t clock, int in_game) {
    uw_motion *m = o->m;
    char dir[800], path[900];
    uw_blob pd, ark, bg;
    uw_ark loaded;
    int ok = 0;
    slot_dir(o, slot, dir, sizeof dir);
    snprintf(path, sizeof path, "%s/PLAYER.DAT", dir);
    pd = uw_read_file(path);
    snprintf(path, sizeof path, "%s/LEV.ARK", dir);
    ark = uw_read_file(path);
    snprintf(path, sizeof path, "%s/BGLOBALS.DAT", dir);
    bg = uw_read_file(path);
    print_message(m, 0xa6);                          /* "Restoring Game " */
    if (pd.data && ark.data && o->ark && uw_ark_open(&loaded, path)) {
        /* SAVE0's archive replaced by the slot's (dir_copy_contents) */
        uw_ark_close(&loaded);
    }
    if (pd.data && ark.data && o->ark) {
        char tmp[900];
        snprintf(tmp, sizeof tmp, "%s/LEV.ARK", dir);
        uw_ark_close(o->ark);
        if (uw_ark_open(o->ark, tmp)) {
            /* savegame_restore_progress's "..." after SAVE0 emptied and
             * after the slot copied in; the other two after PLAYER.DAT and
             * after the level (uw_motion_restore's) */
            print_message(m, 0xaa);
            print_message(m, 0xaa);
            ok = uw_motion_restore(m, pd.data, pd.size, o->ark, o->terrain, o->terrain_size);
        }
    }
    if (ok && bg.data && o->bglobals) {
        free(*o->bglobals);
        *o->bglobals = malloc(bg.size);
        if (*o->bglobals) { memcpy(*o->bglobals, bg.data, bg.size); *o->bglobals_size = bg.size; }
    }
    uw_free(&pd);
    uw_free(&ark);
    uw_free(&bg);
    if (!ok) {
        /* the progress line ended, then (from the panel) "Restore failed" */
        if (m->scroll) uw_scroll_print(m->scroll, "\n");
        if (in_game) print_message(m, 0xa3);
        return;
    }
    if (!in_game) { uw_motion_menu_restore_tail(m); o->restored = 1; return; }
    /* save_or_restore_slot's tail: weapons_load_colourmap (the boot's),
     * dungeon_tick_update, the level-entered flag, the panels built, the
     * movement mode, the liquid flag, the events */
    uw_motion_restore_tail(m, clock);
    print_message(m, 0xa2);
    o->restored = 1;
}

int uw_options_restore(uw_options *o, int slot, uint32_t clock) {
    o->restored = 0;
    restore_from_slot(o, slot, clock, 0);
    return o->restored;
}

/* ---- the clicks ------------------------------------------------------------------ */

/* save_game_allowed: not with a thing on the cursor (0xa0,
 * "an action is pending"), not in the ethereal void (0x9f, "Impossible,
 * you are between worlds."), the message printed and 0. */
static int save_game_allowed(uw_motion *m) {
    int msg = 0;
    if (rw(m->ds, ACTION_STATE_WORD)) msg = 0xa0;
    if (rw(m->ds, 0x7278) == 9) msg = 0x9f;
    if (msg) print_message(m, (uint16_t)msg);
    return msg == 0;
}

/* restore_game_allowed: always, a pending action dropped and
 * its cursor popped first. */
static int restore_game_allowed(uw_motion *m) {
    if (rw(m->ds, ACTION_STATE_WORD)) {
        ww(m->ds, ACTION_STATE_WORD, 0);
        cursor_shape_pop(m, 0);
    }
    return 1;
}

/* options_click_main; a refused save or restore does nothing */
static void click_main(uw_options *o, int row) {
    switch (row) {
    case 6: if (save_game_allowed(o->m)) show_page(o, PAGE_SAVE); break;
    case 5: if (restore_game_allowed(o->m)) show_page(o, PAGE_RESTORE); break;
    case 4: show_page(o, PAGE_MUSIC); break;
    case 3: show_page(o, PAGE_SOUND); break;
    case 2: show_page(o, PAGE_DETAIL); break;
    case 1: close_panel(o); break;
    case 0: show_page(o, PAGE_QUIT); break;
    default: break;
    }
}

/* options_click_slots: rows 2..5 are slots 4..1, row 1
 * cancel. Any of them is highlighted and the text window cleared first
 * (text_window_clear(0)), so the slot list leaves the scroll whatever the
 * choice; then save_or_restore_slot, and for a restore action_mode 0 and
 * weapon_stow after it, and the panel closed. A save asks for its
 * description first (save_game's scroll_text_input), the field holding the
 * slot's own description to start with. */
static void click_slots(uw_options *o, int row, uint32_t clock) {
    uw_motion *m = o->m;
    char desc[4][0x28];
    int slot;
    if (row < 1 || row > 5) return;
    highlight(o, row, (5 - row) * 2 + 0x1e);
    if (m->scroll) uw_scroll_clear(m->scroll, 0);
    if (row == 1) { close_panel(o); return; }
    slot = 6 - row;
    if (rw(m->ds, PAGE) == PAGE_RESTORE) {
        if (!(enumerate(o, desc) & (1u << (slot - 1)))) print_message(m, 0xa1);
        else restore_from_slot(o, slot, clock, 1);
        ww(m->ds, 0x268c, 0);
        weapon_stow(m);
        close_panel(o);
        return;
    }
    enumerate(o, desc);
    if (m->scroll) { uw_scroll_clear(m->scroll, 1); }
    print_message(m, 0xa8);                          /* "Please enter a save file description" */
    if (m->scroll) {
        uw_scroll_print(m->scroll, "\n");           /* the field on the line below */
        uw_scroll_print(m->scroll, ">");            /* scroll_text_input's default prompt */
        uw_scroll_edit_begin(m->scroll, &o->edit, desc[slot - 1], 1, 0x1e);
    }
    o->typing = 1;
    o->slot = slot;
    o->typed_len = 0;
    o->typed[0] = '\0';
}

static void click_quit(uw_options *o, int row) {
    if (row == 4) {
        highlight(o, 4, 0x39);
        clear_list_area(o);
        o->quit = 1;
    }
    if (row == 4 || row == 3) close_panel(o);
}

/* options_click_music / _sfx: rows 4 and 3 turn the setting on and off
 * through music_set_enabled and sfx_set_enabled -- nothing without a
 * driver, and with one the music started or stopped and the effects
 * silenced -- and the page drawn again; then the page is left on any row
 * when it was opened for this one setting, else only on row 2, DONE */
static void click_audio(uw_options *o, int row) {
    uint8_t *ds = o->m->ds;
    int music = rw(ds, PAGE) == PAGE_MUSIC;
    if (row == 4 || row == 3) {
        if (music) music_set_enabled(o->m, row == 4);
        else sfx_set_enabled(o->m, row == 4);
        page_audio(o);
    }
    if (ds[CLOSE_AFTER] || row == 2) leave_setting(o);
}

/* options_click_detail: rows 1..4 the level, set_render_detail,
 * and the view rebuilt and presented there and then (view_rebuild_and_draw
 * and screen_present between two gfx_swap_page_tables -- the host's, through
 * `redraw`), then the level's picture and highlight; only row 0, DONE,
 * leaves -- however the page was opened */
static void click_detail(uw_options *o, int row) {
    uint8_t *ds = o->m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    if (row > 0 && row < 5) {
        int level = 4 - row;
        ds[rec + 0xb5] = (uint8_t)((ds[rec + 0xb5] & 0x0f) | (level << 4));
        uw_motion_set_render_detail(o->m);
        o->redraw = 1;
        draw_image(o, 0x35 + level, 5, 0xbc);
        highlight(o, 4 - level, 0x26 + level * 2);
    }
    if (row == 0) leave_setting(o);
}

/* options_click_row: the page's handler with the row, the
 * list area refilled, the cursor shown and the button's release waited
 * for (input_wait_button_release(0)). The handler runs with the cursor hidden, and a
 * restore whose tick finds the Avatar dead never comes back to the show:
 * player_death's game_return_to_menu(1) runs inside it, the title and all,
 * and the show waits until the game is entered again and the calls unwind
 * (`show_owed`, the shell's dungeon_from_menu) -- which is why the title
 * after such a death has a visibility count of 0. */
static void click_row(uw_options *o, int row, uint32_t clock) {
    uw_motion *m = o->m;
    uw_motion_cursor_hide(m);
    switch (rw(m->ds, PAGE)) {
    case PAGE_SAVE: case PAGE_RESTORE: click_slots(o, row, clock); break;
    case PAGE_MUSIC: case PAGE_SOUND: click_audio(o, row); break;
    case PAGE_DETAIL: click_detail(o, row); break;
    case PAGE_QUIT: click_quit(o, row); break;
    case PAGE_MAIN: click_main(o, row); break;
    default: break;
    }
    if (m->return_to_menu) o->show_owed++;
    else {
        uw_motion_cursor_show(m);
        input_wait_button_release(m, 0);
    }
}

/* What options_click_row still owes once game_return_to_menu returns into
 * it: the cursor_show and the release wait, for each click that died. */
void uw_options_pay_owed(uw_options *o) {
    for (; o->show_owed > 0; o->show_owed--) {
        uw_motion_cursor_show(o->m);
        input_wait_button_release(o->m, 0);
    }
}

/* ---- the session ------------------------------------------------------------------ */

void uw_options_init(uw_options *o, uw_motion *m, const char *dir, const char *saves, uw_ark *ark,
                     uint8_t **bglobals, size_t *bglobals_size, const uint8_t *terrain, size_t terrain_size) {
    memset(o, 0, sizeof *o);
    o->m = m;
    snprintf(o->dir, sizeof o->dir, "%s", dir);
    snprintf(o->saves, sizeof o->saves, "%s", saves);
    o->ark = ark;
    o->bglobals = bglobals;
    o->bglobals_size = bglobals_size;
    o->terrain = terrain;
    o->terrain_size = terrain_size;
}

void uw_options_open(uw_options *o) {
    uint8_t *ds = o->m->ds;
    ww(ds, PANEL_ACTIVE, 1);
    ds[EXIT_FLAG] = 0;
    ds[CLOSE_AFTER] = 0;
    o->active = 1;
    o->typing = 0;
    o->restored = 0;
    /* options_menu_loop draws its page between cursor_hide and cursor_show:
     * the panel goes over the screen the cursor's saved background was
     * taken from -- the options button it was just clicked on, which the
     * next move put back over the list */
    uw_motion_cursor_hide(o->m);
    show_page(o, PAGE_MAIN);
    uw_motion_cursor_show(o->m);
}

/* options_key_action(0/2): the highlight one row on -- action
 * 0 down a row (-1), action 2 up (+1) -- and no move at all when that row
 * is off the page or its image in options_row_images[page] is 0 */
static void step_highlight(uw_options *o, int dir) {
    uint8_t *ds = o->m->ds;
    int page = (int16_t)rw(ds, PAGE), row = (int16_t)rw(ds, HIGHLIGHT_ROW) + dir;
    if (page < 0 || page > 6) return;
    if (row < 0 || row > 6 || !ds[(uint16_t)(ROW_IMAGES + page * 7 + row)]) return;
    /* options_key_action draws the row it moves to with the cursor off */
    uw_motion_cursor_hide(o->m);
    highlight(o, row, ds[(uint16_t)(ROW_IMAGES + page * 7 + row)]);
    uw_motion_cursor_show(o->m);
}

/* options_key_action's row for a Ctrl key's code (options_open_from_key):
 * Ctrl-D detail, Ctrl-F sound, Ctrl-M music, Ctrl-Q quit, Ctrl-R restore,
 * Ctrl-S save; -1 for another. */
static int key_row(uint16_t key) {
    switch (key) {
    case 0x164: return 2;
    case 0x166: return 3;
    case 0x16d: return 4;
    case 0x171: return 0;
    case 0x172: return 5;
    case 0x173: return 6;
    default: return -1;
    }
}

void uw_options_open_from_key(uw_options *o, uint16_t key, uint32_t clock) {
    uw_motion *m = o->m;
    uint8_t *ds = m->ds;
    int row = key_row(key);
    if (rw(ds, ACTION_STATE_WORD)) { print_message(m, 0xa0); return; }
    if (row < 0) { uw_options_open(o); return; }
    ds[CLOSE_AFTER] = 1;
    ww(ds, PANEL_ACTIVE, 1);
    ds[EXIT_FLAG] = 0;
    o->active = 1;
    o->typing = 0;
    o->restored = 0;
    ww(ds, PAGE, PAGE_MAIN);
    ww(ds, HIGHLIGHT_IMAGE, 0xffff);
    click_row(o, row, clock);
    /* options_menu_loop(options_page == 6): the main list drawn only when
     * the key's row left it there (a refused save or restore), and under a
     * hidden cursor as the loop draws it */
    if (o->active && rw(ds, PAGE) == PAGE_MAIN) {
        uw_motion_cursor_hide(m);
        page_main(o);
        uw_motion_cursor_show(m);
    }
}

void uw_options_caret(uw_options *o, int counts) {
    if (o->active && o->typing && o->m->scroll) uw_scroll_edit_caret(o->m->scroll, &o->edit, counts);
}

void uw_options_key(uw_options *o, uint16_t code, uint32_t clock) {
    uint8_t *ds = o->m->ds;
    if (!o->active) return;
    if (o->typing) {
        /* scroll_text_input's editor (uw_scroll_edit): Enter saves under
         * the text, Escape closes the panel */
        int end = o->m->scroll ? uw_scroll_edit_key(o->m->scroll, &o->edit, code)
                               : (code == 0x0d || code == 0x1b ? code : 0);
        if (o->m->scroll) {
            snprintf(o->typed, sizeof o->typed, "%s", o->edit.text);
            o->typed_len = (int)strlen(o->typed);
        }
        if (end == 0x1b) {
            /* save_game returns 0 with the window cleared, and
             * save_or_restore_slot says "Save Game Failed." (0xa4) */
            o->typing = 0;
            if (o->m->scroll) uw_scroll_clear(o->m->scroll, 1);
            print_message(o->m, 0xa4);
            close_panel(o);
        } else if (end == 0x0d) {
            o->typing = 0;
            save_to_slot(o, o->slot, o->typed);
            close_panel(o);
        }
        return;
    }
    if (code == 0x0d) click_row(o, (int16_t)rw(ds, HIGHLIGHT_ROW), clock);
    else if (code == 0x1b) close_panel(o);
    else if (code == 0x20 || code == 0x93 || code == 0xab) step_highlight(o, -1);   /* options_key_action(0) */
    else if (code == 0x8d || code == 0xa6) step_highlight(o, 1);                    /* options_key_action(2) */
}

void uw_options_click(uw_options *o, int16_t x, int16_t y, uint32_t clock) {
    int px = x - 4, py = y - 0x52;
    if (!o->active || o->typing) return;
    if (px < 0 || px >= 0x24 || py < 0 || py >= 0x6d) return;
    click_row(o, py / 0xf, clock);
}
