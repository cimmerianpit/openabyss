/* SPDX-License-Identifier: MIT */
/* THE PLAYER'S FRAME: dungeon_frame_tick, the frame timer the
 * dungeon mode runs every pass, and what it drives -- player_frame_update,
 * player_physics_step, and below them the motion
 * integrator the creatures share (motion_run) -- ported over the
 * game's own memory, as the draw list's producer is: `ds` is the data
 * segment and `lseg` the level segment, both 64K images, and every
 * global keeps its address.
 *
 * Built in stages; what a stage does not yet carry is counted in
 * `not_carried`, never silently skipped. */
#ifndef UW_MOTION_H
#define UW_MOTION_H

#include "uw.h"
#include "uw_strings.h"

struct uw_ail;
struct uw_sounds;
struct uw_bank;

struct uw_scroll;

typedef struct uw_motion {
    uint8_t *ds;             /* 6aac, 64K */
    uint8_t *lseg;           /* the level segment, 64K (tiles at +4) */
    const uint8_t *keys;     /* the segment input_key_state's far pointers
                              * address, 64K; NULL when not given */
    const uint8_t *keys_b;   /* input_key_state's 0x80 bytes at the second
                              * state: the keys a held test samples live, as
                              * `buttons` is the mouse's; NULL for `keys` */
    uint8_t key_ring[0x40];  /* kbd_isr's ring, as the
                              * presses kbd_read_scancode lets through: a make
                              * code whose key was up, its key_char_table
                              * index (| 0x80 with shift) -- the host drops
                              * the typematic repeats the driver drops */
    uint8_t key_ring_rd, key_ring_wr;
    uint8_t *ext;            /* the pathfinder's segments: 64K from runtime
                              * paragraph 5e01 (the line walk's step buffer),
                              * the path pool at +0x100, the search's two
                              * frontiers at +0x2c0 and +0x340, atk_charge's
                              * wind-up table at +0x3c0 (the image's; the
                              * boot loads it); NULL when not captured */
    uint8_t *grid;           /* pathfind_between_tiles' node grid, 0x5000
                              * bytes (runtime 476f). It clears the grid
                              * before a search and the renderer's work
                              * buffer overlaps it, so it is scratch: no
                              * state holds what a search left there */
    uint8_t *palette;        /* the 768-byte working palette; NULL when not
                              * given */
    uint8_t *elem;           /* the screen-element manager's segment
                              * (runtime 5c9b), 0x610 bytes: 64 records from
                              * +8, the four groups' dirty lists at 0x408 and
                              * visible lists at 0x508, the scan at 0x608;
                              * NULL when not captured */
    uint16_t elem_any_dirty; /* the word at the start of the program's first
                              * code segment */
    uint8_t *screen;         /* the screen page elem_flush draws into, 320 x
                              * 200 palette indices with the top row first;
                              * NULL when pixels are not drawn */
    uint8_t *screen_written; /* a byte a pixel, set where the port drew */
    const uint8_t *(*art)(void *user, uint16_t id, int *w, int *h);
                             /* an art id's image from 0x2000 (the ones
                              * gr_load_all decodes into video memory), or
                              * NULL */
    void    *art_user;
    const uint8_t *(*gr_file)(void *user, const char *name, int index, int *w, int *h);
    const uint8_t *(*gr_entry)(void *user, const char *name, int index, size_t len);
                             /* a .GR entry's raw bytes, for the file whose
                              * entries have no header: PANELS.GR, whose
                              * three panel backgrounds and edge-on card the
                              * panel flip blits at sizes of its own */
                             /* a GR file's image by name and index, for
                              * the art loaded over art ids at run time --
                              * the paperdoll's body and armour over INV.GR's
                              * first six; NULL when not given */
    uint8_t *panel_ems;      /* the panel flip's three one-page EMS handles
                              * (the work page, the new panel's, the scaled
                              * frame's), 0x4000
                              * bytes each, the host's: with them the flip
                              * is the eight scaled frames the original
                              * plays with EMS; NULL, the three-frame flip
                              * it plays without */
    uint8_t *vram;           /* video RAM from plane address 0, 0x40000 bytes
                              * four to a plane address:
                              * where imgbuf_capture saves an element's
                              * background and imgbuf_restore takes it from;
                              * NULL when not given */
    uint8_t *imgheap;        /* the saved-image heap's segment, 0x10000
                              * bytes: 13-byte records from 0x14 to the end
                              * pointer at 0x12, each a handle -- the save
                              * area's plane address -- and the rectangle
                              * captured there */
    uint16_t cursor_areas[2]; /* gfx_colour_mode_areas' entries 0x100 and
                              * 0x101: the save area --
                              * a plane address, cursor_init's image buffer
                              * -- cursor_draw saves the background under the
                              * cursor into and cursor_erase restores it
                              * from; 0 when not given */
    uint16_t farheap[3];     /* Turbo C++'s far heap: __first, __last and
                              * __rover, the words in the runtime library's
                              * code segment that farmalloc keeps */
    uint8_t  farheap_given;  /* farheap was given */
    uint8_t  view_unknown;   /* the view was presented in the pass where the
                              * port does not draw it: over the view the
                              * cursor's save area is what cursor_blit_in_view
                              * saved from the viewport */
    long     pixels_not_drawn; /* draws and restores the port does not make */
    uint8_t  span_variant;   /* gfx_span_variant: gr_draw_art skips colour 0 */
    uint8_t  clip_foot;      /* the cursor's blit: its window clip keeps the screen's last line out */
    const uint8_t *font_small; /* FONT4X5P.SYS, which the inventory's stack
                              * counts are drawn in; NULL when not given */
    size_t   font_small_size;
    const uint8_t *font_italic; /* FONT5X6I.SYS, which the stats panel's
                              * header lines are drawn in; NULL when not given */
    size_t   font_italic_size;
    const uint8_t *font;     /* the font font_open last read, as the pair
                              * carries it (FONT); NULL when not given */
    size_t   font_size;
    const uint8_t *art_size; /* width and height, a byte each, of the images
                              * art ids 0x1000.. name: BUTTONS.GR, CURSORS.GR
                              * and 3DWIN.GR in the order gr_load_all loads
                              * them after gr_id_base_1000 (the cursor shapes
                              * among them); NULL when not given */
    uint16_t art_size_count;
    const uint8_t *obj_art_size;
                             /* the same of OBJECTS.GR's images, the object
                              * art ids below 0x1000 (gr_load_objects); NULL
                              * when not given */
    uint16_t obj_art_size_count;
    const uint8_t *shades;   /* SHADES.DAT: twelve bytes a light level, six
                              * words; NULL when not given */
    uw_strings *strings;     /* STRINGS.PAK, for print_message */
    const uint8_t *grave;    /* GRAVE.DAT: a cutscene number an epitaph
                              * (look_at_scenery), 512 bytes; NULL when not
                              * given, which counts a gravestone's look */
    size_t   grave_size;
    const uint8_t *weapons_dat; /* WEAPONS.DAT and WEAPONS.GR for
                              * weapons_load_anim when a weapon of another
                              * kind is readied: the frames' positions and
                              * their lengths; NULL when not given, which
                              * counts the load */
    size_t   weapons_dat_size;
    const uint8_t *weapons_gr;
    size_t   weapons_gr_size;
    const uint8_t *weapons_cm; /* WEAPONS.CM, the arm's two colour maps */
    size_t   weapons_cm_size;
    /* The sound (src/uw_motion_sound.c): the AIL driver the music and the
     * effects go to (src/uw_ail.c), which the host ticks at 120 Hz and
     * plays through its synthesiser; SOUNDS.DAT; the timbre bank the
     * driver is fed from (UW.AD for the AdLib); and the tracks, SOUND\AWnn
     * by track number, as music_read_xmi_file reads them. With no driver
     * the DS flags say none is available, and nothing is asked of it. */
    struct uw_ail *ail;
    const struct uw_sounds *sounds;
    const struct uw_bank *timbre_bank;
    const uint8_t *xmi[16];
    size_t   xmi_size[16];
    /* What the game asked of the renderer that the data segment does not
     * hold -- the rasteriser's texture masks, the shading ramp, the
     * floor textures, the working palette -- for the host's scene to take
     * up; the host clears each as it does. */
    struct {
        int      masks;      /* view_effect_warp: `mask` into the six texture
                              * records' masks (record i's word +6) */
        uint16_t mask[6];
        int      shade;      /* shade_reload_or_scramble: 1 the ramp blanked, 2
                              * reloaded from the file (a light-level change
                              * into or out of 5 reloads too) */
        int      floors;     /* apply_maze_texture: the floor textures again,
                              * from floor_texture_assign */
        int      palette;    /* palette_load(n): n + 1 */
    } render;
    struct uw_scroll *scroll;/* the text windows messages print to
                              * (src/uw_scroll.c); NULL when not given */
    uint32_t clock;          /* the 256 Hz clock at the frame (*clock_ptr) */
    uint32_t clock_first;    /* dungeon_frame_tick's first reading of it, the
                              * elapsed time's, when the timer ticked before
                              * the reading the stamp takes; 0 when the same */
    uint16_t buttons;        /* the mouse buttons INT 33h reports to a sample */
    const uint8_t *(*pick_map)(void *user, uint8_t *ds);
                             /* view_refresh_pick_map: the viewport buffer
                              * as the replay leaves it (4f4b, row r of the
                              * view at 2 + r * (view_width + 2), row 0 the
                              * bottom), its writes to `ds` made; NULL when
                              * not given */
    void    *pick_user;
    void   (*view_hook)(void *user, struct uw_motion *m, int kind);
                             /* the host's views from inside a call, drawn
                             * and shown before it returns: kind
                             * UW_VIEW_CAMERAS_FADE (cameras_fade: the view
                             * faded over to the free camera, held while a
                             * button is down, faded back) and
                             * UW_VIEW_VORTEX_FRAME (establish_view with the
                             * vortex's eye); NULL: not shown, counted */
    void    *view_user;
    uint16_t view_wait;      /* the return address a view action's release
                              * wait begun in the pair stands at, for its end
                              * (uw_motion_view_wait_end); 0 none */
    long     not_carried;    /* calls into what is not ported yet */
    uint8_t  screen_flash;   /* screen_show_frame is due: the view cleared to
                              * `screen_flash_colour` and presented for one
                              * frame (src/uw_screen.h), which the frame
                              * cannot do because it presents. The host runs
                              * it and lets the next refresh draw over it */
    uint8_t  screen_flash_colour;
    uint8_t  screenshot;     /* screenshot_write_gif asked for (Alt-q): the
                              * host writes the screen through the working
                              * palette under the first free uwpicNNN name
                              * (three octal digits, as the original counts) */
    uint8_t  screen_fade;    /* screen_fade_out (bit 0) and screen_fade_in
                              * (bit 1) are due, for the host to run over the
                              * view (src/uw_screen.h): the teleport raises
                              * both around a level change and player_sleep
                              * around a night. The frame cannot run them
                              * itself -- each is a loop that presents -- so
                              * it says they happened and the shell plays
                              * them out before and after its next refresh,
                              * which is when the departure's and arrival's
                              * pixels are the ones on the screen. Each
                              * stands for the view_rebuild_and_draw the
                              * original runs before it too. Bit 2 is
                              * view_rebuild_and_draw and screen_present with
                              * no fade (a dreamless night's end): the view
                              * refreshed */
    uint16_t debris_bp;      /* object_damage_debris's frame, when the caller
                              * of apply_damage knows it (damage_objects_in_tile);
                              * 0 counts the debris' placement as not carried */
    uint16_t use_bp;         /* object_use_dispatch's frame, when its caller
                              * knows it (action_use); 0 counts a used thing's
                              * trigger runs as not carried */
    long     physics_steps;  /* player_physics_step runs */
    uint8_t  drag_wait;      /* paperdoll_click stands in cursor_wait_for_drag:
                              * the button down on a slot's thing (the shell
                              * ends it: uw_motion_inventory_drag past six of
                              * cursor movement, uw_motion_inventory_click_end
                              * on the release) */
    uint8_t  view_drag;      /* the drag_wait is action_look's (the default right
                              * click): uw_motion_view_drag on a drag,
                              * uw_motion_view_click_end on the release */
    uint8_t  drag_param_set; /* drag_param holds paperdoll_click's argument */
    int16_t  drag_param;
    uint8_t  options_open;   /* panel_button_click's options button: the panel
                              * opens once the click's wait ends (the shell's
                              * src/uw_options.c); or options_open_from_key's
                              * Ctrl key, its code in options_key */
    uint16_t options_key;
    uint16_t stack_ask;      /* inventory_split_stack's "Move how many? " is up
                              * for this object (the shell answers it:
                              * uw_motion_stack_answer); 0 none */
    uint8_t  stack_ask_path; /* 1 action_pickup's, 2 paperdoll_click's drag, 3 barter_slot_click's */
    int16_t  stack_ask_slot; /* the drag's slot */
    uint16_t talk_object;    /* action_talk's object: conv_begin_with_object
                              * runs on it once the click's release wait ends
                              * (the shell's src/uw_talk.c); 0 none. Goal 10's
                              * creature_goal_talk leaves it here too, from
                              * inside the frame, when a creature hails the
                              * Avatar itself */
    uint8_t  talk_object_temp; /* talk_object was made for the conversation
                              * alone (conv_begin_with_door's item 0x40) and
                              * is freed when it ends */
    uint8_t  map_open;       /* use_readable of the map (item 0x13b): game
                              * mode 2, the automap screen (src/uw_automap.c) */
    uint8_t  death;          /* player_death stands at perform_pending_teleport:
                              * the silver tree's teleport is pending, and the
                              * host runs it and then uw_motion_death_teleported
                              * with its result */
    uint8_t  ending;         /* game_ending_sequence's Slasher stage stands at
                              * perform_pending_teleport: the teleport to level 9
                              * is pending, and the host runs it and puts the fade
                              * flag back (uw_motion_ending_teleported) */
    uint8_t  ending_show;    /* the ending proper is due: cutscene 1, win1.byt with
                              * palette 7, a key, win2.byt with the statistics
                              * (uw_victory_draw), a key, game_return_to_menu(0) --
                              * the host's */
    uint8_t  return_to_menu; /* game_return_to_menu(show - 1) is due: 1 plain
                              * (the ending), 2 with the death's cutscene 0x103
                              * -- the host's, around the menu (uw_motion_leave_game
                              * and uw_motion_enter_game) */
    uint8_t  cutscene_due;   /* cutscene_play(cutscene_wanted) is due: game
                              * mode 0x10, which the host plays out through
                              * src/uw_cutplay.c once the pass is over, as the
                              * original plays it inside the call (the shell's
                              * cutscene mode; a pair harness counts it) */
    uint16_t cutscene_wanted;
    uint16_t cutscene_arg;   /* cutscene_open_file's word: with 0x100 and
                              * 0x101 (the level's and the gravestone's
                              * cutscene) the script's words at 4, 6 and 12
                              * are patched to it before it plays; 0 plain */
    uint8_t  barter_wait;    /* barter_slot_click stands in a wait (src/uw_motion_barter.c):
                              * 1 cursor_wait_for_drag (with drag_wait), 2 the
                              * release wait after the lift, 3 the placing's */
    uint8_t  barter_whose;   /* the click's side, 1 the player's */
    int16_t  barter_slot;
    uint8_t  barter_loaded;  /* the cursor loaded at entry or by the lift */
    uint8_t  menu_choose;    /* conv_menu_choose asked for (the shell's src/uw_talk.c) */
    int16_t  menu_option;    /* its argument: 0 the option under the cursor */
    /* ---- the prompts a use handler stands in (src/uw_motion_prompt.c) ---- */
    uint8_t  mantra_ask;     /* chant_mantra's "Chant the mantra: " is up:
                              * scroll_text_input's field of up to ten
                              * characters, which the host edits and answers
                              * with uw_motion_mantra_answer */
    uint8_t  yesno_ask;      /* scroll_ask_yes_no is up: Yes or No shown,
                              * yesno_value the current one; y, n, Enter,
                              * Escape and the buttons the host's
                              * (uw_motion_yesno_redraw as the answer moves),
                              * the answer uw_motion_yesno_answer's. The value
                              * says whose question it is: 1 item_repair's, 2
                              * action_look's "disarm it?" */
    uint8_t  yesno_value;
    uint16_t prompt_obj;     /* the asker's object and skill, for its rest */
    int16_t  prompt_skill;
    uint8_t  cutscene_then;  /* what a handler left to run when the cutscene it
                              * asked for ends (uw_motion_cutscene_finished): 1
                              * item_repair's apply */
    uint8_t  instrument;     /* play_instrument stands in its key loop: which +
                              * 1 (1 the mandolin, 2 the flute); the host hands
                              * it keys (uw_motion_instrument_key), polls it
                              * (uw_motion_instrument_tick) and Escape ends it
                              * (uw_motion_instrument_end) */
    uint8_t  instrument_usable, instrument_channel, instrument_sounding, instrument_playing, instrument_ring_at;
    uint8_t  instrument_ring[16];
    uint32_t instrument_started;
    uint16_t delay_ticks;    /* delay_ticks(n) a handler ran: the host lets that
                              * many ticks pass after the pass */
} uw_motion;

