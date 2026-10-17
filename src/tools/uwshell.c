/* SPDX-License-Identifier: MIT */
/* uwshell -- the port run as a program: a window, a clock, a keyboard and a
 * mouse over the dungeon frame, tick and panels, in the original's own pass.
 *
 * THE WINDOW is resizable, 4:3 by default -- the frame scaled up four times
 * with nearest sampling and stretched to 320 x 240 with a filtered one, as
 * a VGA monitor showed mode 13h -- or square with --square; Alt-Enter
 * toggles full screen; vsync unless --no-vsync; --pace sets a pass's least
 * length. A button down in the 3-D view holds the pointer until the
 * buttons are up, the motion then added to the cursor as the driver's
 * deltas are, so a walk cannot run the pointer out of the window.
 *
 * THE GAME'S FILES are found as --dir, else $UW_DATA, else the working
 * directory when it has DATA/, else the one the build names
 * (UW_GAME_DIR_FALLBACK), and
 * refused, with what is missing named, unless DATA/, CRIT/ and CUTS/ are
 * there (check_game_directories). THE SOUND PLAYS BY DEFAULT -- the music
 * through the port's AdLib model and the cutscenes' voices through its
 * digital one -- where the original takes both from DATA/UW.CFG's drivers
 * and a copy INSTALL never configured names none. --nosound silences it;
 * --sound is on regardless, a restored game's own switches included.
 *
 *     uwshell [--dir DIR] [--saves DIR] [--scale N] [--sound|--nosound] ...
 *
 * A development harness can drive the program through the hooks in
 * uwshell.h -- a script, a staged new game, a saved memory image, dumps and
 * traces; built with UW_NO_HARNESS there is none, and the hooks do nothing.
 *
 * THE MODES. game_loop is one loop for every game mode, and
 * which handlers a pass runs is a table: event_handlers[mode][event]
 * (three rows, indexed as game_set_mode maps the mode number --
 * 2 to 1, 4 to 2, everything else to 0), with [mode][0] the enter handler
 * and [mode][15] the leave handler that game_change_mode runs around a
 * change. The shell keeps the same shape: `modes[]` below is a row a mode,
 * each with its pass and its leave, dispatched on the mode the game is in
 * (uw_shell.mode: 1 the dungeon, 2 the automap, 4 a conversation, and the
 * character generation screen, which in the original is new_game's own
 * loop before game_loop and here is a mode of the shell's). What the
 * original runs as a loop MODAL inside a click handler -- scroll_text_input's
 * "Move how many? " and options_menu_loop -- is not a mode, and runs as an
 * overlay on whatever mode is up (modal_pass).
 *
 * THE PASS in the dungeon is game_loop's: event_dispatch runs the dungeon
 * mode's handlers in event order for the bits pending -- 1 the view refresh
 * (dungeon_refresh_view, rendered through src/uw_scene.c and presented by
 * uw_motion_view_present), 3 the pending teleport, 9 the inventory refresh,
 * 11 the frame, 12 the tick, 13 the panels -- re-arms the standing mask
 * 0x3800, runs the cursor bounds countdown; then input_tick
 * polls one source, keyboard or mouse in turn, and delivers a
 * key to input_dispatch_key or a click to the hotspot table. A handler's
 * button-release wait (mouse_button_released -1) keeps the passes running
 * without input until the button is up, when the wait's rest runs.
 *
 * THE CLOCK is the 256 Hz counter the game reads through clock_ptr, here
 * SDL's nanoseconds from the first state's frame stamp on. THE KEYBOARD is
 * the driver's key state array: a byte a scan code, set while a key is down,
 * which the frame reads for the movement keys and keyboard_scan_held
 * round-robins for the bound keys, 0x1e ticks apart, through
 * key_char_table. THE MOUSE is cursor_update_position's move and the cursor
 * interrupt's click latch: a press with nothing pending latches the cursor's
 * position and the button mask, which mouse_read delivers on the
 * release, or the live mask while a button stays down. */
#include "uwshell.h"
#include "../uw_gamedir.h"
#include "../uw_trig.h"
#include <SDL3/SDL_main.h>     /* main as the entry point on every platform, WinMain included */

#include <sys/stat.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint16_t gr_sizes(const char *dir, const char *const *files, int nfiles, uint8_t *out, uint16_t max) {
    uint16_t n = 0;
    int f;
    for (f = 0; f < nfiles; f++) {
        char path[768];
        uw_blob g;
        uint16_t k, count;
        snprintf(path, sizeof path, "%s/DATA/%s", dir, files[f]);
        g = uw_read_file(path);
        if (!g.data || g.size < 3) { fprintf(stderr, "%s: cannot read\n", path); exit(1); }
        count = (uint16_t)(g.data[1] | g.data[2] << 8);
        for (k = 0; k < count && n < max; k++, n++) {
            size_t at = 3 + (size_t)k * 4, off;
            if (at + 4 > g.size) break;
            off = (size_t)(g.data[at] | g.data[at + 1] << 8 | g.data[at + 2] << 16 | (uint32_t)g.data[at + 3] << 24);
            out[n * 2] = off + 3 <= g.size ? g.data[off + 1] : 0;
            out[n * 2 + 1] = off + 3 <= g.size ? g.data[off + 2] : 0;
        }
        uw_free(&g);
    }
    return n;
}

/* ---- the view, through the scene builder (as motionframe's refresh_view) */

uw_scene *sc;
static int sc_opened;
/* A place this build looks for the game's files before the player's own
 * configuration (-DUW_GAME_DIR_FALLBACK=\"DIR\"); "." when it names none. */
#ifndef UW_GAME_DIR_FALLBACK
#define UW_GAME_DIR_FALLBACK "."
#endif
char game_dir[512] = UW_GAME_DIR_FALLBACK;

/* The player's configuration file, beside the saves SDL keeps per user:
 * the game's directory once found (`game_dir`), and the UW.EXE of another
 * release already warned about (`accepted_exe`). Empty until asked for. */
static char config_file[1024];

static const char *config_path(void) {
    if (!config_file[0]) {
        char *pref = SDL_GetPrefPath(NULL, UW_PREF_DIR);
        if (pref) {
            snprintf(config_file, sizeof config_file, "%s%s.cfg", pref, UW_PROGRAM);
            SDL_free(pref);
        }
    }
    return config_file[0] ? config_file : NULL;
}

/* SDL's folder dialog, waited for: the folder chosen, or "" when the
 * player cancelled or there is no dialog to show. */
typedef struct { int done; char path[1024]; } folder_pick;

static void SDLCALL folder_picked(void *user, const char *const *files, int filter) {
    folder_pick *p = user;
    (void)filter;
    if (files && files[0]) snprintf(p->path, sizeof p->path, "%s", files[0]);
    p->done = 1;
}

/* GOG's CD image, game.gog: its UW folder copied once into the per-user
 * folder -- into UW.part, renamed UW when whole, so a copy cut short is
 * never taken for the game -- and that copy the game's directory. A whole
 * copy made before is used as it is. 1 with it in game_dir. */
static int game_from_image(const char *image) {
    char *pref = SDL_GetPrefPath(NULL, UW_PREF_DIR);
    char dst[1100], part[1100], why[200];
    if (!pref) {
        fprintf(stderr, UW_PROGRAM ": %s holds the game, and there is no per-user folder to copy it to\n", image);
        return 0;
    }
    snprintf(dst, sizeof dst, "%sUW", pref);
    snprintf(part, sizeof part, "%sUW.part", pref);
    SDL_free(pref);
    if (!uw_game_check(dst, NULL, 0)) {
        fprintf(stderr, UW_PROGRAM ": copying the game out of %s into %s\n", image, dst);
        if (!uw_iso_extract_game(image, part, why, sizeof why)) {
            fprintf(stderr, UW_PROGRAM ": the copy failed: %s\n", why);
            return 0;
        }
        if (rename(part, dst) != 0 && !uw_iso_extract_game(image, dst, why, sizeof why)) {
            fprintf(stderr, UW_PROGRAM ": the copy failed: %s\n", why);
            return 0;
        }
    }
    if (strlen(dst) + 1 > sizeof game_dir) return 0;
    snprintf(game_dir, sizeof game_dir, "%s", dst);
    return 1;
}

/* A place the game may be: the game's directory (or the folder holding it
 * in UW), a folder holding GOG's game.gog, or game.gog itself. 1 with the
 * game's directory in game_dir; *copied set when it came out of an image. */
static int take_place(const char *place, int *copied) {
    char image[1100];
    *copied = 0;
    if (uw_game_find_in(place, game_dir, sizeof game_dir)) return 1;
    if (uw_game_image_in(place, image, sizeof image)) return *copied = game_from_image(image);
    if (!uw_is_dir(place) && uw_iso_holds_game(place)) return *copied = game_from_image(place);
    return 0;
}

/* The player asked for the game's directory: told what is wanted, shown the
 * folder dialog, and asked again (up to three times) when the folder chosen
 * is not the game's. 1 with it in game_dir. */
static int ask_for_game_dir(void) {
    char msg[1400], why[64];
    int tries, copied;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return 0;
    snprintf(msg, sizeof msg, "%s needs the files of your copy of Ultima Underworld.\n\n"
             "Choose the folder GOG installed it to (the one with game.gog in it), or a folder with "
             "UW.EXE and the folders DATA, CRIT and CUTS.", UW_WINDOW_TITLE);
    for (tries = 0; tries < 3; tries++) {
        folder_pick p = { 0, "" };
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, UW_WINDOW_TITLE, msg, NULL);
        SDL_ShowOpenFolderDialog(folder_picked, &p, NULL, NULL, false);
        while (!p.done) {
            SDL_Event e;
            SDL_WaitEventTimeout(&e, 50);
        }
        if (!p.path[0]) return 0;
        if (take_place(p.path, &copied)) return 1;
        uw_game_check(p.path, why, sizeof why);
        snprintf(msg, sizeof msg, "%s is not Ultima Underworld's folder (%s, and no game.gog).\n\n"
                 "Choose the folder GOG installed it to, or one with UW.EXE and the folders DATA, CRIT "
                 "and CUTS in it.", p.path, why);
    }
    return 0;
}

/* Where the game's files are: --dir, else UW_DATA, else the working
 * directory when it holds them, else the build's own place, else the
 * directory the player's configuration names, else one a store installed
 * it to (uw_game_detect), else -- where someone can answer -- the player is
 * asked, and what was found by looking or asking is written to the
 * configuration. `locate` (--locate) asks whatever the configuration says.
 * Every place may be the game's directory, a directory holding it in UW,
 * a GOG install holding game.gog, or game.gog itself (take_place); an
 * image is copied out once and the copy remembered. Then check_game_directories's test, DATA, CRIT and CUTS
 * all directories there, which the original makes fatal error 0x3001 --
 * here the directory and what it lacks named. 1 when found. */
static int find_game_dir(const char *given, int interactive, int locate) {
    const char *env = getenv("UW_DATA"), *cfg, *home;
    char conf[1024], why[64];
    int found = 0, remember = 0, copied = 0, image = 0;
    if (given) {
        if (!take_place(given, &copied)) snprintf(game_dir, sizeof game_dir, "%s", given);
        found = 1;
    } else if (env && *env) {
        if (!take_place(env, &copied)) snprintf(game_dir, sizeof game_dir, "%s", env);
        found = 1;
    } else if (!locate && take_place(".", &copied)) {
        found = 1;
    } else if (!locate && strcmp(UW_GAME_DIR_FALLBACK, ".") && uw_game_find_in(UW_GAME_DIR_FALLBACK, game_dir, sizeof game_dir)) {
        found = 1;
    }
    if (!found && !locate && (cfg = config_path()) && uw_config_get(cfg, "game_dir", conf, sizeof conf)
        && uw_game_find_in(conf, game_dir, sizeof game_dir))
        found = 1;
#ifdef _WIN32
    home = getenv("USERPROFILE");
#else
    home = getenv("HOME");
#endif
    if (!found && !locate && uw_game_detect(home, conf, sizeof conf, &image)) found = remember = take_place(conf, &copied);
    if (!found && interactive && ask_for_game_dir()) found = remember = 1;
    if (!found) {
        fprintf(stderr, UW_PROGRAM ": the game's files were not found; give the directory with UW.EXE, DATA/, "
                        "CRIT/, CUTS/ and SOUND/ in it as --dir DIR or in UW_DATA\n");
        return 0;
    }
    if (!uw_game_check(game_dir, why, sizeof why)) {
        fprintf(stderr, UW_PROGRAM ": %s is not the game's directory: %s\n", game_dir, why);
        fprintf(stderr, UW_PROGRAM ": give the directory with UW.EXE, DATA/, CRIT/, CUTS/ and SOUND/ in it"
                        " as --dir DIR or in UW_DATA\n");
        return 0;
    }
    if ((remember || copied) && (cfg = config_path()) && !uw_config_set(cfg, "game_dir", game_dir))
        fprintf(stderr, UW_PROGRAM ": %s will not write; the game's directory is not remembered\n", cfg);
    return 1;
}

/* The release the game's UW.EXE is. The port is made against GOG's; any
 * other runs untested, said on the terminal every time and -- where
 * someone can answer -- once in a message box, the executable then
 * remembered as accepted. */
static void check_release(int interactive) {
    uw_game_release r;
    char crc[16], accepted[16], msg[700];
    const char *cfg;
    if (!uw_game_release_of(game_dir, &r) || r.supported) return;
    snprintf(crc, sizeof crc, "%08x", (unsigned)r.crc);
    snprintf(msg, sizeof msg, "This copy's UW.EXE (%ld bytes, CRC-32 %s%s%s) is not the release %s is made "
             "and tested against, which is GOG's. It may work, or it may not; a report of how it goes is welcome.",
             r.size, crc, r.name ? ", " : "", r.name ? r.name : "", UW_WINDOW_TITLE);
    fprintf(stderr, UW_PROGRAM ": %s\n", msg);
    if (!interactive || !(cfg = config_path())) return;
    if (uw_config_get(cfg, "accepted_exe", accepted, sizeof accepted) && !strcmp(accepted, crc)) return;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO))
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, UW_WINDOW_TITLE, msg, NULL);
    uw_config_set(cfg, "accepted_exe", crc);
}

static int scene_at(uw_motion *m) {
    const uint8_t *ds = m->ds;
    if (!sc) {
        sc = calloc(1, sizeof *sc);
        sc_opened = sc && uw_scene_open(sc, game_dir);
    }
    return sc_opened && uw_scene_level(sc, (int16_t)(ds[0x7278] | ds[0x7279] << 8) - 1, m->lseg + 4, 0x7c08);
}

static void scene_camera(uw_motion *m) {
    const uint8_t *ds = m->ds;
    /* what the game asked of the renderer outside the data segment: the
     * warp's texture masks, the ramp blanked or read again, the floor
     * textures reloaded (uw_motion.render) */
    if (m->render.masks) { uw_scene_masks(sc, m->render.mask); m->render.masks = 0; }
    if (m->render.shade) { uw_scene_shade_blank(sc, m->render.shade == 1); m->render.shade = 0; }
    if (m->render.floors) { uw_scene_floors(sc, ds + 0x717c); m->render.floors = 0; }
    uw_scene_place(sc, rw(ds, 0x2780), rw(ds, 0x2782), rw(ds, 0x2784), rw(ds, 0x727a), ds[0x7288 + 0xb5] >> 4, ds[0x1c50]);
    uw_scene_sway(sc, ds[0x3578], (int16_t)rw(ds, 0x3580), (int16_t)rw(ds, 0x3582), (int16_t)rw(ds, 0x3584),
                  (int16_t)rw(ds, 0x3586));
    memcpy(sc->ds + 0x3588, ds + 0x3588, 4);
    /* the automap's switch (level 9 and the crystal ball turn it off):
     * automap_mark_tile writes a tile's code only while it is set */
    sc->ds[0x546] = ds[0x546];
    /* camera_follow_ptr: on the Avatar, the scene's own Avatar; null, the
     * free camera (Roaming Sight's), its angles and position with it */
    if (rw(ds, 0x2e10) | rw(ds, 0x2e12)) {
        memcpy(sc->ds + 0x2e10, sc->ds + 0x7274, 4);
    } else {
        memset(sc->ds + 0x2e10, 0, 4);
        memcpy(sc->ds + 0x358c, ds + 0x358c, 12);
    }
}

static const uint8_t *pick_map(void *user, uint8_t *ds) {
    static uint8_t scratch[64000], map[0x5000];
    static uint16_t rows320[200], rows174[200];
    uw_motion *m = user;
    uw_fb f;
    int i;
    if (!scene_at(m)) return NULL;
    for (i = 0; i < 200; i++) { rows320[i] = (uint16_t)(i * 320); rows174[i] = (uint16_t)(2 + i * 174); }
    scene_camera(m);
    f.pixels = scratch; f.size = sizeof scratch; f.row = rows320; f.n_rows = 200;
    if (uw_scene_draw(sc, &f) < 0) return NULL;
    memset(map, 0, sizeof map);
    f.pixels = map; f.size = sizeof map; f.row = rows174; f.n_rows = 114;
    if (uw_scene_pick(sc, &f) < 0) return NULL;
    /* the pick tables whole, 0xc0 entries each: one data segment in the
     * original, so a colour past the count still names what an earlier
     * frame left there */
    for (i = 0; i < 0xc0; i++) {
        memcpy(ds + 0x2e1c + 2 * i, sc->ds + 0x2e1c + 2 * i, 2);
        memcpy(ds + 0x2f9c + 2 * i, sc->ds + 0x2f9c + 2 * i, 2);
    }
    memcpy(ds + 0x311e, sc->ds + 0x311e, 2);
    /* tilemap_origin_ptr and automap_tile_end_ptr, which the sweep wrote:
     * their offsets, with the segment the game's own tables name (the
     * scene's level segment is its own) */
    memcpy(ds + 0x3120, sc->ds + 0x3120, 2);
    memcpy(ds + 0x3122, ds + 0x19b6, 2);
    memcpy(ds + 0x3128, sc->ds + 0x3128, 2);
    memcpy(ds + 0x312a, ds + 0x19b6, 2);
    return map;
}

/* The renderer's work buffer, 320 wide with row 0 the
 * view's bottom as uw_motion_view_present takes it: the view the last
 * refresh drew, which view_present blits whole.
 *
 * EVERY EXECUTION OF THE LIST STARTS FROM A BLACK VIEWPORT.
 * gfx_execute_draw_list calls FOUR routines and the first is
 * gfx_clear_work_buffer, which zeroes the viewport's 19,662 bytes before
 * rast_execute runs the list -- the fades reach the same code two bytes
 * in, past the `xor ax,ax` that makes the frame path's
 * fill colour 0. So a pixel no face covers is BLACK, not what it was: the
 * traversal stops at unlit cells, and the pixels past the last one it
 * reached are the dark end of a corridor. Keeping them would trail light
 * wall texels across the dark while the view turned. */
static uint8_t work_view[64000], work_all[64000];

/* dungeon_refresh_view: view_render -- the traversal,
 * cursor_poll, the list -- and view_present. */
