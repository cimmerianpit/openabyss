/* SPDX-License-Identifier: MIT */
/* uwshell.h -- what the program (tools/uwshell.c) and a development
 * harness share: the shell's state, its modes, and the hooks through which
 * the harness drives it.
 *
 * The program alone is the port as a player runs it: the title, the menu,
 * a game. The harness adds what the development checks run it with --
 * a scripted clock and input (--play), a new game staged from the command
 * line (--new and its levers), a saved memory image, dumps and traces. A
 * build with UW_NO_HARNESS leaves the harness out: its hooks below become
 * no-ops, and tracing compiles away. */
#ifndef UWSHELL_H
#define UWSHELL_H

#include "../uw_motion.h"
#include "../uw_scroll.h"
#include "../uw_image.h"
#include "../uw_scene.h"
#include "../uw_screen.h"
#include "../uw_cutplay.h"
#include "../uw_boot.h"
#include "../uw_talk.h"
#include "../uw_options.h"
#include "../uw_ail.h"
#include "../uw_adlib.h"
#include "../uw_opl.h"
#include "../uw_sound.h"
#include "../uw_chargen_ui.h"
#include "../uw_automap.h"
#include "../uw_menu.h"

#include <SDL3/SDL.h>
#include <stdio.h>

/* The program's name in its messages, its window's title, and the per-user
 * directory its saves go to when the game's own takes no files, which a
 * build may set (-DUW_PROGRAM=\"name\", -DUW_WINDOW_TITLE=\"title\",
 * -DUW_PREF_DIR=\"dir\"). */
#ifndef UW_PROGRAM
#define UW_PROGRAM "uwshell"
#endif
#ifndef UW_WINDOW_TITLE
#define UW_WINDOW_TITLE "Ultima Underworld"
#endif
#ifndef UW_PREF_DIR
#define UW_PREF_DIR "ultima-underworld"
#endif

static inline uint16_t rw(const uint8_t *m, uint16_t at) { return (uint16_t)(m[at] | m[(uint16_t)(at + 1)] << 8); }
static inline void ww(uint8_t *m, uint16_t at, uint16_t v) { m[at] = (uint8_t)v; m[(uint16_t)(at + 1)] = (uint8_t)(v >> 8); }
static inline uint32_t rd(const uint8_t *m, uint16_t at) { return (uint32_t)rw(m, at) | (uint32_t)rw(m, (uint16_t)(at + 2)) << 16; }

typedef struct uw_shell uw_shell;

/* A row of the mode table: what the mode runs a pass, and what it runs on
 * the way out (event_handlers[mode][15]). The way IN is the transition's
 * own code, as it is in the original, where dungeon_draw_main_screen is
 * run by whoever comes back to mode 1 and not by dungeon_mode_enter. */
typedef struct {
    int         number;             /* the game's mode number, or the shell's */
    const char *name;
    void      (*pass)(uw_shell *);
    void      (*leave)(uw_shell *);
    void      (*present)(uw_shell *);   /* the screen at the pass's end; NULL: the game's */
} uw_shell_mode;

enum {
    MODE_NONE     = 0,              /* no game: --cutscene N, and the loop ends when it is entered */
    MODE_DUNGEON  = 1,              /* game mode 1: the dungeon */
    MODE_MAP      = 2,              /* game mode 2: the automap (src/uw_automap.c) */
    MODE_CONV     = 4,              /* game mode 4: a conversation (src/uw_talk.c) */
    MODE_CUTSCENE = 0x10,           /* game mode 0x10: cutscene_play (src/uw_cutplay.c) */
    MODE_MENU     = 0x100,          /* the title screen: main_menu's loop (src/uw_menu.c) */
    MODE_CHARGEN  = 0x101,          /* the shell's: new_game's character generation loop */
    MODE_TITLE    = 0x102,          /* the shell's: game_main before main_menu -- the two
                                     * presentation screens and the title cutscene */
    MODE_LEAVING  = 0x103,          /* the shell's: game_return_to_menu after its cutscene */
    MODE_ENDING   = 0x104,          /* the shell's: game_ending_sequence's screens */
    MODE_DRAINING = 0x105           /* the shell's: game_return_to_menu's input drain, a button still down */
};

