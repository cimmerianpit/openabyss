/* SPDX-License-Identifier: MIT */
/* THE TEXT WINDOWS: scroll_print and the chain under it --
 * scroll_print_segments, text_window_emit_lines, text_window_emit
 * with its escape language and [MORE], text_window_emit_wrapped -- and the
 * window selectors and text_window_clear, ported from the instructions for
 * what they keep in the data segment: the two 21-byte window records
 * (the message scroll and the conversation panel), the
 * current window's pointer, the lines left before [MORE], the
 * mode and pending-clear flags, the scroll's end caps and the panel's edge
 * frames. The drawing is the graphics module's and is not modelled; the
 * text's widths come from the font font_open loaded, which the caller gives.
 *
 * A print that reaches a wait -- an escape's pause (\P, \p) or [MORE] --
 * stops there when `stop_at_wait` is set: a later state stands inside that
 * wait. `resumed` runs a print from inside its opening pause instead, as a
 * state that stood there goes on. */
#ifndef UW_SCROLL_H
#define UW_SCROLL_H

#include "uw.h"
#include "uw_motion.h"

enum {
    UW_SCROLL_WINDOW = 0x0a60,      /* scroll_window */
    UW_CONV_WINDOW   = 0x0a75       /* conv_text_window */
};

typedef struct uw_scroll {
    uw_motion     *m;              /* the data segment, the cursor, the panels */
    const uint8_t *font;           /* the current font as font_open read it: the
                                    * 12-byte FONT*.SYS header, then 128 glyphs */
    size_t         font_size;
    int            stop_at_wait;   /* stop a print at its first wait */
    int            resumed;        /* the print goes on from inside its opening pause */
    int            stopped;        /* a print stopped at a wait */
    int            hold_waits;     /* the dungeon's: stop at every wait, as stop_at_wait,
                                    * and queue the prints made while stopped (kind 5)
                                    * for uw_scroll_resume -- the shell holds the game
                                    * meanwhile, as the original's scroll_wait does */
    uint8_t        colour;         /* the text colour (the graphics module's) */
    long           not_carried;
    /* A stopped print's continuation, for a program that waits where the
     * original waits (src/uw_talk.c): what each level of the emit chain
     * had left when the wait stopped it, innermost first, and the wait's
     * kind -- 1 a \P pause (200 ticks), 2 a \p pause (600), 3 a [MORE]
     * from the \m escape, 4 a [MORE] before an open line's advance; a
     * pending piece of kind 5 is a whole later print (hold_waits) --
     * which uw_scroll_resume finishes before emitting the rest. */
    struct { char text[0x800]; uint16_t flag; uint8_t kind; } pending[16];   /* a line is at most 0x800 */
    int            npending, insert_at;
    int            stop_kind;
    uint8_t        more_colour;    /* the colour [MORE] replaced */
    int16_t        echo_x, echo_y;  /* text_window_echo_x: where a field's text starts, and its line */

} uw_scroll;

/* scroll_print(str): -1 unless the game mode mask is 1 or 4,
 * else the current window's line count. */
int16_t uw_scroll_print(uw_scroll *s, const char *text);

/* text_window_select_scroll, _select_conv_npc, _select_conv_menu:
 * the window and the mode, 0, 1 or 2. */
void uw_scroll_select_scroll(uw_scroll *s);
void uw_scroll_select_conv_npc(uw_scroll *s);
void uw_scroll_select_conv_menu(uw_scroll *s);

/* text_window_clear(manage_cursor). */
void uw_scroll_clear(uw_scroll *s, int manage_cursor);

/* scroll_text_input's field: the prompt ">" and the text so
 * far on the current window's line, the line cleared first (the caret
 * with it, which the next caret step puts back). The caller keeps the
 * text and calls this as it changes. */
void uw_scroll_field(uw_scroll *s, const char *text);
/* scroll_text_input, the scroll's line editor: the field after a prompt the
 * caller has printed, holding
 * `initial` SELECTED (the insertion point its length negated: the first
 * character typed replaces it), at most `max` characters (50 at most) and
 * no wider than the window's right less the prompt's end less 20, digits
 * only unless `any`. Its keys: the arrows' codes left (0xa8, 0x162, 0x8f)
 * and right (0xa9, 0x166, 0x91), Home (0x161, 0xa5, 0x8c) and End (0xaa,
 * 0x165, 0x92), Delete (0x164, 0x96), Backspace, 0x16b cutting the text at
 * the point, a printable character inserted there; Enter, Escape and the
 * three buttons (1..3) end it -- Escape putting `initial` back, erasing the
 * field and printing "-", the others leaving the window's x at the point.
 * The caret blinks at the point on a count the original's loop advances
 * once a round, from 3000: up in 2000..3999, at 4000 erased and the count
 * back to 0; that loop runs as fast as its machine, so the rate is the
 * host's, `counts` rounds a step. */
typedef struct {
    char    text[52];
    char    initial[52];
    int     pos;            /* the insertion point; -(length) while the initial text is selected */
    int     max;
    int     any;
    int16_t room;           /* the field's width in pixels */
    int     caret;
} uw_scroll_edit;
/* After the prompt is printed: the field from the window's x, its room
 * the window's right less that x less 20 (the original's prompt width and
 * home x, for a prompt that begins its line). */
void uw_scroll_edit_begin(uw_scroll *s, uw_scroll_edit *e, const char *initial, int any, int max);
/* A key's code; 0 while the input goes on, else the code that ended it. */
int  uw_scroll_edit_key(uw_scroll *s, uw_scroll_edit *e, int code);
void uw_scroll_edit_caret(uw_scroll *s, uw_scroll_edit *e, int counts);
/* scroll_text_input's entry, after its prompt is printed: text_window_echo_x
 * recorded as the window's x, where uw_scroll_field draws the text from
 * and the line it draws it on. */
void uw_scroll_field_begin(uw_scroll *s);
/* scroll_echo_number: the field's line erased from the echo x,
 * the window's x put back there, and the number printed. */
void uw_scroll_echo_number(uw_scroll *s, int n);
/* scroll_redraw_yes_no, for any text: the same erase and the
 * text printed from the echo x. */
void uw_scroll_echo_text(uw_scroll *s, const char *text);

/* A print stopped at a wait (stop_at_wait) taken up again, the wait over:
 * [MORE]'s line cleared or the pause's end, then what the print had left.
 * 1 when the print finished, 0 when it stopped at a further wait. */
int uw_scroll_resume(uw_scroll *s);

#endif