static void refresh_view(uw_motion *m) {
    static uint8_t pa[64000];
    static uint16_t rows[200];
    int i, y;
    uw_fb fa;
    for (i = 0; i < 200; i++) rows[i] = (uint16_t)(i * 320);
    if (!scene_at(m)) { m->pixels_not_drawn++; return; }
    scene_camera(m);
    fa.pixels = pa; fa.size = sizeof pa; fa.row = rows; fa.n_rows = 200;
    /* gfx_clear_work_buffer, the first of gfx_execute_draw_list's four */
    memset(pa, 0, sizeof pa);
    /* automap_tiles: the view's own sweep marks them, and the
     * scene keeps the segment the sweep runs over -- the original has one
     * data segment and this is the seam, so the game's map goes across
     * before the draw (a restore's, a level's) and the marks come back
     * after it, for the map screen and for what a save writes; and what
     * the sweep saw for the first time is experience (view_build_draw_list) */
    memcpy(sc->ds + 0x3820, m->ds + 0x3820, 0x1000);
    if (uw_scene_draw(sc, &fa) < 0) { m->pixels_not_drawn++; return; }
    memcpy(m->ds + 0x3820, sc->ds + 0x3820, 0x1000);
    uw_motion_view_explored(m, sc->vl.newly_seen);
    m->ds[0x311e] = 0;
    m->ds[0x2e1c] = m->ds[0x2e1d] = 0;
    uw_motion_cursor_poll(m);
    /* the frame the list left over the cleared viewport, and all of it
     * presented: every pixel of the view is this frame's, so there is no
     * mask to keep -- a second pass over another background, which is what
     * told the two apart while the buffer persisted, no longer says
     * anything the buffer does not */
    if (!work_all[0]) memset(work_all, 1, sizeof work_all);
    for (y = 0; y < UW_VIEW_H; y++)
        memcpy(work_view + y * 320, pa + y * 320, UW_VIEW_W);
    uw_motion_view_present(m, work_view, work_all);
}

/* input_tick's hotspot walk, for the handler a click at the
 * cursor reaches: the table backwards, the first record whose rectangle
 * holds the cursor and whose mode mask meets the event's. */
static uint32_t hotspot_at(const uint8_t *ds) {
    uint16_t ev = rw(ds, 0x00e2), table = rw(ds, 0x24a4);
    int16_t x = (int16_t)rw(ds, ds[0x011d] ? 0x0117 : 0x010e), y = (int16_t)rw(ds, ds[0x011d] ? 0x0119 : 0x0110), i;
    for (i = (int16_t)((int16_t)rw(ds, 0x24a8) - 1); i >= 0; i--) {
        uint16_t r = (uint16_t)(table + i * 0x12);
        uint32_t handler = (uint32_t)rw(ds, (uint16_t)(r + 0x10)) << 16 | rw(ds, (uint16_t)(r + 0xe));
        if (x < (int16_t)rw(ds, (uint16_t)(r + 6)) || y < (int16_t)rw(ds, (uint16_t)(r + 8)) || (int16_t)rw(ds, (uint16_t)(r + 2)) < x
            || (int16_t)rw(ds, (uint16_t)(r + 4)) < y || !(rw(ds, (uint16_t)(r + 0xc)) & rw(ds, (uint16_t)(ev + 8))) || !handler)
            continue;
        return handler;
    }
    return 0;
}

/* ---- the keyboard ---------------------------------------------------------- */

/* SDL's scancodes to the XT set the driver's key state array is indexed by
 * -- the one table, and the --play scripts give the XT codes
 * themselves. The grey keys are E0-prefixed on an AT keyboard, and
 * kbd_read_scancode turns those in its table
 * into scan codes of their own, 0x60 and up: grey Home 0x66, Up 0x67, PgUp
 * 0x68, Left 0x69, Right 0x6a, End 0x6b, Down 0x6c, PgDn 0x6d, Ins 0x6e,
 * Del 0x6f, the keypad's Enter 0x61 and / 0x63, right Ctrl 0x62, PrtSc
 * 0x64, right Alt 0x65, Ctrl-Break 0x60. key_char_table gives the keypad
 * 0x8c..0x96 -- the keyboard's cursor glide (cursor_key_move) -- and the
 * grey arrows 0xa5..0xac, which the dungeon binds to nothing and the text
 * fields' editor takes (uw_scroll_edit); so the port keeps them apart as
 * the original does. */
static uint8_t xt_scan(SDL_Scancode s) {
    static const uint8_t letters[26] = { 0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                         0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c };
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return letters[s - SDL_SCANCODE_A];
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_0) return (uint8_t)(0x02 + (s - SDL_SCANCODE_1));
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) return (uint8_t)(0x3b + (s - SDL_SCANCODE_F1));
    switch (s) {
    case SDL_SCANCODE_RETURN: return 0x1c;
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_BACKSPACE: return 0x0e;
    case SDL_SCANCODE_TAB: return 0x0f;
    case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_MINUS: return 0x0c;
    case SDL_SCANCODE_EQUALS: return 0x0d;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1a;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1b;
    case SDL_SCANCODE_BACKSLASH: return 0x2b;
    case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_F11: return 0x57;
    case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_LSHIFT: return 0x2a;
    case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_LCTRL: return 0x1d;
    case SDL_SCANCODE_LALT: return 0x38;
    case SDL_SCANCODE_CAPSLOCK: return 0x3a;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45;
    case SDL_SCANCODE_SCROLLLOCK: return 0x46;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
    case SDL_SCANCODE_KP_7: return 0x47;
    case SDL_SCANCODE_KP_8: return 0x48;
    case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_MINUS: return 0x4a;
    case SDL_SCANCODE_KP_4: return 0x4b;
    case SDL_SCANCODE_KP_5: return 0x4c;
    case SDL_SCANCODE_KP_6: return 0x4d;
    case SDL_SCANCODE_KP_PLUS: return 0x4e;
    case SDL_SCANCODE_KP_1: return 0x4f;
    case SDL_SCANCODE_KP_2: return 0x50;
    case SDL_SCANCODE_KP_3: return 0x51;
    case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_PERIOD: return 0x53;
    /* the E0-prefixed keys, as kbd_read_scancode renumbers them */
    case SDL_SCANCODE_PAUSE: return 0x60;
    case SDL_SCANCODE_KP_ENTER: return 0x61;
    case SDL_SCANCODE_RCTRL: return 0x62;
    case SDL_SCANCODE_KP_DIVIDE: return 0x63;
    case SDL_SCANCODE_PRINTSCREEN: return 0x64;
    case SDL_SCANCODE_RALT: return 0x65;
    case SDL_SCANCODE_HOME: return 0x66;
    case SDL_SCANCODE_UP: return 0x67;
    case SDL_SCANCODE_PAGEUP: return 0x68;
    case SDL_SCANCODE_LEFT: return 0x69;
    case SDL_SCANCODE_RIGHT: return 0x6a;
    case SDL_SCANCODE_END: return 0x6b;
    case SDL_SCANCODE_DOWN: return 0x6c;
    case SDL_SCANCODE_PAGEDOWN: return 0x6d;
    case SDL_SCANCODE_INSERT: return 0x6e;
    case SDL_SCANCODE_DELETE: return 0x6f;
    default: return 0;
    }
}

/* keyboard_scan_held and keyboard_read's translation: 0 until 0x1e ticks since the last key taken, then the next
 * held scan code from key_scan_index on, through key_char_table (| 0x80
 * with shift), a letter case-shifted under Caps Lock, an extended code +
 * 0x400 with shift, + 0x200 with Alt and + 0x100 with Ctrl. */
static int keyboard_scan_held(uw_motion *m, uint8_t *keys, uint16_t *code, uint16_t *index) {
    uint8_t *ds = m->ds;
    uint16_t state = rw(ds, 0x2344), chartab = rw(ds, 0x2354);
    uint8_t shift = keys[rw(ds, 0x2334)], caps = keys[rw(ds, 0x2338)], alt = keys[rw(ds, 0x233c)], ctrl = keys[rw(ds, 0x2340)];
    uint16_t start, i, c;
    if (m->clock - rd(ds, 0x012e) < 0x1e) return 0;
    start = i = (uint16_t)((rw(ds, 0x0128) + 1) & 0x7f);
    do {
        if (keys[(uint16_t)(state + i)]) {
            uint16_t at = (uint16_t)(shift ? i | 0x80 : i);
            c = keys[(uint16_t)(chartab + at)];
            if (c) {
                if (!(c & 0x80)) {
                    if (caps && (ds[(uint16_t)(0x1e01 + c)] & 0xc)) c = (uint16_t)(shift ? c + 0x20 : c - 0x20);
                } else if (shift) {
                    c |= 0x400;
                }
                if (alt) c |= 0x200;
                if (ctrl) c |= 0x100;
                *code = c;
                *index = at;
                ww(ds, 0x0128, at);
                return 1;
            }
        }
        i = (uint16_t)((i + 1) & 0x7f);
    } while (i != start);
    /* key_scan_index goes round with the scan: nothing
     * held leaves it one on from where it was */
    ww(ds, 0x0128, start);
    return 0;
}

/* keyboard_read(0), input_poll's key -- what every modal loop
 * reads, the text fields, the yes/no, the menus and the waits: the next
 * press from the driver's ring through key_char_table (kbd_read_scancode,
 * which drops the make code of a key already down, so a key
 * held is one press), a letter case-shifted under Caps Lock, an extended
 * code + 0x400 with shift, + 0x200 with Alt and + 0x100 with Ctrl, and
 * key_repeat_time stamped. A press whose table entry is 0 is taken and
 * gives nothing. */
static int keyboard_read_press(uw_motion *m, uint8_t *keys, uint16_t *code) {
    uint8_t *ds = m->ds;
    uint8_t shift = keys[rw(ds, 0x2334)], caps = keys[rw(ds, 0x2338)], alt = keys[rw(ds, 0x233c)], ctrl = keys[rw(ds, 0x2340)];
    uint8_t at;
    uint16_t c;
    if (!uw_motion_key_take(m, &at)) return 0;
    c = keys[(uint16_t)(rw(ds, 0x2354) + at)];
    if (!c) return 0;
    ww(ds, 0x012e, (uint16_t)m->clock);
    ww(ds, 0x0130, (uint16_t)(m->clock >> 16));
    if (!(c & 0x80)) {
        if (caps && (ds[(uint16_t)(0x1e01 + c)] & 0xc)) c = (uint16_t)(shift ? c + 0x20 : c - 0x20);
    } else if (shift) {
        c |= 0x400;
    }
    if (alt) c |= 0x200;
    if (ctrl) c |= 0x100;
    *code = c;
    return 1;
}

/* keyboard_read(1), input_poll_repeat's key -- input_tick's, and the
 * release and drag waits': the driver's next press taken and thrown away
 * (kbd_read_scancode is called before the argument is looked at), then
 * keyboard_scan_held. */
static int keyboard_read_held(uw_motion *m, uint8_t *keys, uint16_t *code, uint16_t *index) {
    uint8_t at;
    uw_motion_key_take(m, &at);
    if (!keyboard_scan_held(m, keys, code, index)) return 0;
    ww(m->ds, 0x012e, (uint16_t)m->clock);          /* key_repeat_time, on every key taken */
    ww(m->ds, 0x0130, (uint16_t)(m->clock >> 16));
    return 1;
}

/* ---- the program ------------------------------------------------------------ */

static int usage(void) {
    fprintf(stderr, "usage: " UW_PROGRAM " [--dir DIR] [--saves DIR] [--scale N] [--sound|--nosound]   (the program: the title, the menu, the game)\n"
                    "       " UW_PROGRAM " --cutscene N [--dir DIR] [--scale N]   (N decimal; the intro is 1)\n");
    harness_usage();
    fprintf(stderr, "The game's directory (UW.EXE, DATA/, CRIT/, CUTS/, SOUND/) is --dir, else $UW_DATA, else the\n"
                    "working directory%s%s, else the one the configuration remembers, else one a store installed\n"
                    "the game to; else it is asked for, and remembered. --locate asks for it again.\n"
                    "The sound plays by default: --nosound silences it, --sound\n"
                    "is on regardless of a saved game's own switches.\n",
            strcmp(UW_GAME_DIR_FALLBACK, ".") ? ", else " : "", strcmp(UW_GAME_DIR_FALLBACK, ".") ? UW_GAME_DIR_FALLBACK : "");
    fprintf(stderr,
                    "The window: --scale N, --fullscreen (Alt-Enter toggles), --square for square pixels (4:3 is the\n"
                    "default, as a VGA monitor showed 320x200), --no-vsync, --pace MS for a pass's least length (8;\n"
                    "a dungeon pass at least nine ticks of the game's clock, the original's frame).\n");
    return 2;
}

/* screenshot_write_gif, Alt-q, as the host's: the first free
 * name of uwpic000 .. uwpic777 in the working directory -- three octal
 * digits, as the original counts; its search has no end past the last and
 * the port's stops there -- and the screen through the working palette,
 * each component shifted left by two as the original shifts it, written
 * as PNG where the original wrote a GIF. */
static void screenshot_write(const uint8_t *screen, const uint8_t *pal) {
    char name[32];
    int n, i;
    FILE *f;
    SDL_Surface *img;
#if SDL_VERSION_ATLEAST(3, 4, 0)
    const char *ext = "png";
#else
    const char *ext = "bmp";                /* SDL before 3.4 has no PNG writer */
#endif
    for (n = 0; n < 512; n++) {
        snprintf(name, sizeof name, "uwpic%o%o%o.%s", (n >> 6) & 7, (n >> 3) & 7, n & 7, ext);
        if (!(f = fopen(name, "rb"))) break;
        fclose(f);
    }
    if (n == 512 || !(img = SDL_CreateSurface(320, 200, SDL_PIXELFORMAT_RGB24))) return;
    for (i = 0; i < 64000; i++) {
        uint8_t *px = (uint8_t *)img->pixels + (i / 320) * img->pitch + (i % 320) * 3;
        const uint8_t *c = pal + screen[i] * 3;
        px[0] = (uint8_t)(c[0] << 2); px[1] = (uint8_t)(c[1] << 2); px[2] = (uint8_t)(c[2] << 2);
    }
#if SDL_VERSION_ATLEAST(3, 4, 0)
    if (!SDL_SavePNG(img, name)) fprintf(stderr, UW_PROGRAM ": %s: %s\n", name, SDL_GetError());
#else
    if (!SDL_SaveBMP(img, name)) fprintf(stderr, UW_PROGRAM ": %s: %s\n", name, SDL_GetError());
#endif
    SDL_DestroySurface(img);
}

/* The present at the bottom of the pass: the screen's indexes through the
 * palette into the texture. A fade runs its own, a frame at a time. */
/* The host's presentation. A VGA monitor showed mode 13h's
 * 320 x 200 on a 4:3 tube, each pixel 1.2 times as tall as wide, and the
 * art was drawn for that: by default the frame goes up four times with
 * nearest sampling and is stretched to a 320 x 240 logical window with a
 * filtered one, so the pixels stay crisp and the picture keeps its shape;
 * --square shows the pixels square. `present_up` is that intermediate. */
static int present_aspect = 1;
static SDL_Texture *present_up;
static int present_pace_ms = 8;             /* --pace: a pass's least length */

static void show(const uint8_t *screen, const uint8_t *pal, uint32_t *frame,
                 SDL_Texture *tex, SDL_Renderer *ren) {
    int i;
    for (i = 0; i < 64000; i++) {
        const uint8_t *c = pal + screen[i] * 3;
        frame[i] = (uint32_t)(c[0] * 255 / 63) << 16 | (uint32_t)(c[1] * 255 / 63) << 8 | (uint32_t)(c[2] * 255 / 63);
    }
    SDL_UpdateTexture(tex, NULL, frame, 320 * 4);
    if (present_up) {
        SDL_SetRenderTarget(ren, present_up);
        SDL_RenderTexture(ren, tex, NULL, NULL);
        SDL_SetRenderTarget(ren, NULL);
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, present_up, NULL, NULL);
    } else {
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);
    }
    SDL_RenderPresent(ren);
}

/* the logical presentation's y for a screen row, and back */
static float logical_y(float screen_y) { return present_aspect ? screen_y * 240.0f / 200.0f : screen_y; }
static float screen_y_of(float logical) { return present_aspect ? logical * 200.0f / 240.0f : logical; }

/* the captured mouse's motion in whole screen pixels, the rest kept for the
 * next event. A window larger than the screen moves the mouse a fraction of
 * a pixel an event; adding that to the cursor and truncating the sum rounded
 * every step up and to the left, so a slow drag down or right never moved
 * and one up or left moved a whole pixel an event (openabyss issue 1) */
static int drag_step(float *rest, float delta) {
    int whole;
    *rest += delta;
    whole = (int)*rest;
    *rest -= (float)whole;
    return whole;
}

/* screen_fade_out and screen_fade_in, which the
 * frame asks for and cannot run: thirteen remaps of the work buffer through
 * the rasteriser's light table and a clear, a present after each
 * (src/uw_screen.h).
 *
 * The original's work buffer reaches the screen through
 * gfx_blit_planar_to_screen -- x 52, the viewport's rows 1..112 onto rows
 * 130..19, which is the blit uw_motion_view_present makes. The port keeps no
 * second copy of it between refreshes, so the fade lifts those rows back off
 * the screen, runs over them and writes them back. They are the same pixels,
 * and the panels around the view are left alone as the original leaves
 * them. */
static void screen_fade_run(uw_motion *m, int out, const uint8_t *light, const uint8_t *pal,
                            uint32_t *frame, SDL_Texture *tex, SDL_Renderer *ren) {
    enum { VX = 52, VTOP = 19, VW = UW_VIEW_W, VH = UW_VIEW_H };
    static uint8_t view[UW_VIEW_BYTES];
    uw_screen_wipe w;
    int x, y;
    if (!m->screen || !light) { UW_NOT_CARRIED(m->not_carried); return; }
    /* screen_wipe_forward and _backward run between
     * cursor_hide and cursor_show: the rows the wipe lifts off the screen
     * and writes back are the page, and the cursor drawn on them would be
     * faded with them and left in the saved background */
    uw_motion_cursor_hide(m);
    for (y = 1; y < VH; y++)
        for (x = 0; x < VW; x++)
            view[y * VW + x] = m->screen[(VTOP + VH - 1 - y) * 320 + VX + x];
    uw_screen_fade_begin(&w, out);
    while (uw_screen_fade_step(&w, view, sizeof view, light)) {
        for (y = 1; y < VH; y++)
            for (x = 0; x < VW; x++) {
                m->screen[(VTOP + VH - 1 - y) * 320 + VX + x] = view[y * VW + x];
                if (m->screen_written) m->screen_written[(VTOP + VH - 1 - y) * 320 + VX + x] = 1;
            }
        show(m->screen, pal, frame, tex, ren);
    }
    /* the fade out ends with gfx_clear_work_buffer_thunk(0xf1) over the
     * buffer, which the next refresh clears again (through the
     * entry two bytes earlier), so nothing of it reaches a later frame */
    uw_motion_cursor_show(m);
}

/* palette_fade_out and palette_fade_in: the DAC
 * ramped over steps * 8 frames, the screen's indexes untouched. `live` is
 * the palette the presenter reads; a fade out ramps it down to black from
 * where it is, a fade in ramps it up to `target`. */
static void palette_fade_run(uw_motion *m, uint8_t *live, const uint8_t *target, int steps, int out,
                             uint32_t *frame, SDL_Texture *tex, SDL_Renderer *ren) {
    uw_palette_fade f;
    if (!live || !m->screen || (!out && !target)) return;
    uw_palette_fade_begin(&f, out ? live : target, steps, out);
    while (uw_palette_fade_step(&f)) {
        memcpy(live, f.pal, 768);
        show(m->screen, live, frame, tex, ren);
    }
}

