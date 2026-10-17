/* SPDX-License-Identifier: MIT */
/* A SCENE: the game's memory for drawing the 3-D view, assembled from the
 * shipped files alone, and a draw that runs the whole renderer over it.
 *
 * What the original has in memory when view_render runs, and where this gets
 * it:
 *
 *   6aac  the data segment       UW.EXE's image; COMOBJ.DAT at 0x5b6e;
 *                                 ALLPALS.DAT at 0x573e; TERRAIN.DAT's floor
 *                                 kinds for the level at 0x7192; the level
 *                                 and object pointers; the player's position,
 *                                 heading and record; SHADES.DAT's record for
 *                                 the light level; the detail setting as
 *                                 set_render_detail patches it
 *   5723  the rasteriser         UW.EXE's image (the model programs and
 *                                 handler tables ship in it); gr_load_all's
 *                                 image tables and crit_pages_init's creature
 *                                 tables; the camera program's depth scale
 *   4963  the graphics module    UW.EXE's image; the light ramp from SHADES
 *   6624  the private segment    UW.EXE's image, whose view-state block the
 *                                 camera port fills
 *   EMS   the image pool         gr_load_all's packing of the .GR files and
 *                                 gr_load_door_textures' door textures
 *   level segment                LEV.ARK's level block: tiles at +4, mobile
 *                                 objects from +0x4004, static from +0x5b04
 *   texture slots                W64/F32/W16/F16.TR by the level's texture map
 *
 * and a draw is view_place_camera, view_setup_frame, view_build_light_map,
 * view_traverse_spans and view_build_draw_list (src/uw_viewlist.c,
 * uw_viewspan.c), then rast_execute's setup (uw_rastframe.c) and the list run
 * through the executor (uw_drawlist.c) into the caller's buffer.
 *
 * WHAT A FIRST DRAW IS NOT. The executor's memory carries state from frame
 * to frame -- the shader and polygon words, the shape records, the slot
 * window -- and the original's first frame after loading ran over whatever
 * came before. uw_scene_draw therefore runs the list once before the frame it
 * returns, the first time. One warm-up reaches the steady state except
 * where a list inherits a polygon count no list of its own writes (one
 * detail-1 scene). */
#ifndef UW_SCENE_H
#define UW_SCENE_H

#include "uw.h"
#include "uw_image.h"
#include "uw_drawlist.h"
#include "uw_viewlist.h"
#include "uw_rastframe.h"
#include "uw_effects.h"

#define UW_SCENE_EMS_PAGES 64

/* The three 3DWIN.GR images the interface draws over the dungeon view: a
 * 47 x 3 strip at the top and a 1 x 5 mark at each side. */
typedef struct {
    int     w, h, x, y;          /* y is the BOTTOM row: image row 0 lands there */
    uint8_t px[47 * 3];
} uw_view_overlay;
#define UW_SCENE_SEG       0x7f5e  /* the segment the level block's pointers name */

typedef struct {
    char     dir[512];                     /* the game directory (UW.EXE, DATA/, CRIT/) */

    /* ---- the game's memory ------------------------------------------- */
    uint8_t  ds[0x10000], rast[0x10000], gfx[0x10000], priv[0x10000];
    uint8_t  level[0x10000];
    uint8_t  ems[UW_SCENE_EMS_PAGES * 0x4000u];
    uint8_t  auxpals[0x1000], xfer[0x600], light[0x1000];

    /* ---- what the loaders recorded ------------------------------------ */
    uint16_t texture_base;                 /* gr_texture_base */
    uint16_t images;                       /* gr_id_base_2000: EMS bitmaps */
    int      level_no;                     /* 0-based */
    uw_tr    tr[4];                        /* W64, F32, W16, F16 */
    const uint8_t *tex[UW_DL_TEX];
    size_t   tex_len[UW_DL_TEX];
    struct { int page, file; uw_blob b; } crit[64];
    int      n_crit;
    uint8_t  animation_props[64];          /* OBJECTS.DAT's last section */
    uw_objpool pool;                       /* the level segment's object pools */
    uw_effects effects;                    /* the level's animated objects */
    uw_rng   rng;                          /* rt_rand, for a kind that picks frames */
    uw_view_overlay overlay[3];            /* 3DWIN.GR 0..2, for the view's frame */
    uw_blob  shades_file, light_file, mono_file;  /* SHADES.DAT, LIGHT.DAT, MONO.DAT, read once */

    /* ---- the view ------------------------------------------------------ */
    int      view_width, view_height;      /* 172 x 113, the dungeon view */
    uint16_t projection_scale;             /* view_projection_scale: 25000 */

    /* ---- the renderer's working state, kept across draws --------------- */
    uw_vl    vl;
    uw_rast_frame rf;
    uw_dl    dl;
    int      drawn;                        /* frames drawn since uw_scene_level */
    int      ever_drawn;                   /* a frame drawn on any level: the executor has a past */
    int      shade_blank;                  /* shade_reload_or_scramble(1): the ramp's first two
                                            * bytes a row zeroed until it is read again */
    long     unsupported;                  /* what a stage reported not carrying */
} uw_scene;

