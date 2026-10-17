/* SPDX-License-Identifier: MIT */
/* The text windows, from the original's instructions. See uw_scroll.h. */
#include "uw_scroll.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum {
    TEXT_WINDOW       = 0x3650,   /* text_window: the current record */
    CURSOR_OVER       = 0x3654,   /* text_window_cursor_over */
    LINES_BEFORE_MORE = 0x0a8a,
    WINDOW_MODE       = 0x0a8c,
    CLEAR_PENDING     = 0x0a8e,
    WINDOW_DIRTY      = 0x0a8f,
    LAST_PRINT_TIME   = 0x0a90,   /* 32 bits, written and never read */
    END_FRAME         = 0x0a94,   /* scroll_animate_ends' frame, 0..4 */
    EDGE_FRAME        = 0x0a96,   /* conv_panel_animate_edges' frame, 0..5 */
    ESCAPES_ON        = 0x0a98,
    GIVE_UP_TEXT      = 0x0aa0,   /* "\n", printed when not one character fits */
    GAME_MODE_MASK    = 0x565e
};

/* A window record's fields; screen y grows upward. */
enum {
    W_Y_TOP = 0x00, W_Y_BOTTOM = 0x02, W_X_LEFT = 0x04, W_X_RIGHT = 0x06, W_CUR_X = 0x08, W_CUR_Y = 0x0a,
    W_HOME_X = 0x0c, W_HOME_Y = 0x0e, W_PENDING = 0x10, W_LINES = 0x11, W_COLOUR = 0x13
};

static uint16_t rw(const uint8_t *b, uint16_t at) { return (uint16_t)(b[at] | b[(uint16_t)(at + 1)] << 8); }
static int16_t rs(const uint8_t *b, uint16_t at) { return (int16_t)rw(b, at); }
static void ww(uint8_t *b, uint16_t at, uint16_t v) { b[at] = (uint8_t)v; b[(uint16_t)(at + 1)] = (uint8_t)(v >> 8); }

static uint16_t window(uw_scroll *s) { return rw(s->m->ds, TEXT_WINDOW); }
static int16_t wfield(uw_scroll *s, int off) { return rs(s->m->ds, (uint16_t)(window(s) + off)); }
static void wset(uw_scroll *s, int off, uint16_t v) { ww(s->m->ds, (uint16_t)(window(s) + off), v); }

/* The font's height, gfx_font_height: the header's fourth word. */
static int16_t font_height(uw_scroll *s) {
    if (!s->font || s->font_size < 12) return 0;
    return (int16_t)rw(s->font, 6);
}

/* gfx_string_width through gfx_font_string_width:
 * the width bytes of at most 54 characters, each at its glyph record's
 * `height << (row_bytes - 1)`, the records `that + 1` bytes apart from the
 * glyph table's start. A character past the 128 glyphs reads beyond the
 * table, and is counted. */
static int16_t string_width(uw_scroll *s, const char *str) {
    int16_t w = 0;
    int k;
    size_t rec;
    if (!s->font || s->font_size < 12) {
        UW_NOT_CARRIED(s->not_carried);
        return 0;
    }
    rec = (size_t)((uint16_t)(rw(s->font, 6) << ((rw(s->font, 8) - 1) & 0x1f))) + rw(s->font, 0);
    for (k = 0; k < 0x36 && str[k]; k++) {
        uint8_t c = (uint8_t)str[k];
        size_t at = 12 + c * rec + (rec - rw(s->font, 0));
        if (c >= 0x80 || at >= s->font_size) {
            UW_NOT_CARRIED(s->not_carried);
            continue;
        }
        w = (int16_t)(w + s->font[at]);
    }
    return w;
}

/* gfx_draw_string(str, x, y) into the motion port's screen, when
 * it has one: each character's glyph record from the font as string_width
 * finds it, `height` rows of `row_bytes` bytes, most significant bit
 * leftmost, a set bit a pixel in the text colour and a clear one left as it
 * is; the glyph's top row on screen row 199 - y, the next character at x
 * plus the width byte (bow14's 031: "You need more space to fire that
 * weapon." at the scroll's (15, 24), all 287 pixels colour 46). The
 * trampoline copies at most 0x84 bytes of the string. */