/* dungeon_draw_main_screen whole, which is how mode 1 is
 * re-entered -- from the map, from a restored game and from a conversation:
 * the palette faded out two steps, the page redrawn (the library's
 * uw_boot_draw_main_screen), palette_read(0) and the ramp back in. With
 * `enter` it is game_change_mode(1)'s enter handler, event_handlers[0][0],
 * and makes the bindings too: viewport_unbind_hotspots and the view's
 * (uw_motion_dungeon_viewport) before the fade, dungeon_mode_enter on the
 * new page (uw_motion_dungeon_enter). Either way the cursor is hidden first
 * and shown at the end, as the original's cursor_hide and cursor_show
 * bracket the redraw: the visibility count comes out as it went in. */
static void main_screen_redraw(uw_motion *m, const char *dir, uint8_t *pal,
                               uint32_t *frame, SDL_Texture *tex, SDL_Renderer *ren, int enter) {
    char pals[600];
    uw_blob pl;
    uw_motion_cursor_hide(m);               /* dungeon_draw_main_screen's first call; the page's draw shows it */
    if (enter) uw_motion_dungeon_viewport(m);
    palette_fade_run(m, pal, NULL, 2, 1, frame, tex, ren);
    uw_boot_draw_main_screen(m, dir);
    if (enter) uw_motion_dungeon_enter(m, m->clock);
    /* view_rebuild_and_draw and screen_present: the view on the page
     * before the fade in brings it up, the work buffer's pixels where the
     * list does not write */
    refresh_view(m);
    snprintf(pals, sizeof pals, "%s/DATA/PALS.DAT", dir);
    pl = uw_read_file(pals);
    if (pl.data && pl.size >= 768) palette_fade_run(m, pal, pl.data, 2, 0, frame, tex, ren);
    uw_free(&pl);
}

/* the panel flip's three one-page EMS handles (uw_motion.panel_ems) */
uint8_t panel_ems[3 * 0x4000];

/* keyboard_read's stamp of a key taken (key_repeat_time and
 * key_scan_index), which the dungeon's dispatch makes itself */
static void key_taken(uw_shell *sh, uint16_t index) {
    ww(sh->ds, 0x0128, index);
    ww(sh->ds, 0x012e, (uint16_t)sh->m.clock);
    ww(sh->ds, 0x0130, (uint16_t)(sh->m.clock >> 16));
}

/* A latched click with the button up, taken: the latch cleared as
 * mouse_read clears it. */
static int click_taken(uw_shell *sh) {
    if (rw(sh->ds, 0x0115) == 0xffff || sh->m.buttons) return 0;
    ww(sh->ds, 0x0115, 0xffff);
    sh->ds[0x011d] = 0;
    return 1;
}

static int16_t cursor_x(const uw_shell *sh) { return (int16_t)rw(sh->ds, 0x010e); }
static int16_t cursor_y(const uw_shell *sh) { return (int16_t)rw(sh->ds, 0x0110); }

static void set_mode(uw_shell *sh, int mode);
static const uw_shell_mode *mode_row(int number);
static void title_screen(uw_shell *sh, const char *file, int palette);

/* ---- the host's events into the driver's state ---- */
static void host_events(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uint8_t *ds = sh->ds, *keybuf = sh->keybuf;
    SDL_Event e;
    int moved = 0;
    /* the edges are this pass's: a key or button that went down in an
     * earlier one was the earlier poll's to take (an Escape that put an
     * instrument down must not skip the next cutscene) */
    sh->input_edge = 0;
    sh->esc_edge = 0;
    for (;;) {
        if (sh->script) {
            int got = harness_event(sh, &e);
            if (!got) break;
            if (got == 2) continue;
        } else if (!SDL_PollEvent(&e)) break;
        if (e.type == SDL_EVENT_QUIT) sh->running = 0;
        else if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
            uint8_t sc_ = sh->script ? (uint8_t)e.key.raw : xt_scan(e.key.scancode);
            int down = e.type == SDL_EVENT_KEY_DOWN;
            if (e.key.repeat) continue;
            /* Alt-Enter the host's, the window full screen and back: no key
             * of the original's (its Alt-Enter is bound to nothing) */
            if (!sh->script && e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)) {
                if (down) {
                    sh->fullscreen = !sh->fullscreen;
                    SDL_SetWindowFullscreen(sh->win, sh->fullscreen != 0);
                }
                continue;
            }
            if (sc_) keybuf[(uint16_t)(rw(ds, 0x2344) + sc_)] = (uint8_t)down;
            /* kbd_isr's ring: the press, with the shift the driver's table
             * half is chosen by (a script's shift is its own keys') */
            if (sc_ && down && sc_ < 0x80) {
                int shifted = sh->script ? (keybuf[(uint16_t)(rw(ds, 0x2344) + 0x2a)] | keybuf[(uint16_t)(rw(ds, 0x2344) + 0x36)]) != 0
                                         : (e.key.mod & SDL_KMOD_SHIFT) != 0;
                uw_motion_key_press(&sh->m, (uint8_t)(shifted ? sc_ | 0x80 : sc_));
            }
            if (down) { sh->input_edge |= 1; if (sc_ == 0x01) sh->esc_edge = 1; }
            if (sh->script) {
                /* the modifiers from the state array, as the driver keeps them */
                keybuf[rw(ds, 0x2334)] = (uint8_t)(keybuf[(uint16_t)(rw(ds, 0x2344) + 0x2a)] | keybuf[(uint16_t)(rw(ds, 0x2344) + 0x36)]);
                keybuf[rw(ds, 0x233c)] = keybuf[(uint16_t)(rw(ds, 0x2344) + 0x38)];
                keybuf[rw(ds, 0x2340)] = keybuf[(uint16_t)(rw(ds, 0x2344) + 0x1d)];
            } else {
                keybuf[rw(ds, 0x2334)] = (uint8_t)((e.key.mod & SDL_KMOD_SHIFT) != 0);
                keybuf[rw(ds, 0x233c)] = (uint8_t)((e.key.mod & SDL_KMOD_ALT) != 0);
                keybuf[rw(ds, 0x2340)] = (uint8_t)((e.key.mod & SDL_KMOD_CTRL) != 0);
                keybuf[rw(ds, 0x2338)] = (uint8_t)((e.key.mod & SDL_KMOD_CAPS) != 0);
            }
        } else if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                   || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            float fx, fy;
            int16_t x, y;
            if (!sh->script) SDL_ConvertEventToRenderCoordinates(sh->ren, &e);
            fx = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.x : e.button.x;
            fy = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.y : e.button.y;
            if (!sh->script) fy = screen_y_of(fy);
            if (sh->relative && e.type == SDL_EVENT_MOUSE_MOTION) {
                /* captured: the mouse's motion added to where the cursor
                 * is, as the driver's deltas are in the original */
                fx = (float)((int16_t)rw(ds, 0x010e) + drag_step(&sh->drag_fx, e.motion.xrel));
                fy = (float)(199 - (int16_t)rw(ds, 0x0110) + drag_step(&sh->drag_fy, screen_y_of(e.motion.yrel)));
            } else if (sh->relative) {
                /* a button while captured: the host's hidden pointer is
                 * still where the grab began, so the press or release
                 * is where the cursor is (a lift from the view let go
                 * over the inventory drops there) */
                fx = (float)(int16_t)rw(ds, 0x010e);
                fy = (float)(199 - (int16_t)rw(ds, 0x0110));
            }
            x = (int16_t)(fx < 0 ? 0 : fx > 319 ? 319 : fx);
            y = (int16_t)(199 - (int)(fy < 0 ? 0 : fy > 199 ? 199 : fy));
            if (sh->have_game && (x != (int16_t)rw(ds, 0x010e) || y != (int16_t)rw(ds, 0x0110))) {
                uw_motion_cursor_move(m, x, y);
                moved = 1;
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                uint16_t mask = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2 : 0;
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    /* a button down in the 3-D view walks or swings with the
                     * cursor's place: the host's pointer captured until the
                     * buttons are up, so it cannot leave the window */
                    if (!sh->script && !sh->relative && x >= 52 && x < 224 && y >= 68 && y < 181) {
                        SDL_SetWindowRelativeMouseMode(sh->win, true);
                        sh->relative = 1;
                        sh->drag_fx = sh->drag_fy = 0;
                    }
                    sh->mouse_mask |= mask;
                    m->buttons |= mask;
                    sh->input_edge |= 2;
                    /* the cursor interrupt's click latch */
                    if (mask && rw(ds, 0x0115) == 0xffff) {
                        ww(ds, 0x0117, (uint16_t)x);
                        ww(ds, 0x0119, (uint16_t)y);
                        ww(ds, 0x0115, mask);
                    }
                } else {
                    sh->mouse_mask &= (uint16_t)~mask;
                    m->buttons &= (uint16_t)~mask;
                    if (sh->relative && !m->buttons) {
                        float wx, wy;
                        SDL_SetWindowRelativeMouseMode(sh->win, false);
                        sh->relative = 0;
                        if (SDL_RenderCoordinatesToWindow(sh->ren, (float)(int16_t)rw(ds, 0x010e),
                                                          logical_y((float)(199 - (int16_t)rw(ds, 0x0110))), &wx, &wy))
                            SDL_WarpMouseInWindow(sh->win, wx, wy);
                    }
                }
            }
        }
    }
    harness_events_done(sh);
    /* mouse_btns: with no mouse button down, keypad 0 (scan code 0x52, Ins)
     * is the left button and keypad . (0x53, Del) the right -- the
     * original's keyboard play; a press latched as the interrupt's would */
    if (!sh->mouse_mask) {
        uint16_t state = rw(ds, 0x2344);
        uint16_t kp = (uint16_t)((keybuf[(uint16_t)(state + 0x52)] ? 1 : 0) | (keybuf[(uint16_t)(state + 0x53)] ? 2 : 0));
        if (kp & ~m->buttons) {
            sh->input_edge |= 2;
            if (rw(ds, 0x0115) == 0xffff) {
                ww(ds, 0x0117, rw(ds, 0x010e));
                ww(ds, 0x0119, rw(ds, 0x0110));
                ww(ds, 0x0115, (uint16_t)(kp & ~m->buttons));
            }
        }
        m->buttons = kp;
    }
    /* cursor_update_position with the mouse still: the keyboard's glide,
     * and the host's pointer put where it took the cursor, so the mouse
     * goes on from there as the original's deltas would */
    if (sh->have_game && !moved && (int16_t)rw(ds, 0x0125) >= 0) {
        uw_motion_cursor_glide(m);
        if (!sh->script && sh->win && sh->ren) {
            float wx, wy;
            if (SDL_RenderCoordinatesToWindow(sh->ren, (float)(int16_t)rw(ds, 0x010e),
                                              logical_y((float)(199 - (int16_t)rw(ds, 0x0110))), &wx, &wy))
                SDL_WarpMouseInWindow(sh->win, wx, wy);
        }
    }
}

/* ---- the overlays: loops the original runs modal inside a handler ---- */

/* scroll_text_input's caret rounds since the last pass: its loop runs as
 * fast as the machine, which the port stands in for with twenty rounds a
 * tick of the 256 Hz clock -- the caret's 4000 rounds in about 0.8 s, up
 * for half of them (uw_scroll_caret_step) */
static int caret_counts(uw_shell *sh) {
    static uint32_t last;
    uint32_t now = sh->m.clock, d = now - last;
    last = now;
    return d > 256 ? 20 * 256 : (int)d * 20;
}

/* "Move how many? " up (inventory_split_stack): its field on the scroll,
 * typed into; Enter answers, Escape cancels, the left button one and the
 * right the whole stack.
 *
 * scroll_text_input(0x18c1, "1", out, digits only, 3): the prompt printed
 * by stack_ask, the default "1" drawn after it and SELECTED -- the first
 * digit typed replaces it, a backspace takes its last character -- at most
 * three digits; Enter answers with the number, Escape cancels (the field
 * erased and "-" printed), a left click answers one and a right click the
 * whole stack, the number echoed over the field (scroll_echo_number); then
 * "\n" and the take, in uw_motion_stack_answer. */
static void stack_ask_pass(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uint8_t *ds = sh->ds;
    static int asking = 0;
    static uw_scroll_edit ed;
    int end = 0, how_many = 0, all = 0, cancel = 0, clicked = 0;
    uint16_t code;
    if (!asking) {
        asking = 1;
        if (m->scroll) uw_scroll_edit_begin(m->scroll, &ed, "1", 0, 3);
        caret_counts(sh);
    }
    if (keyboard_read_press(m, sh->keybuf, &code)) {
        end = m->scroll ? uw_scroll_edit_key(m->scroll, &ed, code) : (code == 0x0d || code == 0x1b ? code : 0);
    } else if (rw(ds, 0x0115) != 0xffff && !m->buttons) {
        uint16_t pend = rw(ds, 0x0115);
        ww(ds, 0x0115, 0xffff);
        ds[0x011d] = 0;
        clicked = 1;
        end = (pend & 2) ? 2 : 1;
        if (m->scroll) uw_scroll_edit_key(m->scroll, &ed, end);
    }
    if (!end && m->scroll) uw_scroll_edit_caret(m->scroll, &ed, caret_counts(sh));
    if (end) {
        uint16_t count = (uint16_t)((rw(sh->lseg, (uint16_t)(m->stack_ask + 6)) >> 6) & 0x3ff);
        asking = 0;
        if (end == 0x1b) cancel = 1;
        else if (clicked) { if (end == 2) all = 1; else how_many = 1; }
        else how_many = atoi(ed.text);
        trace_line(sh, "stack answer %d all %d cancel %d", how_many, all, cancel);
        if (m->scroll && clicked) uw_scroll_echo_number(m->scroll, all ? count : 1);
        uw_motion_stack_answer(m, how_many, all, cancel);
    }
    sh->passes++;
}

/* chant_mantra's scroll_text_input: the field after the
 * prompt, up to ten printable characters, Backspace, and Enter or a button
 * answering with the text, Escape with the initial text -- none. */
static void mantra_pass(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uint8_t *ds = sh->ds;
    static int asking = 0;
    static uw_scroll_edit ed;
    int end = 0;
    uint16_t code;
    if (!asking) {
        asking = 1;
        if (m->scroll) uw_scroll_edit_begin(m->scroll, &ed, "", 1, 10);
        caret_counts(sh);
    }
    if (keyboard_read_press(m, sh->keybuf, &code)) {
        end = m->scroll ? uw_scroll_edit_key(m->scroll, &ed, code) : (code == 0x0d || code == 0x1b ? code : 0);
    } else if (rw(ds, 0x0115) != 0xffff && !m->buttons) {
        ww(ds, 0x0115, 0xffff);
        ds[0x011d] = 0;
        end = 1;
        if (m->scroll) uw_scroll_edit_key(m->scroll, &ed, end);
    }
    if (!end && m->scroll) uw_scroll_edit_caret(m->scroll, &ed, caret_counts(sh));
    if (end) {
        const char *said = m->scroll ? ed.text : "";
        asking = 0;
        trace_line(sh, "mantra \"%s\"", said);
        uw_motion_mantra_answer(m, said);
    }
    sh->passes++;
}

static int input_poll(uw_shell *sh, uint16_t *code);

/* scroll_ask_yes_no and its callers' reading of its answer:
 * the cursor hidden; y and n move the answer (scroll_redraw_yes_no when it
 * changes), Enter takes it, Escape answers no, a button answers yes for the
 * right one alone and no for the left or both -- each of those redrawn;
 * the cursor shown again. */
static void yesno_pass(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uint8_t *ds = sh->ds;
    static int asking = 0, releasing = 0;
    int answered = 0, yes = 0, set = -1;
    uint16_t code;
    if (!asking) {
        asking = 1;
        releasing = 1;
        uw_motion_cursor_hide(m);
    }
    /* its input_wait_button_release(0) first: the click that
     * asked is over before the question reads anything */
    if (releasing) {
        if (m->buttons) { sh->passes++; return; }
        releasing = 0;
        ww(ds, 0x0115, 0xffff);
        ds[0x011d] = 0;
    }
    /* then input_poll: a key, or a button -- held, mouse_read's live
     * mask, so it answers on the press */
    if (input_poll(sh, &code)) {
        if (code <= 3) {
            answered = 1;
            yes = code == 2;
            set = yes;
        } else if (code == 0x0d) { answered = 1; yes = m->yesno_value; }
        else if (code == 0x1b) { answered = 1; yes = 0; set = 0; }
        else if (code == 'y' || code == 'Y') set = 1;
        else if (code == 'n' || code == 'N') set = 0;
    }
    if (set >= 0 && (answered || set != m->yesno_value)) uw_motion_yesno_redraw(m, set);
    if (answered) {
        asking = 0;
        uw_motion_cursor_show(m);
        trace_line(sh, "yes/no answer %d", yes);
        uw_motion_yesno_answer(m, yes);
    }
    sh->passes++;
}

/* play_instrument's loop: input_poll each turn -- the timing
 * check, then a key to the handler; a click is nothing to it. */
static void instrument_pass(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uint8_t *ds = sh->ds;
    uint16_t code;
    uw_motion_instrument_tick(m);
    if (keyboard_read_press(m, sh->keybuf, &code)) {
        trace_line(sh, "instrument key %04x", code);
        uw_motion_instrument_key(m, code);
        if (!m->instrument) trace_line(sh, "the instrument put down, %ld not carried", m->not_carried);
    } else if (rw(ds, 0x0115) != 0xffff && !m->buttons) {
        ww(ds, 0x0115, 0xffff);
        ds[0x011d] = 0;
    }
    sh->passes++;
}

/* input_poll, input_poll_source(0): keyboard_read(0) and
 * mouse_read in turn, whichever the toggle puts first, the first answer
 * taken -- a key's code, or the buttons: held, the live mask; up again with
 * a click latched, the latch, cleared. 0 when neither has anything. */
static int input_poll(uw_shell *sh, uint16_t *code) {
    uw_motion *m = &sh->m;
    uint8_t *ds = sh->ds;
    int first = sh->source_toggle, k;
    sh->source_toggle ^= 1;
    for (k = 0; k < 2; k++) {
        if ((first ^ k) == 0) {
            if (keyboard_read_press(m, sh->keybuf, code)) return 1;
        } else if (m->buttons) {
            *code = (uint16_t)m->buttons;
            return 1;
        } else if (rw(ds, 0x0115) != 0xffff) {
            *code = rw(ds, 0x0115);
            ww(ds, 0x0115, 0xffff);
            ds[0x011d] = 0;
            return 1;
        }
    }
    return 0;
}

static void sound_after_restore(uw_shell *sh);

/* The options panel up (src/uw_options.c): its loop is modal inside the
 * button's click -- keys and clicks, no passes. */
static void options_pass(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uw_options *o = &sh->options;
    uint16_t code;
    uw_motion_music_restart(m);                             /* options_menu_loop's */
    if (keyboard_read_press(m, sh->keybuf, &code)) {
        trace_line(sh, "options key %04x", code);
        uw_options_key(o, code, m->clock);
    } else if (click_taken(sh)) {
        trace_line(sh, "options click at %d,%d", cursor_x(sh), cursor_y(sh));
        uw_options_click(o, cursor_x(sh), cursor_y(sh), m->clock);
    }
    if (o->redraw) {
        /* options_click_detail's view_rebuild_and_draw and screen_present:
         * the new detail in the view while the panel is still up */
        o->redraw = 0;
        refresh_view(m);
    }
    uw_options_caret(o, caret_counts(sh));
    if (o->quit) sh->running = 0;
    if (!o->active && o->restored) {
        o->restored = 0;
        trace_line(sh, "restored: level %d", rw(sh->ds, 0x7278));
        main_screen_redraw(m, game_dir, sh->pal, sh->frame, sh->tex, sh->ren, 0);
        sound_after_restore(sh);
    }
    sh->passes++;
}

