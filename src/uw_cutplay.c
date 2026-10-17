/* SPDX-License-Identifier: MIT */
/* The cutscene player -- cutscene_process_data as a stepper.
 * See uw_cutplay.h for the shape and for what is not carried. */
#include "uw_cutplay.h"

#include <stdio.h>
#include <string.h>

#define STRING_BLOCK_BASE 0xc00   /* cutscene_play: block = 0xc00 + number */

/* ---- the animation files ------------------------------------------------ */

static void close_anim(uw_cutscene *c) {
    if (c->have_anim) uw_anim_close(&c->anim);
    c->have_anim = 0;
}

/* The interpreter's own filename step: "CUTS\csXXX.nXX" with both halves
 * octal, the file counter stepped after each header is read. */
static bool open_anim(uw_cutscene *c) {
    char name[32], path[512];
    int n, i, j;
    close_anim(c);
    uw_cut_filename(c->cut_no, c->file_no, name, sizeof name);
    snprintf(path, sizeof path, "%s/%s", c->cuts, name);
    if (!uw_anim_open(&c->anim, path)) return false;
    c->files++;
    c->file_no++;                      /* stepped after the header is read */
    n = c->anim.page_count > UW_CUT_MAX_PAGES ? UW_CUT_MAX_PAGES : c->anim.page_count;
    /* sort_indices_by_key: the pages play in ascending order of
     * their record's first word, and it is a bubble sort of their indices */
    for (i = 0; i < n; i++) c->order[i] = i;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++) {
            uw_anim_page a, b;
            uw_anim_page_at(&c->anim, c->order[i], &a);
            uw_anim_page_at(&c->anim, c->order[j], &b);
            if (b.order < a.order) { int t = c->order[i]; c->order[i] = c->order[j]; c->order[j] = t; }
        }
    uw_anim_palette(&c->anim, c->palette);   /* palette_to_dac(header + 0x100) */
    for (i = 0; i < 16; i++) {
        /* cutscene_read_header's normalisation of the cycles' rates */
        const uint8_t *r = c->anim.file.data + 0x80 + i * 8;
        c->cycles[i].started = (uint16_t)(r[0] | r[1] << 8);
        c->cycles[i].rate = (uint16_t)(((r[2] | r[3] << 8) >> 8) & 0x3f);
        c->cycles[i].first = r[6];
        c->cycles[i].last = r[7];
    }
    c->rate = c->anim.frame_rate ? c->anim.frame_rate : 1;
    c->page = 0;
    c->frame_in_page = 0;
    c->frame = 1;                      /* the counter starts at 1 in each file */
    c->repeat = 0;
    c->lines = 0;
    c->have_anim = 1;
    c->flags |= UW_CUT_SEGMENT;
    return true;
}

/* The frames of the page the player is on, less one on the last page when
 * the header's +0x1a says so. */
static int page_frames(uw_cutscene *c) {
    uw_anim_page p;
    int last = c->anim.page_count - 1;
    if (!uw_anim_page_at(&c->anim, c->order[c->page], &p)) return 0;
    if (c->anim.last_page_short && c->page == last && p.frame_count)
        return (int)p.frame_count - 1;
    return (int)p.frame_count;
}

/* One frame into the animation buffer: kind 0 copies 64,000 bytes, kind 1 is
 * rle_expand over what is there, and a frame with no body leaves the
 * picture alone. */
static void decode_frame(uw_cutscene *c) {
    size_t len = 0, skip = 0;
    const uint8_t *fr = uw_anim_frame(&c->anim, c->order[c->page], c->frame_in_page, &len);
    int kind;
    if (!fr) return;
    kind = uw_anim_frame_body(fr, len, &skip);
    if (kind == 0) {
        if (len >= skip + 2 + UW_CUT_SCREEN) memcpy(c->image, fr + skip + 2, UW_CUT_SCREEN);
    } else if (kind == 1) {
        /* a delta leaves every byte it does not name -- so it must expand
         * over the animation as it was, never over a page the text has been
         * drawn on, or the text stays in the picture */
        uw_rle_expand(fr, len, skip + 2, c->image, UW_CUT_SCREEN, NULL);
    }
}