static void draw_string(uw_scroll *s, const char *str, int x, int y) {
    uw_motion_draw_string(s->m, s->font, s->font_size, str, x, y, s->colour);
}

/* text_window_check_cursor. */
static void check_cursor(uw_scroll *s) {
    s->m->ds[CURSOR_OVER] = (uint8_t)uw_motion_cursor_overlaps_rect(s->m, wfield(s, W_X_LEFT), wfield(s, W_Y_BOTTOM),
                                                                     wfield(s, W_X_RIGHT), wfield(s, W_Y_TOP));
}

/* The decoration a clear or a scroll redraws: scroll_animate_ends after panel_set_value(4, 1) for the message scroll, else
 * conv_panel_animate_edges. Their art is pixels; the frames are
 * the data segment's. */
static void decorate(uw_scroll *s) {
    uint8_t *ds = s->m->ds;
    if (window(s) == UW_SCROLL_WINDOW) {
        uw_motion_panel_set_value(s->m, 4, 1);
        /* the left cap's five frames from 0x20d5 at x 11, the right's from
         * 0x20da at x 306, both at y 30 */
        uw_motion_gr_draw_art(s->m, (uint16_t)(0x20d5 + rw(ds, END_FRAME)), 0x0b, 0x1e);
        uw_motion_gr_draw_art(s->m, (uint16_t)(0x20da + rw(ds, END_FRAME)), 0x132, 0x1e);
        ww(ds, END_FRAME, (uint16_t)(rw(ds, END_FRAME) + 1));
        if (rw(ds, END_FRAME) == 5) ww(ds, END_FRAME, 0);
    } else {
        int i;
        /* three tiles down each side of the panel: frames from 0x20df at x
         * 52 and from 0x20e5 at x 220, from y 148 down by 27 */
        for (i = 0; i < 3; i++) {
            uw_motion_gr_draw_art(s->m, (uint16_t)(0x20df + rw(ds, EDGE_FRAME)), 0x34, 0x94 - i * 0x1b);
            uw_motion_gr_draw_art(s->m, (uint16_t)(0x20e5 + rw(ds, EDGE_FRAME)), 0xdc, 0x94 - i * 0x1b);
        }
        ww(ds, EDGE_FRAME, (uint16_t)(rw(ds, EDGE_FRAME) + 1));
        if (rw(ds, EDGE_FRAME) == 6) ww(ds, EDGE_FRAME, 0);
    }
}

static void emit(uw_scroll *s, char *str, uint16_t flag, const char *buf);
static void emit_lines(uw_scroll *s, char *str, uint16_t flag, const char *buf);
static void print_segments(uw_scroll *s, char *str, uint16_t flag, const char *buf);
static void print_chunks(uw_scroll *s, const char *text);

/* What a level of the emit chain had left when a wait stopped it, kept
 * for uw_scroll_resume: `kind` 0 for emit, 1 emit_lines, 2 print_segments,
 * 3 the print's own chunking. Inserted after the last piece pushed, so an
 * inner level's rest comes before its callers'. */
static void push_pending(uw_scroll *s, int kind, const char *text, uint16_t flag) {
    int at = s->insert_at, n = (int)(sizeof s->pending / sizeof s->pending[0]);
    if (s->npending >= n || !text || !*text) return;
    if (at > s->npending) at = s->npending;
    memmove(&s->pending[at + 1], &s->pending[at], (size_t)(s->npending - at) * sizeof s->pending[0]);
    snprintf(s->pending[at].text, sizeof s->pending[at].text, "%s", text);
    s->pending[at].flag = flag;
    s->pending[at].kind = (uint8_t)kind;
    s->npending++;
    s->insert_at = at + 1;
}

/* scroll_wait_click_or_timeout(ticks, show): the input layer's
 * release and poll -- input_poll_source marks the text window
 * dirty each time -- the cursor shown over the window while waiting and
 * hidden after. 0 when the print stops here: a later state stands in it. */
static int wait_click_or_timeout(uw_scroll *s, int show) {
    uint8_t *ds = s->m->ds;
    if (s->resumed) {
        s->resumed = 0;                     /* the state stood here: its end */
    } else {
        ds[WINDOW_DIRTY] = 1;
        if (ds[CURSOR_OVER] && show) uw_motion_cursor_show(s->m);
        if (s->stop_at_wait || s->hold_waits) {
            s->stopped = 1;
            return 0;
        }
    }
    ds[WINDOW_DIRTY] = 1;
    check_cursor(s);
    if (ds[CURSOR_OVER] && show) uw_motion_cursor_hide(s->m);
    return 1;
}

