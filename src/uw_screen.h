/* SPDX-License-Identifier: MIT */
/* The fades and the screen transitions, the one
 * thing every ported screen asked for and none of them had.
 *
 * TWO DIFFERENT THINGS ARE CALLED A FADE HERE, and the original keeps them
 * apart:
 *
 *   palette_fade_out and palette_fade_in ramp the
 *   DAC. Nothing on the screen is touched; every pixel keeps its index and
 *   the colours those indexes mean go to black and come back. That is what
 *   dungeon_draw_main_screen runs, two steps each way.
 *
 *   screen_fade_out and screen_fade_in rewrite the
 *   VIEW. They are screen_wipe_forward and screen_wipe_backward driving
 *   gfx_remap_buffer_shaded over the work buffer -- thirteen passes of
 *   LIGHT.DAT's rows 0..12 -- and then a clear to colour 0xf1, which
 *   PALS.DAT's palette 0 makes black. The panels around the view are not
 *   touched. That is what perform_pending_teleport and
 *   player_sleep run.
 *
 * WHAT A PORT MUST DO DIFFERENTLY. Both fades are loops with a wait and a
 * present inside them, and neither is the host's to run: the original
 * spins on the clock at [0x2364] for eight ticks a frame (palette) or
 * leans on screen_present's own pacing (view). So both are steppers here --
 * the caller runs a frame, presents it, and comes back. That is the same
 * shape the shell already drives a button-release wait with.
 */
#ifndef UW_SCREEN_H
#define UW_SCREEN_H

#include "uw.h"
#include "uw_lighting.h"

/* ---- gfx_remap_buffer_shaded -------------------------------
 *
 * Replaces every byte of a buffer with table[byte], the table being one
 * 256-byte row of LIGHT.DAT. The original walks DOWNWARD from 0x4d7e and
 * stops when its index reaches zero, so byte 0 of the work buffer is never
 * remapped and 402 bytes past the 172 x 113 view are -- an artefact of
 * hand-written assembly counting to a round number, invisible on screen
 * either way. `len` here is the caller's and every byte of it is remapped.
 */
void uw_gfx_remap_buffer(uint8_t *buf, size_t len, const uint8_t *table);

/* ---- palette_fade_out, palette_fade_in ---------
 *
 * steps * 8 frames, and the ramp carries its remainder: a 16-bit
 * accumulator per component holds frames * value, each frame moves it by
 * one value and stores accumulator / frames back. That is how an eight-bit
 * ramp comes out smooth with no fixed point anywhere.
 *
 * `steps == 0` means at once: one frame, black for the fade out and the
 * palette itself for the fade in.
 */
typedef struct {
    uint8_t  from[768];   /* the palette the ramp is built from */
    uint16_t acc[768];    /* the accumulator, one per component */
    uint8_t  pal[768];    /* the frame to upload: what palette_upload sends */
    int      frames;      /* steps * 8 */
    int      frame;       /* the next one to run */
    int      out;         /* 1 fading out, 0 fading in */
} uw_palette_fade;

void uw_palette_fade_begin(uw_palette_fade *f, const uint8_t *pal, int steps, int out);

/* One frame: 1 when `f->pal` is a new palette to upload, 0 when the fade is
 * over. The caller waits the eight ticks between frames. */
int  uw_palette_fade_step(uw_palette_fade *f);

/* How long a fade frame takes on the 256 Hz clock. The loop spins until
 * eight ticks have passed since the last upload and then uploads through
 * vga_set_palette, which first waits for the vertical retrace:
 * at mode 13h's 70.086 Hz (25.175 MHz over 800 x 449) eight ticks end
 * after the second retrace, so each frame is the third, 10.96 ticks -- a
 * fade of four steps holds its caller 1.37 s. A fade of no steps is one
 * upload, with at most a retrace's wait. */
#define UW_FADE_FRAME_TICKS (3.0 * 256.0 * 800.0 * 449.0 / 25175000.0)

/* ---- screen_wipe_forward, screen_wipe_backward --
 *
 * The forward wipe calls its callback with i = 0..n and presents after
 * each, then clears the work buffer and presents once more. The backward
 * one counts DOWN, and has to keep an EMS copy of the untouched buffer
 * because it restores it before every step -- so a backward wipe's frames
 * each apply ONE row to the original image, where a forward wipe's
 * compound, each row applied to what the row before it left.
 *
 * The two are not mirror images and this is the whole of the difference.
 * A fade out through rows 0..12 of the retail LIGHT.DAT takes the view's
 * mean luminance from 23.0 to 7.3 and leaves the 44 colours the ramp
 * fixes; a fade in walks 12 down to 1 over the image that is coming back.
 */
#define UW_VIEW_W 172
#define UW_VIEW_H 113
#define UW_VIEW_BYTES (UW_VIEW_W * UW_VIEW_H)

/* screen_fade_out and screen_fade_in push 12 and ignore the word their
 * callers pass -- perform_pending_teleport passes a variable and
 * cameras_fade passes 5, and neither reaches the loop. */
#define UW_SCREEN_FADE_STEPS 12
#define UW_SCREEN_FADE_CLEAR 0xf1

typedef struct {
    int     n;            /* the top row, 12 for the two screen fades */
    int     i;            /* the next row to apply */
    int     out;          /* 1 forward (fading out), 0 backward */
    int     stage;        /* 0 not begun, 1 stepping, 2 the last frame done */
    uint8_t saved[UW_VIEW_BYTES];   /* the EMS copy a backward wipe keeps */
    size_t  saved_len;
} uw_screen_wipe;

/* screen_fade_out(view) when `out`, screen_fade_in(view) otherwise. */
void uw_screen_fade_begin(uw_screen_wipe *w, int out);

/* screen_show_frame: the work buffer cleared to `colour` and
 * presented once, with the cursor hidden over it -- one forward-wipe step
 * without the loop. It is the flash a critical hit
 * shows (0xb8), the one a fall shows (0xc6) and the one non-lethal damage
 * shows (0xa8); the picture comes back with the next view refresh, which
 * draws over the clear exactly as it does after a fade out. */
void uw_screen_show_frame(uint8_t *view, size_t len, uint8_t colour);

/* One frame of the wipe over `view`: 1 when the buffer is a frame to
 * present, 0 when the wipe is over. Thirteen rows and one clear either
 * way, so fourteen frames. `light` is the 16 x 256 table the rasteriser
 * holds -- LIGHT.DAT, or MONO.DAT at light level 5, whichever
 * shade_set_level last loaded, which is what the remap reads. */
int  uw_screen_fade_step(uw_screen_wipe *w, uint8_t *view, size_t len,
                         const uint8_t *light);

#endif