struct uw_shell {
    uw_motion      m;
    uint8_t       *ds, *lseg, *pal, *screen, *keybuf;
    uw_boot       *boot;            /* --new: the from-files boot */
    int            new_game;
    int            digital_cfg;     /* the cutscenes' voices: on with the sound */
    int            sound_noted;     /* a restored game's own switches reported once */
    int            sound_forced;    /* --sound: on regardless, the saved game's switches too */
    int            fullscreen;      /* --fullscreen, Alt-Enter */
    int            vsync;           /* 0 on (the default), -1 --no-vsync */
    int            relative;        /* the host's pointer captured while a button walks in the view */
    float          drag_fx, drag_fy; /* the captured motion's part of a pixel not yet moved */
    int            objcheck;        /* --objcheck: objcheck_run over each level entered */
    uint16_t       objcheck_level;
    uw_talk        talk;
    int            talk_open;
    uw_options     options;
    uw_chargen_ui  cgui;
    uw_automap     map;
    uw_scroll      scroll;
    uint32_t       scroll_wait_until;  /* a dungeon print's pause: its end (0: a [MORE], none) */
    int            scroll_wait_set;
    uint16_t       mouse_mask;         /* the host mouse's buttons, before mouse_btns' keypad */
    long           beep_left;          /* do_beep's tone: samples still to sound */
    double         beep_phase, beep_step;
    uw_strings     pak;
    uw_ark         ark;
    uw_blob        terrain;
    int            start_x, start_y, start_heading;
    const char    *script;
    const char    *pair;           /* a memory image's file, which the harness boots */
    int            trace;
    int            have_game;       /* a game's memory is up (not --cutscene alone) */
    int            mode;            /* MODE_*: the mode whose pass runs */
    int            prev_mode;       /* game_prev_mode: where a cutscene returns to */
    int            running;
    /* the cutscene up (mode 0x10) */
    uw_cutscene    cut;
    long           cut_frames, cut_records, cut_files, cut_nc;   /* the last one's counts */
    int            cuts_shown, cuts_seen[16];   /* the cutscenes that opened, in order: the first sixteen's numbers */
    uint8_t        input_edge;      /* a key (1) or a button (2) went down this pass */
    uint8_t        esc_edge;        /* Escape went down this pass */
    /* the program: game_main's sequence before game_loop */
    int            program;         /* uwshell with no pair and no --new: the whole program */
    int            title_step;      /* where game_main is before the menu */
    uw_menu        menu;
    int            menu_ok;
    uint8_t        menu_dac[768];   /* the DAC while the title is up: palette 2 faded in,
                                     * the working palette's 0x40..0x7f rotating over it */
    int            menu_error_wait; /* main_menu's "Error: Bad save file", waiting for a key */
    int            chargen_from_menu;
    long           nc_at_boot;      /* the boot's own counted calls, which its finish discards */
    int            left_game;       /* game_return_to_menu ran: the way back runs the enter handler */
    int            leave_show;      /* game_return_to_menu's argument */
    int            hp_override;     /* --hp N: the Avatar's hit points after the boot (a test lever) */
    int            level_override;  /* --level N: the game moved there after the boot (a test lever) */
    int            ending_step;     /* where game_ending_sequence's ending is */
    uint32_t       clock0;          /* the first state's frame stamp */
    uint64_t       t0, pass_start;
    /* --sound: the driver model and the files sound_init and load_xmi
     * read, and the two timers' progress against the clock */
    int            sound;
    uw_ail         ail;
    uw_adlib       adlib;
    uw_opl         opl;
    uw_blob        adlib_file;
    SDL_AudioStream *audio;         /* NULL in a scripted run: the chip runs, nothing is heard */
    FILE          *audio_dump;      /* --audio-dump: the samples as a WAV, for a run nobody hears */
    long           audio_samples;
    double         audio_frac;      /* the samples a tick is short of a whole one */
    int32_t        dc_x, dc_y;      /* the AdLib's output coupling: a first-order high-pass */
    /* the digital side, the cutscenes' voices: SOUND\NN.VOC as the engine
     * queues it, at the chip's rate and the mixer's level */
    int            digital;         /* digital_is_available() */
    int16_t       *voice;           /* NULL when none plays */
    long           voice_len, voice_pos;
    uw_sounds      sounds;
    uw_bank        timbres;
    uw_blob        xmi[16];
    uint32_t       sound_clock0;
    uint64_t       ail_ticks, age_ticks;
    long           passes, renders, traced_nc;
    /* input_tick's bookkeeping between passes */
    int            source_toggle;   /* keyboard or mouse first this pass */
    int            waiting;         /* a release wait or a drag wait stood at the pass's top */
    int            map_release;     /* the map's erase done on its press: mouse_release(1) until the button is up */
    uint32_t       click_handler;   /* the hotspot the last click reached */
    int            click_panel_mode;
    int16_t        drag_x, drag_y;  /* where the button went down, for cursor_wait_for_drag */
    char           typed[0x34];     /* scroll_text_input's field */
    int            typed_len;
    /* the window */
    uint32_t      *frame;
    SDL_Window    *win;
    SDL_Renderer  *ren;
    SDL_Texture   *tex;
};