/* gfx_fill_rect(x0, y0, x1, y1) in the colour last set, and
 * gfx_move_rect(x, y, w, h, to_x, to_y), a rectangle moved within
 * the page -- y the top row in the engine's upward rows -- into the motion
 * port's screen when it has one. */
static void fill_rect(uw_scroll *s, int x0, int y0, int x1, int y1, uint8_t colour) {
    uw_motion_fill_rect(s->m, x0, y0, x1, y1, colour);
}

static void move_rect(uw_scroll *s, int x, int y, int w, int h, int to_x, int to_y) {
    uw_motion *m = s->m;
    int r, c, step, first, last;
    if (!m->screen || w <= 0 || h <= 0) return;
    /* overlap-safe: moving up, copy from the top row down */
    step = to_y >= y ? 1 : -1;
    first = step > 0 ? 0 : h - 1;
    last = step > 0 ? h : -1;
    for (r = first; r != last; r += step)
        for (c = 0; c < w; c++) {
            int sy = 199 - y + r, sx = x + c, dy = 199 - to_y + r, dx = to_x + c;
            if (sx < 0 || sx >= 320 || sy < 0 || sy >= 200 || dx < 0 || dx >= 320 || dy < 0 || dy >= 200) continue;
            m->screen[dy * 320 + dx] = m->screen[sy * 320 + sx];
            if (m->screen_written) m->screen_written[dy * 320 + dx] = 1;
        }
}

/* text_window_scroll_up(y): the text from one line below home
 * down to y moved up a line to home, the space it leaves from cur_y down to
 * y + 1 filled with colour 0x2a, and the decoration. */
static void scroll_up(uw_scroll *s, int y) {
    int fh = font_height(s), home = wfield(s, W_HOME_Y);
    move_rect(s, wfield(s, W_X_LEFT), home - fh, wfield(s, W_X_RIGHT) - wfield(s, W_X_LEFT) + 1,
              home - fh - y + 1, wfield(s, W_X_LEFT), home);
    fill_rect(s, wfield(s, W_X_LEFT), wfield(s, W_CUR_Y), wfield(s, W_X_RIGHT), y + 1, 0x2a);
    decorate(s);
}

/* write_text_with_more: a line scrolled up for "[MORE]", drawn
 * in colour 0x60, the wait, the line cleared, the colour back; the lines
 * left before the next [MORE] one short of the window's count. */
static void write_more(uw_scroll *s) {
    uint8_t colour = s->colour;
    int cur_y;
    scroll_up(s, wfield(s, W_CUR_Y) - font_height(s));
    cur_y = wfield(s, W_CUR_Y);
    s->colour = 0x60;
    draw_string(s, (const char *)s->m->ds + 0x0a99, wfield(s, W_HOME_X), cur_y);     /* "[MORE]" */
    s->more_colour = colour;
    if (!wait_click_or_timeout(s, 1)) return;
    fill_rect(s, wfield(s, W_HOME_X), cur_y, wfield(s, W_X_RIGHT), wfield(s, W_Y_BOTTOM), 0x2a);
    s->colour = colour;
    ww(s->m->ds, LINES_BEFORE_MORE, (uint16_t)(wfield(s, W_LINES) - 1));
    wset(s, W_CUR_X, (uint16_t)wfield(s, W_HOME_X));
}

/* write_more's tail, for a resume: the [MORE] line cleared, the colour
 * back, the lines before the next one, the write position home. */
static void write_more_tail(uw_scroll *s) {
    fill_rect(s, wfield(s, W_HOME_X), wfield(s, W_CUR_Y), wfield(s, W_X_RIGHT), wfield(s, W_Y_BOTTOM), 0x2a);
    s->colour = s->more_colour;
    ww(s->m->ds, LINES_BEFORE_MORE, (uint16_t)(wfield(s, W_LINES) - 1));
    wset(s, W_CUR_X, (uint16_t)wfield(s, W_HOME_X));
}

