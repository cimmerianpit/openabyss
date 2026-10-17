/* SPDX-License-Identifier: MIT */
/* THE BOOT: the game's memory built from UW.EXE and DATA/ alone, as
 * game_init, new_game, the level's load and
 * dungeon_draw_main_screen leave it.
 *
 * What the original has in memory when the dungeon's first pass runs, and
 * where this gets it:
 *
 *   the data segment    UW.EXE's image of 6aac with the linker's relocations
 *                       applied at the runtime load segment (the 153 far
 *                       pointers in it, +0x824), then the loaders' writes:
 *                       OBJECTS.DAT's sections and COMOBJ.DAT
 *                       (obj_properties_load), ALLPALS.DAT, CMB.DAT
 *                       (cmb_load), gr_load_all's texture base and
 *                       drawlist_reset's labels (through uw_scene), the
 *                       input tables (input_tables_init, then the bindings
 *                       debug_bind_key, dungeon_mode_setup, view_set_viewport,
 *                       inventory_panel_init, dungeon_bind_hotspots and
 *                       parse_command_line make, on the near heap where the
 *                       original's malloc put them), cursor_init and the
 *                       cursor regions the panels register, motion_filters_init,
 *                       dungeon_mode_setup's stores, player_init, the level's
 *                       load (uw_motion_level_load over a copy of LEV.ARK,
 *                       SAVE0's) and place_player_in_tile at level 1's start
 *   the level segment   LEV.ARK's first block, through the port's level load
 *   the key segment     UW.EXE's image of 6624 -- key_char_table at 0x10 --
 *                       with the driver's state bytes clear
 *   the element manager panel_build_elements' records
 *   the screen          MAIN.BYT, the panels drawn over it, the cursor shown
 *   the palette         PALS.DAT's first
 */
#ifndef UW_BOOT_H
#define UW_BOOT_H

#include "uw_motion.h"
#include "uw_scroll.h"
#include "uw_scene.h"
#include "uw_strings.h"
#include "uw_chargen.h"

typedef struct {
    char     dir[512];
    /* ---- the segments the motion context points at ---- */
    uint8_t  ds[0x10000], lseg[0x10000], keys[0x10000], ext[0x10000], grid[0x5000];
    uint8_t  palette[768], elem[0x612], screen[64000], written[64000];
    uint8_t  vram[0x40000], imgheap[0x10000];
    uint8_t  art_size[0x400], obj_art_size[0x400];
    uw_blob  shades, font_small, font, font_italic, terrain, grave, weapons_dat, weapons_gr, weapons_cm;
    uw_strings strings;
    int      have_strings;
    uw_scroll scroll;
    uw_ark   ark;                  /* SAVE0's LEV.ARK: DATA's copy, in memory */
    int      have_ark;
    int      program;              /* the program's own boot (game_main): game_init's
                                    * player_init(0) rather than new_game's (1) */
} uw_boot;

/* The character generation's answers (chargen_run_steps): the
 * eight steps' choices as indices into CHRGEN.DAT's lists -- sex 0 male 1
 * female, handedness 0 left 1 right, class 0..7 (Fighter, Mage, Bard,
 * Tinker, Druid, Paladin, Ranger, Shepherd), the skill choices the class's
 * SKILLS.DAT records offer in turn (each an index into that record's list),
 * portrait 0..4, difficulty 0 standard 1 easy, and the name (up to 29
 * characters). */
typedef struct {
    int  sex, handedness, cls, portrait, difficulty;
    int  skill[5];
    char name[30];
} uw_character;

/* Open the game at `dir` (UW.EXE, DATA/) and build a new game's memory into
 * `b`, pointing `m` at it: the Avatar as new_game makes one -- player_init(1)
 * and the character generation's result for `ch` (NULL: a right-handed male
 * Fighter with the first choices, named "AVATAR"), the attributes rolled from
 * SKILLS.DAT's class row and its pool, the skills granted through
 * player_raise_skill -- standing on level 1's start tile facing north, at
 * `clock` on the 256 Hz counter; `ark_path` is the LEV.ARK the new game's
 * SAVE0 copies, DATA's when NULL (a staged copy for tests); `start_x`,
 * `start_y` the tile the Avatar is placed in, level 1's start (32, 2) when
 * either is negative. `scene` is an opened scene (uw_scene_open), whose loaders'
 * writes to the data segment the boot copies and whose level entry it
 * makes. False when a file is missing. The boot is large: allocate it. */
