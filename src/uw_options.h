/* SPDX-License-Identifier: MIT */
/* THE OPTIONS PANEL as the program runs it: options_menu_loop
 * and its pages, save_game and
 * save_or_restore_slot -- over the ported writers and readers
 * (uw_motion_save_player_dat, uw_motion_level_save, uw_motion_restore) and
 * the files a save is: SAVEn/PLAYER.DAT, LEV.ARK, BGLOBALS.DAT and DESC.
 * For the shell (src/tools/uwshell.c).
 *
 * The original's loop is modal inside the options button's click: its
 * pages draw OPTBTNS.GR's images over the action buttons' column -- a
 * background 35 x 108 at (4, 0xbd) and a 31 x 14 row image at (5, 0xbb -
 * (6 - row) * 15), the highlighted variant the next image -- and it polls
 * the input: Return activates the highlighted row, Escape closes, Space
 * and the keypad's up step the highlight up and its down keys step it
 * down, and a click's row is (y - 0x52) / 15 of the cursor in the panel.
 * Here the session keeps the state between the shell's passes: the shell
 * feeds it keys and clicks and asks `active`. A save asks the scroll for
 * a description (scroll_text_input: `typing`), then writes the slot's
 * four files and reloads the player; a restore reads them and rebuilds
 * the game as savegame_restore_progress does, and `restored` tells the
 * shell to redraw the dungeon screen and re-enter the level. */
#ifndef UW_OPTIONS_H
#define UW_OPTIONS_H

#include "uw_motion.h"
#include "uw_scroll.h"

typedef struct {
    uw_motion *m;
    char       dir[512];           /* the game directory */
    char       saves[512];         /* where SAVE1..SAVE4 live */
    uw_ark    *ark;                /* SAVE0's LEV.ARK, in memory */
    uint8_t  **bglobals;           /* SAVE0's BGLOBALS.DAT, in memory (the talk's) */
    size_t    *bglobals_size;
    const uint8_t *terrain;
    size_t     terrain_size;
    int        active;             /* options_panel_active */
    int        typing;             /* the description field is up */
    int        slot;               /* the slot the description is for */
    char       typed[0x34];
    int        typed_len;
    uw_scroll_edit edit;           /* the description's editor (scroll_text_input) */
    int        quit;               /* game_request_quit: the game ends */
    int        restored;           /* a restore rebuilt the game: redraw */
    int        show_owed;          /* options_click_row's cursor_show, still to come after a death in its handler */
    int        redraw;             /* the detail changed: view_rebuild_and_draw, screen_present now */
    long       not_carried;
} uw_options;

/* The session over `m`, with the archives the boot and the talk keep. */
void uw_options_init(uw_options *o, uw_motion *m, const char *dir, const char *saves, uw_ark *ark,
                     uint8_t **bglobals, size_t *bglobals_size, const uint8_t *terrain, size_t terrain_size);

/* options_menu_loop(1)'s opening: the main list drawn, the panel active. */
void uw_options_open(uw_options *o);

/* options_open_from_key: the panel for one setting, the Ctrl
 * key's code choosing the page (Ctrl-D detail, -F sound, -M music, -Q
 * quit, -R restore, -S save); with an action in progress message 0xa0
 * instead. The panel closes after the change. */
void uw_options_open_from_key(uw_options *o, uint16_t key, uint32_t clock);

/* A key's code (keyboard_read's) while the panel is up. */
void uw_options_key(uw_options *o, uint16_t code, uint32_t clock);
/* The save description's caret stepped by `counts` rounds while it is
 * typed (uw_scroll_caret_step). */
void uw_options_caret(uw_options *o, int counts);

/* A click at the cursor (screen x, y with y up) while the panel is up. */
void uw_options_click(uw_options *o, int16_t x, int16_t y, uint32_t clock);

/* What a click whose handler died owes once the game is entered again:
 * options_click_row's cursor_show and its release wait, `show_owed` times. */
void uw_options_pay_owed(uw_options *o);

/* savegame_restore_progress(slot) as the title screen's Journey Onward runs
 * it (main_menu_save_list): the slot's files over SAVE0's and the game
 * rebuilt, then weapons_load_colourmap and main_menu's weapon_stow -- not
 * the options panel's tail, whose tick waits for the game loop's event 12;
 * 1 when it restored (`restored` is left set for the host's redraw). */
int uw_options_restore(uw_options *o, int slot, uint32_t clock);

#endif