/* text_window_emit_wrapped(str, flag): the head that fits
 * emitted with a newline, the tail after it. The last space that fits, or
 * failing one a mid-word cut: that search restores each character one place
 * past where it took it from (`mov [si+1],al` after `dec si`), so every step
 * past the first shifts the tail right and leaves a second terminator --
 * the tail emitted is the one character at the cut. With not even one
 * character fitting, "\n" and then the whole string. */
static void emit_wrapped(uw_scroll *s, char *str, uint16_t flag, const char *buf) {
    char *si = strrchr(str, ' ');
    char c1, c2;
    while (si) {
        char *t;
        *si = '\0';
        if (wfield(s, W_CUR_X) + string_width(s, str) < wfield(s, W_X_RIGHT)) {
            *si = ' ';
            break;
        }
        t = strrchr(str, ' ');
        *si = ' ';
        si = t;
    }
    if (!si) {
        size_t n = strlen(str);
        char c;
        if (n < 2) {
            UW_NOT_CARRIED(s->not_carried);
            return;
        }
        si = str + n - 1;
        c = *si;
        si--;
        for (;;) {
            si[1] = c;
            si--;
            c = *si;
            *si = '\0';
            if (si <= str) break;
            if (wfield(s, W_CUR_X) + string_width(s, str) < wfield(s, W_X_RIGHT)) break;
        }
        if (si <= str) {
            /* The string itself, in the data segment: its newline is cut
             * off in place, as the original's is. */
            char *give_up = (char *)s->m->ds + GIVE_UP_TEXT;
            emit(s, give_up, 1, give_up);
            if (s->stopped) { push_pending(s, 0, str, flag); return; }
            emit(s, str, flag, buf);
            return;
        }
        *si = c;
    }
    c1 = si[0];
    c2 = si[1];
    si[0] = '\n';
    si[1] = '\0';
    emit(s, str, 1, buf);
    si[0] = c1;
    si[1] = c2;
    if (s->stopped) { push_pending(s, 0, si + (c1 == ' '), flag); return; }
    emit(s, si + (c1 == ' '), flag, buf);
}

/* text_window_emit(str, flag), from the instructions: a leading
 * escape -- \0..\6 a colour, \P and \p a pause of 200 and 600 ticks, \m a
 * [MORE] -- and the colour kept in the window; a line left open advanced,
 * scrolling while the lines before [MORE] allow `flag` more and the window
 * has room below, or [MORE] when they do not; then the piece drawn at the
 * write position, wrapped when it does not fit, a trailing newline leaving
 * the line open. `buf` is the print's buffer, which a piece's end is
 * checked within. */
static void emit(uw_scroll *s, char *str, uint16_t flag, const char *buf) {
    uint8_t *ds = s->m->ds;
    char *si = str;
    int16_t w;
    ptrdiff_t n;
    if (ds[ESCAPES_ON] && si[0] == '\\') {
        char c = si[1];
        si += 2;
        switch (c) {
        case '0': s->colour = 0x2e; break;
        case '1': s->colour = 0x26; break;
        case '2': s->colour = 0xf1; break;
        case '3': s->colour = 0x60; break;
        case '4': s->colour = 0xb4; break;
        case '5': s->colour = 0xc4; break;
        case '6': s->colour = 0xd4; break;
        case 'p': /* 600 ticks */
        case 'P': /* 200 ticks */
            s->stop_kind = c == 'p' ? 2 : 1;
            if (!wait_click_or_timeout(s, 1)) { push_pending(s, 0, si, flag); return; }
            break;
        case 'm':
            s->stop_kind = 3;
            write_more(s);
            if (s->stopped) { push_pending(s, 0, si, flag); return; }
            break;
        default:
            break;
        }
        wset(s, W_COLOUR, s->colour);
    }
    if (ds[(uint16_t)(window(s) + W_PENDING)]) {
        int16_t fh = font_height(s), di = (int16_t)(wfield(s, W_CUR_Y) - fh);
        int full = (int16_t)(di - fh) < (int16_t)(wfield(s, W_Y_BOTTOM) - 1);
        if ((int16_t)(rs(ds, LINES_BEFORE_MORE) - (int16_t)flag) < 0) {
            if (full) {
                s->stop_kind = 4;
                write_more(s);
                if (s->stopped) { push_pending(s, 0, si, flag); return; }
                di = wfield(s, W_CUR_Y);
            } else {
                wset(s, W_LINES, (uint16_t)(wfield(s, W_LINES) + 1));
            }
        } else if (full) {
            scroll_up(s, di);
            di = wfield(s, W_CUR_Y);
            ww(ds, LINES_BEFORE_MORE, (uint16_t)(rw(ds, LINES_BEFORE_MORE) - 1));
        } else {
            wset(s, W_LINES, (uint16_t)(wfield(s, W_LINES) + 1));
        }
        wset(s, W_CUR_Y, (uint16_t)di);
        wset(s, W_CUR_X, (uint16_t)wfield(s, W_HOME_X));
        ds[(uint16_t)(window(s) + W_PENDING)] = 0;
    }
    w = string_width(s, si);
    if (wfield(s, W_X_RIGHT) <= (int16_t)(wfield(s, W_CUR_X) + w)) {
        emit_wrapped(s, si, flag, buf);
        return;
    }
    n = (ptrdiff_t)strlen(si) - 1;
    if (n >= 0 ? si[n] == '\n' : (si > buf && si[-1] == '\n')) {
        if (n >= 0) si[n] = '\0';
        else si[-1] = '\0';
        ds[(uint16_t)(window(s) + W_PENDING)] = 1;
    }
    draw_string(s, si, wfield(s, W_CUR_X), wfield(s, W_CUR_Y));
    wset(s, W_CUR_X, (uint16_t)(wfield(s, W_CUR_X) + string_width(s, si)));
}

