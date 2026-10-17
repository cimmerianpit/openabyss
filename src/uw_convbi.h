/* SPDX-License-Identifier: MIT */
/* The conversation builtins a CALLI reaches, by the import name the
 * conversation binds to its operand. Ported from their instructions.
 *
 * A builtin is handed the stack slot of its argument count; each argument
 * below it is an INDEX into the VM's memory (the conversation pushes
 * addresses with PUSHI_EFF), so every builtin reads `mem[top[-k]]` -- through
 * conv_mem_get in the original. The first argument pushed is the deepest.
 */
#ifndef UW_CONVBI_H
#define UW_CONVBI_H

#include "uw.h"
#include "uw_convvm.h"
#include "uw_strings.h"
#include "uw_motion.h"
#include "uw_scroll.h"

/* A variable the conversation imports: its name and its index in the VM's
 * memory (the import record's +0x1a). */
typedef struct { char name[24]; uint16_t addr; } uw_convvar;

typedef struct {
    uint8_t     *ds;           /* the game's data segment: player record, RNG, ctype */
    uw_motion   *level;        /* the level segment's objects, over the same ds */
    const uw_convvar *vars;    /* the conversation's variable imports */
    size_t       nvars;
    uw_strcache *strings;      /* get_string's blocks; the string builtins need it */
    char         input[0xa0];  /* the typed answer's buffer */
    /* time(0)'s low word, which barter_item_price reseeds rt_rand from: the
     * host's clock, an input like the player's answers. */
    uint16_t     clock_low;
    long         not_carried;  /* builtins called that are not ported */
    uw_convvm   *vm;           /* the machine of the builtin being called */
    /* conv_line_buffer: the last line a text builtin
     * composed for scroll_print -- the one output of the text path a later
     * state still holds. */
    char         line[0x800];
    uw_scroll   *scroll;       /* the text windows the lines print to; NULL, not printed */
    /* babl_display: the listed options' texts, which converse_event_loop
     * prints again when a print has cleared the menu */
    char         menu_text[16][0xa0];
} uw_convbi;

/* The builtin `name` for a VM stopped at its CALLI: its result, and *ported
 * cleared (and not_carried counted) when it is not one this module carries. */
int16_t uw_convbi_call(uw_convbi *b, uw_convvm *vm, const char *name, uint16_t *top, int *ported);

/* conv_expand_string: `src` itself when it holds no '@', else a
 * malloc'd expansion the caller frees. */
char *uw_convbi_expand(uw_convbi *b, uw_convvm *vm, char *src);

/* The tail of babl_ask once the player has typed `typed`: the
 * text copied into `input`, registered in block 0x7c the first time (its id
 * kept) and re-registered only when the id has come back empty.
 * Returns the id, which is what the builtin answers. */
uint16_t uw_convbi_babl_ask(uw_convbi *b, const char *typed);

/* Which side spoke last, as the text builtins leave it once their
 * text is out: conv_bi_say (SAY_OP's) clears it, conv_bi_respond
 * (RESPOND_OP's) and conv_print_npc_line -- conv_menu_choose's echo of the
 * chosen option -- set it. conv_start holds the ended conversation open for
 * 500 ticks when it is set. */
void uw_convbi_spoke(uw_convbi *b, int player);

/* The text builtins' lines as they compose them in `line` before printing:
 * conv_bi_say "\\P" text "\n", conv_bi_respond text "\n" (both given the
 * string SAY_OP or RESPOND_OP expanded), and conv_print_npc_line's echo of a
 * chosen option, "\\1" text "\\0\n". Each also marks who spoke, and prints
 * the line through `scroll` when there is one: say and the echo in the NPC's
 * window, respond in the menu's -- the echo after conv_menu_choose
 * has closed the menu. */
void uw_convbi_say(uw_convbi *b, uw_convvm *vm, uint16_t string_id, int respond);
void uw_convbi_menu_echo(uw_convbi *b, uw_convvm *vm, uint16_t string_id);

/* What babl_menu and babl_fmenu do before they
 * wait in conv_menu_run: the options listed -- fmenu's those whose flag word
 * is nonzero -- their ids in the option table from index 1, the
 * conversation menu window cleared and each option printed as "N. text",
 * the rows each takes recorded, and the menu flags. `top` is
 * the CALLI's argument slot. */
void uw_convbi_menu_prints(uw_convbi *b, uw_convvm *vm, uint16_t *top, int fmenu);

/* conv_menu_run, when a print has cleared menus_active while
 * the menu waits -- a look at the trade table or the pack: after its
 * scroll_wait (the caller's), the menu window cleared and each option
 * again as "N. text\n" from babl_display's copies, the scroll selected,
 * the flag set. The rows' table is left as the listing made it. */
void uw_convbi_menu_relist(uw_convbi *b);

/* conv_export_npc_vars, what conv_start does when the run ends:
 * the talker's variables written back into its object and the player's into
 * the record. Returns 1 when npc_attitude came back 0. */
int uw_convbi_export_npc_vars(uw_convbi *b, uw_convvm *vm, uint16_t talker);

/* A conversation's opening, what runs before conv_vm_run's first
 * instruction. barter_setup, from converse_draw_screen: the
 * trade slots and flags cleared, the price caches -1, rt_srand from the
 * talker's object index, four jittered traits from its critter row less or
 * plus the Avatar's charm, the likes and dislikes cleared, and rt_srand
 * from time(0) -- `clock_low`. conv_bind_npc_vars: the thirty-one
 * variables conv_set_variable writes by the names in the data segment. */
void uw_convbi_barter_setup(uw_convbi *b);
/* converse_draw_screen's drawing, into the level's screen: see
 * uw_convbi.c. draw_screen is what comes before the windows' clears, draw_heads
 * what comes after; `data_dir` holds the GR files and fonts. */
void uw_convbi_draw_screen(uw_convbi *b, const char *data_dir);
void uw_convbi_draw_heads(uw_convbi *b, const char *data_dir);
void uw_convbi_bind_npc_vars(uw_convbi *b, uw_convvm *vm, uint16_t talker);

/* conv_bi_end_barter, when the conversation screen comes down with
 * a barter running: whatever is still in the player's slots set
 * down at the player's feet and whatever is in the NPC's at the NPC's,
 * object_place_at_own_coords(owner, item, 5, 0), one slot pair at a time;
 * the slots themselves are not cleared. */
void uw_convbi_end_barter(uw_convbi *b);

/* STRCMP_OP's comparison (conv_op_strcmp): the two strings
 * expanded and strcmp'd, case and all -- nonzero when they differ. */
int uw_convbi_strcmp_ids(uw_convbi *b, uw_convvm *vm, uint16_t below, uint16_t top);

#endif