enum { UW_VIEW_CAMERAS_FADE = 1, UW_VIEW_VORTEX_FRAME = 2 };

/* dungeon_frame_tick: the clock since the last frame's stamp, capped at 0x40
 * ticks; the turn phase and the effect ticks from its 16- and 64-tick
 * boundaries; play time; the stamp; player_frame_update. */
void uw_motion_frame(uw_motion *m);

/* dungeon_tick_update, the handler the pass runs next: the
 * idle swing's button sample, the panels' values, the palette's cycle, the
 * 256 Hz clock's whole seconds into the game step's accumulator. It reads
 * the clock again, later than the frame's stamp: `clock` is that reading.
 * A nonzero `stop` is an offset in the handler: the
 * return address of the call a saved state was inside, or the instruction
 * after the one it stood at. The port runs the calls that return before it. */
void uw_motion_tick_update(uw_motion *m, uint32_t clock, uint16_t stop);

/* panel_redraw_dispatch, the pass's next standing handler: the
 * nine panel elements' drawers by their dirty bits -- panel_dirty_bits_2 at
 * once, _1 when the clock's low byte crosses a 32-tick boundary since the
 * last redraw, panel_dirty_bits a 64-tick one with a flask bubble and a
 * dragon flick drawn at random -- as far as they write the data segment.
 * `clock` is the pass's reading. A nonzero `stop` is the return offset of
 * the call a saved state was inside: at 0x06e2, elem_flush, every drawer has
 * run and the clock is not yet recorded. */