/* text_window_emit_lines(str, flag): cut after each newline
 * that is not the last character, the pieces but the last with flag 1. */
static void emit_lines(uw_scroll *s, char *str, uint16_t flag, const char *buf) {
    char *di = str, *si;
    while ((si = strchr(di, '\n')) != NULL && si[1] != '\0') {
        char save = si[1];
        si[1] = '\0';
        emit(s, di, 1, buf);
        si[1] = save;
        if (s->stopped) { push_pending(s, 1, si + 1, flag); return; }
        di = si + 1;
    }
    emit(s, di, flag, buf);
}

/* scroll_print_segments(str, flag): cut before each backslash
 * past the first character; a piece followed by more than one character, or
 * by \m, goes with flag 1. */
static void print_segments(uw_scroll *s, char *str, uint16_t flag, const char *buf) {
    char *di = str, *si;
    while ((si = strchr(di + 1, '\\')) != NULL) {
        uint16_t f2;
        *si = '\0';
        f2 = (si[2] != '\0' || si[1] == 'm') ? 1 : flag;
        emit_lines(s, di, f2, buf);
        *si = '\\';
        if (s->stopped) { push_pending(s, 2, si, flag); return; }
        di = si;
    }
    emit_lines(s, di, flag, buf);
}

/* scroll_print's opening: the cursor hidden over the window, a pending
 * clear done (not in the NPC's window), the lines before [MORE] reset. */
static void print_prologue(uw_scroll *s) {
    uint8_t *ds = s->m->ds;
    check_cursor(s);
    if (ds[CURSOR_OVER]) uw_motion_cursor_hide(s->m);
    if (ds[CLEAR_PENDING] && rw(ds, WINDOW_MODE) != 1) {
        ds[CLEAR_PENDING] = 0;
        uw_scroll_clear(s, 0);
    }
    ww(ds, LINES_BEFORE_MORE, (uint16_t)wfield(s, W_LINES));
    ds[WINDOW_DIRTY] = 0;
}

int16_t uw_scroll_print(uw_scroll *s, const char *text) {
    uint8_t *ds = s->m->ds;
    static char local[0x32];    /* the print's 49-byte buffer and its terminator */
    const char *p = text;
    uint16_t mode = rw(ds, GAME_MODE_MASK);
    size_t left;
    if (mode != 1 && mode != 4) return -1;
    if (!window(s)) { UW_NOT_CARRIED(s->not_carried); return -1; }
    if (s->stopped && s->hold_waits) {
        /* a print while one waits: the original's caller would be inside
         * that wait still, so this one runs after it, whole */
        s->insert_at = s->npending;
        push_pending(s, 5, text, 0);
        return -1;
    }
    s->stopped = 0;
    if (!s->resumed) print_prologue(s);
    s->colour = ds[(uint16_t)(window(s) + W_COLOUR)];
    s->npending = 0;
    s->insert_at = 0;
    (void)local;
    (void)p;
    (void)left;
    print_chunks(s, text);
    if (s->stopped) return -1;
    ww(ds, LAST_PRINT_TIME, (uint16_t)s->m->clock);
    ww(ds, (uint16_t)(LAST_PRINT_TIME + 2), (uint16_t)(s->m->clock >> 16));
    if (ds[CURSOR_OVER]) uw_motion_cursor_show(s->m);
    return wfield(s, W_LINES);
}