/* An overlay took the pass, or none is up. */
/* The motion core's views from inside a call (uw_motion.view_hook).
 *
 * cameras_fade, the crystal ball's: the view drawn at the ball's shade, the
 * camera detached (attach_eye(-1), the free camera the ball set), the view
 * faded out and in on it, held while a mouse button is down
 * (mouse_release(1)), drawn again and faded out, the camera on the Avatar
 * again, and faded in. The hold shows a still. */
/* A vortex frame: the eye from the vortex's state -- the bearing's sine
 * and cosine / 0x100, drawn in by (0x40 - timer) / 0x40, times the radius
 * / 2 in 16 bits, about the gate's corner plus 0x80; the height the
 * Avatar's eye less the timer * 2; the heading the bearing + 0x7fff, no
 * pitch, the roll the timer << 11 -- drawn and shown. The port's view
 * takes it as the free camera, which is put back after. */
static void view_hook(void *user, uw_motion *m, int kind) {
    uw_shell *sh = user;
    uint8_t *ds = m->ds;
    const uint8_t *light = sc ? sc->light : NULL;
    if (kind == UW_VIEW_CAMERAS_FADE) {
        /* view_rebuild_and_draw first: the Avatar's own view at the shade
         * crystal_ball just set, which the fade out starts from */
        refresh_view(m); sh->renders++;
        ww(ds, 0x2e10, 0); ww(ds, 0x2e12, 0);
        screen_fade_run(m, 1, light, sh->pal, sh->frame, sh->tex, sh->ren);
        refresh_view(m); sh->renders++;
        screen_fade_run(m, 0, light, sh->pal, sh->frame, sh->tex, sh->ren);
        trace_line(sh, "the crystal ball's view, held while a button is down");
        while (m->buttons && sh->running) {
            if (sh->script) m->clock += 4;
            else m->clock = sh->clock0 + (uint32_t)((SDL_GetTicksNS() - sh->t0) * 256 / 1000000000ull);
            host_events(sh);
            show(m->screen, sh->pal, sh->frame, sh->tex, sh->ren);
            if (!sh->script) SDL_DelayNS(8000000);
        }
        /* the far view drawn again before it fades */
        refresh_view(m); sh->renders++;
        screen_fade_run(m, 1, light, sh->pal, sh->frame, sh->tex, sh->ren);
        /* attach_eye(1): the camera on the Avatar, event 2 */
        memcpy(ds + 0x2e10, ds + 0x7274, 4);
        ww(ds, 0x56aa, (uint16_t)(rw(ds, 0x56aa) | 2));
        refresh_view(m); sh->renders++;
        screen_fade_run(m, 0, light, sh->pal, sh->frame, sh->tex, sh->ren);
    } else if (kind == UW_VIEW_VORTEX_FRAME) {
        uint8_t saved[16], ptr[4];
        int16_t s, c, x, y, timer = (int16_t)rw(ds, 0x357c), rad = (int16_t)rw(ds, 0x357a);
        uint16_t theta = rw(ds, 0x357e);
        memcpy(saved, ds + 0x358c, 12);
        memcpy(ptr, ds + 0x2e10, 4);
        uw_sincos_lerp(theta, &s, &c);
        x = (int16_t)(s / 0x100);
        y = (int16_t)(c / 0x100);
        x = (int16_t)(x * (0x40 - timer) / 0x40);
        y = (int16_t)(y * (0x40 - timer) / 0x40);
        ww(ds, 0x3592, (uint16_t)((ds[0x3598] << 8) + (int16_t)(x * rad) / 2 + 0x80));
        ww(ds, 0x3594, (uint16_t)((ds[0x3599] << 8) + (int16_t)(y * rad) / 2 + 0x80));
        ww(ds, 0x3596, (uint16_t)(rw(ds, 0x2784) + 0xa4 - timer * 2));
        ww(ds, 0x358c, (uint16_t)(theta + 0x7fff));
        ww(ds, 0x358e, 0);
        ww(ds, 0x3590, (uint16_t)(timer << 11));
        ww(ds, 0x2e10, 0); ww(ds, 0x2e12, 0);
        refresh_view(m); sh->renders++;
        show(m->screen, sh->pal, sh->frame, sh->tex, sh->ren);
        harness_vortex_frame(sh, timer);
        memcpy(ds + 0x358c, saved, 12);
        memcpy(ds + 0x2e10, ptr, 4);
    }
}

/* do_beep(hz, ms): sound(hz), delay(ms), nosound() -- the PC speaker's own
 * tone, whatever UW.CFG's drivers, the program's one direct use of it. A
 * square wave mixed into the output for its length; the 10 ms delay is the
 * host's. */
static void speaker_beep(uw_shell *sh, int hz, int ms) {
    sh->beep_left = (long)UW_OPL_RATE * ms / 1000;
    sh->beep_step = (double)hz / UW_OPL_RATE;
    sh->beep_phase = 0;
    trace_line(sh, "speaker beep %d Hz, %d ms", hz, ms);
}

/* A dungeon print stopped at its wait -- a \P or \p pause (200 or 600
 * ticks) or a [MORE] (no timeout) -- holds the game as the original's
 * scroll_wait does, inside the print: nothing runs until the input
 * changes or the pause is over, then the print goes on, and the prints
 * made behind it (src/uw_scroll.c's hold_waits). */
static void scroll_wait_pass(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uw_scroll *s = &sh->scroll;
    uint16_t code;
    int input = 0;
    if (!sh->scroll_wait_until && !sh->scroll_wait_set) {
        sh->scroll_wait_set = 1;
        sh->scroll_wait_until = s->stop_kind == 1 ? m->clock + 200 : s->stop_kind == 2 ? m->clock + 600 : 0;
        trace_line(sh, "the scroll waits (kind %d)", s->stop_kind);
    }
    if (click_taken(sh)) input = 1;
    if (keyboard_read_press(m, sh->keybuf, &code)) input = 1;
    sh->passes++;
    /* the timeout's `ja`: the clock past the end, not at it */
    if (!input && !(sh->scroll_wait_until && (int32_t)(m->clock - sh->scroll_wait_until) > 0)) return;
    sh->scroll_wait_set = 0;
    sh->scroll_wait_until = 0;
    if (!uw_scroll_resume(s)) trace_line(sh, "the scroll waits again");
}

static int modal_pass(uw_shell *sh) {
    sh->scroll.hold_waits = sh->mode == MODE_DUNGEON;
    if (sh->mode == MODE_DUNGEON && sh->scroll.stopped) { scroll_wait_pass(sh); return 1; }
    if (sh->m.stack_ask) { stack_ask_pass(sh); return 1; }
    if (sh->m.mantra_ask) { mantra_pass(sh); return 1; }
    if (sh->m.yesno_ask) { yesno_pass(sh); return 1; }
    if (sh->m.instrument) { instrument_pass(sh); return 1; }
    if (sh->options.active) { options_pass(sh); return 1; }
    return 0;
}

/* ---- input_tick, or a release wait's polls ---- */
static void input_tick_pass(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uint8_t *ds = sh->ds;
    uint16_t code, index;
    /* the drag wait's turn: keyboard_read(1) to cursor_key_move, the
     * keypad gliding the cursor while the button is down */
    if (m->drag_wait && keyboard_read_held(m, sh->keybuf, &code, &index)) uw_motion_cursor_key(m, code);
    if (m->drag_wait) {
        /* cursor_wait_for_drag: the cursor more than six
         * from where the button went down is a drag, the lift; the
         * button up before that a click */
        int dx = abs(cursor_x(sh) - sh->drag_x), dy = abs(cursor_y(sh) - sh->drag_y);
        if (dx + dy > 6) {
            trace_line(sh, "drag, the lift, %ld not carried", m->not_carried);
            m->drag_wait = 0;
            if (m->view_drag) uw_motion_view_drag(m);
            else if (m->barter_wait == 1) uw_motion_barter_drag(m);
            else uw_motion_inventory_drag(m);
        } else if (!m->buttons) {
            trace_line(sh, "no drag, the click");
            m->drag_wait = 0;
            if (m->view_drag) uw_motion_view_click_end(m);
            else if (m->barter_wait == 1) uw_motion_barter_click_end(m);
            else uw_motion_inventory_click_end(m);
            ww(ds, 0x0115, 0xffff);
            if (!m->buttons) ww(ds, 0x011b, 0);
        }
    } else if (sh->waiting) {
        /* input_wait_button_release's turn: input_poll_repeat
         * first, keyboard or mouse by the toggle -- a held key's code, once
         * keyboard_scan_held's gate lets it through, ends the wait with the
         * button still down; the mouse's mask keeps it -- then the body's
         * keyboard_read(1) to cursor_key_move */
        int broke = 0;
        if (m->buttons) {
            int first = sh->source_toggle;
            sh->source_toggle ^= 1;
            if (first == 0 && keyboard_read_held(m, sh->keybuf, &code, &index)) broke = 1;
            else if (keyboard_read_held(m, sh->keybuf, &code, &index)) uw_motion_cursor_key(m, code);
        }
        if (!m->buttons || broke) {
            int held = rw(ds, 0x5b06) || rw(ds, 0x5b08);
            trace_line(sh, "%s, view_wait %04x handler %08x held %d, %ld not carried", broke ? "a key breaks the wait" : "release",
                       m->view_wait, sh->click_handler, held, m->not_carried);
            if (!broke) uw_motion_input_release(m, 0);
            if (m->options_open) {
                /* panel_button_click's options button: the panel's
                 * loop, modal in the click */
                m->options_open = 0;
                uw_options_open(&sh->options);
                trace_line(sh, "the options panel opened");
            } else if (m->view_wait) uw_motion_view_wait_end(m, m->view_wait);
            else if (m->talk_object) {
                /* action_talk's rest: conv_begin_with_object */
                uint16_t obj = m->talk_object;
                m->talk_object = 0;
                if (sh->talk_open) {
                    int began = uw_talk_begin(&sh->talk, obj, m->clock);
                    trace_line(sh, "talk to %04x: %s, wait %d", obj, began ? "began" : "no response", sh->talk.wait);
                } else UW_NOT_CARRIED(m->not_carried);
            } else if (sh->click_handler == 0x2b13131du && sh->click_panel_mode == 0) {
                /* paperdoll_click past its release wait: the drop of
                 * what is held, or the click's rest. Only in panel mode
                 * 0: rune_bag_click and stats_skill_scroll_click end at
                 * their own wait and have no rest to run. */
                if (held) uw_motion_inventory_release(m);
                else uw_motion_inventory_click_end(m);
            } else if (held && sh->click_handler == 0x2b130effu) {
                /* a thing picked up from the view (inventory_begin_drag)
                 * dropped where the button came up */
                uw_motion_inventory_release(m);
            } else uw_motion_input_wait_end(m);
            ww(ds, 0x0115, 0xffff);
            /* a rest's own release wait ends at its first poll with
             * the button up */
            if (!m->buttons) ww(ds, 0x011b, 0);
            if (broke) {
                /* the wait's tail with the button still down: the
                 * click latched again where the cursor is, for input_tick
                 * to deliver -- the handler runs again while the key and
                 * the button are held. 0x11b is the port's own "in a wait",
                 * which the original never reads outside the loop */
                ww(ds, 0x0115, (uint16_t)m->buttons);
                ww(ds, 0x0117, rw(ds, 0x010e));
                ww(ds, 0x0119, rw(ds, 0x0110));
                ww(ds, 0x011b, 0);
            }
            if (m->options_open) {
                /* the options button's rest, after its wait */
                m->options_open = 0;
                uw_options_open(&sh->options);
                trace_line(sh, "the options panel opened");
            }
        }
    } else {
        int got = 0, k;
        for (k = 0; k < 2 && !got; k++) {
            int keyboard = (sh->source_toggle ^ k) == 0;
            if (keyboard) {
                if (keyboard_read_held(m, sh->keybuf, &code, &index)) {
                    uw_motion_input_key(m, code, index);
                    trace_line(sh, "key %04x index %02x, %ld not carried", code, index, m->not_carried);
                    if (m->options_open) {
                        /* options_open_from_key: the panel for the key's setting */
                        m->options_open = 0;
                        uw_options_open_from_key(&sh->options, m->options_key, m->clock);
                        m->options_key = 0;
                    }
                    got = 1;
                }
            } else {
                /* mouse_read: the latched click on the release, or the
                 * live mask; nothing pending clears the latch */
                uint16_t pend = rw(ds, 0x0115);
                if (pend == 0xffff) {
                    ds[0x011d] = 0;
                    if (m->buttons) { uw_motion_input_click(m, (int16_t)m->buttons, 1); got = 1; }
                    else ww(ds, 0x011e, 0);
                } else if (m->buttons) {
                    uw_motion_input_click(m, (int16_t)m->buttons, 1);
                    got = 1;
                } else {
                    uw_motion_input_click(m, (int16_t)pend, 0);
                    got = 1;
                }
                if (got) {
                    sh->click_handler = hotspot_at(ds);
                    /* which of panel_inventory_click's three the click
                     * went to: the paperdoll (0) has a rest past its
                     * release wait, the rune bag (1) and the skill
                     * scroll (2) are over when their wait is. No click
                     * handler writes panel_mode, so reading it here
                     * reads it as the click found it. */
                    sh->click_panel_mode = ds[0x784];
                    if (m->drag_wait) { sh->drag_x = cursor_x(sh); sh->drag_y = cursor_y(sh); }
                    /* action_handlers[1]: a click in the view in fight
                     * mode is action_combat's, which the port leaves to
                     * its host */
                    if (sh->click_handler == 0x2b130effu && rw(ds, 0x268c) == 2 && (rw(ds, 0x011e) & 2))
                        uw_motion_action_combat_click(m);
                }
                if (got)
                    trace_line(sh, "click %04x %s at %d,%d action %04x wait %04x view_wait %04x, %ld not carried",
                               rw(ds, 0x011e), pend == 0xffff || m->buttons ? "held" : "latched",
                               cursor_x(sh), cursor_y(sh), rw(ds, 0x268c), rw(ds, 0x011b), m->view_wait,
                               m->not_carried);
            }
        }
        sh->source_toggle ^= 1;
    }
}

/* ---- mode 1: the dungeon ---- */

/* The map asked for (use_readable of item 0x13b): automap_draw
 * -- the level's marks saved, the screen drawn, mode 2. */
static void map_enter(uw_shell *sh) {
    uint8_t *ds = sh->ds;
    sh->m.map_open = 0;
    trace_line(sh, "the map opened");
    if (uw_automap_open(&sh->map, &sh->m, game_dir, &sh->ark)) {
        ww(ds, (uint16_t)(rw(ds, 0x00e2) + 8), 2);
        ww(ds, 0x565e, 2);
        set_mode(sh, MODE_MAP);                             /* game_change_mode(2): the dungeon's leave */
        uw_motion_music_theme(&sh->m, 0xd);                 /* automap_draw: set_theme_music(0xd), sound_update */
    } else UW_NOT_CARRIED(sh->m.not_carried);
    sh->passes++;
}

/* palette_load(n): PALS.DAT's palette n into the working
 * palette, uploaded -- the confusion's eight alternatives, and 0 back */
static void shell_palette_load(uw_shell *sh, int n) {
    char path[600];
    uw_blob pals;
    snprintf(path, sizeof path, "%s/DATA/PALS.DAT", game_dir);
    pals = uw_read_file(path);
    if (pals.data && pals.size >= (size_t)(n + 1) * 768) memcpy(sh->pal, pals.data + n * 768, 768);
    else UW_NOT_CARRIED(sh->m.not_carried);
    uw_free(&pals);
}

/* perform_pending_teleport: event 3 of the dungeon's row, and
 * the call player_death and game_ending_sequence make themselves, with
 * what each does after it -- the silver tree's arrival, the moongate's
 * fade flag back */
static void pending_teleport(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uint8_t *ds = sh->ds;
    /* perform_pending_teleport's frame, as the original's stack holds it */
    uint16_t bp = 0x955e;
    int level = rw(ds, 0x7278);
    int ok = uw_motion_pending_teleport(m, &sh->ark, sh->terrain.data, sh->terrain.size, bp);
    if (rw(ds, 0x7278) != level) ww(ds, 0x56aa, (uint16_t)(rw(ds, 0x56aa) | 2));
    /* player_death's teleport to the silver tree: its continuation */
    if (m->death) { uw_motion_death_teleported(m, ok); trace_line(sh, "the Avatar died and %s", ok ? "rose at the tree" : "found no place to rise"); }
    /* the Slasher stage's teleport to level 9: the fade flag back */
    if (m->ending) { uw_motion_ending_teleported(m); trace_line(sh, "the moongate: level %d", rw(ds, 0x7278)); }
}

static void dungeon_pass(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uint8_t *ds = sh->ds;
    uint16_t pending, bit;
    if (m->map_open) { map_enter(sh); return; }
    /* --objcheck: the original's own validator over each level the game
     * comes to, reported when a list is not sound */
    if (sh->objcheck && rw(ds, 0x7278) != sh->objcheck_level) {
        sh->objcheck_level = rw(ds, 0x7278);
        if (!uw_motion_objcheck(m)) fprintf(stderr, UW_PROGRAM ": level %u's object lists are not sound (objcheck_run)\n", sh->objcheck_level);
        else trace_line(sh, "level %u's object lists sound", sh->objcheck_level);
    }
    if (m->render.palette) {
        shell_palette_load(sh, m->render.palette - 1);
        trace_line(sh, "palette_load(%d)", m->render.palette - 1);
        m->render.palette = 0;
    }

    /* ---- event_dispatch: the pending word tested afresh for
     * each bit, so a handler's post of a later bit runs this pass, and the
     * handler looked up by the mode index as it stands -- the dungeon's
     * handlers only while it is still the dungeon's ---- */
    for (bit = 0; bit < 15; bit++) {
        pending = rw(ds, 0x56aa);
        if (!(pending & (1u << bit))) continue;
        ww(ds, 0x56aa, (uint16_t)(pending & ~(1u << bit)));
        if (rw(ds, 0x5664) != 0) continue;
        switch (bit) {
        case 1:
            refresh_view(m); sh->renders++;
            /* the arrival's view is on the screen now, so the fade the
             * teleport or the night asked for has something to bring
             * back (uw_motion.screen_fade) */
            if (m->screen_fade & 2) {
                screen_fade_run(m, 0, sc ? sc->light : NULL, sh->pal, sh->frame, sh->tex, sh->ren);
                m->screen_fade &= (uint8_t)~2u;
                trace_line(sh, "the view faded in");
            }
            break;
        case 3: pending_teleport(sh); break;
        case 9: uw_motion_dungeon_refresh_inventory(m); break;
        case 10:                                            /* event 0x400: game_ending_sequence */
            uw_motion_ending(m);
            if (m->ending_show || m->ending)
                trace_line(sh, "game_ending_sequence: %s", m->ending_show ? "the ending" : "the Slasher of Veils");
            break;
        case 11: uw_motion_frame(m); break;
        case 12: uw_motion_tick_update(m, m->clock, 0); break;
        case 13: uw_motion_panels_redraw(m, m->clock, 0); break;
        default: break;
        }
        /* player_death and game_ending_sequence call perform_pending_teleport
         * themselves, straight after trap_teleport (the ending's moongate,
         * the silver tree's): the host's here, after the handler that asked
         * -- not at event 3, which only a moving Avatar's physics step posts,
         * so a standing one would never go through the moongate */
        if (m->death || m->ending) pending_teleport(sh);
        if (sh->trace && m->not_carried != sh->traced_nc) {
            trace_line(sh, "event %u left %ld calls not carried", bit, m->not_carried);
            sh->traced_nc = m->not_carried;
        }
    }
    /* the mode's standing mask, event_mask_table[mode]: 0x3800
     * for the dungeon */
    ww(ds, 0x56aa, (uint16_t)(rw(ds, 0x56aa) | rw(ds, (uint16_t)(0x13ac + rw(ds, 0x5664) * 2))));
    uw_motion_dispatch_countdown(m);
    sh->passes++;

    /* A creature that opened the conversation itself: goal 10's
     * creature_goal_talk does it from inside the frame, where the
     * original's conv_begin_with_object runs its own loop. The port
     * leaves the object here and the conversation starts at the top of
     * the next pass, which is the same deferral action_talk takes. */
    if (m->talk_object) {
        uint16_t obj = m->talk_object;
        m->talk_object = 0;
        if (sh->talk_open) {
            int began = uw_talk_begin(&sh->talk, obj, m->clock);
            trace_line(sh, "hailed by %04x: %s, wait %d", obj, began ? "began" : "no response", sh->talk.wait);
        } else UW_NOT_CARRIED(m->not_carried);
    }

    input_tick_pass(sh);

    /* conv_begin_with_object's game_change_mode(4): the conversation's
     * passes run from the next pass on, this one's input_tick having run
     * as the dungeon's */
    if (sh->talk.wait != UW_TALK_IDLE) {
        set_mode(sh, MODE_CONV);                            /* the dungeon's leave */
        uw_motion_music_theme(m, 0xd);                      /* converse_draw_screen: set_theme_music(0xd), sound_update */
    }
}