/* the program's, which the harness's boots use too */
extern uw_scene *sc;
extern char game_dir[512];
extern uint8_t panel_ems[3 * 0x4000];
const uint8_t *presented_page(uw_shell *sh, const uint8_t **dac);

#ifdef UW_NO_HARNESS

/* nothing traced: the arguments are named inside sizeof, never evaluated,
 * so a value kept only for its trace line is still used */
#define trace_line(sh, ...) ((void)(sh), (void)sizeof(printf(__VA_ARGS__)))

static inline void harness_usage(void) {}
static inline int harness_arg(uw_shell *sh, int argc, char **argv, int *i) { (void)sh; (void)argc; (void)argv; (void)i; return 0; }
static inline int harness_setup(uw_shell *sh) { (void)sh; return 1; }
static inline const char *harness_ark_path(void) { return NULL; }
static inline void harness_after_begin(uw_shell *sh) { (void)sh; }
static inline int harness_new_game(uw_shell *sh) { (void)sh; return 0; }
static inline void harness_before_adopt(uw_shell *sh) { (void)sh; }
static inline int harness_after_boot(uw_shell *sh) { (void)sh; return 1; }
static inline int harness_boot_pair(uw_shell *sh) { (void)sh; return 0; }
static inline int harness_image(uw_shell *sh) { (void)sh; return 1; }
static inline int harness_event(uw_shell *sh, SDL_Event *e) { (void)sh; (void)e; return 0; }
static inline void harness_events_done(uw_shell *sh) { (void)sh; }
static inline int harness_script_over(const uw_shell *sh) { (void)sh; return 1; }
static inline void harness_audio(uw_shell *sh) { (void)sh; }
static inline void harness_end(uw_shell *sh) { (void)sh; }
static inline void harness_cutscene_end(uw_shell *sh, int cutscene) { (void)sh; (void)cutscene; }
static inline void harness_vortex_frame(uw_shell *sh, int frame) { (void)sh; (void)frame; }

#else

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void trace_line(const uw_shell *sh, const char *fmt, ...);

/* the harness's lines of the usage */
void harness_usage(void);
/* argv[*i] when it is the harness's (and its values, *i moved past them): 1;
 * 0 when it is not; -1 when it is malformed */
int harness_arg(uw_shell *sh, int argc, char **argv, int *i);
/* after the arguments: the script read, the trace switch; 0 on failure */
int harness_setup(uw_shell *sh);
/* --ark: the level archive a staged new game boots over (NULL: the game's) */
const char *harness_ark_path(void);
/* --seed, into the boot's data segment before anything draws from it */
void harness_after_begin(uw_shell *sh);
/* --new's second half: the generation screen (--chargen), or the character
 * the levers make and the game finished; 0 on failure */
int harness_new_game(uw_shell *sh);
/* --heading, before the shell takes the boot's segments */
void harness_before_adopt(uw_shell *sh);
/* --level and --hp after the boot; 0 on failure */
int harness_after_boot(uw_shell *sh);
/* a memory image in place of a boot; 0 on failure */
int harness_boot_pair(uw_shell *sh);
/* --image's ranges of UW.EXE's data segment: 1, or 0 when UW.EXE will not
 * read and -1 when the ranges are malformed (the usage) */
int harness_image(uw_shell *sh);
/* the script's next event, due at the clock: 1 with *e filled as SDL would
 * report it, 2 when the harness took it itself (a snap, a mark, a walk), 0
 * when none is due */
int harness_event(uw_shell *sh, SDL_Event *e);
/* the pass's events taken: a walk's step */
void harness_events_done(uw_shell *sh);
/* the script run out, its walk ended */
int harness_script_over(const uw_shell *sh);
/* --audio-dump's WAV header, written at the start and again at the end */
void harness_audio(uw_shell *sh);
/* the run over: --dump, the WAV closed, the counts printed */
void harness_end(uw_shell *sh);
/* --cutscene N over: its counts, and --dump's screen and palette */
void harness_cutscene_end(uw_shell *sh, int cutscene);
/* a vortex frame shown: a vpage's page written at its frame */
void harness_vortex_frame(uw_shell *sh, int frame);

#endif

#endif
