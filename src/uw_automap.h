/* SPDX-License-Identifier: MIT */
/* THE AUTOMAP SCREEN: game mode 2 -- automap_draw, which the
 * map (item 0x13b) opens when it is used from the inventory, and the
 * drawing under it: automap_render, automap_draw_tiles,
 * automap_draw_tile, automap_draw_tile_wall,
 * automap_draw_door, automap_tint_pixel, the notes
 * (automap_notes_load, automap_draw_notes) and
 * automap_show_level behind the paging buttons; for the shell
 * (src/tools/uwshell.c).
 *
 * WHAT THE MAP IS DRAWN FROM. automap_tiles, a byte a tile,
 * which the view's own sweep writes as the player walks: the raw tile type
 * 1..9 for a tile stood in and automap_tile_codes' 0x0a..0x0f for one the
 * sweep merely revealed. Only the first kind is drawn -- three pixels a
 * tile from automap_tile_patterns, with walls laid along any edge whose
 * neighbour is not mapped floor, lighter where that neighbour is 0x0b (an
 * open tile seen but not walked). The map's one primitive is
 * automap_tint_pixel, which READS the parchment pixel under it and darkens
 * it by a constant plus a random spread, which is where the hand-drawn
 * stipple comes from; a port that fills flat colour looks wrong for a
 * reason nothing else in the code explains.
 *
 * The high nibble's two fields -- the floor's fill and an overlay -- are
 * carried (the door mark, the two palette fills and the dark wash), though
 * a UW1 block never sets them: they are the terrain automap_mark_tile ORs
 * in for UW2's sake. */
#ifndef UW_AUTOMAP_H
#define UW_AUTOMAP_H

#include "uw_motion.h"

/* A note as the level's block holds one: fifty bytes of text, then its
 * place on the map. A record whose x is negative is the delete marker
 * automap_click leaves and automap_notes_save compacts away. */
typedef struct { char text[0x32]; int16_t x, y; } uw_map_note;
#define UW_MAP_NOTES 100

typedef struct {
    uw_motion *m;
    char       dir[512];
    uw_blob    blank;          /* BLNKMAP.BYT, the parchment */
    uw_blob    font_big;       /* FONTBIG.SYS, the level number */
    uw_blob    font_small;     /* FONT4X5P.SYS, the notes */
    uw_blob    font_cur;       /* FONT5X6P.SYS, the font every path leaves loaded: the erase measures in it */
    uw_ark    *ark;            /* the level archive: the blocks and the notes */
    int        level_shown;    /* automap_level_shown */
    int        active;         /* the screen is up */
    int        leave;          /* the player asked to go back to the dungeon */
    uw_map_note note[UW_MAP_NOTES];
    int        nnotes, notes_dirty;
    /* the note editor (automap_click's last region) and the delete before it */
    int        editing, typed_len, deleting;
    int        beeps;          /* do_beep(0x12c, 10)s owed: the host's speaker */
    char       typed[0x32];
    int16_t    note_x, note_y;
    long       not_carried;
} uw_automap;

/* automap_draw's entry: the current level's marks saved into the archive,
 * the screen rendered and the map's palette loaded. False when a file is
 * missing. `ark` is the game's LEV.ARK (the shell's boot copy), whose
 * automap and note blocks the paging buttons read. */
bool uw_automap_open(uw_automap *a, uw_motion *m, const char *dir, uw_ark *ark);
void uw_automap_close(uw_automap *a);

/* input_poll's answer while the map is up: Escape leaves (the caller
 * redraws the dungeon), and a click is automap_click's regions -- the two
 * corners page a level, CLOSE leaves, the button under it begins a delete
 * and anywhere else begins a NOTE, typed until Return or Escape. */
void uw_automap_key(uw_automap *a, uint16_t code);
void uw_automap_click(uw_automap *a, int16_t x, int16_t y);

#endif
