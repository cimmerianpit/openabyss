/* SPDX-License-Identifier: MIT */
/* A CONVERSATION as the program runs one: conv_begin_with_object
 * through conv_start and converse_refresh, over the ported VM (src/uw_convvm.c), builtins
 * (src/uw_convbi.c), string cache and BGLOBALS -- from CNV.ARK, STRINGS.PAK,
 * BABGLOBS.DAT and the game's memory, for the shell (src/tools/uwshell.c).
 *
 * The original runs a conversation MODALLY: conv_begin_with_object switches
 * to game mode 4, whose enter handler converse_draw_screen draws the screen
 * and calls conv_start, which runs the VM until it ends; a builtin that
 * waits for the player -- babl_menu, babl_fmenu, babl_ask -- loops inside
 * its CALLI, pumping event_dispatch (nothing stands in mode 4) and polling
 * the input, and the digit keys 1..4 and a click on the menu window's rows
 * choose an option (conv_menu_choose). The port's VM pauses at
 * such a CALLI instead, and this session keeps the state between the
 * shell's passes: `wait` says what it waits for, and uw_talk_choose or
 * uw_talk_answer resumes it. The run's end exports the variables, saves
 * the globals, and holds the screen until any input -- or 500 ticks when the
 * player spoke last and nothing changed; uw_talk_finish is converse_refresh and the mode's return. */
#ifndef UW_TALK_H
#define UW_TALK_H

#include "uw_motion.h"
#include "uw_scroll.h"
#include "uw_conv.h"
#include "uw_convvm.h"
#include "uw_convbi.h"
#include "uw_strings.h"

enum { UW_TALK_IDLE = 0, UW_TALK_MENU, UW_TALK_ASK, UW_TALK_HOLD, UW_TALK_DONE, UW_TALK_MORE };

typedef struct {
    uw_motion  *m;
    char        dir[512];
    uw_ark      cnv;               /* CNV.ARK */
    int         have_cnv;
    uw_strcache sc;                /* the run-time strings over STRINGS.PAK */
    uint8_t    *bglobals;          /* SAVE0's BGLOBALS.DAT, in memory */
    size_t      bglobals_size;
    uw_conv     conv;
    uint16_t    code[0x8000];
    uint16_t    mem[0x10000];
    uw_convvar  vars[256];
    size_t      nvars;
    struct { uint16_t id; char name[24]; } imps[256];
    size_t      nimps;
    uw_convvm   vm;
    uw_convbi   bi;
    uint16_t    talker;            /* the object talked to (its offset) */
    int         talker_temp;       /* made for this conversation alone: freed at its end */
    uint16_t    slot;              /* conv_slot_wanted */
    int         wait;              /* UW_TALK_* */
    uint32_t    hold_until;        /* UW_TALK_HOLD and a pause: the clock it ends at (0: a click) */
    int         say_respond;       /* the stopped say is RESPOND_OP's */
    uint8_t     say_spoke;         /* who spoke, before the stopped say */
    int         menu_options;      /* the options listed at a menu */
    uint32_t    relist_at;         /* the menu cleared by a print: when it is listed again (0 none) */
    uw_scroll_edit edit;           /* babl_ask's field (scroll_text_input) */
    long        not_carried;
} uw_talk;

/* Open the archives and the strings: CNV.ARK and STRINGS.PAK from `dir`,
 * and SAVE0's BGLOBALS.DAT as bglobals_create makes it from BABGLOBS.DAT (a
 * new game's). `m` is the game the conversations run over; its `strings`
 * and `scroll` are used. */
bool uw_talk_open(uw_talk *t, uw_motion *m, const char *dir);
void uw_talk_close(uw_talk *t);

/* init_babl, which main_menu's Create Character runs every time: the
 * globals made again from BABGLOBS.DAT, every conversation's zero. */
void uw_talk_new_globals(uw_talk *t);

/* conv_begin_with_object(obj): the guard -- a creature that talks, its slot
 * live -- else the "no response" message and false. True when the
 * conversation screen is up and the VM has run to its first wait (or its
 * end: `wait` says). `clock` is the 256 Hz clock. */
bool uw_talk_begin(uw_talk *t, uint16_t obj, uint32_t clock);

/* At UW_TALK_MENU: option `n` (1..menu_options), as the digit keys and
 * conv_menu_choose take one; ignored outside the range. At UW_TALK_ASK:
 * the typed answer. Each runs the VM on to its next wait. */
void uw_talk_choose(uw_talk *t, int n, uint32_t clock);
void uw_talk_answer(uw_talk *t, const char *typed, uint32_t clock);
/* At UW_TALK_ASK: a key's code to the field's editor (uw_scroll_edit),
 * which answers with the text when Enter or Escape ends it (Escape's the
 * initial text, empty); and the caret stepped by `counts` rounds. */
void uw_talk_key(uw_talk *t, int code, uint32_t clock);
void uw_talk_caret(uw_talk *t, int counts);
/* conv_menu_choose(0): the option under the cursor -- the row (0x1e -
 * (cursor_y + 1)) / font height in the rows table -- or 0. */
int  uw_talk_option_at_cursor(const uw_talk *t);

/* A pass while the conversation is up: at UW_TALK_MENU, a menu a print has
 * cleared is listed again 500 ticks or a `click` later; at UW_TALK_HOLD,
 * the hold ends at its clock (0: none) or on `click`; at UW_TALK_MORE -- a say stopped at a pause or
 * a [MORE] (uw_scroll's waits) -- a pause ends at its clock or on `click`,
 * a [MORE] on `click` only, and the print goes on. Returns the wait. */
int  uw_talk_tick(uw_talk *t, uint32_t clock, int click);

/* converse_refresh and game_change_mode(-1)'s return to the dungeon's mode:
 * the barter's table set down, the panel mode put back, the scroll
 * selected, mode 1 with event 0x7ffe posted. The dungeon screen's redraw is
 * the caller's (uw_boot_draw_main_screen). */
void uw_talk_finish(uw_talk *t);

#endif