/* ---- mode 2: the automap (src/uw_automap.c), which using the map (item
 * 0x13b) enters and Escape leaves ---- */

static void map_pass(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uw_automap *a = &sh->map;
    uint16_t code, index;
    int key;
    if (sh->map_release) {
        /* the erase's closing mouse_release(1): the press taken, nothing
         * more until the button is up */
        ww(sh->ds, 0x0115, 0xffff);
        sh->ds[0x011d] = 0;
        sh->passes++;
        if (m->buttons) return;
        sh->map_release = 0;
        return;
    }
    if (a->deleting) {
        /* the erase's `while (mouse_get_input() != 1)`, which polls the
         * input alone (no music): keys and the right button read and thrown
         * away; the left PRESS ends it -- input_poll answers the held mask
         * -- and the note under the cursor then is erased at once */
        (void)keyboard_read_press(m, sh->keybuf, &code);
        if (rw(sh->ds, 0x0115) == 2 && !m->buttons) { ww(sh->ds, 0x0115, 0xffff); sh->ds[0x011d] = 0; }
        if (m->buttons == 1) {
            trace_line(sh, "map erase at %d,%d", cursor_x(sh), cursor_y(sh));
            uw_automap_click(a, cursor_x(sh), cursor_y(sh));
            sh->map_release = 1;
        }
        sh->passes++;
        return;
    }
    uw_motion_music_restart(m);                             /* automap_tick's */
    /* a note being typed is automap_click's own input_poll loop; the map's
     * Escape otherwise is input_tick's */
    if (a->deleting) key = 0;
    else if (a->editing) key = keyboard_read_press(m, sh->keybuf, &code);
    else if ((key = keyboard_read_held(m, sh->keybuf, &code, &index)) != 0) key_taken(sh, index);
    {
        int16_t cx = cursor_x(sh), cy = cursor_y(sh);
        if (key) {
            trace_line(sh, "map key %04x", code);
            uw_automap_key(a, code);
            if (a->beeps) { speaker_beep(sh, 300, 10); a->beeps = 0; }
        } else if (click_taken(sh)) {
            trace_line(sh, "map click at %d,%d", cursor_x(sh), cursor_y(sh));
            uw_automap_click(a, cursor_x(sh), cursor_y(sh));
        }
        /* the note's caret moved the cursor (mouse_putxy), which reads the
         * driver's motion and drops it, so the mouse goes on from the
         * caret: the host's pointer put there too, or its next motion would
         * pull the cursor back under it */
        if ((cursor_x(sh) != cx || cursor_y(sh) != cy) && !sh->script && sh->win && sh->ren) {
            float wx, wy;
            if (SDL_RenderCoordinatesToWindow(sh->ren, (float)cursor_x(sh), logical_y((float)(199 - cursor_y(sh))), &wx, &wy))
                SDL_WarpMouseInWindow(sh->win, wx, wy);
        }
    }
    if (a->leave) set_mode(sh, MODE_DUNGEON);
    sh->passes++;
}

/* game_change_mode(1) from the map: the dungeon's mode and its screen. */
static void map_leave(uw_shell *sh) {
    uint8_t *ds = sh->ds;
    trace_line(sh, "the map closed, %ld not carried", sh->map.not_carried);
    uw_automap_close(&sh->map);
    uw_motion_music_pick_level_theme(&sh->m);               /* automap_click's, on the way out */
    ww(ds, (uint16_t)(rw(ds, 0x00e2) + 8), 1);
    ww(ds, 0x565e, 1);
    /* the dungeon's enter, the redraw with its two palette fades, and the
     * palette_load(0) inside it puts the dungeon's own back over the map's */
    main_screen_redraw(&sh->m, game_dir, sh->pal, sh->frame, sh->tex, sh->ren, 1);
    ww(ds, 0x56aa, (uint16_t)(rw(ds, 0x56aa) | 0x7dfe));
}

/* ---- mode 4: a conversation (src/uw_talk.c), which has no standing
 * events; the menu's digits and clicks, the question's typing ---- */

static void conv_pass(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uw_talk *t = &sh->talk;
    uint16_t code;
    static int prev_wait = -1;
    if (sh->trace && t->wait != prev_wait) {
        trace_line(sh, "talk wait now %d, scroll kind %d pending %d stopped %d hold %u menu %d",
                   t->wait, m->scroll ? m->scroll->stop_kind : -1, m->scroll ? m->scroll->npending : -1,
                   m->scroll ? m->scroll->stopped : -1, t->hold_until ? t->hold_until - sh->clock0 : 0, t->menu_options);
        prev_wait = t->wait;
    }
    if (t->wait == UW_TALK_IDLE) {
        /* ended by something other than its own passes: back to the dungeon */
        set_mode(sh, MODE_DUNGEON);
        sh->passes++;
        return;
    }
    if (t->wait == UW_TALK_ASK) {
        /* babl_ask's scroll_text_input polls the keys itself: the field's
         * editor (uw_talk_key), Enter or Escape answering */
        static int asking;
        if (!asking) { asking = 1; caret_counts(sh); }
        if (keyboard_read_press(m, sh->keybuf, &code)) {
            if (code == 0x0d || code == 0x1b) trace_line(sh, "answer \"%s\"", code == 0x1b ? "" : t->edit.text);
            uw_talk_key(t, code, m->clock);
        }
        if (t->wait == UW_TALK_ASK) uw_talk_caret(t, caret_counts(sh));
        else asking = 0;
        sh->passes++;
        return;
    }
    if (t->wait == UW_TALK_HOLD || t->wait == UW_TALK_MORE) {
        /* a hold, a pause or a [MORE]: any input change ends it */
        int before = t->wait, click = 0;
        if (click_taken(sh)) click = 1;
        if (keyboard_read_press(m, sh->keybuf, &code)) click = 1;
        uw_talk_tick(t, m->clock, click);
        if (sh->trace && (click || t->wait != before))
            trace_line(sh, "talk wait %d -> %d, scroll kind %d pending %d stopped %d hold %u",
                       before, t->wait, m->scroll ? m->scroll->stop_kind : -1, m->scroll ? m->scroll->npending : -1,
                       m->scroll ? m->scroll->stopped : -1, t->hold_until ? t->hold_until - sh->clock0 : 0);
        sh->passes++;
        if (t->wait == UW_TALK_DONE) set_mode(sh, MODE_DUNGEON);
        return;
    }
    /* UW_TALK_MENU (conv_menu_run's pass) or UW_TALK_DONE: no standing
     * events; input_tick through the port -- mode 4's bindings are the
     * digits 1..4 and the menu window's rows (conv_menu_choose, which sets
     * menu_choose for below), Escape (a no-op), the trade table's slots
     * (src/uw_motion_barter.c) and the inventory panel -- and the waits
     * those begin. Each pass loop_music_maybe first (conv_menu_run's) */
    sh->passes++;
    /* conv_menu_run's loop_music_maybe, once a time round its loop: not
     * inside the relist's scroll_wait, which never polls the music */
    if (!t->relist_at) uw_motion_music_restart(m);
    if (t->wait == UW_TALK_MENU && !m->ds[0x0a8e] && !sh->waiting && !m->buttons) {
        /* a print cleared the menu: scroll_wait(500, 0) -- any input or the
         * ticks -- and the options listed again (uw_talk_tick); not while a
         * handler's own wait (a release, a drag) is still going on, which
         * the original finishes before its loop comes round */
        int click = 0;
        if (click_taken(sh)) click = 1;
        if (keyboard_read_press(m, sh->keybuf, &code)) click = 1;
        uw_talk_tick(t, m->clock, click);
        if (!m->ds[0x0a8e]) return;
    }
    input_tick_pass(sh);
    /* conv_menu_choose, asked for by a digit key or the menu window's
     * hotspot: the option, or the one under the cursor */
    if (m->menu_choose) {
        int opt = m->menu_option ? m->menu_option : uw_talk_option_at_cursor(t);
        m->menu_choose = 0;
        trace_line(sh, "menu choose %d (option %d)", m->menu_option, opt);
        if (t->wait == UW_TALK_MENU && opt) uw_talk_choose(t, opt, m->clock);
    }
    if (t->wait == UW_TALK_DONE) set_mode(sh, MODE_DUNGEON);
}

/* The conversation's end: conv_start's tail and the dungeon's enter,
 * game_change_mode(1)'s, whatever ended it. */
static void conv_leave(uw_shell *sh) {
    uw_talk *t = &sh->talk;
    if (t->wait != UW_TALK_IDLE) {
        uw_talk_finish(t);
        uw_motion_music_pick_level_theme(&sh->m);           /* converse_refresh's */
        trace_line(sh, "the conversation ended, %ld not carried", t->not_carried + t->bi.not_carried);
    }
    main_screen_redraw(&sh->m, game_dir, sh->pal, sh->frame, sh->tex, sh->ren, 1);
}

/* ---- the character generation screen (src/uw_chargen_ui.c): new_game's
 * own loops, before the dungeon has a pass ---- */

static void chargen_pass(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uw_chargen_ui *u = &sh->cgui;
    uint16_t code;
    uw_motion_music_restart(m);                             /* chargen_select_from_list's */
    /* chargen_list_run and chargen_list_move_highlight
     * draw between cursor_hide and cursor_show, which is what
     * keeps the cursor's saved background the screen it will be put back
     * over: drawn on, it would keep the pixels of the screen before the
     * answer and paint them back where the cursor next moved from */
    if (keyboard_read_press(m, sh->keybuf, &code)) {
        trace_line(sh, "chargen step %d key %04x", u->step, code);
        uw_motion_cursor_hide(m);
        uw_chargen_ui_key(u, code);
        uw_motion_cursor_show(m);
    } else if (click_taken(sh)) {
        trace_line(sh, "chargen step %d click at %d,%d", u->step, cursor_x(sh), cursor_y(sh));
        uw_motion_cursor_hide(m);
        uw_chargen_ui_click(u, cursor_x(sh), cursor_y(sh));
        uw_motion_cursor_show(m);
    }
    if (u->state) set_mode(sh, u->state > 0 || !sh->chargen_from_menu ? MODE_DUNGEON : MODE_MENU);
    sh->passes++;
}

static void menu_redraw(uw_shell *sh);
static void dungeon_from_menu(uw_shell *sh);

/* The character made, or abandoned: new_game's second half, and the
 * dungeon's first pass armed; abandoned, the program ends -- or, from the
 * title screen, main_menu's loop goes round again. */
static void chargen_leave(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uint8_t *ds = sh->ds;
    int made = sh->cgui.state > 0;
    trace_line(sh, "the character is %s, %ld not carried", made ? "made" : "abandoned", sh->cgui.not_carried);
    uw_chargen_ui_close(&sh->cgui);
    if (!made) {
        if (sh->chargen_from_menu) { sh->chargen_from_menu = 0; menu_redraw(sh); return; }
        sh->running = 0;
        return;
    }
    if (sh->chargen_from_menu && sh->left_game) {
        /* new_game from the title after a game: SAVE0 emptied and
         * DATA\lev.ark copied over it, then level_load(1) -- the archive in
         * memory opened again from DATA and level 1 loaded */
        char path[600];
        snprintf(path, sizeof path, "%s/DATA/LEV.ARK", game_dir);
        uw_ark_close(&sh->ark);
        if (!uw_ark_open(&sh->ark, path) || !uw_motion_level_load(m, &sh->ark, 1, sh->terrain.data, sh->terrain.size))
            UW_NOT_CARRIED(m->not_carried);
    }
    /* Create Character's init_babl, after the archive's copy: every
     * conversation's globals zero again (MAINMENU.C:217-231 in the
     * reference) -- after a game they would hold the last game's */
    if (sh->chargen_from_menu) uw_talk_new_globals(&sh->talk);
    {
        /* the finish leaves the count at zero -- the boot's own are the
         * boot's -- but the program's since the boot (the title, the menu)
         * stay */
        long since = m->not_carried - sh->nc_at_boot;
        if (!sh->chargen_from_menu) uw_motion_cursor_show(m);   /* the title's cursor_show, which --chargen skips */
        if (!uw_boot_new_game_finish(sh->boot, m, sc, game_dir, sh->clock0, sh->start_x, sh->start_y)) {
            fprintf(stderr, UW_PROGRAM ": cannot finish the boot\n");
            exit(1);
        }
        if (since > 0) m->not_carried += since;
    }
    if (sh->start_heading >= 0) {
        ww(ds, 0x727a, (uint16_t)(sh->start_heading * 0x2000));
        ww(ds, (uint16_t)(rw(ds, 0x7270) + 0x5a), (uint16_t)(sh->start_heading * 0x2000));
    }
    ww(ds, 0x56aa, (uint16_t)(rw(ds, 0x56aa) | 0x3802));
    if (sh->chargen_from_menu) { sh->chargen_from_menu = 0; dungeon_from_menu(sh); }
}

/* ---- mode 0x10: a cutscene (src/uw_cutplay.c) ----
 *
 * cutscene_play is one call in the original, with the loop
 * inside cutscene_process_data; here the call is deferred by
 * the port (uw_motion.cutscene_due) and played out as a mode. Each pass
 * asks the player for everything the interpreter would have done by the
 * clock -- frames, fades, a voice to start -- in order and at the ticks it
 * would have done them, and presents where that leaves the screen. The
 * screen rules are the interpreter's own:
 * the working palette is COPIED and, full screen, the copy faded out over
 * 2; the frames go to the other page, so the game's page is untouched;
 * each animation file's palette replaces the copy (full screen only -- an
 * in-view cutscene keeps the game's) but reaches the DAC only through a
 * fade in that op10 asks for; at the end a full-screen cutscene fades out
 * over 2 unless one was pending, and the page is cleared. The player keeps
 * that DAC (`dac`), and the game's own palette is presented again on
 * return. */

/* The screen as the cutscene shows it: the frame page full screen, or its
 * bottom-right 0xac x 0x70 -- the source gfx_blit's last two arguments skip
 * to -- over the view's rows of the game's screen. Before the
 * first frame it is the game's page copied (gfx_copy_screen_from_page). */
static void cut_compose(uw_shell *sh, uint8_t *out) {
    const uw_cutscene *c = &sh->cut;
    enum { VX = 0x34, VTOP = 199 - 0xb4, VW = 0xac, VH = 0x70 };
    int i, j;
    if (!c->frames) { memcpy(out, sh->screen, 64000); return; }
    if (c->full_screen) { memcpy(out, c->screen, UW_CUT_SCREEN); return; }
    memcpy(out, sh->screen, 64000);
    for (j = 0; j < VH; j++)
        for (i = 0; i < VW; i++)
            out[(VTOP + j) * 320 + VX + i] = c->screen[(200 - VH + j) * 320 + 320 - VW + i];
}

static void cutscene_present(uw_shell *sh) {
    static uint8_t composed[64000];
    cut_compose(sh, composed);
    show(composed, sh->cut.dac, sh->frame, sh->tex, sh->ren);
}

static int  voice_open(uw_shell *sh, int n);          /* with the sound, below */
static void voice_close(uw_shell *sh);

/* op15's effect, as many as the player owes */
static void cut_effects(uw_shell *sh) {
    for (; sh->cut.effects > 0; sh->cut.effects--) uw_motion_sound_effect(&sh->m, 0x11, 0x40, 0);
}

/* voc_play_file(n): the voice opened and its length handed back -- or, one
 * that will not open, played as already over, the digital driver gone
 * with it as voc_play_file's failure takes it */
static void cut_voice(uw_shell *sh) {
    uw_cutscene *c = &sh->cut;
    int n = c->voice_start;
    if (sh->digital && voice_open(sh, n)) {
        trace_line(sh, "voice %d starts", n);
        uw_cutscene_voice_started(c, (long)(((int64_t)sh->voice_len * 256 + UW_OPL_RATE - 1) / UW_OPL_RATE));
    } else {
        trace_line(sh, "voice %d will not open", n);
        sh->digital = 0;
        uw_cutscene_voice_started(c, -1);
    }
}

/* cutscene_play's entry and the interpreter's opening, then mode 0x10
 * (game_change_mode(0x10), the mode it returns to kept as game_prev_mode
 * keeps it). */
static void cutscene_enter(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uw_cutscene *c = &sh->cut;
    int n = m->cutscene_wanted;
    m->cutscene_due = 0;
    if (!uw_cutscene_begin_patched(c, game_dir, n, m->cutscene_arg, m->strings, m->clock, sh->digital, sh->pal)) {
        trace_line(sh, "cutscene %d will not open under %s", n, game_dir);
        {
            /* said once, a player's: the game goes on without it */
            static int told;
            if (!told++) fprintf(stderr, UW_PROGRAM ": a cutscene's files are missing under %s/CUTS; cutscenes that will not open are skipped\n", game_dir);
        }
        UW_NOT_CARRIED(m->not_carried);
        /* cutscene_play returned at once: what the handler left for after
         * it runs now */
        if (sh->have_game) uw_motion_cutscene_finished(m);
        return;
    }
    trace_line(sh, "cutscene %d begins%s", n, c->full_screen ? "" : " in the view");
    if (sh->cuts_shown < 16) sh->cuts_seen[sh->cuts_shown] = n;
    sh->cuts_shown++;
    if (sh->have_game) {
        /* no mode change: cutscene_play is a call, the music plays on
         * under it but for 1..3's; the dungeon's leave runs at its end */
        if (n == 1 || n == 2 || n == 3) uw_motion_music_load(m, 4);
        uw_motion_cursor_hide(m);                           /* cursor_hide */
    }
    sh->prev_mode = sh->mode;
    trace_line(sh, "mode %s -> cutscene", mode_row(sh->mode)->name);
    sh->mode = MODE_CUTSCENE;
    cut_effects(sh);                                        /* the records at frame 0 */
}