void uw_motion_panels_redraw(uw_motion *m, uint32_t clock, uint16_t stop);

/* action_combat: a click in the 3-D view in fight mode, as the
 * view cell 1..9 it falls in -- combat_swing(cell). */
void uw_motion_action_combat(uw_motion *m, int16_t cell);
/* action_combat itself: the cell worked out of the pending
 * event's position in the view, for a host that delivers the click. */
void uw_motion_action_combat_click(uw_motion *m);

/* game_ending_sequence, event 10 (0x400): while
 * trap_pending_code is 0 and the ritual counter is 0, the Slasher stage --
 * object 0x15a made at tile (32, 32) for the fade, message 0x117, the view
 * wiped, the object gone again, the teleport to level 9's (0x1b, 0x17)
 * with the fade-out flag off and the counter 0xff, message 0x118 -- and the
 * host runs the teleport (uw_motion_ending_teleported after); with the
 * code set, the ending proper is the host's (ending_show). */
void uw_motion_ending(uw_motion *m);
void uw_motion_ending_teleported(uw_motion *m);

/* player_death's continuation after the silver tree's teleport:
 * `ok` from perform_pending_teleport -- the resurrection's cutscene 0x102,
 * the view cleared to 0xf1 and the text window cleared, or, with no
 * landing, game_return_to_menu(1). */