/* scroll_print's own loop: the text in pieces of at most 0x31, cut at the
 * last space, each through print_segments with flag 1, the last with 0. */
static void print_chunks(uw_scroll *s, const char *text) {
    static char local[0x32];
    const char *p = text;
    size_t left = strlen(text);
    while (left > 0x31) {
        char *sp, save;
        size_t cut;
        memcpy(local, p, 0x31);
        local[0x31] = '\0';
        sp = strrchr(local, ' ');
        if (!sp) sp = local + 0x31;
        save = *sp;
        *sp = '\0';
        cut = (size_t)(sp - local);
        if (!cut) {
            UW_NOT_CARRIED(s->not_carried);
            return;
        }
        print_segments(s, local, 1, local);
        *sp = save;
        if (s->stopped) { push_pending(s, 3, p + cut, 0); return; }
        p += cut;
        left -= cut;
    }
    memcpy(local, p, left + 1);
    print_segments(s, local, 0, local);
}

int uw_scroll_resume(uw_scroll *s) {
    uint8_t *ds = s->m->ds;
    if (!s->stopped) return 1;
    s->stopped = 0;
    /* the wait's end (wait_click_or_timeout's tail) */
    ds[WINDOW_DIRTY] = 1;
    check_cursor(s);
    if (ds[CURSOR_OVER]) uw_motion_cursor_hide(s->m);
    if (s->stop_kind == 3 || s->stop_kind == 4) {
        write_more_tail(s);
        if (s->stop_kind == 4) {
            /* the open line's advance, past its [MORE] */
            wset(s, W_CUR_X, (uint16_t)wfield(s, W_HOME_X));
            ds[(uint16_t)(window(s) + W_PENDING)] = 0;
        }
    }
    wset(s, W_COLOUR, s->colour);
    while (s->npending) {
        static char text[0x800];
        uint16_t flag = s->pending[0].flag;
        int kind = s->pending[0].kind;
        memcpy(text, s->pending[0].text, sizeof text);
        memmove(&s->pending[0], &s->pending[1], (size_t)(s->npending - 1) * sizeof s->pending[0]);
        s->npending--;
        s->insert_at = 0;
        switch (kind) {
        case 0: emit(s, text, flag, text); break;
        case 1: emit_lines(s, text, flag, text); break;
        case 2: print_segments(s, text, flag, text); break;
        case 5:
            /* a later print, queued behind the wait: its own opening */
            print_prologue(s);
            s->colour = ds[(uint16_t)(window(s) + W_COLOUR)];
            print_chunks(s, text);
            break;
        default: print_chunks(s, text); break;
        }
        if (s->stopped) return 0;
    }
    ww(ds, LAST_PRINT_TIME, (uint16_t)s->m->clock);
    ww(ds, (uint16_t)(LAST_PRINT_TIME + 2), (uint16_t)(s->m->clock >> 16));
    if (ds[CURSOR_OVER]) uw_motion_cursor_show(s->m);
    return 1;
}

void uw_scroll_select_scroll(uw_scroll *s) {
    uint8_t *ds = s->m->ds;
    ww(ds, WINDOW_MODE, 0);
    ds[WINDOW_DIRTY] = 1;
    ww(ds, TEXT_WINDOW, UW_SCROLL_WINDOW);
}

void uw_scroll_select_conv_npc(uw_scroll *s) {
    uint8_t *ds = s->m->ds;
    ww(ds, WINDOW_MODE, 1);
    ds[WINDOW_DIRTY] = 1;
    ww(ds, TEXT_WINDOW, UW_CONV_WINDOW);
}