/* ---- the script --------------------------------------------------------- */

static uint16_t rw(const uint8_t *d, size_t at) { return uw_u16(d + at); }

static int string_width(const uw_blob *font, const char *s);

/* The width of `n` bytes of `s` in the cutscene's font. */
static int span_width(const uw_cutscene *c, const char *s, size_t n) {
    char buf[UW_CUT_LINE * 2];
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    return string_width(&c->font, buf);
}

/* op0 and op13: the string cut at its newlines, then each line wrapped by
 * width, word by word -- a word's width with its trailing space, the last
 * word's without -- a line broken before the word that would take it past
 * 320 pixels (the space before it dropped); six lines at most. */
static void set_text(uw_cutscene *c, uint8_t colour, uint16_t id) {
    char text[512];
    int block = id >> 9, index = id & 0x1ff, n = 0;
    char *p;
    c->colour = colour;
    c->lines = 0;
    if (!c->strings) { UW_NOT_CARRIED(c->not_carried); return; }
    if (!block) block = STRING_BLOCK_BASE + c->number;
    /* the scripts carry a BLOCK ID, and the file keys its blocks by id
     * rather than by position (uw_strings_find_block) */
    {
        int pos = uw_strings_find_block(c->strings, (uint16_t)block);
        if (pos < 0 || uw_strings_get(c->strings, pos, index, text, (int)sizeof text) < 0) {
            UW_NOT_CARRIED(c->not_carried);
            return;
        }
    }
    for (p = text; *p && n < UW_CUT_LINES; ) {
        char *nl = strchr(p, '\n'), *end = nl ? nl : p + strlen(p), *start = p, *w = p;
        int used = 0;
        while (w && n < UW_CUT_LINES) {
            char *sp = memchr(w, ' ', (size_t)(end - w));
            int width = sp ? span_width(c, w, (size_t)(sp + 1 - w)) : span_width(c, w, (size_t)(end - w));
            if (used + width > 320 && w > start) {
                /* the line up to the space before this word; the word
                 * begins the next */
                snprintf(c->line[n++], UW_CUT_LINE, "%.*s", (int)(w - 1 - start), start);
                start = w;
                used = 0;
                continue;
            }
            used += width;
            if (!sp) {
                snprintf(c->line[n++], UW_CUT_LINE, "%.*s", (int)(end - start), start);
                break;
            }
            w = sp + 1;
        }
        p = nl ? nl + 1 : end;
    }
    c->lines = n;
}

/* One script record, through the sixteen handlers' own effects. The
 * interpreter calls the handler with the operands and adds twice its return
 * value to the cursor; the counts are uw_cut_operands. */