void uw_motion_death_teleported(uw_motion *m, int ok);

/* game_return_to_menu in its two halves around the cutscene
 * the host plays between them: `leave` is what runs before -- the cursor
 * bounds reset, with `show` the heading's panel value, the compass and the
 * vitality flask drawn -- and `leave_finish` what runs after: the text
 * window cleared with `show`, the mode's leave handler (dungeon_leave
 * below), the mode fields cleared, game_reset_player_state. The palette's
 * fade and main_menu are the host's. `enter_game` is the way back after
 * the menu: in-game set, mode 1, and the enter handler's bindings and
 * dungeon_mode_enter (the page is the host's, drawn before). */
void uw_motion_leave_game(uw_motion *m, int show);
void uw_motion_leave_game_finish(uw_motion *m, int show);
void uw_motion_enter_game(uw_motion *m, uint32_t clock);

/* The dungeon's pair in event_handlers, which game_change_mode runs around
 * every change of mode -- to the automap, to a conversation, back from
 * them, and after a full-screen cutscene played in the dungeon -- and
 * game_return_to_menu around the menu.
 *
 * `dungeon_leave` is the leave handler, dungeon_leave_handler:
 * viewport_unbind_hotspots -- the view's hotspot unbound and its
 * eight arrow cursor regions removed -- dungeon_mode_leave -- the panel's
 * five hotspots unbound, music_stop -- and panel_mode_restore. The enter
 * handler is dungeon_draw_main_screen: `dungeon_viewport` is
 * its bindings before the page is drawn -- the teardown again and
 * view_set_viewport's viewport_bind_hotspots, the view's hotspot and
 * regions back -- and `dungeon_enter` its dungeon_mode_enter after: the
 * scroll reset, music_resume, the five hotspots bound, the action button
 * pressed again, dungeon_tick_update. The page between them, its fades and
 * the view's refresh, are the host's. */
void uw_motion_dungeon_leave(uw_motion *m);
void uw_motion_dungeon_viewport(uw_motion *m);
void uw_motion_dungeon_enter(uw_motion *m, uint32_t clock);