void uw_scroll_select_conv_menu(uw_scroll *s) {
    uint8_t *ds = s->m->ds;
    ww(ds, WINDOW_MODE, 2);
    ds[WINDOW_DIRTY] = 1;
    ww(ds, TEXT_WINDOW, UW_SCROLL_WINDOW);
}

void uw_scroll_clear(uw_scroll *s, int manage_cursor) {
    uint8_t *ds = s->m->ds;
    if (manage_cursor) {
        check_cursor(s);
        if (ds[CURSOR_OVER]) uw_motion_cursor_hide(s->m);
    }
    fill_rect(s, wfield(s, W_X_LEFT), wfield(s, W_Y_TOP), wfield(s, W_X_RIGHT), wfield(s, W_Y_BOTTOM), 0x2a);
    wset(s, W_CUR_Y, (uint16_t)wfield(s, W_HOME_Y));
    wset(s, W_CUR_X, (uint16_t)wfield(s, W_HOME_X));
    ds[(uint16_t)(window(s) + W_PENDING)] = 0;
    wset(s, W_LINES, 0);
    decorate(s);
    if (manage_cursor && ds[CURSOR_OVER]) uw_motion_cursor_show(s->m);
}

void uw_scroll_field_begin(uw_scroll *s) {
    if (!window(s)) return;
    s->echo_x = wfield(s, W_CUR_X);
    s->echo_y = wfield(s, W_CUR_Y);
}

/* scroll_text_input's redraw of its buffer: the line erased from the echo x
 * to the window's right edge, the text drawn from the echo x, and the
 * window's x left after it (where Enter leaves text_window+8). */
void uw_scroll_field(uw_scroll *s, const char *text) {
    uw_motion *m = s->m;
    int16_t x, y, right, fh;
    if (!window(s) || !s->font) return;
    x = s->echo_x;
    y = s->echo_y;
    right = wfield(s, W_X_RIGHT);
    fh = font_height(s);
    uw_motion_fill_rect(m, x, y, right, (int16_t)(y - fh + 1), 0x2a);
    uw_motion_draw_string(m, s->font, s->font_size, text, x, y, m->ds[(uint16_t)(window(s) + W_COLOUR)]);
    wset(s, W_CUR_X, (uint16_t)(x + string_width(s, text)));
}

/* the editor's field redrawn: the line cleared from the echo x to the
 * window's right, the text drawn from the echo x */
static void edit_redraw(uw_scroll *s, uw_scroll_edit *e) {
    uw_motion *m = s->m;
    int16_t fh = font_height(s);
    uw_motion_fill_rect(m, s->echo_x, s->echo_y, wfield(s, W_X_RIGHT), (int16_t)(s->echo_y - fh + 1), 0x2a);
    uw_motion_draw_string(m, s->font, s->font_size, e->text, s->echo_x, s->echo_y, m->ds[(uint16_t)(window(s) + W_COLOUR)]);
}

/* the caret's column: the echo x and the text before the point */
static int16_t edit_point_x(uw_scroll *s, uw_scroll_edit *e) {
    char head[52];
    int n = e->pos < 0 ? -e->pos : e->pos;
    snprintf(head, sizeof head, "%.*s", n, e->text);
    return (int16_t)(s->echo_x + string_width(s, head));
}

static void edit_caret_block(uw_scroll *s, uw_scroll_edit *e, uint8_t colour) {
    int16_t x = edit_point_x(s, e), fh = font_height(s);
    uw_motion_fill_rect(s->m, x, s->echo_y, x + 4, (int16_t)(s->echo_y - fh + 1), colour);
}

void uw_scroll_edit_begin(uw_scroll *s, uw_scroll_edit *e, const char *initial, int any, int max) {
    memset(e, 0, sizeof *e);
    e->max = max > 0x32 ? 0x32 : max;
    e->any = any;
    e->caret = 3000;
    snprintf(e->initial, sizeof e->initial, "%s", initial ? initial : "");
    if (!window(s)) return;
    e->room = (int16_t)(wfield(s, W_X_RIGHT) - wfield(s, W_CUR_X) - 0x14);
    uw_scroll_field_begin(s);
    snprintf(e->text, sizeof e->text, "%s", e->initial);
    e->pos = -(int)strlen(e->text);
    if (s->font) uw_scroll_field(s, e->text);
}