static void run_record(uw_cutscene *c, uint16_t op, const uint8_t *o, int n) {
    c->records++;
    switch (op) {
    case 0:                                   /* display_string(colour, id) */
        if (c->flags & UW_CUT_ACCEPT) set_text(c, (uint8_t)rw(o, 0), rw(o, 2));
        break;
    case 1: c->lines = 0; break;              /* the lines zeroed -- the text cleared */
    case 2: break;                            /* nop */
    case 3:                                   /* pause(limit) */
        if (c->flags & UW_CUT_ACCEPT) {
            c->flags &= ~UW_CUT_WAITING;
            c->pause_frame = c->frame;
            c->pause_limit = (int)rw(o, 0);
        }
        break;
    case 4:                                   /* play_to_frame(target, limit) */
        /* with bit 0, the target -- and with no digital driver a
         * pause of the second operand's seconds on the frame before it */
        if (c->flags & UW_CUT_ACCEPT) {
            c->flags &= ~UW_CUT_WAITING;
            c->target = (int)rw(o, 0);
            if (!(c->flags & UW_CUT_DIGITAL)) {
                c->pause_limit = (int)rw(o, 2);
                c->pause_frame = c->target - 1;
            }
        }
        break;
    case 5: c->flags &= ~UW_CUT_SEGMENT; break;   /* end_segment */
    case 6: c->flags &= ~UW_CUT_RUNNING; break;   /* end */
    case 7:                                   /* repeat_segment(count) */
        /* the count, the target the record's own frame + 1, the
         * pause frame gone, bit 1 cleared; the repeat loop in the frame step
         * does the rest */
        c->repeat = (int)rw(o, 0);
        c->target = (int)rw(o - 4, 0) + 1;
        c->pause_frame = 0;
        c->flags &= ~UW_CUT_WAITING;
        break;
    case 8:                                   /* change_file(cutscene, file) */
        /* only the next file's name: the file playing goes on to its own
         * op5 or its last page */
        c->cut_no = (int)rw(o, 0);
        c->file_no = (int)rw(o, 2);
        break;
    /* fade_out(speed) and fade_in(speed): taken only in a full-screen
     * cutscene (the window 320 x 200) and while the field is above -2 */
    case 9: if (c->full_screen && c->fade_out > -2) c->fade_out = (int)rw(o, 0); break;
    case 10: if (c->full_screen && c->fade_in > -2) c->fade_in = (int)rw(o, 0); break;
    case 11:                                  /* hold_to_frame(frame + 1) */
        /* unless it stands on the frame before its target, the
         * frames up to it run unshown -- bit 0 cleared, the pause gone */
        if (rw(o - 4, 0) != (uint16_t)(rw(o, 0) - 1)) {
            c->target = (int)rw(o, 0) - 1;
            c->pause_frame = 0;
            c->pause_limit = 0;
            c->flags |= UW_CUT_WAITING;
            c->flags &= ~UW_CUT_ACCEPT;
        }
        break;
    case 12:                                  /* set_skippable(flags) */
        if (rw(o, 0) & 1) c->flags |= UW_CUT_SKIPPY;
        else c->flags &= ~UW_CUT_SKIPPY;
        break;
    case 13:                                  /* text_and_sound(colour, id, voice) */
        /* with no digital driver it is op0 on the same operands;
         * with one it records the voice and puts up no text */
        if (!(c->flags & UW_CUT_DIGITAL)) {
            if (c->flags & UW_CUT_ACCEPT) set_text(c, (uint8_t)rw(o, 0), rw(o, 2));
        } else {
            c->voice = rw(o, 4) == UW_CUT_NO_VOICE ? -1 : (int)rw(o, 4);
        }
        break;
    case 14:                                  /* wait(limit, limit_if_voiced) */
        if (c->flags & UW_CUT_ACCEPT) {
            c->flags &= ~UW_CUT_WAITING;
            c->pause_frame = c->frame;
            c->pause_limit = (int)rw(o, (c->flags & UW_CUT_DIGITAL) ? 2 : 0);
            c->flags |= UW_CUT_VOICED;
        }
        break;
    case 15: c->effects++; break;             /* klang: play_sound_effect(0x11, 0x40, 0) */
    default: break;
    }
    (void)n;
}

/* Every record due at `frame`: the interpreter tests the record's frame
 * word against the frame counter and runs the chain of records that
 * follow it immediately, while the file is still playing (op5 and op6
 * clear bit 2, and the chain stops there). */
static void run_due(uw_cutscene *c, uint16_t frame) {
    while (c->at + 4 <= c->script.size) {
        uint16_t f = rw(c->script.data, c->at);
        uint16_t op = rw(c->script.data, c->at + 2);
        int n;
        /* no record runs while a repeat is pending */
        if (f != frame || c->repeat) break;
        /* a frame's chain stops once the file has ended -- the records
         * before any frame run with no file open, and have no such test */
        if (frame && (!(c->flags & UW_CUT_SEGMENT) || !(c->flags & UW_CUT_RUNNING))) break;
        if (op >= UW_CUT_OPCODES) { c->flags &= ~UW_CUT_RUNNING; return; }
        n = uw_cut_operands[op];
        if (c->at + 4 + (size_t)n * 2 > c->script.size) { c->flags &= ~UW_CUT_RUNNING; return; }
        run_record(c, op, c->script.data + c->at + 4, n);
        c->at += 4 + (size_t)n * 2;
    }
}