/* palette_rotate_range(start, count, dir) over the working
 * palette: `count` entries from `start` turned one place, up with `dir`. */
void uw_motion_palette_rotate_range(uw_motion *m, uint8_t start, uint8_t count, int dir);

/* inventory_panel_init's saved backgrounds, off the page the
 * panel's art is on and before anything is drawn over it: the boot's, as
 * dungeon_mode_enter does it for the original. Runs once (0x18a3). */
void uw_motion_inventory_panel_init(uw_motion *m);

/* viewport_unbind_hotspots alone: the view's hotspot and its eight arrow
 * cursor regions given back, for a program that has not been in the
 * dungeon yet. uw_motion_dungeon_viewport puts them back. */
void uw_motion_view_regions_unbind(uw_motion *m);

/* cutscene_play(n), deferred to the host: the number is left in
 * cutscene_wanted and cutscene_due set. A second request before the host
 * took the first is counted, since the original would have played both. */
void uw_motion_cutscene_request(uw_motion *m, uint16_t n);
/* cutscene_open_file(n, arg): the same, with the script's words at
 * 4, 6 and 12 written as `arg` before it plays -- the original patches the
 * file on disk (CUTS/CS400.N00 and CS401.N00 ship with the last value used),
 * the port the bytes it read (uw_cutscene_begin_patched). */
void uw_motion_cutscene_request_numbered(uw_motion *m, uint16_t n, uint16_t arg);
/* The prompts' continuations (src/uw_motion_prompt.c): the mantra typed
 * (empty for Escape), the yes/no question's answer, a cutscene a handler
 * asked for ended, and the instrument's keys, polls and end. */
void uw_motion_mantra_answer(uw_motion *m, const char *typed);
void uw_motion_yesno_answer(uw_motion *m, int yes);
/* scroll_redraw_yes_no: the answer shown again, "Yes"
 * or "No", from the field's start; yesno_value follows it. */
void uw_motion_yesno_redraw(uw_motion *m, int yes);
void uw_motion_cutscene_finished(uw_motion *m);
void uw_motion_instrument_key(uw_motion *m, uint16_t code);
void uw_motion_instrument_tick(uw_motion *m);
void uw_motion_instrument_end(uw_motion *m);
/* For the conversation builtins and the talk: chant_mantra (a shrine talked
 * to), skill_gain (x_skills' 10000) and inventory_remove_quantity
 * (give_ptr_npc), the motion port's. */
void uw_motion_chant_mantra(uw_motion *m);
/* action_look's cursor_wait_for_drag answered (view_drag). */
void uw_motion_view_drag(uw_motion *m);
void uw_motion_view_click_end(uw_motion *m);
int  uw_motion_skill_gain(uw_motion *m, int skill);
/* The sound (src/uw_motion_sound.c): the host's driver attached -- the
 * data segment's driver id, handle and "available" flags as sound_init
 * leaves them -- the settings applied for a new game, and AIL's 16 Hz
 * timer, which ages the effects' slots. */
void uw_motion_sound_attach(uw_motion *m, struct uw_ail *ail, int music_driver);
void uw_motion_sound_enable(uw_motion *m, int music, int effects);
void uw_motion_sound_age_slots(uw_motion *m);
/* play_sound_effect(id, pan, velocity delta), for the modes the
 * host plays -- a cutscene's op15 -- the slot or 0xff */
uint8_t uw_motion_sound_effect(uw_motion *m, int id, int pan, int delta);
/* weapon_composite, which view_present runs, into a 172 x 113
 * viewport with row 0 its bottom -- for a harness to hold against a state */
void uw_motion_weapon_composite(uw_motion *m, uint8_t *buf, uint8_t *bmask);
/* The music calls the other modes make, which the host runs them in:
 * load_xmi(track, 1) (game_init's title theme, 1; cutscene_play's 4 for
 * cutscenes 1..3), music_restart_current (the loops of the menu, the
 * generation screen, the map, a conversation, the options and a
 * cutscene), set_theme_music then sound_update (automap_draw's and
 * converse_draw_screen's 0xd), and pick_level_theme_music (leaving the map
 * or a conversation). */
void uw_motion_music_load(uw_motion *m, int track);
void uw_motion_music_restart(uw_motion *m);
void uw_motion_music_theme(uw_motion *m, int track);
void uw_motion_music_pick_level_theme(uw_motion *m);
/* dungeon_mode_leave's music_stop and dungeon_mode_enter's music_resume,
 * for the modes the host enters and leaves without those handlers */
void uw_motion_music_stop(uw_motion *m);
void uw_motion_music_resume(uw_motion *m);
int  uw_motion_inventory_remove_quantity(uw_motion *m, uint16_t obj, int16_t count);

/* The cursor and the panels as the text windows (uw_scroll.c) use them:
 * cursor_hide and cursor_show as far as the data segment,
 * cursor_overlaps_rect, and panel_set_value. */
/* imgbuf_restore(handle): the rectangle the saved-image heap
 * records for the handle put back from its save area. */
void uw_motion_imgbuf_restore(uw_motion *m, uint16_t handle);
void uw_motion_cursor_shape_push(uw_motion *m, uint16_t shape);
void uw_motion_cursor_shape_pop(uw_motion *m, uint16_t flags);
/* cursor_set_bounds(x0, y0, x1, y1) and cursor_reset_bounds (the whole
 * screen); cursor_move_to(x, y), mouse_putxy, unclamped */