static void cutscene_pass(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uw_cutscene *c = &sh->cut;
    uint16_t code;
    int r;
    if (sh->have_game) uw_motion_music_restart(m);          /* cutscene_service_audio's */
    /* input_poll's answer during a wait: a key, or a
     * button past the debounce, ends a pause and asks a play-to to skip;
     * Escape then ends a skippable cutscene (op12). A key pressed during a
     * fade waits in the driver's ring for the next poll, as it does in the
     * original. */
    if (input_poll(sh, &code)) {
        uw_cutscene_advance(c, m->clock, code > 3);
        if (code == 0x1b && (c->flags & UW_CUT_SKIPPY)) {
            trace_line(sh, "cutscene %d skipped", c->number);
            uw_cutscene_skip(c);
        }
    }
    sh->input_edge = 0;
    sh->esc_edge = 0;
    /* everything the interpreter would have done by now */
    while ((r = uw_cutscene_run(c, m->clock)) != UW_CUT_IDLE) {
        cut_effects(sh);
        if (r == UW_CUT_VOICE) cut_voice(sh);
        else if (r == UW_CUT_SILENT) trace_line(sh, "voice ended");
        else if (r == UW_CUT_ENDED) {
            /* in the view the working palette is the game's, and the file's
             * colour cycles (anm_cycle) leave it rotated */
            if (!c->full_screen) memcpy(sh->pal, c->dac, 768);
            set_mode(sh, sh->prev_mode);
            break;
        }
    }
    cut_effects(sh);
    sh->passes++;
}

/* The interpreter's exit and cutscene_play's tail: the fade out a pending
 * fade in still owes, the page cleared (the other page: the game's stays),
 * the font back, load_textures_and_doors when in game -- the scene keeps
 * its textures, so nothing to reload -- and the return: full screen from
 * the dungeon is game_change_mode(1), the dungeon's leave and enter
 * handlers (dungeon_leave_handler and dungeon_draw_main_screen) over
 * the page the interpreter cleared; from the menu nothing; from another
 * mode palette_load(0) and every event posted; in the view, the view
 * refreshed. */
static void cutscene_leave(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uw_cutscene *c = &sh->cut;
    int n = c->number, to = sh->prev_mode;
    trace_line(sh, "cutscene %d ended: %ld frames, %ld records, %ld files, %ld not carried",
               n, c->frames, c->records, c->files, c->not_carried);
    voice_close(sh);                                        /* voc_stop, voc_shutdown */
    sh->cut_frames = c->frames; sh->cut_records = c->records; sh->cut_files = c->files; sh->cut_nc = c->not_carried;
    m->not_carried += c->not_carried;
    uw_cutscene_end(c);
    if (sh->have_game) uw_motion_cursor_show(m);
    if (n < 0x100) {
        /* the mode field of the event record (DS:[0xe2]+8) decides: 1 is
         * game_change_mode(1) -- the dungeon's leave and enter handlers run
         * again, which the title screen's mode field still says (game_init
         * left it 1) -- 0 nothing, else palette_load(0) and every event */
        uint16_t field = sh->have_game ? rw(sh->ds, (uint16_t)(rw(sh->ds, 0x00e2) + 8)) : 0;
        if (field == 1) {
            /* the enter's fade out runs over the interpreter's page, which
             * its exit cleared, and not over the game's the port keeps */
            uw_motion_dungeon_leave(m);
            memset(sh->screen, 0, 64000);
            main_screen_redraw(m, game_dir, sh->pal, sh->frame, sh->tex, sh->ren, 1);
        }
        else if (field != 0) UW_NOT_CARRIED(m->not_carried);
    } else if (sh->have_game) {
        ww(sh->ds, 0x56aa, (uint16_t)(rw(sh->ds, 0x56aa) | 2));   /* post_event(2) */
    }
    /* main_menu's loop goes round: an answer of 0..2 draws the screen again
     * and fades palette 2 in */
    if (to == MODE_MENU && sh->menu.last != 3) menu_redraw(sh);
    /* what the handler that asked for the cutscene left for after it */
    if (sh->have_game && n >= 0x100) uw_motion_cutscene_finished(m);
}

/* MODE_NONE: nothing to run -- the program is over. */
static void none_pass(uw_shell *sh) { sh->running = 0; }

/* ---- game_ending_sequence's ending, a step a pass ----
 *
 * cutscene_play(1); cursor_hide; screen_clear; show_fullscreen_image(7,
 * win1.byt) and input_poll until a key; the page tables swapped, win2.byt
 * onto the other page and victory_screen over it, the tables swapped back
 * and the screen copied from that page -- so win2 with the statistics is
 * what shows -- and input_poll until a key; game_return_to_menu(0);
 * trap_pending_code cleared. */
static void ending_enter(uw_shell *sh) {
    sh->m.ending_show = 0;
    sh->ending_step = 0;
    trace_line(sh, "mode %s -> ending", mode_row(sh->mode)->name);
    sh->mode = MODE_ENDING;
}

static void ending_pass(uw_shell *sh) {
    uw_motion *m = &sh->m;
    char path[600];
    uw_blob img, pals;
    uint16_t code;
    sh->passes++;
    switch (sh->ending_step) {
    case 0:
        sh->ending_step = 1;
        uw_motion_cutscene_request(m, 1);
        break;
    case 1:
        uw_motion_cursor_hide(m);
        memset(sh->screen, 0, 64000);                       /* screen_clear */
        title_screen(sh, "WIN1.BYT", 7);                    /* show_fullscreen_image(7, win1.byt) */
        trace_line(sh, "ending: win1.byt with palette 7");
        sh->input_edge = 0;
        sh->ending_step = 2;
        break;
    case 2:
        /* game_ending_sequence's `do input_poll while < 0`: a key with a
         * character, or a button, held or clicked */
        if (!input_poll(sh, &code)) break;
        snprintf(path, sizeof path, "%s/DATA/WIN2.BYT", game_dir);
        img = uw_read_file(path);
        if (img.data && img.size >= 64000) memcpy(sh->screen, img.data, 64000);
        else UW_NOT_CARRIED(m->not_carried);
        uw_free(&img);
        if (!uw_victory_draw(m, game_dir, sh->screen)) UW_NOT_CARRIED(m->not_carried);
        trace_line(sh, "ending: win2.byt with the statistics");
        sh->ending_step = 3;
        break;
    case 3:
        if (!input_poll(sh, &code)) break;
        sh->ds[0x1c8f] = 0;                                 /* trap_pending_code cleared */
        m->return_to_menu = 1;                              /* game_return_to_menu(0) */
        sh->ending_step = 4;
        break;
    default:
        break;
    }
    (void)pals;
}

/* ---- the title screen (src/uw_menu.c): main_menu's loop as a mode ----
 *
 * What the original does is in uw_menu.h. The DAC while the title is up is
 * `menu_dac`: palette 2 faded in over 2 at each redraw, and the working
 * palette's 0x40..0x7f rotating over it from menu_idle_tick. The cursor is
 * shape 0x106c (cursor_shape_push), drawn by the presenter over the
 * title's page rather than through the port's cursor -- which draws on
 * the game's page -- with the hot spot cursor_set_shape gives every shape,
 * (w / 2 - 1, h / 2): the crosshair's centre on the cursor, as the
 * original's menu shows it (tools/menu.uwx, held by `vgamem.py cmp`). */

/* A palette fade over any page and DAC, as palette_fade_run does over the
 * game's. */
static void page_fade(uw_shell *sh, const uint8_t *page, uint8_t *dac, const uint8_t *target, int steps, int out) {
    uw_palette_fade f;
    uw_palette_fade_begin(&f, out ? dac : target, steps, out);
    while (uw_palette_fade_step(&f)) {
        memcpy(dac, f.pal, 768);
        show(page, dac, sh->frame, sh->tex, sh->ren);
    }
}

static void menu_compose(uw_shell *sh, uint8_t *out) {
    int w = 0, h = 0, r, c;
    const uint8_t *arrow = uw_art(NULL, 0x106c, &w, &h);
    int cx, top;
    memcpy(out, sh->menu.screen, 64000);
    /* drawn while cursor_visible_count is up: after a death
     * the cutscene hid it and nothing shows it again, and the original's
     * title has no cursor */
    if (!arrow || (int16_t)rw(sh->ds, 0x0112) <= 0) return;
    cx = cursor_x(sh) - (w / 2 - 1);
    top = 199 - (cursor_y(sh) + h / 2);
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) {
            int sx = cx + c, sy = top + r;
            if (!arrow[r * w + c] || sx < 0 || sx >= 320 || sy < 0 || sy >= 200) continue;
            out[sy * 320 + sx] = arrow[r * w + c];
        }
}

static void menu_present(uw_shell *sh) {
    static uint8_t composed[64000];
    /* Introduction and Acknowledgements: main_menu's cutscene_play(0) and
     * the Acknowledgements' cutscene_play(10), played from here as the game's
     * present plays the ones its passes ask for -- the title asks, and
     * nothing else would start them */
    if (sh->m.cutscene_due) {
        cutscene_enter(sh);
        if (sh->mode == MODE_CUTSCENE) { cutscene_present(sh); return; }
    }
    menu_compose(sh, composed);
    show(composed, sh->menu_dac, sh->frame, sh->tex, sh->ren);
}

/* The page and DAC the mode up is presenting -- what a script's snap
 * writes: the title's, a cutscene's, or the game's. */
const uint8_t *presented_page(uw_shell *sh, const uint8_t **dac) {
    static uint8_t composed[64000];
    if (sh->mode == MODE_MENU) { menu_compose(sh, composed); *dac = sh->menu_dac; return composed; }
    if (sh->mode == MODE_CUTSCENE) { cut_compose(sh, composed); *dac = sh->cut.dac; return composed; }
    *dac = sh->pal;
    return sh->screen;
}

/* The loop's top for an answer of 0..2 (or none yet): the screen drawn,
 * palette_read(2) and palette_fade_in over 2. */
static void menu_redraw(uw_shell *sh) {
    static uint8_t composed[64000];
    uw_menu_draw(&sh->menu);
    memset(sh->menu_dac, 0, 768);
    menu_compose(sh, composed);
    page_fade(sh, composed, sh->menu_dac, sh->menu.title_palette, 2, 0);
    sh->mode = MODE_MENU;
}

/* main_menu's way out into the game: game_set_mode(1), post_event(0x7ffe),
 * options_panel_active cleared, and game_main's in-game flag set. The
 * dungeon's page is what the boot or the restore drew. */
static void dungeon_from_menu(uw_shell *sh) {
    uint8_t *ds = sh->ds;
    if (sh->left_game) {
        /* game_return_to_menu's tail: the mode index back, the mode, the
         * ENTER handler -- dungeon_mode_enter */
        uw_motion_enter_game(&sh->m, sh->m.clock);
        /* and what the left game's calls still owed, now that
         * game_return_to_menu returns into them: options_click_row's
         * cursor_show and its release wait, when the death came inside
         * a restore */
        uw_options_pay_owed(&sh->options);
    } else {
        ww(ds, 0x565e, 1);
        ww(ds, 0x5664, 0);
        ww(ds, (uint16_t)(rw(ds, 0x00e2) + 8), 1);
        ds[0x13b3] = 1;
        /* the mode's enter handler as far as the view: its hotspot and the
         * eight arrow cursor regions, which the title did without */
        uw_motion_dungeon_viewport(&sh->m);
    }
    ww(ds, 0x56aa, (uint16_t)(rw(ds, 0x56aa) | 0x7ffe));
    ww(ds, 0x1a30, 0);
    trace_line(sh, "the game begins: level %d", rw(ds, 0x7278));
    sh->mode = MODE_DUNGEON;
}

/* ---- game_return_to_menu: the way out to the title ----
 *
 * What runs before its cutscene is the port's uw_motion_leave_game, what
 * runs after it uw_motion_leave_game_finish; between them, with the death's
 * flag, cutscene 0x103 in the view -- played out as mode 0x10 returning to
 * MODE_LEAVING, whose pass is the rest: the palette copied and faded out
 * over 2 and main_menu(0), the menu again with no introduction. The way
 * back is dungeon_from_menu, with the enter handler this time. */
/* real_death's `while (mouse_get_input() > -1)`: every key and
 * the latched click drained -- and, since input_poll answers the held mask
 * while a button is down, every button let go of -- before anything else
 * runs, so none shortens the death's cutscene. 1 when it is over. */
static int leave_game_drain(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uint16_t code;
    while (keyboard_read_press(m, sh->keybuf, &code)) { }
    ww(sh->ds, 0x0115, 0xffff);
    sh->ds[0x011d] = 0;
    sh->input_edge = 0;
    sh->esc_edge = 0;
    return !m->buttons;
}

static void leave_game_go(uw_shell *sh) {
    uw_motion *m = &sh->m;
    int show = sh->leave_show;
    uw_motion_leave_game(m, show);
    sh->mode = MODE_LEAVING;
    if (show) {
        /* cutscene_play(0x103), returning here */
        uw_motion_cutscene_request(m, 0x103);
        cutscene_enter(sh);
    }
}

static void leave_game_begin(uw_shell *sh) {
    uw_motion *m = &sh->m;
    int show = m->return_to_menu == 2;
    m->return_to_menu = 0;
    sh->leave_show = show;
    trace_line(sh, "game_return_to_menu(%d)", show);
    if (leave_game_drain(sh)) { leave_game_go(sh); return; }
    trace_line(sh, "game_return_to_menu waits for the buttons");
    sh->mode = MODE_DRAINING;
}

/* the drain's later passes: nothing else runs until the buttons are up */
static void draining_pass(uw_shell *sh) {
    sh->passes++;
    if (!leave_game_drain(sh)) return;
    leave_game_go(sh);
    if (sh->mode == MODE_CUTSCENE) cutscene_present(sh);
}

static void leaving_pass(uw_shell *sh) {
    uw_motion *m = &sh->m;
    static uint8_t composed[64000];
    sh->passes++;
    uw_motion_leave_game_finish(m, sh->leave_show);
    sh->left_game = 1;
    /* the live palette copied to the stack and faded out (palette_fade_out
     * consumes what it is given): the DAC the title inherits is black */
    memcpy(sh->menu_dac, sh->pal, 768);
    memcpy(composed, sh->screen, 64000);
    page_fade(sh, composed, sh->menu_dac, NULL, 2, 1);
    /* main_menu(0): no introduction; its cursor_show as at the start */
    if (sh->menu_ok) uw_menu_close(&sh->menu);
    sh->menu_ok = uw_menu_open(&sh->menu, m, game_dir, sh->options.saves);
    uw_motion_cursor_show(m);
    if (!sh->menu_ok) { fprintf(stderr, UW_PROGRAM ": the title screen will not open under %s\n", game_dir); sh->running = 0; return; }
    trace_line(sh, "menu: %d items, cursor %d, saves %x", sh->menu.count, sh->menu.cursor, sh->menu.saves_mask);
    menu_redraw(sh);
}

static void menu_pass(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uw_menu *u = &sh->menu;
    uint16_t code;
    sh->passes++;
    uw_motion_music_restart(m);                             /* main_menu's, menu_select's */
    if (sh->menu_error_wait) {
        /* the error's wait: input_poll until a key, with the idle tick */
        /* vga_set_palette(0x40, 0x40): the rotated entries to the DAC */
        if (uw_menu_idle(u, m->clock))
            memcpy(sh->menu_dac + 0x40 * 3, sh->menu.title_palette + 0x40 * 3, 0x40 * 3);
        if (keyboard_read_press(m, sh->keybuf, &code)) {
            sh->menu_error_wait = 0;
            trace_line(sh, "menu: the error acknowledged");
        }
        return;
    }
    /* music_restart_current: no music yet. menu_idle_tick, and
     * vga_set_palette(0x40, 0x40) after it. */
    if (uw_menu_idle(u, m->clock))
        memcpy(sh->menu_dac + 0x40 * 3, sh->menu.title_palette + 0x40 * 3, 0x40 * 3);
    if (keyboard_read_press(m, sh->keybuf, &code)) {
        uw_menu_key(u, code);
        trace_line(sh, "menu key %04x: cursor %d, choice %d", code, u->list_up ? u->list_cursor : u->cursor, u->choice);
    }
    uw_menu_button(u, m->buttons != 0, cursor_x(sh), cursor_y(sh));
    if (!m->buttons && rw(sh->ds, 0x0115) != 0xffff) { ww(sh->ds, 0x0115, 0xffff); sh->ds[0x011d] = 0; }
    if (u->choice == UW_MENU_NONE) return;
    if (u->list_up && u->choice < 0) {
        /* menu_select's -1 in the list: back to the menu */
        trace_line(sh, "menu: the save list left");
        uw_menu_list_cancel(u);
        menu_present(sh);
        return;
    }
    if (u->list_up) {
        /* main_menu_save_list's pick: the message, then the restore */
        int slot = u->choice;
        u->choice = UW_MENU_NONE;
        trace_line(sh, "menu: Journey Onward, slot %d", slot);
        uw_menu_list_picked(u);
        menu_present(sh);
        if (uw_options_restore(&sh->options, slot, m->clock)) {
            sh->options.restored = 0;
            uw_boot_weapons_load(m, game_dir);          /* weapons_load_colourmap, and the anim the boot loads */
            uw_boot_level_scene(sh->boot, m, sc);       /* the scene's level, as the boot's finish makes it */
            main_screen_redraw(m, game_dir, sh->pal, sh->frame, sh->tex, sh->ren, 0);
            sound_after_restore(sh);
            dungeon_from_menu(sh);
        } else {
            uw_menu_draw_error(u);
            sh->menu_error_wait = 1;
            trace_line(sh, "menu: the restore failed");
        }
        u->last = 3;
        return;
    }
    trace_line(sh, "menu choice %d", u->choice);
    switch (u->choice) {
    case UW_MENU_QUIT:                                  /* game_shutdown(0); exit(1) */
        sh->running = 0;
        break;
    case UW_MENU_INTRO:
        uw_motion_cutscene_request(m, 0);
        break;
    case UW_MENU_ACK:                                   /* cutscene_play(10), post_event(0x7ffe) */
        uw_motion_cutscene_request(m, 10);
        break;
    case UW_MENU_CHARGEN: {
        static uint8_t composed[64000];
        menu_compose(sh, composed);
        page_fade(sh, composed, sh->menu_dac, NULL, 2, 1);   /* palette_fade_out(2) */
        uw_boot_player_init(m);                             /* new_game's player_init(1) */
        /* the cursor off the page while the screen under it is replaced:
         * its saved background is the title's (the menu draws its own
         * cursor over a page of its own, so this one's is black), and the
         * next move would put that back over the new screen */
        uw_motion_cursor_hide(m);
        if (!uw_chargen_ui_open(&sh->cgui, m, game_dir)) {
            trace_line(sh, "the character generation screen will not open");
            UW_NOT_CARRIED(m->not_carried);
            uw_motion_cursor_show(m);
            menu_redraw(sh);
            break;
        }
        uw_motion_cursor_show(m);                           /* over the screen it will be put back on */
        sh->chargen_from_menu = 1;
        trace_line(sh, "mode title -> chargen");
        sh->mode = MODE_CHARGEN;
        break;
    }
    case UW_MENU_JOURNEY:
        uw_menu_list_open(u);
        break;
    default: break;
    }
    u->last = u->choice;
    u->choice = UW_MENU_NONE;
}