/* gfx_string_width and gfx_draw_string over the
 * .SYS font's own records -- the 12-byte header, then a glyph record each
 * of `height << (row_bytes - 1)` bitmap bytes and a width byte. The same
 * walk as uw_motion_draw_string, over this player's own screen page. */
static size_t glyph_record(const uint8_t *f) {
    uint16_t height = (uint16_t)(f[6] | f[7] << 8), row_bytes = (uint16_t)(f[8] | f[9] << 8);
    return (size_t)(uint16_t)(height << ((row_bytes - 1) & 0x1f)) + (uint16_t)(f[0] | f[1] << 8);
}

static int string_width(const uw_blob *font, const char *s) {
    size_t rec;
    int w = 0, k;
    if (!font->data || font->size < 12) return 0;
    rec = glyph_record(font->data);
    for (k = 0; s[k]; k++) {
        size_t g = 12 + (uint8_t)s[k] * rec;
        if ((uint8_t)s[k] >= 0x80 || g + rec > font->size) continue;
        w += font->data[g + rec - 1];
    }
    return w;
}

static void draw_string(uw_cutscene *c, const char *s, int x, int y, uint8_t colour) {
    const uint8_t *f = c->font.data;
    size_t rec;
    uint16_t height, row_bytes;
    int k, top = 199 - y;
    if (!f || c->font.size < 12) { UW_NOT_CARRIED(c->not_carried); return; }
    height = (uint16_t)(f[6] | f[7] << 8);
    row_bytes = (uint16_t)(f[8] | f[9] << 8);
    rec = glyph_record(f);
    for (k = 0; k < 0x84 && s[k]; k++) {
        uint8_t ch = (uint8_t)s[k];
        size_t g = 12 + ch * rec;
        int r, b, w;
        if (ch >= 0x80 || g + rec > c->font.size) continue;
        w = f[g + rec - 1];
        for (r = 0; r < height; r++)
            for (b = 0; b < w && b < row_bytes * 8; b++) {
                int sx = x + b, sy = top + r;
                if (!((f[g + (size_t)r * row_bytes + (size_t)(b / 8)] >> (7 - b % 8)) & 1)) continue;
                if (sx < 0 || sx >= 320 || sy < 0 || sy >= 200) continue;
                c->screen[sy * 320 + sx] = colour;
            }
        x += w;
    }
}

/* The lines the last op0 or op13 left, drawn after every frame: centred in
 * the window and stacked UPWARD from a baseline of `height * lines +
 * (window y - window h) + 2`, in the colour the record gave -- which the
 * interpreter pokes into the font's two colour bytes. */
static void draw_text(uw_cutscene *c) {
    int height, y, i;
    if (c->lines <= 0 || !c->font.data || c->font.size < 12) return;
    height = (uint16_t)(c->font.data[6] | c->font.data[7] << 8);
    y = height * c->lines + (c->win_y - c->win_h) + 2;
    for (i = 0; i < c->lines; i++) {
        int w = string_width(&c->font, c->line[i]);
        draw_string(c, c->line[i], (c->win_x + c->win_w - w) / 2, y, c->colour);
        y -= height;
    }
}

/* ---- the player --------------------------------------------------------- */

/* palette_fade_in / _out as the loop runs them, from `t` */
static void start_fade(uw_cutscene *c, int out, int steps, uint32_t t) {
    const uint8_t *from = out ? c->dac : c->full_screen ? c->palette : c->game_pal;
    uw_palette_fade_begin(&c->fader, from, steps, out);
    c->fading = out ? 2 : 1;
    c->fade_t0 = t;
    c->fade_done = 0;
}

bool uw_cutscene_begin(uw_cutscene *c, const char *dir, int number,
                       uw_strings *strings, uint32_t clock) {
    return uw_cutscene_begin_patched(c, dir, number, 0, strings, clock, 0, NULL);
}