bool uw_boot_new_game(uw_boot *b, uw_motion *m, uw_scene *scene, const char *dir, const uw_character *ch,
                      uint32_t clock, const char *ark_path, int start_x, int start_y);

/* new_game's two halves, for a program that runs the CHARACTER GENERATION
 * SCREEN (src/uw_chargen_ui.c) where the original runs it -- inside
 * new_game, between player_init(1) and the rest. `begin` leaves the memory a
 * generation runs over: everything above, the level loaded and the Avatar's
 * record blank. `apply` is the generation's result for a sheet of answers
 * (what uw_boot_new_game puts between the halves). `finish` is
 * player_state_recalc on: the Avatar placed, the weapons' tables, the clock
 * and the main screen. */
bool uw_boot_new_game_begin(uw_boot *b, uw_motion *m, uw_scene *scene, const char *dir,
                            uint32_t clock, const char *ark_path);
bool uw_boot_apply_character(uw_motion *m, const char *dir, const uw_character *ch);
/* new_game's player_init(1) alone: the record blanked for a generation run
 * again -- the title screen's Create Character after one was abandoned. */
void uw_boot_player_init(uw_motion *m);
/* weapons_load_anim for the ready weapon and weapons_load_colourmap: the
 * tables the first-person weapon draws from -- new_game's and a restored
 * game's (main_menu_save_list runs the colourmap's load after the restore). */
void uw_boot_weapons_load(uw_motion *m, const char *dir);
/* The scene's entry for the level the game is on, and the loader's words
 * the data segment takes from it (0x7192): what the boot's finish makes for
 * level 1, and a game restored from the title screen needs for its own. */
void uw_boot_level_scene(uw_boot *b, uw_motion *m, uw_scene *scene);
/* A test's staging after the finish: the game moved to `level` at tile
 * (x, y) -- the level loaded as level_change loads it, the record's level,
 * the Avatar placed, the scene's entry -- as `--level` stages a run on
 * another level. */
bool uw_boot_go_to_level(uw_boot *b, uw_motion *m, uw_scene *scene, int level, int x, int y);
bool uw_boot_new_game_finish(uw_boot *b, uw_motion *m, uw_scene *scene, const char *dir,
                             uint32_t clock, int start_x, int start_y);

/* The generation's arithmetic, which the screen runs step by step and
 * uw_boot_apply_character runs at once:
 * the class's attributes rolled and its pool spent; the class's next skill
 * hold -- `pos` walked over the five, `list` taking the fixed ones and
 * `offer`/`offered` the choice a longer record makes, 1 while one is
 * offered; and the list's skills granted from `from`, how many. */
void uw_chargen_roll_attributes(uw_motion *m, const uw_skills *sk);
int  uw_chargen_skill_choices(uw_motion *m, const uw_skills *sk, int *pos, uint8_t *list,
                              const uint8_t **offer, int *offered);
int  uw_chargen_apply_skills(uw_motion *m, int from, const uint8_t *list);
/* chargen_run_steps' "keep this character?" answered yes:
 * player_recompute_maxima(1) again, now that the class's skills are
 * granted -- roll_attributes' own ran before them, so the mana a Mana
 * skill gives only counts from here. */
void uw_chargen_keep(uw_motion *m);

/* dungeon_draw_main_screen and dungeon_mode_enter's drawing:
 * MAIN.BYT onto the screen, the panel elements built and drawn
 * (panel_build_elements), the mode panel's image and the paperdoll, the
 * scroll's parchment, the cursor shown, and events 0x7dfe posted -- what
 * mode 1's enter draws, at a new game and on the return from a
 * conversation. False when MAIN.BYT is missing. */
bool uw_boot_draw_main_screen(uw_motion *m, const char *dir);

/* DATA/UW.CFG as snd_read_cfg reads it, "%d %d %x %d" a line:
 * the music driver's id and its IRQ, I/O port and DMA, then the digital
 * driver's. The ids index the driver tables in the data segment -- music 1
 * pcspkr, 2 adlib, 3 sbfm, 4 sbpfm, 5 pasfm, 6 mt32mpu; digital 1 sbdig,
 * 2 sbpdig, 3 pasdig -- and 0 is none; the shipped file, not yet through
 * INSTALL, says 0 twice. A missing file leaves them all 0 (-1 the
 * triples) and answers false, which load_uw_cfg does not mind. */
typedef struct {
    int music, music_hw[3];
    int digital, digital_hw[3];
} uw_config;
bool uw_boot_read_config(const char *dir, uw_config *c);

#endif