/* ---- game_main before the menu: game_init's two screens and the title ----
 *
 * game_init shows DATA\pres1.byt with palette 5 while it loads and
 * DATA\pres2.byt with palette 6 after (show_fullscreen_image), clears the
 * screen and loads palette 0; play_title_cutscene drains the keys held from
 * the prompt and plays cutscene 9; then main_menu(1), whose first act is
 * cutscene 0 when no save exists. Nothing waits: each screen stands for as
 * long as the loads after it take (game_init has no delay). A machine of
 * fixed speed with a fast disk shows PRES1 from 0.07 s to 0.17, black to
 * 0.21 (show_fullscreen_image's blank while pres2.byt loads), PRES2 to
 * 0.33, then black -- the rest of game_init and play_title_cutscene's open
 * -- until the title's fade begins at 1.25 s. A scripted run keeps that
 * timeline; played, each screen is held SPLASH_HOLD longer, about as long
 * as the loads behind it took on a hard disk of the day. */
enum {                                 /* from the program's start, in ticks */
    PRES1_AT = 18,                     /* 0.07 s: after gfx_init's mode set */
    PRES_BLANK_AT = 44,                /* 0.17 s */
    PRES2_AT = 54,                     /* 0.21 s */
    PRES_END_AT = 85,                  /* 0.33 s */
    TITLE_AT = 320,                    /* 1.25 s */
    SPLASH_HOLD = 512                  /* 2 s more on each screen, played */
};

/* when step `at` comes: past each screen already shown, its hold */
static uint32_t title_at(const uw_shell *sh, uint32_t at, int screens_shown) {
    return sh->script ? at : at + (uint32_t)screens_shown * SPLASH_HOLD;
}

static void title_screen(uw_shell *sh, const char *file, int palette) {
    char path[600];
    uw_blob img, pals;
    if (file[0]) {
        snprintf(path, sizeof path, "%s/DATA/%s", game_dir, file);
        img = uw_read_file(path);
        if (img.data && img.size >= 64000) memcpy(sh->screen, img.data, 64000);
        else UW_NOT_CARRIED(sh->m.not_carried);
        uw_free(&img);
    }
    snprintf(path, sizeof path, "%s/DATA/PALS.DAT", game_dir);
    pals = uw_read_file(path);
    if (pals.data && pals.size >= (size_t)(palette + 1) * 768) memcpy(sh->pal, pals.data + palette * 768, 768);
    else UW_NOT_CARRIED(sh->m.not_carried);
    uw_free(&pals);
    trace_line(sh, "title: %s with palette %d", file, palette);
}

static void title_pass(uw_shell *sh) {
    uw_motion *m = &sh->m;
    sh->passes++;
    switch (sh->title_step) {
    case 0:
        if (m->clock - sh->clock0 < PRES1_AT) break;
        title_screen(sh, "PRES1.BYT", 5);
        uw_motion_music_load(m, 1);                         /* game_init's load_xmi(1, 1) */
        sh->title_step = 1;
        break;
    case 1:
        if (m->clock - sh->clock0 < title_at(sh, PRES_BLANK_AT, 1)) break;
        memset(sh->screen, 0, 64000);                       /* show_fullscreen_image's blank */
        sh->title_step = 2;
        break;
    case 2:
        if (m->clock - sh->clock0 < title_at(sh, PRES2_AT, 1)) break;
        title_screen(sh, "PRES2.BYT", 6);
        sh->title_step = 3;
        break;
    case 3:
        if (m->clock - sh->clock0 < title_at(sh, PRES_END_AT, 2)) break;
        memset(sh->screen, 0, 64000);                       /* screen_clear */
        title_screen(sh, "", 0);                            /* palette_load(0) */
        sh->title_step = 4;
        break;
    case 4:
        if (m->clock - sh->clock0 < title_at(sh, TITLE_AT, 2)) break;
        {
            /* titlescr's `while (mouse_get_input() > 3)`: the keys pressed
             * over the presentation screens read and thrown away, so none
             * skips or advances the title */
            uint16_t code;
            while (keyboard_read_press(m, sh->keybuf, &code)) { }
            sh->input_edge &= ~1;
            sh->esc_edge = 0;
        }
        uw_motion_cutscene_request(m, 9);                   /* play_title_cutscene */
        sh->title_step = 5;
        break;
    case 5:
        /* main_menu(1): the records, the saves, the intro when none; then
         * cursor_shape_push(0x106c) and cursor_show -- the title's cursor
         * one more on the visibility count, which nothing takes back */
        sh->menu_ok = uw_menu_open(&sh->menu, m, game_dir, sh->options.saves);
        uw_motion_cursor_show(m);
        if (!sh->menu_ok) { fprintf(stderr, UW_PROGRAM ": the title screen will not open under %s\n", game_dir); sh->running = 0; break; }
        trace_line(sh, "menu: %d items, cursor %d, saves %x", sh->menu.count, sh->menu.cursor, sh->menu.saves_mask);
        sh->title_step = 6;
        if (uw_menu_no_saves(&sh->menu)) { uw_motion_cutscene_request(m, 0); break; }
        /* fall through */
    case 6:
        sh->title_step = 7;
        menu_redraw(sh);
        break;
    default:
        break;
    }
}

/* ---- the table ---- */

/* event_handlers[0][15], dungeon_leave_handler: game_change_mode's on
 * the way to the automap or a conversation (src/uw_motion_save.c). */
static void dungeon_leave(uw_shell *sh) { uw_motion_dungeon_leave(&sh->m); }

static const uw_shell_mode modes[] = {
    { MODE_DUNGEON,  "dungeon",      dungeon_pass,  dungeon_leave,  NULL },
    { MODE_MAP,      "automap",      map_pass,      map_leave,      NULL },
    { MODE_CONV,     "conversation", conv_pass,     conv_leave,     NULL },
    { MODE_CUTSCENE, "cutscene",     cutscene_pass, cutscene_leave, cutscene_present },
    { MODE_CHARGEN,  "chargen",      chargen_pass,  chargen_leave,  NULL },
    { MODE_MENU,     "title",        menu_pass,     NULL,           menu_present },
    { MODE_TITLE,    "game_main",    title_pass,    NULL,           NULL },
    { MODE_LEAVING,  "leaving",      leaving_pass,  NULL,           NULL },
    { MODE_ENDING,   "ending",       ending_pass,   NULL,           NULL },
    { MODE_DRAINING, "draining",     draining_pass, NULL,           NULL },
    { MODE_NONE,     "none",         none_pass,     NULL,           NULL },
};

static const uw_shell_mode *mode_row(int number) {
    size_t i;
    for (i = 0; i < sizeof modes / sizeof modes[0]; i++)
        if (modes[i].number == number) return &modes[i];
    return &modes[0];
}

/* game_change_mode: the leaving mode's [15], then the new
 * mode. The entering side is the transition's own code (see uw_shell_mode). */
static void set_mode(uw_shell *sh, int mode) {
    const uw_shell_mode *from = mode_row(sh->mode);
    if (mode == sh->mode) return;
    if (from->leave) from->leave(sh);
    trace_line(sh, "mode %s -> %s", from->name, mode_row(mode)->name);
    sh->mode = mode;
}

/* ---- the screen ---- */

static void present(uw_shell *sh) {
    uw_motion *m = &sh->m;
    uint8_t *ds = sh->ds;
    /* The fade out runs here, where the screen still holds the view the
     * pass was left with -- the departure's, or the one the Avatar lay
     * down in. The fade in waits for the refresh that draws what is
     * coming back, and arms it, as the view_rebuild_and_draw the
     * original runs before its own fade in does. */
    if (m->screen_flash) {
        /* screen_show_frame: the view cleared to a colour
         * and presented once -- a critical hit's flash, a fall's, the
         * one non-lethal damage shows. The next refresh draws over it,
         * which is armed here as the original's next frame does it. */
        static uint8_t flash[UW_VIEW_BYTES];
        int fx, fy;
        enum { VX = 52, VTOP = 19, VW = UW_VIEW_W, VH = UW_VIEW_H };
        /* screen_show_frame draws between cursor_hide and cursor_show too */
        uw_motion_cursor_hide(m);
        uw_screen_show_frame(flash, sizeof flash, m->screen_flash_colour);
        for (fy = 1; fy < VH; fy++)
            for (fx = 0; fx < VW; fx++) {
                sh->screen[(VTOP + VH - 1 - fy) * 320 + VX + fx] = flash[fy * VW + fx];
                if (m->screen_written) m->screen_written[(VTOP + VH - 1 - fy) * 320 + VX + fx] = 1;
            }
        show(sh->screen, sh->pal, sh->frame, sh->tex, sh->ren);
        uw_motion_cursor_show(m);
        trace_line(sh, "the view flashed %02x", m->screen_flash_colour);
        m->screen_flash = 0;
        ww(ds, 0x56aa, (uint16_t)(rw(ds, 0x56aa) | 2));
    }
    if (m->screen_fade & 1) {
        screen_fade_run(m, 1, sc ? sc->light : NULL, sh->pal, sh->frame, sh->tex, sh->ren);
        m->screen_fade &= (uint8_t)~1u;
        trace_line(sh, "the view faded out");
    }
    if (m->screen_fade & 2) ww(ds, 0x56aa, (uint16_t)(rw(ds, 0x56aa) | 2));
    /* view_rebuild_and_draw and screen_present with no fade: the refresh */
    if (m->screen_fade & 4) {
        ww(ds, 0x56aa, (uint16_t)(rw(ds, 0x56aa) | 2));
        m->screen_fade &= (uint8_t)~4u;
    }
    if (m->screenshot) {
        screenshot_write(sh->screen, sh->pal);
        trace_line(sh, "a screenshot written");
        m->screenshot = 0;
    }
    /* the ending proper (game_ending_sequence with the code set) */
    if (m->ending_show) ending_enter(sh);
    /* game_return_to_menu, which player_death and the ending ask for:
     * after the fade out the same pass asked for */
    if (m->return_to_menu) {
        leave_game_begin(sh);
        if (sh->mode == MODE_CUTSCENE) { cutscene_present(sh); return; }
        show(sh->screen, sh->pal, sh->frame, sh->tex, sh->ren);
        return;
    }
    /* a cutscene the pass asked for plays out from here: cutscene_play is
     * synchronous in the original, so it comes after the fade out the same
     * pass asked for and before anything the next pass draws */
    if (m->cutscene_due) {
        cutscene_enter(sh);
        if (sh->mode == MODE_CUTSCENE) { cutscene_present(sh); return; }
    }
    show(sh->screen, sh->pal, sh->frame, sh->tex, sh->ren);
}

/* The pass's pace: the original ran its passes flat out; this paces them
 * at eight milliseconds, two ticks, so an idle game is idle. A scripted
 * run ends with its script.
 *
 * The dungeon's passes are held to nine ticks of the 256 Hz clock, the
 * original's own frame on its reference machine: movement integrates once a frame
 * and rounds its step, so a shorter frame loses slow moves towards +x and
 * +y -- at two ticks a backward walk covered 1023 where nine-tick frames
 * cover 1366. A scripted run keeps its four ticks a pass. */
#define DUNGEON_FRAME_NS (9ull * 1000000000ull / 256)
static void pace(uw_shell *sh) {
    if (!sh->script) {
        uint64_t now = SDL_GetTicksNS();
        uint64_t least = (uint64_t)present_pace_ms * 1000000ull;
        if (sh->mode == MODE_DUNGEON && least < DUNGEON_FRAME_NS) least = DUNGEON_FRAME_NS;
        if (now - sh->pass_start < least) SDL_DelayNS(least - (now - sh->pass_start));
        sh->pass_start = SDL_GetTicksNS();
    } else if (harness_script_over(sh)) {
        sh->running = 0;
    }
}

/* ---- the sound: sound_init with the AdLib configured ---- */

/* The driver model's synthesiser: its messages and timbres to ADLIB.ADV's
 * voice layer, and that layer's register writes to the OPL2. */
static void synth_message(void *user, uint8_t status, uint8_t d1, uint8_t d2) {
    uw_adlib_message(&((uw_shell *)user)->adlib, status, d1, d2);
}
static void synth_timbre(void *user, uint8_t bank, uint8_t program, const uint8_t *patch, size_t len) {
    uw_adlib_timbre(&((uw_shell *)user)->adlib, bank, program, patch, len);
}
static void synth_tick(void *user) { uw_adlib_service(&((uw_shell *)user)->adlib); }
static void chip_write(void *user, uint8_t reg, uint8_t value) { uw_opl_write(&((uw_shell *)user)->opl, reg, value); }

/* The files sound_init and load_xmi read -- SOUNDS.DAT, the AdLib bank and
 * driver, and the AdLib set of tracks, SOUND\AWnn.XMI by the track's two
 * octal digits -- the driver model with the voice layer and the chip
 * behind it, attached (uw_motion_sound_attach); and, playing for a
 * person, an SDL audio stream at the chip's own rate. */
static int sound_open(uw_shell *sh) {
    static const uw_ail_synth synth = { NULL, synth_message, synth_timbre, synth_tick };
    uw_ail_synth sy = synth;
    char path[600];
    int t;
    snprintf(path, sizeof path, "%s/SOUND/SOUNDS.DAT", game_dir);
    if (!uw_sounds_open(&sh->sounds, path)) return 0;
    snprintf(path, sizeof path, "%s/SOUND/UW.AD", game_dir);
    if (!uw_bank_open(&sh->timbres, path)) return 0;
    for (t = 0; t < 16; t++) {
        snprintf(path, sizeof path, "%s/SOUND/AW%c%c.XMI", game_dir, '0' + (t >> 3), '0' + (t & 7));
        sh->xmi[t] = uw_read_file(path);
        sh->m.xmi[t] = sh->xmi[t].data;
        sh->m.xmi_size[t] = sh->xmi[t].size;
    }
    snprintf(path, sizeof path, "%s/SOUND/ADLIB.ADV", game_dir);
    sh->adlib_file = uw_read_file(path);
    uw_opl_reset(&sh->opl);
    if (!uw_adlib_init(&sh->adlib, sh->adlib_file.data, sh->adlib_file.size, chip_write, sh)) return 0;
    sy.user = sh;
    uw_ail_init(&sh->ail, &sy);
    if (!sh->script) {
        SDL_AudioSpec spec = { SDL_AUDIO_S16, 1, UW_OPL_RATE };
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            fprintf(stderr, UW_PROGRAM ": no sound: SDL has no audio: %s\n", SDL_GetError());
        } else {
            sh->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
            if (sh->audio) SDL_ResumeAudioStreamDevice(sh->audio);
            else fprintf(stderr, UW_PROGRAM ": no sound: the audio device will not open: %s\n", SDL_GetError());
        }
    }
    sh->m.sounds = &sh->sounds;
    sh->m.timbre_bank = &sh->timbres;
    uw_motion_sound_attach(&sh->m, &sh->ail, 2);
    sh->digital = sh->digital_cfg;          /* the digital driver's voices, on with the sound */
    return 1;
}

/* The voice's level against the chip's: the original's mixer puts the
 * introduction's voices at 248 an 8-bit step and the chip at 1.28 of this
 * port's output. */
#define VOICE_GAIN (248.0 / 1.28)

static void voice_close(uw_shell *sh) {
    free(sh->voice);
    sh->voice = NULL;
    sh->voice_len = sh->voice_pos = 0;
}

/* voc_play_file(n) as far as what is heard: SOUND\NN.VOC's one
 * block of 8-bit samples at 1000000 / (256 - time constant) Hz -- less its
 * last 31, since the engine charges the 32-byte preamble against the
 * bytes left twice and never queues the file's last 32 bytes -- carried
 * to the chip's rate (linear, as the mixer resamples). 0 when the file
 * will not open, which leaves voc_playback_finished true. */
static int voice_open(uw_shell *sh, int n) {
    char path[600];
    uw_voc v;
    long rate, played, len, k;
    voice_close(sh);
    snprintf(path, sizeof path, "%s/SOUND/%02d.VOC", game_dir, n);
    if (!uw_voc_open(&v, path)) return 0;
    rate = uw_voc_rate(&v);
    played = (long)v.file.size - 64;
    if (played > (long)v.samples) played = (long)v.samples;
    if (v.pack || rate <= 0 || played <= 1) { uw_voc_close(&v); return 0; }
    len = (long)((double)played * UW_OPL_RATE / (double)rate);
    sh->voice = malloc((size_t)len * sizeof *sh->voice);
    if (!sh->voice) { uw_voc_close(&v); return 0; }
    for (k = 0; k < len; k++) {
        double at = (double)k * (double)rate / UW_OPL_RATE, f;
        long i = (long)at;
        const uint8_t *x = v.file.data + v.samples_at;
        double a = x[i] - 128.0, b = (i + 1 < played ? x[i + 1] : x[i]) - 128.0;
        f = at - (double)i;
        sh->voice[k] = (int16_t)lround((a + (b - a) * f) * VOICE_GAIN);
    }
    sh->voice_len = len;
    uw_voc_close(&v);
    return 1;
}

/* A tick's worth of the chip's output -- 49716 / 120 samples, the
 * fraction carried by the caller -- through the output's coupling (the
 * card's capacitor: a first-order high-pass, which takes away the constant
 * a held operator leaves), the voice added, into the audio stream. */
