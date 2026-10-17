/* SPDX-License-Identifier: MIT */
/* The fades and the screen transitions. */
#include "uw_screen.h"

#include <string.h>

/* gfx_remap_buffer_shaded, from the bytes:
 *
 *     push ds; push es; mov bx,0x4f4b; mov ds,bx; mov bx,0x16e7; mov es,bx
 *     xchg cl,ch; mov si,cx; mov di,0x4d7e
 *   loop:
 *     xor bx,bx; mov bl,[di]; mov bl,es:[bx+si+0x696e]; mov [di],bl
 *     dec di; jnz loop; pop es; pop ds; retf
 *
 * `si` is the caller's cx with its bytes swapped -- the shade level times
 * 256 -- which is how one argument selects a row of the 16 x 256 table the
 * rasteriser keeps. */
void uw_gfx_remap_buffer(uint8_t *buf, size_t len, const uint8_t *table) {
    size_t i;
    for (i = 0; i < len; i++) buf[i] = table[buf[i]];
}

void uw_screen_show_frame(uint8_t *view, size_t len, uint8_t colour) {
    memset(view, colour, len);
}

/* ---- the palette ramp -------------------------------------------------- */

void uw_palette_fade_begin(uw_palette_fade *f, const uint8_t *pal, int steps, int out) {
    int i;
    memset(f, 0, sizeof *f);
    memcpy(f->from, pal, sizeof f->from);
    f->out = out;
    f->frames = steps * 8;
    f->frame = 0;
    if (f->frames <= 0) return;
    /* palette_fade_out builds the accumulator full and walks it down;
     * palette_fade_in starts at zero and walks up. The multiply is
     * sixteen-bit and so is the accumulator: a palette component is six
     * bits in the DAC, so 0x3f * frames overflows only past 260 steps. */
    if (out)
        for (i = 0; i < 768; i++)
            f->acc[i] = (uint16_t)((uint16_t)f->frames * f->from[i]);
}

int uw_palette_fade_step(uw_palette_fade *f) {
    int i;
    if (f->frames <= 0) {
        /* `steps == 0` means at once: one upload and no ramp at all. The
         * fade out zeroes its buffer, the fade in sends the palette. */
        if (f->frame) return 0;
        f->frame = 1;
        if (f->out) memset(f->pal, 0, sizeof f->pal);
        else memcpy(f->pal, f->from, sizeof f->pal);
        return 1;
    }
    if (f->frame >= f->frames) return 0;
    for (i = 0; i < 768; i++) {
        if (f->out) f->acc[i] = (uint16_t)(f->acc[i] - f->from[i]);
        else        f->acc[i] = (uint16_t)(f->acc[i] + f->from[i]);
        f->pal[i] = (uint8_t)(f->acc[i] / (uint16_t)f->frames);
    }
    f->frame++;
    return 1;
}

/* ---- the view wipe ------------------------------------------------------ */

/* The EMS copy put back: 0x4bec bytes of a frame buffer whose rows lie 174
 * bytes apart from its offset 2 (setup_frame_buf), so only the pixels below
 * that byte come back to each step -- frame row 111 up to column 120, none
 * of row 112, the view's top line -- and the rest keep what the step left.
 * The fade's end shows the whole image. */
static void wipe_restore(uint8_t *view, const uint8_t *saved, size_t len) {
    size_t k;
    for (k = 0; k < len; k++) {
        size_t r = k / UW_VIEW_W, c = k % UW_VIEW_W;
        if (2 + r * (UW_VIEW_W + 2) + c < 0x4bec) view[k] = saved[k];
    }
}

void uw_screen_fade_begin(uw_screen_wipe *w, int out) {
    w->n = UW_SCREEN_FADE_STEPS;
    w->out = out;
    w->stage = 0;
    w->saved_len = 0;
    w->i = out ? 0 : w->n;
}

int uw_screen_fade_step(uw_screen_wipe *w, uint8_t *view, size_t len,
                        const uint8_t *light) {
    size_t keep = len < sizeof w->saved ? len : sizeof w->saved;
    if (w->stage == 2) return 0;
    if (w->out) {
        /* screen_wipe_forward: the callback for i = 0..n, each row applied
         * to what the row before it left, and then the clear. Row 0 of the
         * retail LIGHT.DAT is the identity, so the first frame is the
         * picture unchanged -- the fade starts by presenting what is
         * already there. */
        if (w->i <= w->n) {
            uw_gfx_remap_buffer(view, len, light + w->i * UW_LIGHT_ROW);
            w->i++;
            return 1;
        }
        memset(view, UW_SCREEN_FADE_CLEAR, len);
        w->stage = 2;
        return 1;
    }
    /* screen_wipe_backward: the EMS copy first, then the clear, then the
     * rows n..1 each over the restored image, then the image itself. */
    if (w->stage == 0) {
        memcpy(w->saved, view, keep);
        w->saved_len = keep;
        memset(view, UW_SCREEN_FADE_CLEAR, len);
        w->stage = 1;
        return 1;
    }
    if (w->i > 0) {
        /* the restore the original does after each present, done here
         * before the next frame instead: the caller presented in between */
        if (w->i < w->n) wipe_restore(view, w->saved, w->saved_len);
        uw_gfx_remap_buffer(view, len, light + w->i * UW_LIGHT_ROW);
        w->i--;
        return 1;
    }
    /* the end: the whole view, its top line too -- orb1's states after
     * each fade-in show the image there, where the steps showed the clear */
    memcpy(view, w->saved, w->saved_len);
    w->stage = 2;
    return 1;
}