int uw_scroll_edit_key(uw_scroll *s, uw_scroll_edit *e, int code) {
    int len = (int)strlen(e->text), k;
    if (!window(s) || !s->font) return code == 0x0d || code == 0x1b || (code >= 1 && code <= 3) ? code : 0;
    if (code == 0x0d || code == 0x1b || code == 1 || code == 2 || code == 3) {
        if (e->caret >= 2000) {
            edit_caret_block(s, e, 0x2a);
            if ((unsigned)e->pos < (unsigned)len) edit_redraw(s, e);
        }
        if (code == 0x1b) {
            int16_t fh = font_height(s);
            snprintf(e->text, sizeof e->text, "%s", e->initial);
            uw_motion_fill_rect(s->m, s->echo_x, s->echo_y, wfield(s, W_X_RIGHT), (int16_t)(s->echo_y - fh + 1), 0x2a);
            wset(s, W_CUR_X, (uint16_t)s->echo_x);
            uw_scroll_print(s, "-");
        } else {
            wset(s, W_CUR_X, (uint16_t)edit_point_x(s, e));
        }
        return code;
    }
    if (code == -1) return 0;
    if (code == 0xa8 || code == 0x162 || code == 0x8f) {            /* left */
        if (e->pos < 0) e->pos = -e->pos;
        if (e->pos > 0) e->pos--;
    } else if (code == 0x161 || code == 0xa5 || code == 0x8c) {     /* Home */
        e->pos = 0;
    } else if (code == 0xa9 || code == 0x166 || code == 0x91) {     /* right */
        if (e->pos < 0) e->pos = -e->pos;
        if (e->pos < len) e->pos++;
    } else if (code == 0xaa || code == 0x165 || code == 0x92) {     /* End */
        e->pos = len;
    } else if (code == 0x164 || code == 0x96) {                     /* Delete */
        if (e->pos < 0) e->pos = -e->pos;
        if (e->pos < len) memmove(e->text + e->pos, e->text + e->pos + 1, (size_t)(len - e->pos));
    } else if (code == 0x16b) {                                     /* cut at the point */
        if (e->pos < 0) e->pos = -e->pos;
        e->text[e->pos] = 0;
    } else if (code == 8) {                                         /* Backspace */
        if (e->pos < 0) e->pos = -e->pos;
        if (e->pos > 0) {
            memmove(e->text + e->pos - 1, e->text + e->pos, (size_t)(len - e->pos + 1));
            e->pos--;
        }
    } else {
        if (e->pos < 0) { e->pos = 0; e->text[0] = 0; len = 0; }
        if (code > 0x1f && code < 0x7f && string_width(s, e->text) < e->room && len < e->max
            && (e->any || (code >= '0' && code <= '9'))) {
            for (k = len; k >= e->pos; k--) e->text[k + 1] = e->text[k];
            e->text[e->pos++] = (char)code;
        }
    }
    edit_redraw(s, e);
    return 0;
}

void uw_scroll_edit_caret(uw_scroll *s, uw_scroll_edit *e, int counts) {
    uw_motion *m = s->m;
    if (!window(s) || !s->font || counts <= 0) return;
    e->caret += counts;
    if (e->caret >= 4000) {
        edit_caret_block(s, e, 0x2a);
        if ((unsigned)e->pos < strlen(e->text)) edit_redraw(s, e);
        e->caret %= 4000;
    }
    if (e->caret >= 2000) edit_caret_block(s, e, m->ds[(uint16_t)(window(s) + W_COLOUR)]);
}

void uw_scroll_echo_text(uw_scroll *s, const char *text) {
    int16_t y, fh;
    if (!window(s) || !s->font) return;
    y = wfield(s, W_CUR_Y);
    fh = font_height(s);
    uw_motion_fill_rect(s->m, s->echo_x, y, wfield(s, W_CUR_X), (int16_t)(y - fh + 1), 0x2a);
    wset(s, W_CUR_X, (uint16_t)s->echo_x);
    uw_scroll_print(s, text);
}

void uw_scroll_echo_number(uw_scroll *s, int n) {
    char digits[12];
    snprintf(digits, sizeof digits, "%d", n);
    uw_scroll_echo_text(s, digits);
}