/* Load UW.EXE's data segments and the level-independent files: object
 * properties, the aux palettes, XFER.DAT, the .GR images packed into EMS as
 * gr_load_all packs them, the creature tables, and the four texture files.
 * `dir` holds UW.EXE, DATA/ and CRIT/. The scene is large: allocate it. */
bool uw_scene_open(uw_scene *s, const char *dir);
void uw_scene_close(uw_scene *s);

/* Enter dungeon level `level` (0-based): its level block -- `block`, or
 * DATA/LEV.ARK's when NULL (a saved game's LEV.ARK holds the live one) -- its
 * texture map, terrain kinds and door textures. */
bool uw_scene_level(uw_scene *s, int level, const uint8_t *block, size_t len);

/* The level's animated objects, from the same archive as its block:
 * uw_scene_level loads DATA/LEV.ARK's (slot 9 + level) and this replaces
 * them with `block` (0x180 bytes -- a saved game's). False for another size. */
bool uw_scene_effects(uw_scene *s, const uint8_t *block, size_t len);

/* One level_effects_tick(ticks): each animated object steps a frame (the
 * step does not scale with `ticks`, which only runs timed effects down).
 * The game makes one call per frame that crosses a 64-tick boundary of its
 * 256 Hz clock, four a second. Returns what it did not carry. */
long uw_scene_tick(uw_scene *s, int ticks);

/* Put the eye: the player's world position (tile << 8 | fraction, and the
 * height), 16-bit heading (0 is +y, 0x4000 +x), the detail setting 0..3 and
 * the light level 0..7 (the brightest carried light source's brightness). */
void uw_scene_place(uw_scene *s, uint16_t x, uint16_t y, uint16_t z,
                    uint16_t heading, int detail, int light);

/* The step sway: when `on` -- player_stepped_this_frame, which
 * player_physics_step sets on each step it takes -- view_place_camera adds
 * the four offsets view_apply_impairment leaves to the
 * eye's height, heading, pitch and roll. uw_scene_place clears them. */
/* What the game asks of the renderer outside the data segment (uw_motion's
 * `render`): view_effect_warp's six texture masks into the rasteriser's
 * slot records (the word at +6 of each); shade_reload_or_scramble -- 1 the
 * ramp's first two bytes of each of its sixteen rows zeroed, 0 read again
 * from the file; and apply_maze_texture's reload of the floor textures
 * from floor_texture_assign's ten words. */
void uw_scene_masks(uw_scene *s, const uint16_t mask[6]);
void uw_scene_shade_blank(uw_scene *s, int blank);
void uw_scene_floors(uw_scene *s, const uint8_t *assign);

void uw_scene_sway(uw_scene *s, int on, int16_t height, int16_t heading,
                   int16_t pitch, int16_t roll);

/* Draw the view into `fb` (addressed through its row table; the view is
 * view_width x view_height from row 0), the interface's overlay included.
 * Returns the number of faces and sprites drawn, or -1 when the list did not
 * run. */
long uw_scene_draw(uw_scene *s, const uw_fb *fb);

/* view_refresh_pick_map after a draw: the list rebuilt through
 * drawlist_begin_frame's preamble from the last draw's camera and column
 * array (uw_vl.pick), and run with gfx_row_fill_masked selected, into `fb`
 * -- each object its index, each wall and floor its surface code, nothing
 * drawn over it. The build's writes (the object tables and the counter)
 * are left in s->ds. Returns the events
 * drawn, or -1. */
long uw_scene_pick(uw_scene *s, const uw_fb *fb);

/* THE INTERFACE OVER THE VIEW. weapon_composite, which
 * screen_present runs for the dungeon, ends -- after the weapon, whether or
 * not one is drawn -- with gr_draw_art_by_id of art 0x107f at (62, 3) and
 * 0x1080 and 0x1081 at (0, 13) and (171, 13): 3DWIN.GR's first three images,
 * the ids gr_load_all hands out. The blit draws an image's rows UPWARD from
 * the y it is given and skips colour 0; with both, all 110 opaque pixels of
 * the strip and the ten of the marks land exactly. Load them from `dir`'s DATA/3DWIN.GR; draw them over
 * a frame. */
bool uw_view_overlay_load(uw_view_overlay ov[3], const char *dir);
void uw_view_overlay_draw(const uw_view_overlay ov[3], const uw_fb *fb);

/* The loaders, for tools that hold them against save states: pack the .GR
 * files into `pool` (a byte map of what was written in `written` when not
 * NULL) and the tables into `rast`, returning the images packed and setting
 * *texture_base; lay the door textures of a level's texture block over their
 * slots; build the creature tables. The first EMS page is 4. */
uint16_t uw_scene_gr_boot(const char *dir, uint8_t *rast, uint8_t *pool, size_t cap,
                          uint8_t *written, uint16_t *texture_base);
void     uw_scene_gr_doors(const char *dir, const uint8_t *rast, uint8_t *pool,
                           size_t cap, uint8_t *written, uint16_t slot0,
                           const uint8_t *tex_block, size_t tex_len);
void     uw_scene_crit_tables(const char *dir, uint8_t *rast);

#endif