bool uw_cutscene_begin_patched(uw_cutscene *c, const char *dir, int number, int arg,
                               uw_strings *strings, uint32_t clock, int digital,
                               const uint8_t *dac) {
    char name[32], path[512];
    memset(c, 0, sizeof *c);
    snprintf(c->dir, sizeof c->dir, "%s", dir);
    snprintf(c->cuts, sizeof c->cuts, "%s/CUTS", dir);
    c->number = number;
    c->cut_no = number;
    c->strings = strings;
    c->full_screen = number < 0x100;
    /* cutscene_play's own two window rectangles, and the font it opens for
     * the duration, "fontbig.sys", swapping back to "font5x6p.sys"
     * afterwards */
    c->win_x = c->full_screen ? 0 : 0x34;
    c->win_y = c->full_screen ? 0xc7 : 0xb4;
    c->win_w = c->full_screen ? 0x140 : 0xac;
    c->win_h = c->full_screen ? 0xc8 : 0x70;
    if (number == 0x103) { c->win_y += 9; c->win_h += 0xb; }   /* the death's, taller */
    c->fade_in = -1;
    c->fade_out = -2;
    c->file_no = 0;
    uw_cut_filename(number, 0, name, sizeof name);
    snprintf(path, sizeof path, "%s/%s", c->cuts, name);
    c->script = uw_read_file(path);
    if (!c->script.data) return false;
    if (arg && c->script.size >= 14) {
        /* cutscene_open_file's three writes: at 4, at 6, and at 12 after a
         * seek of 4 past the second */
        uint8_t *p = c->script.data;
        p[4] = p[6] = p[12] = (uint8_t)arg;
        p[5] = p[7] = p[13] = (uint8_t)(arg >> 8);
    }
    snprintf(path, sizeof path, "%s/DATA/FONTBIG.SYS", dir);
    c->font = uw_read_file(path);
    if (!c->font.data) UW_NOT_CARRIED(c->not_carried);
    c->file_no = 1;                       /* .n01 is the first animation file */
    c->flags = UW_CUT_RUNNING | UW_CUT_ACCEPT | UW_CUT_SKIPPY;
    if (digital) c->flags |= UW_CUT_DIGITAL;  /* digital_is_available(), before any record */
    c->voice = -1;
    c->voice_start = -1;
    c->due = clock;
    c->now = clock;
    /* movedata(the working palette) and, full screen, palette_fade_out(copy,
     * 2) -- before the records at frame 0 */
    if (dac) memcpy(c->dac, dac, 768);
    memcpy(c->game_pal, c->dac, 768);
    if (c->full_screen) start_fade(c, 1, 2, clock);
    run_due(c, 0);                        /* the records due before any frame */
    return true;
}

void uw_cutscene_end(uw_cutscene *c) {
    close_anim(c);
    uw_free(&c->script);
    uw_free(&c->font);
}

void uw_cutscene_skip(uw_cutscene *c) {
    if (c->flags & UW_CUT_SKIPPY) {
        c->flags &= ~UW_CUT_RUNNING;
        c->skipped = 1;
    }
}

/* cutscene_service_audio's voice half: a voice started and
 * recorded whose playback has finished is forgotten, bit and number
 * together -- whichever number op13 has put there since. */
static void service_voice(uw_cutscene *c) {
    if ((c->flags & UW_CUT_SPEAKING) && c->voice != -1 && !c->playing) {
        c->flags &= ~UW_CUT_SPEAKING;
        c->voice = -1;
    }
}

/* A large page begins -- the first of a file, the next, or the first again
 * on op7's repeat: the pause frame and its limit zeroed and bit 1 cleared
 * before the page's first record, so a pause op4 set for a frame on a
 * later page never holds. The original's reset comes before the wait
 * that holds the page's last frame up (0dfa..0e33), so a key in that wait
 * is still there for the skip test after it: the port clears bit 1 at the
 * end of the step that shows a page's last frame (page_ends), before its
 * wait, and only the pause here. */
static void page_begins(uw_cutscene *c) {
    c->pause_frame = 0;
    c->pause_limit = 0;
}

static void page_ends(uw_cutscene *c) {
    if (c->frame_in_page >= page_frames(c)) c->flags &= ~UW_CUT_WAITING;
}