void uw_motion_cursor_bounds(uw_motion *m, int16_t x0, int16_t y0, int16_t x1, int16_t y1);
void uw_motion_cursor_reset_bounds(uw_motion *m);
void uw_motion_cursor_move_to(uw_motion *m, int16_t x, int16_t y);
/* imgbuf_alloc(w, h): a saved-image record, its handle; and
 * imgbuf_capture(handle, x, y, w, h): the screen rectangle
 * (y its bottom row, up) saved into it. */
uint16_t uw_motion_imgbuf_alloc(uw_motion *m, uint16_t w, uint16_t h);
/* imgbuf_free(handle): the record freed, merged with its free neighbours,
 * the block at the top given back; 1 for an unknown handle, 0 after a
 * merge, the handle when nothing merged. */
uint16_t uw_motion_imgbuf_free(uw_motion *m, uint16_t handle);
void     uw_motion_imgbuf_capture(uw_motion *m, uint16_t handle, int x, int y, int w, int h);
/* view_present after the draw list has run: the viewport
 * `view` (172 x 113 in a 320-wide buffer, row 0 the view's bottom) and `mask`,
 * its written pixels, onto the screen with the cursor's handling around it. */
void uw_motion_view_present(uw_motion *m, const uint8_t *view, const uint8_t *mask);
/* The pass's input_tick for a click whose button is up again:
 * mouse_read delivering the pending `code` -- at the latched position, or with
 * the button still `down` at the cursor's -- the
 * hotspot table walked from its last record, and the handler the position
 * and the event's mode mask pick -- print_flask_status and print_time_status
 * ported, view_action_dispatch the pair's action_combat, any other counted. */
void uw_motion_input_click(uw_motion *m, int16_t code, int down);
/* The pass's input_tick for a key: input_poll_source's dirty
 * text window, keyboard_read through keyboard_scan_held
 * taking the key held at scan `index` -- key_scan_index left there,
 * key_repeat_time stamped with the clock -- the event's +4 cleared, and
 * input_dispatch_key: the first of key_binding_count twelve-byte
 * records for `code` whose mode mask meets
 * the event's +8 and whose handler is set, called with its parameter. */
void uw_motion_input_key(uw_motion *m, uint16_t code, uint16_t index);
/* The keyboard driver's ring: kbd_isr's append of a press (`at`, the
 * key_char_table index, | 0x80 with shift), dropped when the ring is full;
 * kbd_read_scancode's take of the oldest, 0 when empty -- which every
 * keyboard_read makes, the held one included; and keyboard_drain,
 * the ring emptied. */
void uw_motion_key_press(uw_motion *m, uint8_t at);
int uw_motion_key_take(uw_motion *m, uint8_t *at);
void uw_motion_keyboard_drain(uw_motion *m);
/* The clicked handler's button-release wait ending in the pair: the
 * handler the latched click finds, from its wait on. */
void uw_motion_input_wait_end(uw_motion *m);
/* The inventory's drag: paperdoll_click on from
 * cursor_wait_for_drag's answer of a drag -- the lift and its release wait
 * begun (DRAG) -- and on from that wait's end, the drop (RELW). */
void uw_motion_inventory_drag(uw_motion *m);
/* view_render's cursor_poll: the button latch a render leaves. */
void uw_motion_cursor_poll(uw_motion *m);
/* The cursor moved by the mouse to (x, y), as cursor_update_position does. */
void uw_motion_cursor_move(uw_motion *m, int16_t x, int16_t y);
/* paperdoll_click past a drag test the button ended (DRGE) */
void uw_motion_inventory_click_end(uw_motion *m);
/* event_dispatch's bounds countdown, once a pass before the input */
void uw_motion_dispatch_countdown(uw_motion *m);
void uw_motion_inventory_release(uw_motion *m);
/* The trade table's drag wait (barter_wait 1), ended as the paperdoll's:
 * the cursor moved six, the lift; the button up before that, the click's
 * look or toggle. And its slots drawn (barter_draw_slot, _frame). */
void uw_motion_barter_drag(uw_motion *m);
void uw_motion_barter_click_end(uw_motion *m);
void uw_motion_barter_draw_slot(uw_motion *m, int whose, int slot);
void uw_motion_barter_draw_slot_frame(uw_motion *m, int whose, int slot);
/* The release that ends a button-release wait, seen by the wait's polls:
 * mouse_button_released 0 (no button down), mouse_button_held as
 * the second state shows it (mouse_sample_buttons), and with it 0 the cursor
 * interrupt's release latched at the cursor (mouse_read's click
 * latch). */
void uw_motion_input_release(uw_motion *m, uint16_t held);
/* A wait's own key (input_wait_button_release's and cursor_wait_for_drag's
 * keyboard_read(1)) handed to cursor_key_move: the keypad glides
 * the cursor while a button is held. */
void uw_motion_cursor_key(uw_motion *m, uint16_t code);
/* A view action's release wait ending in the pair, by the return address in
 * the action handler it stood at: action_use's
 * rest (0x0e49), action_look's end (0x0e35). */
void uw_motion_view_wait_end(uw_motion *m, uint16_t ret);
/* savegame_write's PLAYER.DAT from the data segment and the level
 * segment: the seed, the 210-byte record XOR-encoded (player_save_record),
 * the object count and the packed buffer (savegame_pack). `known` marks the
 * bytes the port can know -- not the packed slot table's entries past 18 or
 * the unused record at buffer index 0, which are what the EMS frame held.
 * Returns the length, 0 when an object on the cursor would be packed (not
 * carried). The data segment's record is updated as the original does. */