static void sound_render(uw_shell *sh, int n) {
    int16_t buf[UW_OPL_RATE / UW_AIL_TICK_HZ + 2];
    int i;
    uw_opl_render(&sh->opl, buf, n);
    for (i = 0; i < n; i++) {
        int32_t x = buf[i], y = x - sh->dc_x + (sh->dc_y * 255) / 256;
        sh->dc_x = x;
        sh->dc_y = y;
        if (sh->voice && sh->voice_pos + i < sh->voice_len) y += sh->voice[sh->voice_pos + i];
        if (sh->beep_left > 0) {
            y += sh->beep_phase < 0.5 ? 6000 : -6000;
            sh->beep_phase += sh->beep_step;
            if (sh->beep_phase >= 1) sh->beep_phase -= 1;
            sh->beep_left--;
        }
        buf[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    }
    if (sh->audio) SDL_PutAudioStreamData(sh->audio, buf, n * 2);
    if (sh->audio_dump) fwrite(buf, 2, (size_t)n, sh->audio_dump);
    sh->audio_samples += n;
}

/* The driver's timer at 120 Hz -- its sequencer, then the voices' own
 * service -- and sfx_timer_install's at 16 Hz, as many calls as the clock
 * has passed since the sound opened; the chip's samples for each tick when
 * there is someone to hear them. */
static void sound_timers(uw_shell *sh) {
    uint32_t elapsed;
    if (!sh->sound) return;
    elapsed = sh->m.clock - sh->sound_clock0;
    while (sh->ail_ticks < (uint64_t)elapsed * UW_AIL_TICK_HZ / 256) {
        int n;
        uw_ail_tick(&sh->ail);
        sh->audio_frac += (double)UW_OPL_RATE / UW_AIL_TICK_HZ;
        n = (int)sh->audio_frac;
        sh->audio_frac -= n;
        if (sh->audio || sh->audio_dump) sound_render(sh, n);
        /* the voice runs out on the same count heard or not, so a scripted
         * run's cutscene waits as long as a played one */
        if (sh->voice) {
            sh->voice_pos += n;
            if (sh->voice_pos >= sh->voice_len) voice_close(sh);
        }
        sh->ail_ticks++;
    }
    while (sh->age_ticks < elapsed / 16) {
        uw_motion_sound_age_slots(&sh->m);
        sh->age_ticks++;
    }
}

/* A saved game carries its own sound and music switches: the player
 * record's +0xb5, bits 0..1 the effects and bits 2..3 the music, which
 * player_load_record applies on every restore. So a game saved with them
 * off comes back silent however the sound was opened -- the save the
 * original ships is one of those (0x30: detail 3, both off), and so is any
 * game this port saved while it had no sound. With --sound the switches
 * go back on; otherwise it is said once, where someone is listening. */
static void sound_after_restore(uw_shell *sh) {
    enum { MUSIC_ENABLED = 0x0135, SFX_ENABLED = 0x0136 };
    if (!sh->sound) return;
    if (sh->sound_forced) {
        /* --sound is on regardless: the switches the restore just applied
         * go back on, which is the one thing that overrides a saved game's
         * own setting (music_set_enabled, sfx_set_enabled) */
        uw_motion_sound_enable(&sh->m, 1, 1);
        return;
    }
    if (sh->script || sh->sound_noted) return;
    if (sh->ds[MUSIC_ENABLED] || sh->ds[SFX_ENABLED]) return;
    sh->sound_noted = 1;
    fprintf(stderr, UW_PROGRAM ": the restored game has its sound and its music switched off -- the "
                    "saved game's own setting, which the options panel turns back on (Ctrl-F "
                    "sound, Ctrl-M music)\n");
}

/* ---- game_loop ---- */
static void game_loop(uw_shell *sh) {
    while (sh->running && (!sh->have_game || rw(sh->ds, 0x5666))) {   /* game_running (game_request_quit) */
        sh->waiting = rw(sh->ds, 0x011b) == 0xffff || sh->m.drag_wait;
        if (sh->script) sh->m.clock += 4;
        else sh->m.clock = sh->clock0 + (uint32_t)((SDL_GetTicksNS() - sh->t0) * 256 / 1000000000ull);
        host_events(sh);
        if (!modal_pass(sh)) mode_row(sh->mode)->pass(sh);
        sound_timers(sh);
        if (mode_row(sh->mode)->present) mode_row(sh->mode)->present(sh);
        else present(sh);
        if (sh->m.delay_ticks) {
            /* delay_ticks(n) inside the pass: the original spun for n ticks */
            if (sh->script) sh->m.clock += sh->m.delay_ticks;
            else SDL_DelayNS((uint64_t)sh->m.delay_ticks * 1000000000ull / 256);
            sh->m.delay_ticks = 0;
        }
        pace(sh);
    }
}

/* ---- the window ---- */
static int window_open(uw_shell *sh, int scale) {
    int logical_h;
    if (sh->script) { SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen"); present_aspect = 0; }
    logical_h = present_aspect ? 240 : 200;
    /* SDL's own SIGINT and SIGTERM handlers turn the signals into quit
     * events, which a loop stuck outside SDL_PollEvent never sees: a hung
     * shell then ignores timeout and kill and spins at full CPU. Off. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 0; }
    if (!SDL_CreateWindowAndRenderer(UW_WINDOW_TITLE, 320 * scale, logical_h * scale,
                                     sh->script ? 0 : SDL_WINDOW_RESIZABLE | (sh->fullscreen ? SDL_WINDOW_FULLSCREEN : 0),
                                     &sh->win, &sh->ren)) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 0;
    }
    SDL_SetRenderLogicalPresentation(sh->ren, 320, logical_h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    sh->tex = SDL_CreateTexture(sh->ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 200);
    SDL_SetTextureScaleMode(sh->tex, SDL_SCALEMODE_NEAREST);
    if (present_aspect) {
        present_up = SDL_CreateTexture(sh->ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_TARGET, 1280, 800);
        if (present_up) SDL_SetTextureScaleMode(present_up, SDL_SCALEMODE_LINEAR);
    }
    /* vsync keeps the presents at the display's rate; the game's clock is
     * the wall's regardless (the 256 Hz clock is read from SDL's) */
    if (!sh->script && sh->vsync >= 0) SDL_SetRenderVSync(sh->ren, 1);
    SDL_HideCursor();
    return 1;
}

/* The window's icon: the silver tree (object art 0x1ca) out of the
 * player's own OBJECTS.GR, through PALS.DAT's first palette with colour 0
 * clear, scaled up four times. Nothing of the game ships with the program,
 * so the icon is drawn from the copy it runs on; none when the art is not
 * loaded. */
static void set_window_icon(SDL_Window *win) {
    enum { SILVER_TREE = 0x1ca, SCALE = 4 };
    int w = 0, h = 0, x, y;
    const uint8_t *px = uw_art(NULL, SILVER_TREE, &w, &h);
    char path[600];
    uw_blob pals;
    SDL_Surface *icon;
    if (!win || !px || w <= 0 || h <= 0 || w > 64 || h > 64) return;
    snprintf(path, sizeof path, "%s/DATA/PALS.DAT", game_dir);
    pals = uw_read_file(path);
    if (pals.data && pals.size >= 768 && (icon = SDL_CreateSurface(w * SCALE, h * SCALE, SDL_PIXELFORMAT_RGBA32)) != NULL) {
        for (y = 0; y < h * SCALE; y++)
            for (x = 0; x < w * SCALE; x++) {
                uint8_t c = px[(y / SCALE) * w + x / SCALE];
                uint8_t *d = (uint8_t *)icon->pixels + y * icon->pitch + x * 4;
                d[0] = (uint8_t)(pals.data[c * 3] * 255 / 63);
                d[1] = (uint8_t)(pals.data[c * 3 + 1] * 255 / 63);
                d[2] = (uint8_t)(pals.data[c * 3 + 2] * 255 / 63);
                d[3] = c ? 255 : 0;
            }
        SDL_SetWindowIcon(win, icon);
        SDL_DestroySurface(icon);
    }
    uw_free(&pals);
}

/* ---- --cutscene N: the cutscene mode with no game under it ----
 *
 * The same mode the game plays a cutscene through, entered from MODE_NONE
 * and returning to it, which ends the loop: the frames src/uw_cutplay.c
 * produces with the interpreter's fades, and any key or click passed to
 * it as the input poll does. */
static int cutscene_only(uw_shell *sh, int cutscene, int scale) {
    static uw_strings cut_strings;
    char path[768];
    snprintf(path, sizeof path, "%s/DATA/STRINGS.PAK", game_dir);
    if (uw_strings_open(&cut_strings, path)) sh->m.strings = &cut_strings;
    if (!window_open(sh, scale)) return 1;
    sh->m.ds = sh->ds;
    sh->mode = MODE_NONE;
    sh->t0 = SDL_GetTicksNS();
    sh->pass_start = sh->t0;
    harness_audio(sh);
    if (sh->sound) {
        if (sound_open(sh)) uw_motion_sound_enable(&sh->m, 1, 1);   /* the settings as sound_init leaves them */
        else sh->sound = 0;
        sh->sound_clock0 = sh->m.clock;
    }
    uw_motion_cutscene_request(&sh->m, (uint16_t)cutscene);
    cutscene_enter(sh);
    if (sh->mode != MODE_CUTSCENE) {
        fprintf(stderr, UW_PROGRAM ": cutscene %d will not open under %s\n", cutscene, game_dir);
        return 1;
    }
    game_loop(sh);
    harness_cutscene_end(sh, cutscene);
    SDL_Quit();
    return 0;
}

/* ---- the program ------------------------------------------------------------ */

int main(int argc, char **argv) {
    static uint8_t ds_[0x10000], lseg_[0x10000], pal_[768], art[0x400];
    static uint8_t screen_[64000], obj_art[0x400], keybuf_[0x10000];
    static uint32_t frame[64000];
    static uw_shell shell;
    uw_shell *sh = &shell;
    uw_motion *m = &shell.m;
    uw_blob shades, font_small, grave, weapons_dat, weapons_gr, weapons_cm;
    char path[768];
    const char *saves_dir = NULL, *dir_given = NULL;
    int cutscene = -1;                  /* --cutscene N: play one and exit */
    int locate = 0;                     /* --locate: ask for the game's directory again */
    int i, scale = 3;
    char saves[600];

    sh->ds = ds_; sh->lseg = lseg_; sh->pal = pal_; sh->screen = screen_; sh->keybuf = keybuf_;
    sh->frame = frame;
    sh->start_x = sh->start_y = sh->start_heading = -1;
    sh->running = 1;
    sh->hp_override = -1;
    sh->mode = MODE_DUNGEON;

    for (i = 1; i < argc; i++) {
        int h = harness_arg(sh, argc, argv, &i);
        if (h) continue;
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir_given = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cutscene") && i + 1 < argc) cutscene = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--saves") && i + 1 < argc) saves_dir = argv[++i];
        else if (!strcmp(argv[i], "--sound")) sh->sound = 1;
        else if (!strcmp(argv[i], "--nosound")) sh->sound = -1;
        else if (!strcmp(argv[i], "--square")) present_aspect = 0;
        else if (!strcmp(argv[i], "--locate")) locate = 1;
        else if (!strcmp(argv[i], "--fullscreen")) sh->fullscreen = 1;
        else if (!strcmp(argv[i], "--no-vsync")) sh->vsync = -1;
        else if (!strcmp(argv[i], "--pace") && i + 1 < argc) present_pace_ms = atoi(argv[++i]);
        else return usage();
    }
    if (scale < 1) return usage();
    if (!find_game_dir(dir_given, !sh->script, locate)) return 1;
    check_release(!sh->script);
    {
        /* THE SOUND IS THE HOST'S, AND IT PLAYS. The original
         * takes it from DATA/UW.CFG -- a music driver played here as the
         * AdLib's, since the port has ADLIB.ADV's voice layer and an OPL2
         * and not a PC speaker's or an MT-32's, and a digital driver for
         * the cutscenes' voices -- and a copy INSTALL never configured
         * names neither, which is the file most copies carry and which left
         * this port silent on them. So the drivers are the host's now:
         * --nosound is the way to silence, and the game's own UW.CFG fields
         * still go into the data segment where the original put them
         * (uw_boot_read_config, in the boot).
         *
         * What a saved game says about its sound is NOT the host's: the
         * player record's +0xb5 carries the two switches and a restore
         * applies them, so a game saved with the music off comes back with
         * it off. --sound overrides that too; nothing else does. */
        sh->sound_forced = sh->sound > 0;
        sh->sound = sh->sound >= 0;
        sh->digital_cfg = sh->sound;
    }
    if (!sh->pair && !sh->new_game && cutscene < 0) sh->program = 1;
    if (!harness_setup(sh)) return 1;

    if (cutscene >= 0) return cutscene_only(sh, cutscene, scale);
    sh->have_game = 1;

    if (sh->new_game || sh->program) {
        /* the game's memory from the files, through the scene the view
         * refresh draws with */
        sc = calloc(1, sizeof *sc);
        sh->boot = calloc(1, sizeof *sh->boot);
        sc_opened = sc && uw_scene_open(sc, game_dir);
        /* new_game in its two halves, with the character
         * generation between them: the screen when it was asked for
         * (src/uw_chargen_ui.c, run over the passes below), else the
         * answers --class/--female/--left/--name make */
        if (sh->boot) sh->boot->program = sh->program;
        if (!sc_opened || !sh->boot || !uw_boot_new_game_begin(sh->boot, m, sc, game_dir, 0x100, harness_ark_path())) {
            fprintf(stderr, UW_PROGRAM ": cannot boot a new game from %s\n", game_dir);
            return 1;
        }
        harness_after_begin(sh);
        if (sh->program) {
            /* game_main: the screens, the title and the menu come first;
             * the game's own memory is game_init's until a character is
             * made or a game restored */
            sh->mode = MODE_TITLE;
            /* and game_init's mode field with it: parse_command_line
             * leaves the event record's +8 at 0, which is what
             * the shipped data segment holds too, and game_set_mode(1) is
             * the start of a game -- not of the program. cutscene_play
             * reads it when a full-screen cutscene ends and calls
             * game_change_mode(1) for a 1, so a 1 here would draw the
             * dungeon's main screen over the title */
            ww(sh->boot->ds, (uint16_t)(rw(sh->boot->ds, 0x00e2) + 8), 0);
            /* and the view's own cursor regions, which view_set_viewport
             * registers when the dungeon's screen is drawn: the title and
             * the character generation screen are before that, and the
             * cursor kept the shape the menu pushed over all of them --
             * the port's boot binds the eight arrows, which would turn it
             * into a movement arrow over the middle of the chargen screen */
            uw_motion_view_regions_unbind(m);
        } else if (!harness_new_game(sh)) return 1;
        m->pick_map = pick_map;
        m->pick_user = m;
        harness_before_adopt(sh);
        sh->ds = sh->boot->ds; sh->lseg = sh->boot->lseg; sh->pal = sh->boot->palette;
        sh->screen = sh->boot->screen; sh->keybuf = sh->boot->keys;
        sh->nc_at_boot = m->not_carried;
        /* the program path never runs the boot's finish, which discards the
         * boot's own counted call (the level load's texture pixels) */
        if (sh->program) { m->not_carried = 0; sh->nc_at_boot = 0; }
        if (!harness_after_boot(sh)) return 1;
    } else if (!harness_boot_pair(sh)) return 1;

    switch (harness_image(sh)) {
    case 0: return 1;
    case -1: return usage();
    default: break;
    }

    /* the level's key state array: every key up at the start */
    memset(sh->keybuf + rw(sh->ds, 0x2344), 0, 0x80);
    sh->keybuf[rw(sh->ds, 0x2334)] = sh->keybuf[rw(sh->ds, 0x2338)] = sh->keybuf[rw(sh->ds, 0x233c)] = sh->keybuf[rw(sh->ds, 0x2340)] = 0;

    snprintf(path, sizeof path, "%s/DATA", game_dir);
    uw_art_load(path);
    m->art = uw_art;
    m->gr_file = uw_gr_file;
    m->gr_entry = uw_gr_entry;
    /* EMS for the panel flip, as the original has it with EMS present */
    m->panel_ems = panel_ems;
    m->pick_map = pick_map;
    m->pick_user = m;
    m->view_hook = view_hook;
    m->view_user = sh;
    {
        static const char *const f1000[3] = { "BUTTONS.GR", "CURSORS.GR", "3DWIN.GR" };
        static const char *const fobj[1] = { "OBJECTS.GR" };
        m->art_size = art; m->art_size_count = gr_sizes(game_dir, f1000, 3, art, 0x200);
        m->obj_art_size = obj_art; m->obj_art_size_count = gr_sizes(game_dir, fobj, 1, obj_art, 0x200);
    }
    snprintf(path, sizeof path, "%s/DATA/SHADES.DAT", game_dir);
    shades = uw_read_file(path);
    m->shades = shades.data && shades.size >= 96 ? shades.data : NULL;
    snprintf(path, sizeof path, "%s/DATA/GRAVE.DAT", game_dir);
    grave = uw_read_file(path);
    m->grave = grave.data; m->grave_size = grave.size;
    snprintf(path, sizeof path, "%s/DATA/WEAPONS.DAT", game_dir);
    weapons_dat = uw_read_file(path);
    m->weapons_dat = weapons_dat.data; m->weapons_dat_size = weapons_dat.size;
    snprintf(path, sizeof path, "%s/DATA/WEAPONS.GR", game_dir);
    weapons_gr = uw_read_file(path);
    m->weapons_gr = weapons_gr.data; m->weapons_gr_size = weapons_gr.size;
    snprintf(path, sizeof path, "%s/DATA/WEAPONS.CM", game_dir);
    weapons_cm = uw_read_file(path);
    m->weapons_cm = weapons_cm.data; m->weapons_cm_size = weapons_cm.size;
    snprintf(path, sizeof path, "%s/DATA/FONT4X5P.SYS", game_dir);
    font_small = uw_read_file(path);
    m->font_small = font_small.data; m->font_small_size = font_small.size;
    snprintf(path, sizeof path, "%s/DATA/STRINGS.PAK", game_dir);
    if (uw_strings_open(&sh->pak, path)) {
        m->strings = &sh->pak;
        memset(&sh->scroll, 0, sizeof sh->scroll);
        sh->scroll.m = m; sh->scroll.font = m->font; sh->scroll.font_size = m->font_size;
        if (m->font) m->scroll = &sh->scroll;
    }
    sh->talk_open = uw_talk_open(&sh->talk, m, game_dir);
    if (sh->new_game || sh->program) {
        sh->ark = sh->boot->ark;
        sh->terrain = sh->boot->terrain;
    } else {
        snprintf(path, sizeof path, "%s/DATA/LEV.ARK", game_dir);
        if (!uw_ark_open(&sh->ark, path)) { fprintf(stderr, "%s: cannot open\n", path); return 1; }
        snprintf(path, sizeof path, "%s/DATA/TERRAIN.DAT", game_dir);
        sh->terrain = uw_read_file(path);
    }
    /* SAVE1..4 where the original keeps them, beside the game's files,
     * when that directory takes a file; else the user's data directory
     * (SDL's preference path); --saves overrides, and a scripted run's
     * default is the suite's build/uwshell-saves. SAVE0, the original's
     * scratch copy of the game being played, is the port's in memory. */
    if (saves_dir) snprintf(saves, sizeof saves, "%s", saves_dir);
    else if (sh->script) snprintf(saves, sizeof saves, "build/uwshell-saves");
    else {
        char probe[640];
        FILE *pf;
        snprintf(probe, sizeof probe, "%s/.uwshell-probe", game_dir);
        if ((pf = fopen(probe, "wb")) != NULL) {
            fclose(pf);
            remove(probe);
            snprintf(saves, sizeof saves, "%s", game_dir);
        } else {
            char *pref = SDL_GetPrefPath(NULL, UW_PREF_DIR);
            snprintf(saves, sizeof saves, "%s", pref ? pref : ".");
            if (pref) {
                size_t len = strlen(saves);
                if (len > 1 && saves[len - 1] == '/') saves[len - 1] = 0;
                SDL_free(pref);
            }
            fprintf(stderr, UW_PROGRAM ": %s takes no files; the saves go to %s\n", game_dir, saves);
        }
    }
    uw_options_init(&sh->options, m, game_dir, saves, &sh->ark, &sh->talk.bglobals, &sh->talk.bglobals_size,
                    sh->terrain.data, sh->terrain.size);

    if (!window_open(sh, scale)) return 1;
    set_window_icon(sh->win);

    /* the clock from the first state's frame stamp on */
    sh->clock0 = rd(sh->ds, 0x0774);
    sh->t0 = SDL_GetTicksNS();
    sh->pass_start = sh->t0;
    m->clock = sh->clock0;
    harness_audio(sh);
    if (sh->sound && !sound_open(sh)) {
        fprintf(stderr, UW_PROGRAM ": --sound: the SOUND directory's files did not open\n");
        sh->sound = 0;
    }
    sh->sound_clock0 = m->clock;
    /* the view drawn once, and the standing handlers -- in the game;
     * the program's first pass is game_main's */
    if (!sh->program) ww(sh->ds, 0x56aa, (uint16_t)(rw(sh->ds, 0x56aa) | 0x3802));

    game_loop(sh);

    harness_end(sh);
    SDL_Quit();
    return 0;
}