/* One turn of the frame loop at `clock`, the time the frame is due: 1
 * with a frame up, 0 when frames ran unshown and something waits on the
 * host (a fade out, a voice, an effect), -1 at the end. */
static int step_frame(uw_cutscene *c, uint32_t clock) {
    if (c->skipped) return -1;
    service_voice(c);                         /* it ran through the wait just over */
    c->pausing = 0;
    c->stepped = 1;

    for (;;) {
        int shown;
        /* the next animation file, when the last one ended or none is open */
        while (!c->have_anim || !(c->flags & UW_CUT_SEGMENT)) {
            if (!open_anim(c)) { c->flags &= ~UW_CUT_RUNNING; return -1; }
            c->advanced = clock;              /* the debounce's clock, at the open */
            page_begins(c);
        }

        /* the page's frames run out: the next page, or the segment ends and
         * the next file opens */
        while (c->frame_in_page >= page_frames(c)) {
            c->page++;
            c->frame_in_page = 0;
            if (c->page >= c->anim.page_count) {
                c->flags &= ~UW_CUT_SEGMENT;
                if (!(c->flags & UW_CUT_RUNNING)) return -1;
                if (!open_anim(c)) { c->flags &= ~UW_CUT_RUNNING; return -1; }
                c->advanced = clock;
            }
            page_begins(c);
        }

        decode_frame(c);
        c->frame_in_page++;
        if (c->flags & UW_CUT_ACCEPT) {
            /* a key in the last wait (bit 1) during a
             * play-to skips to its target -- the frames between run unshown
             * -- unless the picture is faded out */
            if ((int)c->frame < c->target && (c->flags & UW_CUT_WAITING)) {
                if (c->fade_out == -2) c->flags &= ~UW_CUT_WAITING;
                else {
                    c->flags &= ~UW_CUT_ACCEPT;
                    c->pause_limit = 0;
                    c->pause_frame = 0;
                    c->repeat = 0;
                }
            }
        } else if ((int)c->frame == c->target) {
            /* the target reached, the frames show again */
            c->pause_limit = 0;
            c->pause_frame = 0;
            c->target = 0;
            c->flags = (c->flags & ~UW_CUT_WAITING) | UW_CUT_ACCEPT;
        }
        run_due(c, c->frame);                 /* the frame's records, then its text */
        shown = (c->flags & UW_CUT_ACCEPT) != 0;
        if (shown) {
            /* gfx_blit of the whole buffer onto the page the interpreter
             * then draws the text on and flips to: which is what takes the
             * last frame's text off the screen */
            memcpy(c->screen, c->image, UW_CUT_SCREEN);
            draw_text(c);
            c->frames++;
            c->frame_start = clock;
        }

        /* once the frame is up, a voice recorded while none
         * is sounding starts -- voc_play_file, and bit 6 */
        if ((c->flags & UW_CUT_DIGITAL) && !(c->flags & UW_CUT_SPEAKING)
            && c->voice >= 0 && c->voice < UW_CUT_NO_VOICE) {
            c->voice_start = c->voice;
            c->playing = 1;
            c->flags |= UW_CUT_SPEAKING;
        }

        /* op7's repeat: on the record's own frame (the
         * target less one) with the count not spent, the count down one and
         * the file from its first page again, the frame counter 1 -- a
         * jump back past this frame's wait */
        if (c->repeat > 0 && (int)c->frame == c->target - 1) {
            c->repeat--;
            c->page = 0;
            c->frame_in_page = 0;
            c->frame = 1;
            if (c->flags & UW_CUT_RUNNING) c->flags |= UW_CUT_SEGMENT;
            page_begins(c);
            c->flags &= ~UW_CUT_WAITING;
            /* The repeat's own frame is blitted and the jump goes on to frame
             * 1 at once. */
            continue;
        }

        if (!shown) {
            /* A frame run unshown takes no time: no blit, no wait, and no
             * pause can be set with bit 0 clear. Back to the host only for
             * what it must do before the next: a fade out, a voice or an
             * effect to start, the end. */
            c->frame++;
            page_ends(c);
            if (!(c->flags & UW_CUT_RUNNING)) return -1;
            if (c->fade_out >= 0 || c->voice_start >= 0) {
                c->due = clock;
                return 0;
            }
            continue;
        }

        /* THE CLOCK. A frame is held
         * for `0x100 / header[0x44]` ticks of the 256 Hz counter; a pause
         * set by op3, op4 or op14 at THIS frame holds it instead for its own
         * count of SECONDS, measured from the same start, and a limit of 999
         * holds it until the player says so. A key or a click cuts a pause
         * short, which is uw_cutscene_advance. */
        c->due = clock + (uint32_t)(0x100 / c->rate);
        c->forever = 0;
        if (c->pause_limit && c->pause_frame == (int)c->frame) {
            c->pausing = 1;
            if (c->pause_limit == UW_CUT_FOREVER) {
                c->forever = 1;
                c->due = 0xffffffffu;
            } else {
                uint32_t until = clock + (uint32_t)c->pause_limit * 256u;
                if (until > c->due) c->due = until;
            }
            /* op14's bit 7: while the voice plays
             * the wait holds whatever the seconds say; a voice already over
             * ends the hold at once, the seconds waited so far -- none --
             * added */
            if ((c->flags & UW_CUT_VOICED) && c->playing) {
                c->voiced_limit = c->pause_limit;
                c->forever = 0;
                c->due = 0xffffffffu;
            } else {
                c->flags &= ~UW_CUT_VOICED;
            }
            c->pause_limit = 0;
        }
        c->frame++;
        page_ends(c);
        return 1;
    }
}