size_t uw_motion_save_player_dat(uw_motion *m, uint8_t *out, size_t cap, uint8_t *known);
/* level_save into `ark`, SAVE0's LEV.ARK in memory, after
 * savegame_write's PLAYER.DAT: the player's inventory freed back to the pools
 * and the Avatar unlinked from its tile, then the level block with its
 * trailer to slot level - 1, the active effects to level + 8, the texture
 * map to level + 17 and the automap to level + 26, each through
 * uw_ark_write_block. 1 on success. The segments are left as level_save
 * leaves them before savegame_read(0) reloads the player, which is not
 * carried (and counted). */
int uw_motion_level_save(uw_motion *m, uw_ark *ark);
/* savegame_read(0) after level_save (save_game's tail): the inventory
 * unpacked from the frame back into the pools and the Avatar into its tile,
 * so the game goes on from the save. */
void uw_motion_savegame_reload(uw_motion *m);
/* inventory_split_stack's answer, `n` from the field: 0 or
 * Escape cancels; more than the stack is the stack; the whole stack goes
 * as it is, less has a second object made (obj_alloc, the record copied)
 * with the rest and inserted after the original in its chain, the
 * original keeping `n`; then the action that asked goes on (action_pickup
 * or the paperdoll's lift). `all` answers with the whole stack (the right
 * button), `cancel` with nothing (Escape). */
void uw_motion_stack_answer(uw_motion *m, int n, int all, int cancel);
/* level_load from `ark`, SAVE0's LEV.ARK: the inventory parked
 * (savegame_write(0)), the level block and its effects over the segments
 * (level_block_load), the inventory unpacked into the new pools with
 * player_state_recalc (savegame_read(0)), the texture map with TERRAIN.DAT's
 * words for it (NULL: not carried), the automap cleared and the level's
 * block loaded over it, the creatures' combat flags and the assault record
 * reset. Returns level_block_load's result: 1, or 0 when the effects block
 * is wrong (no textures then), and 0 when the cursor holds an object. */
int uw_motion_level_load(uw_motion *m, const uw_ark *ark, int level, const uint8_t *terrain, size_t terrain_size);
/* savegame_restore_progress once SAVEn is copied into SAVE0:
 * game_reset_player_state, savegame_read of PLAYER.DAT's bytes -- the record
 * decoded and applied, the inventory unpacked -- level_load of the level it
 * names, level_transition_effects(level, 3) and combat_state_restore. 1 on
 * success. */
int uw_motion_restore(uw_motion *m, const uint8_t *player_dat, size_t size, const uw_ark *ark,
                      const uint8_t *terrain, size_t terrain_size);
/* save_or_restore_slot's tail after a restore: dungeon_tick_update
 * at `clock`, the level-entered flag, the movement mode re-applied, the
 * liquid flag set and every event but the first posted. The weapon colour
 * map and the panel's elements are not carried. */
void uw_motion_restore_tail(uw_motion *m, uint32_t clock);
/* Journey Onward's after a restore, which is not the panel's:
 * main_menu_save_list's weapons_load_colourmap and main_menu's weapon_stow
 * -- no tick (the game loop's event 12 is the first), no movement mode, no
 * liquid flag. A save made with the weapon drawn is stowed here, and the
 * stow draws a random number. */
void uw_motion_menu_restore_tail(uw_motion *m);
/* set_render_detail: the record's detail nibble into the
 * renderer's flag and its two emitters. */
void uw_motion_set_render_detail(uw_motion *m);
/* perform_pending_teleport, the pending teleport trap_teleport
 * set: to another level, level_change -- combat_reset,
 * leaving's npc_settle_level (the creatures sent home, the rest settled to
 * the floor, the races' attitudes), level_save into `ark`, level_load and
 * arriving's combat reset -- then find_landing_spot near the destination,
 * strict and then relaxed, and place_player_in_tile. `bp` is the routine's
 * frame. The fades and an arrival callback are not carried. 0 when no
 * landing spot is found (the Avatar's hit points zeroed). */
int uw_motion_pending_teleport(uw_motion *m, uw_ark *ark, const uint8_t *terrain, size_t terrain_size, uint16_t bp);
void uw_motion_cursor_hide(uw_motion *m);
/* objcheck_run, the debug validator the original ships: every
 * object index free or placed exactly once, the two pools' tallies whole.
 * 1 when the level's object lists are sound. */
int uw_motion_objcheck(uw_motion *m);
/* cursor_update_position with no motion from the mouse: the
 * keyboard's glide stepped (cursor_key_move aims it), for the host to run
 * each input poll the mouse did not move in. */
void uw_motion_cursor_glide(uw_motion *m);
void uw_motion_cursor_show(uw_motion *m);
int  uw_motion_cursor_overlaps_rect(uw_motion *m, int16_t x1, int16_t y1, int16_t x2, int16_t y2);
void uw_motion_panel_set_value(uw_motion *m, int8_t element, uint16_t value);
/* gr_draw_art(art, x, y, ...) into the screen when there is one:
 * the image's top row on 199 - y, colour 0 drawn (gfx_span_variant is set
 * only for ids 0x101b..0x101e, which are not drawn: the ids below 0x2000
 * are counted in pixels_not_drawn). */
void uw_motion_gr_draw_art(uw_motion *m, uint16_t art, int x, int y);
/* dungeon_refresh_inventory: with the inventory the panel shown
 * (or the current event's +8 word 4), the body's art loaded again, the
 * paperdoll redrawn and the container corner and slots after it. */
void uw_motion_dungeon_refresh_inventory(uw_motion *m);
/* gfx_draw_string(str, x, y) in `font` (a .SYS file's bytes or
 * the in-memory font: the 12-byte header, then a glyph record a character)
 * and `colour`, into the screen when there is one; at most 0x84 characters.
 * Nothing is drawn unless the text's top and bottom rows and its left edge
 * are on the screen (the right end is not tested), and a character outside
 * 0x20..0x7a is drawn, and advances, as '?'. */