void uw_cutscene_advance(uw_cutscene *c, uint32_t clock, int key) {
    /* nothing advances while a voice is recorded -- not
     * even the debounce's clock moves */
    if (c->voice != -1) return;
    /* the interpreter's own debounce, for a button only: one within 64
     * ticks of the last input or the file's opening is the same press;
     * a key is taken whenever it comes */
    if (!key && (clock < c->advanced || clock - c->advanced <= 0x40)) return;
    if (c->pausing) {
        /* a pause cut short: the key ORed into bit 1 and cleared again at
         * once (128e..129e), the input's time not written -- so it never
         * also skips -- though not the frame's own period, which the next
         * frame waits out from the flip as ever */
        uint32_t period = c->frame_start + (uint32_t)(0x100 / c->rate);
        c->pausing = 0;
        c->forever = 0;
        c->due = period > clock ? period : clock;
        return;
    }
    c->advanced = clock;
    /* bit 1: the play-to's skip, at the next frame */
    c->flags |= UW_CUT_WAITING;
}

/* The voice has played out (voc_playback_finished turning true):
 * cutscene_service_audio's turn, and a pause waiting on the voice given
 * its limit -- the whole seconds waited since the frame went up added,
 * the seconds then running against it as any pause's
 * do. */
static void voice_played_out(uw_cutscene *c, uint32_t clock) {
    c->playing = 0;
    service_voice(c);
    if ((c->flags & UW_CUT_VOICED) && c->due == 0xffffffffu && !c->forever) {
        c->flags &= ~UW_CUT_VOICED;
        c->due = c->frame_start + ((clock - c->frame_start) / 256u + (uint32_t)c->voiced_limit) * 256u;
        c->voiced_limit = 0;
    }
}

void uw_cutscene_voice_started(uw_cutscene *c, long ticks) {
    c->voice_start = -1;
    if (ticks < 0) voice_played_out(c, c->now);
    else c->voice_end = c->now + (uint32_t)ticks;
}

/* anm_cycle: each cycle with a rate moves once 0x38e / rate ticks have
 * passed since it last did -- the DAC's entries first..last rotated down
 * by one (rotate_bank(first, n, 0)), the first's colour to the last. 1 when
 * any moved. */
static int cycle_tick(uw_cutscene *c, uint32_t clock) {
    int i, moved = 0;
    if (!c->have_anim) return 0;
    for (i = 0; i < 16; i++) {
        uint8_t first = c->cycles[i].first, saved[3];
        int k, count = (uint8_t)(c->cycles[i].last - first + 1);
        if (!c->cycles[i].rate) continue;
        if ((uint16_t)((uint16_t)clock - c->cycles[i].started) < 0x38e / c->cycles[i].rate) continue;
        memcpy(saved, c->dac + first * 3, 3);
        for (k = 0; k < count - 1 && first + k + 1 < 256; k++) memcpy(c->dac + (first + k) * 3, c->dac + (first + k + 1) * 3, 3);
        if (first + k < 256) memcpy(c->dac + (first + k) * 3, saved, 3);
        c->cycles[i].started = (uint16_t)clock;
        moved = 1;
    }
    return moved;
}

int uw_cutscene_run(uw_cutscene *c, uint32_t clock) {
    for (;;) {
        uint32_t t;
        if (c->over) return UW_CUT_ENDED;
        /* a fade the loop is inside: its frames, and nothing else, until
         * the last -- the voice's service included */
        if (c->fading) {
            /* the fade is over at its last upload -- one, for no steps */
            int last = c->fader.frames > 0 ? c->fader.frames : 1;
            if (c->fade_done < last) {
                double at = c->fader.frames > 0 ? c->fade_t0 + (c->fade_done + 1) * UW_FADE_FRAME_TICKS
                                                 : c->fade_t0;
                uint32_t tf = (uint32_t)at + ((double)(uint32_t)at < at);
                if (tf > clock) return UW_CUT_IDLE;
                if (tf > c->now) c->now = tf;
                uw_palette_fade_step(&c->fader);
                memcpy(c->dac, c->fader.pal, 768);
                c->fade_done++;
                return UW_CUT_DAC;
            }
            c->fading = 0;
            if (c->closing) { c->over = 1; return UW_CUT_ENDED; }
            continue;
        }
        /* voc_play_file, once any fade in is over */
        if (c->voice_start >= 0) return UW_CUT_VOICE;
        /* the end: voc_stop, and full screen the fade out a fade in still
         * owes, over 2 -- once a pause set on the end frame is over, and
         * op9's fade out after it */
        /* Escape (skipped) goes to the end at once, from the frame's wait or
         * the pause's (0e36..0e52, 1236..1240 to 13b7) */
        if (!(c->flags & UW_CUT_RUNNING) && (c->skipped || (!c->pausing && c->fade_out < 0))) {
            c->playing = 0;
            if (c->full_screen && c->frames && c->fade_out != -2 && !c->closing) {
                c->closing = 1;
                start_fade(c, 1, 2, c->now);
                continue;
            }
            c->over = 1;
            return UW_CUT_ENDED;
        }
        /* the voice running out, seen the first time the loop is free */
        if (c->playing) {
            uint32_t tv = c->voice_end > c->now ? c->voice_end : c->now;
            if (tv <= clock && (c->due == 0xffffffffu || tv <= c->due)) {
                c->now = tv;
                voice_played_out(c, tv);
                return UW_CUT_SILENT;
            }
        }
        if (c->due == 0xffffffffu || c->due > clock) {
            /* anm_cycle, each turn of the frame's and the pause's waits */
            return cycle_tick(c, clock) ? UW_CUT_DAC : UW_CUT_IDLE;
        }
        t = c->due > c->now ? c->due : c->now;
        c->now = t;
        /* op9's fade out, once the frame's wait is over,
         * re-arming the fade in */
        if (c->stepped && c->fade_out >= 0) {
            start_fade(c, 1, c->fade_out, t);
            c->fade_out = -2;
            c->fade_in = -1;
            continue;
        }
        /* the end op on a frame that paused: its pause over -- and op9's
         * fade out after it -- the end */
        if (!(c->flags & UW_CUT_RUNNING)) {
            c->pausing = 0;
            continue;
        }
        switch (step_frame(c, t)) {
        case 1:
            /* the frame flipped, then palette_fade_in(speed) when op10
             * asked */
            if (c->fade_in >= 0) {
                start_fade(c, 0, c->fade_in, t);
                c->fade_in = -2;
                c->fade_out = -1;
            }
            return UW_CUT_FRAME;
        default:
            continue;
        }
    }
}