void uw_motion_draw_string(uw_motion *m, const uint8_t *font, size_t font_size, const char *str, int x, int y,
                           uint8_t colour);
/* gfx_blit(x, y, src, h, w, 0, 0): `w` columns of `h` rows of an
 * image `img_w` wide, its top row on 199 - y -- the two size arguments are the
 * height THEN the width (converse_draw_screen's CONVERSE.GR image 0, 94 x 9,
 * goes as 9, 0x5e) -- colour 0 skipped while span_variant is set. */
void uw_motion_blit(uw_motion *m, const uint8_t *px, int img_w, int x, int y, int h, int w);
/* gfx_fill_rect(x0, y0, x1, y1) in `colour`, y0 the top row. */
void uw_motion_fill_rect(uw_motion *m, int x0, int y0, int x1, int y1, uint8_t colour);
/* gfx_string_width: the width bytes of at most 54 characters. */
int uw_motion_string_width(const uint8_t *font, size_t font_size, const char *str);

/* player_frame_update(dt, substeps, still). */
void uw_motion_player_frame(uw_motion *m, uint16_t dt, uint8_t substeps, int still);

/* The level primitives the conversation builtins share (uw_convbi.c):
 * tile_ptr_from_xy, object_remove (0 when removed), creature_set_goal_for
 * (current_npc aimed at `obj` for creature_set_goal, then put
 * back) and player_gain_experience. */
uint16_t uw_motion_tile_ptr(uw_motion *m, uint16_t x, uint16_t y);
uint16_t uw_motion_obj_at(uw_motion *m, uint16_t index);          /* obj_ptr_from_index */
uint16_t uw_motion_deref_link(uw_motion *m, uint16_t link);       /* obj_deref_link */
uint16_t uw_motion_object_remove(uw_motion *m, uint16_t link, uint16_t obj, int force);
void     uw_motion_creature_set_goal_for(uw_motion *m, uint16_t obj, int goal, int gtarg);
void     uw_motion_player_gain_experience(uw_motion *m, int16_t delta);
/* view_build_draw_list's tail: after automap_view_sweep, the
 * tiles it marked on the map for the first time (automap_newly_seen)
 * times the current level, over ten, as experience -- for a player level
 * of 1..15, and not on level 9. The host calls it after each view it
 * draws, with its scene's count (uw_vl.newly_seen). */
void     uw_motion_view_explored(uw_motion *m, long newly_seen);

/* object_find_matching: the first object along the chain from
 * *link of class, subclass and type (0xffff any), searching contents with
 * `recurse`. And the doors: door_open, door_close, door_toggle on the
 * object, at the action target tile. */
uint16_t uw_motion_find_matching(uw_motion *m, uint16_t *link, int recurse, uint16_t cls, uint16_t sub, uint16_t type);
void     uw_motion_door_open(uw_motion *m, uint16_t actor, uint16_t obj);
void     uw_motion_door_close(uw_motion *m, uint16_t obj);
void     uw_motion_door_toggle(uw_motion *m, uint16_t actor, uint16_t obj);

/* object_list_remove through the object pool; and
 * inventory_can_carry: AX, the prospective carried weight's high
 * byte over AL = 1 when object_weight(obj) plus the weight carried is within
 * the capacity. */
void     uw_motion_object_list_remove(uw_motion *m, uint16_t link, uint16_t obj);
void     uw_motion_object_list_insert(uw_motion *m, uint16_t link, uint16_t obj);
void     uw_motion_obj_free(uw_motion *m, uint16_t obj);
uint16_t uw_motion_object_create(uw_motion *m, uint16_t id, int mobile);
/* object_place_at_own_coords: `obj` set down at `owner`'s tile and
 * fine position through object_move_to_coords; `bp` is where the original's
 * frame would put item_fits_in_tile's scratch squares (the stack is in DS). */
int      uw_motion_object_place_at_own_coords(uw_motion *m, uint16_t owner, uint16_t obj, int16_t spread, int keep,
                                              uint16_t bp);
/* spawn_npc_loot: once, unless +0x0d bit 12 says it was. */
void     uw_motion_spawn_npc_loot(uw_motion *m, uint16_t npc);
/* item_enchantment(obj, &effect, &magnitude, &special): 1 when
 * the object carries a spell; a spent linked one may consume an rt_rand. */
int      uw_motion_item_enchantment(uw_motion *m, uint16_t obj, int16_t *effect, int16_t *magnitude, int *special);
/* The placements conv_bi_place_object makes: object_move_to_coords,
 * item_fits_in_tile, object_list_append and
 * placed_object_collision. Each `bp` is the frame the original's
 * would have, whose locals the spatial query borrows. */
int      uw_motion_object_move_to_coords(uw_motion *m, int16_t fx, int16_t fy, int16_t z, uint16_t obj, int16_t spread,
                                         int keep, uint16_t bp);
int      uw_motion_item_fits_in_tile(uw_motion *m, uint16_t id, uint16_t index, int16_t x, int16_t y, int16_t z,
                                     int slope, uint8_t radius, uint16_t bp);
void     uw_motion_object_list_append(uw_motion *m, uint16_t link, uint16_t obj);
uint16_t uw_motion_placed_object_collision(uw_motion *m, uint16_t obj, uint16_t tx, uint16_t ty, int toss,
                                           uint16_t bp);
uint16_t uw_motion_obj_index(uw_motion *m, uint16_t obj);                          /* obj_index_from_ptr */
uint16_t uw_motion_inventory_can_carry(uw_motion *m, uint16_t obj);

#endif
