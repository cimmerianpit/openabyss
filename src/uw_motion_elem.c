/* SPDX-License-Identifier: MIT */
/* the screen-element manager and the saved-
 * image heap, with the drawing primitives the port makes into the
 * screen: gr_draw_art, gfx_blit, gfx_fill_rect, gfx_draw_string and
 * elem_flush.
 *
 * One of src/uw_motion*.c: see uw_motion.h and uw_motion_int.h. */
#include "uw_motion_int.h"

/* ---- the screen-element manager ------------------------------------------
 *
 * Its records and lists are in its own segment, `m->elem`. A record is 16
 * bytes at handle * 0x10 + 8: +0 flags (1 allocated, 2 visible, 4 nothing
 * saved under it, 8 to be freed, 0x10 drawn shaded), +2 x, +4 y (the bottom
 * edge), +5 width, +7 height, +8 rows cropped off the top, +9 group, +0xb
 * art, +0xd the saved background's handle. The pixels -- the art drawn, the
 * backgrounds saved and restored through the image heap -- are not
 * modelled. Without the segment the calls do nothing. */

static uint16_t erw(const uw_motion *m, uint16_t at) { return (uint16_t)(m->elem[at] | m->elem[at + 1] << 8); }
static void eww(uw_motion *m, uint16_t at, uint16_t v) { m->elem[at] = (uint8_t)v; m->elem[at + 1] = (uint8_t)(v >> 8); }

/* A handle into a list of `count` words at list + 2, appended when absent. */
static void elem_list_add(uw_motion *m, uint16_t list, uint16_t h) {
    uint16_t n = (uint16_t)(erw(m, list) >> 1), k;
    for (k = 0; k < n; k++)
        if (erw(m, (uint16_t)(list + 2 + k * 2)) == h) return;
    eww(m, (uint16_t)(list + 2 + n * 2), h);
    eww(m, list, (uint16_t)(erw(m, list) + 2));
}

/* elem_damage, from the instructions: onto its group's dirty
 * list, onto or off its visible list by the visible bit (removal moves the
 * last entry down over it), and every VISIBLE element of a higher group whose
 * rectangle meets it onto that group's dirty list -- where, the append having
 * put the group's base back in BX, the scan restarts from the group's first
 * entry while its count goes on falling. */
static void elem_damage(uw_motion *m, uint16_t h) {
    uint16_t si = (uint16_t)(h << 4), bx = (uint16_t)(erw(m, (uint16_t)(si + 0x11)) << 6), n, k, g;
    m->elem_any_dirty = 1;
    elem_list_add(m, (uint16_t)(0x408 + bx), h);
    n = (uint16_t)(erw(m, (uint16_t)(0x508 + bx)) >> 1);
    for (k = 0; k < n; k++)
        if (erw(m, (uint16_t)(0x50a + bx + k * 2)) == h) break;
    if (k == n) {
        if (erw(m, (uint16_t)(si + 8)) & 2) {
            eww(m, (uint16_t)(0x50a + bx + n * 2), h);
            eww(m, (uint16_t)(0x508 + bx), (uint16_t)(erw(m, (uint16_t)(0x508 + bx)) + 2));
        }
    } else if (!(erw(m, (uint16_t)(si + 8)) & 2)) {
        eww(m, (uint16_t)(0x50a + bx + k * 2), erw(m, (uint16_t)(0x508 + bx + erw(m, (uint16_t)(0x508 + bx)))));
        eww(m, (uint16_t)(0x508 + bx), (uint16_t)(erw(m, (uint16_t)(0x508 + bx)) - 2));
    }
    for (g = (uint16_t)((erw(m, (uint16_t)(si + 0x11)) << 6) + 0x40); ; g = (uint16_t)(erw(m, 0x608) + 0x40)) {
        uint16_t entry;
        eww(m, 0x608, g);
        if (g >= 0x100) return;
        if (!(erw(m, (uint16_t)(0x508 + g)) >> 1)) { eww(m, 0x608, g); continue; }
        eww(m, 0x60a, (uint16_t)(erw(m, (uint16_t)(0x508 + g)) >> 1));
        entry = g;
        do {
            uint16_t di;
            int16_t ox, sx;
            int8_t oy, sy;
            entry = (uint16_t)(entry + 2);
            di = (uint16_t)(erw(m, (uint16_t)(0x508 + entry)) << 4);
            ox = (int16_t)erw(m, (uint16_t)(di + 0xa));
            sx = (int16_t)erw(m, (uint16_t)(si + 0xa));
            oy = (int8_t)m->elem[(uint16_t)(di + 0xc)];
            sy = (int8_t)m->elem[(uint16_t)(si + 0xc)];
            if (ox <= (int16_t)(sx + (int16_t)erw(m, (uint16_t)(si + 0xd)))
                && (int16_t)(ox + (int16_t)erw(m, (uint16_t)(di + 0xd))) >= sx
                && (int8_t)(sy - (int8_t)m->elem[(uint16_t)(si + 0xf)]) <= oy
                && sy >= (int8_t)(oy - (int8_t)m->elem[(uint16_t)(di + 0xf)])) {
                entry = (uint16_t)(erw(m, (uint16_t)(di + 0x11)) << 6);
                elem_list_add(m, (uint16_t)(0x408 + entry), (uint16_t)(di >> 4));
            }
            eww(m, 0x60a, (uint16_t)(erw(m, 0x60a) - 1));
        } while (erw(m, 0x60a));
    }
}

/* elem_alloc(group, width, height), from the instructions: the
 * first record whose bit 0 is clear, flags 5 (or 0x15 with gfx_span_variant
 * up, which the caller says), the group; a group but 0 takes a background
 * buffer of the size from the image heap (imgbuf_alloc_regs, the width
 * and height in AX and BX) -- none, and 0xffff with the record left taken --
 * and the handle is the saved background's, no crop. 0xffff when all 64
 * are taken. */
uint16_t elem_alloc(uw_motion *m, uint16_t group, uint16_t w, uint16_t h, int span_variant) {
    uint16_t si, handle = 0;
    if (!m->elem) { UW_NOT_CARRIED(m->not_carried); return 0; }
    for (si = 8; si < 0x408; si = (uint16_t)(si + 0x10)) {
        if (erw(m, si) & 1) continue;
        eww(m, si, (uint16_t)(span_variant ? 0x15 : 5));
        eww(m, (uint16_t)(si + 9), group);
        if (group) {
            handle = imgbuf_alloc(m, w, h);
            if (!handle) return 0xffff;
        }
        eww(m, (uint16_t)(si + 0xd), handle);
        m->elem[(uint16_t)(si + 8)] = 0;
        return (uint16_t)((si - 8) >> 4);
    }
    return 0xffff;
}

/* The setters, each refusing a handle above 0x40 and
 * ending in elem_damage. */
void elem_set_rect(uw_motion *m, uint16_t h, uint16_t x, uint8_t y, uint16_t w, uint8_t hh) {
    if (!m->elem || h > 0x40) return;
    eww(m, (uint16_t)((h << 4) + 0xa), x);
    m->elem[(uint16_t)((h << 4) + 0xc)] = y;
    eww(m, (uint16_t)((h << 4) + 0xd), w);
    m->elem[(uint16_t)((h << 4) + 0xf)] = hh;
    elem_damage(m, h);
}

void elem_show(uw_motion *m, uint16_t h, uint16_t art) {
    if (!m->elem || h > 0x40) return;
    eww(m, (uint16_t)((h << 4) + 0x13), art);
    eww(m, (uint16_t)((h << 4) + 8), (uint16_t)(erw(m, (uint16_t)((h << 4) + 8)) | 2));
    elem_damage(m, h);
}

void elem_show_shaded(uw_motion *m, uint16_t h, uint16_t art) {
    if (!m->elem || h > 0x40) return;
    eww(m, (uint16_t)((h << 4) + 0x13), art);
    eww(m, (uint16_t)((h << 4) + 8), (uint16_t)(erw(m, (uint16_t)((h << 4) + 8)) | 0x12));
    elem_damage(m, h);
}

void elem_hide(uw_motion *m, uint16_t h) {
    if (!m->elem || h > 0x40) return;
    if (erw(m, (uint16_t)((h << 4) + 8)) & 2) {
        eww(m, (uint16_t)((h << 4) + 8), (uint16_t)(erw(m, (uint16_t)((h << 4) + 8)) & 0xfffd));
        elem_damage(m, h);
    }
}

void elem_move(uw_motion *m, uint16_t h, uint16_t x, uint8_t y) {
    if (!m->elem || h > 0x40) return;
    eww(m, (uint16_t)((h << 4) + 0xa), x);
    m->elem[(uint16_t)((h << 4) + 0xc)] = y;
    elem_damage(m, h);
}

void elem_set_top_crop(uw_motion *m, uint16_t h, uint8_t rows) {
    if (!m->elem || h > 0x40) return;
    m->elem[(uint16_t)((h << 4) + 0x10)] = rows;
    elem_damage(m, h);
}

/* elem_flush, from the instructions: nothing unless something
 * is dirty. Groups 3, 2 and 1 (never 0), front to back: a dirty element with
 * nothing saved under it drops bit 2, any other has its background restored;
 * one to be freed drops bit 0 (its buffer's free is not here: nothing
 * sets the bit -- destroy_sprite has no caller). Then
 * groups 0..3, back to front: 1..3 first save the background under each
 * dirty visible element and mark each invisible one bit 2; every group's
 * dirty list is emptied and its visible elements drawn. */
/* An art id's image: the paperdoll's six from the GR files by what the
 * data segment says was loaded over INV.GR's first six -- the body by the
 * record's +0x64 (paperdoll_load_body_art: sex * 5 + appearance), a worn
 * slot's armour by the item and wear tier 0x5aca/0x5ad0 remember
 * (paperdoll_load_armour_art) -- and the rest from `art`. */
const uint8_t *art_image(uw_motion *m, uint16_t art, int *w, int *h) {
    if (art >= 0x2091 && art <= 0x2096 && m->gr_file) {
        uint8_t rec64 = m->ds[(uint16_t)(rw(m->ds, PLAYER_RECORD_PTR) + 0x64)];
        int sex = (rec64 >> 1) & 1, k = art - 0x2091;
        uint8_t item = m->ds[(uint16_t)(0x5aca + k)], tier = m->ds[(uint16_t)(0x5ad0 + k)];
        if (!k) return m->gr_file(m->art_user, "BODIES", sex * 5 + ((rec64 & 0x1c) >> 2), w, h);
        if (!item || !tier) return NULL;
        return m->gr_file(m->art_user, sex ? "ARMOR_F" : "ARMOR_M", (item - 1) + (tier - 1) * 0xf, w, h);
    }
    return m->art ? m->art(m->art_user, art, w, h) : NULL;
}

/* gr_draw_art and gr_draw_art_cropped as elem_flush
 * calls them for the element at record `di`, into m->screen. The image's top
 * row goes on screen row 199 - y and its left column at x; flag 0x10
 * (gfx_span_variant) skips colour 0. A top crop of k blits the element's
 * rectangle -- its width, and `h + k` rows from row k -- with row k at the
 * top: the arguments gfx_blit_planar receives, as the flask's cropped fill
 * shows. Art below 0x2000, kept in EMS and blitted chunky, goes the same
 * way. */
static void elem_draw(uw_motion *m, uint16_t di) {
    uint16_t flags = erw(m, (uint16_t)(di + 8)), art = erw(m, (uint16_t)(di + 0x13));
    int x = (int)(int16_t)erw(m, (uint16_t)(di + 0xa)), top = 199 - m->elem[(uint16_t)(di + 0xc)];
    int crop = m->elem[(uint16_t)(di + 0x10)], w, h, j0, j1, cols, i, j;
    const uint8_t *px;
    if (!m->screen) return;
    px = art_image(m, art, &w, &h);
    if (!px) {
        m->pixels_not_drawn++;
        return;
    }
    j0 = 0;
    j1 = h;
    cols = w;
    if (crop) {
        int rw = erw(m, (uint16_t)(di + 0xd)), rh = m->elem[(uint16_t)(di + 0xf)];
        j0 = crop;
        j1 = crop + rh < h ? crop + rh : h;
        cols = rw < w ? rw : w;
    }
    for (j = j0; j < j1; j++) {
        int sy = top + (j - j0);
        if (sy < 0 || sy >= 200) continue;
        for (i = 0; i < cols; i++) {
            uint8_t v = px[j * w + i];
            int sx = x + i;
            if ((!v && (flags & 0x10)) || sx < 0 || sx >= 320) continue;
            m->screen[sy * 320 + sx] = v;
            if (m->screen_written) m->screen_written[sy * 320 + sx] = 1;
        }
    }
}

/* The saved-image heap's record for a handle (imgbuf_capture_regs and
 * imgbuf_restore_regs walk it from 0x14 by 13 to the end pointer at 0x12),
 * or 0. */
uint16_t imgbuf_record(const uw_motion *m, uint16_t handle) {
    uint16_t at, end;
    if (!m->imgheap) return 0;
    end = (uint16_t)(m->imgheap[0x12] | m->imgheap[0x13] << 8);
    for (at = 0x14; at < end && at <= 0xfff0; at = (uint16_t)(at + 0xd))
        if ((uint16_t)(m->imgheap[at] | m->imgheap[at + 1] << 8) == handle) return at;
    return 0;
}

/* imgbuf_alloc through imgbuf_alloc_regs(w, h): a
 * save area of ((w >> 2) + 1) * h plane addresses. A free record (+4 set) of
 * exactly that size is taken; a larger one is split -- the bytes from the
 * record's second to the end pointer moved up 13 by a backward byte copy,
 * made even when the heap then turns out full (the end at 0x1054 or more,
 * which fails), and the rest of its size a free record after it at the
 * handle plus the size. With none, a record appended at the next handle (the
 * word at 0x10), while that stays within the limit at 0xe. The first record
 * is looked at before the end pointer is. The handle, or 0; without the heap,
 * counted. */
uint16_t imgbuf_alloc(uw_motion *m, uint16_t w, uint16_t h) {
    uint8_t *hp = m->imgheap;
    uint16_t size = (uint16_t)(((w >> 2) + 1) * h), at = 0x14, next;
    if (!hp) {
        UW_NOT_CARRIED(m->not_carried);
        return 0;
    }
    do {
        if (hp[(uint16_t)(at + 4)]) {
            uint16_t have = rw(hp, (uint16_t)(at + 2)), end = rw(hp, 0x12), n;
            if (size == have) {
                hp[(uint16_t)(at + 4)] = 0;
                return rw(hp, at);
            }
            if (size < have) {
                for (n = (uint16_t)(end - at); n; n--, end--) hp[(uint16_t)(end + 0xd)] = hp[end];
                if (rw(hp, 0x12) >= 0x1054) return 0;
                ww(hp, 0x12, (uint16_t)(rw(hp, 0x12) + 0xd));
                ww(hp, (uint16_t)(at + 2), size);
                ww(hp, (uint16_t)(at + 0xf), (uint16_t)(have - size));
                hp[(uint16_t)(at + 0x11)] = 1;
                ww(hp, (uint16_t)(at + 0xd), (uint16_t)(size + rw(hp, at)));
                hp[(uint16_t)(at + 4)] = 0;
                return rw(hp, at);
            }
        }
        at = (uint16_t)(at + 0xd);
    } while (at < rw(hp, 0x12));
    next = rw(hp, 0x10);
    if ((uint32_t)next + size > 0xffff || (uint16_t)(next + size) > rw(hp, 0xe)) return 0;
    ww(hp, 0x10, (uint16_t)(next + size));
    ww(hp, at, next);
    ww(hp, (uint16_t)(at + 2), size);
    hp[(uint16_t)(at + 4)] = 0;
    ww(hp, 0x12, (uint16_t)(rw(hp, 0x12) + 0xd));
    return next;
}

/* imgbuf_free(handle): 1 when no record has the handle; else 0 after a
 * merge and the handle itself (AX untouched) when nothing merged -- no
 * caller reads it. A free record just
 * before it whose block ends where this one starts takes it in, and a free
 * record after it that starts where it ends is taken in too -- the merged
 * records moved out of the table by a forward byte copy of the bytes after
 * them; with neither, the record is marked free. Then the record the copy's
 * end leaves the pointer on -- the freed one when nothing merged -- gives
 * its block back to the bump pointer when it ends there.
 *
 * Two of the original's slips are kept: a free record after it that is NOT
 * adjacent still sends the walk to the merge's tail, which rewrites the
 * record BEFORE it -- its size to its own (when free) or nothing, plus this
 * one's, and marks it free -- and leaves this one in use; and after a
 * merge the copy's end is the old table's end, so the bump test reads the
 * bytes past the table. */
uint16_t imgbuf_free(uw_motion *m, uint16_t handle) {
    uint8_t *hp = m->imgheap;
    uint16_t bx = 0x14, si, di, cx = 0, b = 0, ax = handle, dx, ret = 0;
    if (!hp) {
        UW_NOT_CARRIED(m->not_carried);
        return 1;
    }
    for (;;) {
        if (rw(hp, bx) == ax) break;
        bx = (uint16_t)(bx + 0xd);
        if (bx == rw(hp, 0x12)) return 1;
    }
    si = di = bx;
    if (hp[(uint16_t)(di - 9)]) {
        dx = (uint16_t)(rw(hp, (uint16_t)(di - 0xd)) + rw(hp, (uint16_t)(di - 0xb)));
        b = rw(hp, (uint16_t)(di - 0xb));
        if (ax == dx) {
            cx = rw(hp, 0x12);
            ww(hp, 0x12, (uint16_t)(rw(hp, 0x12) - 0xd));
            cx = (uint16_t)(cx - di);
            si = (uint16_t)(si + 0xd);
            goto next;
        }
    }
    if (!hp[(uint16_t)(di + 0x11)]) {
        hp[(uint16_t)(di + 4)] = 1;
        ret = handle;
        goto top;
    }
next:
    if (hp[(uint16_t)(di + 0x11)]) {
        ax = (uint16_t)(ax + rw(hp, (uint16_t)(di + 2)));
        dx = rw(hp, (uint16_t)(di + 0xd));
        if (ax == dx) {
            ww(hp, 0x12, (uint16_t)(rw(hp, 0x12) - 0xd));
            b = (uint16_t)(b + rw(hp, (uint16_t)(di + 0xf)));
            si = (uint16_t)(si + 0xd);
            if (!cx) {
                b = rw(hp, (uint16_t)(di + 2));
                si = (uint16_t)(si + 0xd);
                cx = (uint16_t)(rw(hp, 0x12) - di);
                di = (uint16_t)(di + 0xd);
            }
        }
    }
    b = (uint16_t)(b + rw(hp, (uint16_t)(di + 2)));
    ww(hp, (uint16_t)(di - 0xb), b);
    hp[(uint16_t)(di - 9)] = 1;
    for (; cx; cx--, si++, di++) hp[di] = hp[si];        /* rep movsb */
    di = (uint16_t)(di - 0xd);
top:
    if ((uint16_t)(rw(hp, di) + rw(hp, (uint16_t)(di + 2))) == rw(hp, 0x10)) {
        ww(hp, 0x10, (uint16_t)(rw(hp, 0x10) - rw(hp, (uint16_t)(di + 2))));
        ww(hp, 0x12, (uint16_t)(rw(hp, 0x12) - 0xd));
    }
    return ret;
}
uint16_t uw_motion_imgbuf_free(uw_motion *m, uint16_t handle) { return imgbuf_free(m, handle); }

/* imgbuf_capture_regs and imgbuf_restore_regs: the
 * rectangle (x, y, w, h) -- y the top row in the engine's upward rows --
 * copied between the screen page and the save area at the handle's plane
 * address, through gfx_fill_rect under colour 0x109 or 0x10a:
 * gfx_op_span_save and _restore move whole plane bytes, every plane, so a
 * row is the bytes from x >> 2 to (x + w - 1) >> 2 and the rows are packed
 * one after another from the top. Saving records the rectangle in the
 * handle's record. */
void imgbuf_copy(uw_motion *m, uint16_t handle, int x, int y, int w, int h, int restore) {
    int b0 = x >> 2, nb = ((x + w - 1) >> 2) - b0 + 1, ri, i, pl;
    if (w <= 0 || h <= 0 || nb <= 0) return;
    for (ri = 0; ri < h; ri++) {
        int sr = 199 - y + ri;
        for (i = 0; i < nb; i++) {
            uint32_t src = ((uint32_t)handle + (uint32_t)(ri * nb + i)) * 4;
            for (pl = 0; pl < 4; pl++) {
                int sx = (b0 + i) * 4 + pl;
                if (sr < 0 || sr >= 200 || sx < 0 || sx >= 320 || src + pl >= 0x40000) continue;
                if (restore) {
                    m->screen[sr * 320 + sx] = m->vram[src + pl];
                    if (m->screen_written) m->screen_written[sr * 320 + sx] = 1;
                } else {
                    m->vram[src + pl] = m->screen[sr * 320 + sx];
                }
            }
        }
    }
}

void imgbuf_restore(uw_motion *m, uint16_t handle) {
    uint16_t at = imgbuf_record(m, handle);
    if (!m->screen) return;
    if (!at || !m->vram) {
        m->pixels_not_drawn++;
        return;
    }
    imgbuf_copy(m, handle, (int16_t)(m->imgheap[at + 5] | m->imgheap[at + 6] << 8),
                (int16_t)(m->imgheap[at + 7] | m->imgheap[at + 8] << 8),
                (int16_t)(m->imgheap[at + 9] | m->imgheap[at + 0xa] << 8),
                (int16_t)(m->imgheap[at + 0xb] | m->imgheap[at + 0xc] << 8), 1);
}

/* The save elem_flush makes before drawing a group above 0: the element's
 * own rectangle. */
static void imgbuf_capture(uw_motion *m, uint16_t di) {
    uint16_t handle = erw(m, (uint16_t)(di + 0x15)), at = imgbuf_record(m, handle);
    int x = (int16_t)erw(m, (uint16_t)(di + 0xa)), y = m->elem[(uint16_t)(di + 0xc)];
    int w = (int16_t)erw(m, (uint16_t)(di + 0xd)), h = m->elem[(uint16_t)(di + 0xf)];
    if (!m->screen) return;
    if (!at || !m->vram) {
        m->pixels_not_drawn++;
        return;
    }
    m->imgheap[at + 5] = (uint8_t)x; m->imgheap[at + 6] = (uint8_t)(x >> 8);
    m->imgheap[at + 7] = (uint8_t)y; m->imgheap[at + 8] = (uint8_t)(y >> 8);
    m->imgheap[at + 9] = (uint8_t)w; m->imgheap[at + 0xa] = (uint8_t)(w >> 8);
    m->imgheap[at + 0xb] = (uint8_t)h; m->imgheap[at + 0xc] = (uint8_t)(h >> 8);
    imgbuf_copy(m, handle, x, y, w, h, 0);
}

void uw_motion_gr_draw_art(uw_motion *m, uint16_t art, int x, int y) {
    const uint8_t *px;
    int w, h, i, j, variant = m->span_variant || (art >= 0x101b && art <= 0x101e);
    if (!m->screen) return;
    px = art_image(m, art, &w, &h);
    if (!px) {
        m->pixels_not_drawn++;
        return;
    }
    for (j = 0; j < h; j++) {
        int sy = 199 - y + j;
        if (sy < 0 || sy >= 200 || (m->clip_foot && sy == 199)) continue;
        for (i = 0; i < w; i++) {
            int sx = x + i;
            if (sx < 0 || sx >= 320 || (variant && !px[j * w + i])) continue;
            m->screen[sy * 320 + sx] = px[j * w + i];
            if (m->screen_written) m->screen_written[sy * 320 + sx] = 1;
        }
    }
}

void uw_motion_blit(uw_motion *m, const uint8_t *px, int img_w, int x, int y, int h, int w) {
    int i, j;
    if (!m->screen) return;
    if (!px) {
        m->pixels_not_drawn++;
        return;
    }
    for (j = 0; j < h; j++) {
        int sy = 199 - y + j;
        if (sy < 0 || sy >= 200) continue;
        for (i = 0; i < w; i++) {
            int sx = x + i;
            uint8_t v = px[j * img_w + i];
            if (sx < 0 || sx >= 320 || (m->span_variant && !v)) continue;
            m->screen[sy * 320 + sx] = v;
            if (m->screen_written) m->screen_written[sy * 320 + sx] = 1;
        }
    }
}

void uw_motion_fill_rect(uw_motion *m, int x0, int y0, int x1, int y1, uint8_t colour) {
    int sy, sx;
    if (!m->screen) return;
    for (sy = 199 - y0; sy <= 199 - y1; sy++)
        for (sx = x0; sx <= x1; sx++) {
            if (sx < 0 || sx >= 320 || sy < 0 || sy >= 200) continue;
            m->screen[sy * 320 + sx] = colour;
            if (m->screen_written) m->screen_written[sy * 320 + sx] = 1;
        }
}

int uw_motion_string_width(const uint8_t *font, size_t font_size, const char *str) {
    size_t rec;
    int k, w = 0;
    if (!font || font_size < 12) return 0;
    rec = (size_t)((uint16_t)((font[6] | font[7] << 8) << (((font[8] | font[9] << 8) - 1) & 0x1f)))
          + (uint16_t)(font[0] | font[1] << 8);
    for (k = 0; k < 0x36 && str[k]; k++) {
        uint8_t c = (uint8_t)str[k];
        size_t at = 12 + c * rec + rec - 1;
        if (c < 0x80 && at < font_size) w += font[at];
    }
    return (int16_t)w;
}

void uw_motion_draw_string(uw_motion *m, const uint8_t *font, size_t font_size, const char *str, int x, int y,
                           uint8_t colour) {
    size_t rec;
    int k, top = 199 - y;
    uint16_t height, row_bytes;
    if (!m->screen) return;
    if (!font || font_size < 12) {
        m->pixels_not_drawn++;
        return;
    }
    height = (uint16_t)(font[6] | font[7] << 8);
    row_bytes = (uint16_t)(font[8] | font[9] << 8);
    rec = (size_t)((uint16_t)(height << ((row_bytes - 1) & 0x1f))) + (uint16_t)(font[0] | font[1] << 8);
    /* the window test: the top row y, the bottom row y - height + 1 and the
     * left x inside the window (the whole screen whenever text is drawn) */
    if (y < 0 || y > 199 || y - (int)height + 1 < 0 || y - (int)height + 1 > 199 || x < 0 || x > 319) return;
    for (k = 0; k < 0x84 && str[k]; k++) {
        uint8_t c = (uint8_t)str[k];
        /* the compose's filter, a signed compare: 0x80..0xff too */
        if ((int8_t)c < 0x20 || (int8_t)c > 0x7a) c = '?';
        size_t g = 12 + c * rec;
        int r, b, w;
        if (c >= 0x80 || g + rec > font_size) {
            m->pixels_not_drawn++;
            continue;
        }
        w = font[g + rec - 1];
        for (r = 0; r < height; r++)
            for (b = 0; b < w && b < row_bytes * 8; b++) {
                int sx = x + b, sy = top + r;
                if (!((font[g + (size_t)r * row_bytes + (size_t)(b / 8)] >> (7 - b % 8)) & 1)) continue;
                if (sx < 0 || sx >= 320 || sy < 0 || sy >= 200) continue;
                m->screen[sy * 320 + sx] = colour;
                if (m->screen_written) m->screen_written[sy * 320 + sx] = 1;
            }
        x += w;
    }
}

void elem_flush(uw_motion *m) {
    uint16_t bx, k, n;
    if (!m->elem) return;
    if (m->elem_any_dirty) {
        cursor_hide(m);
        for (bx = 0x4c8; bx > 0x408; bx = (uint16_t)(bx - 0x40)) {
            n = (uint16_t)(erw(m, bx) >> 1);
            for (k = 0; k < n; k++) {
                uint16_t di = (uint16_t)(erw(m, (uint16_t)(bx + 2 + k * 2)) << 4);
                if (erw(m, (uint16_t)(di + 8)) & 4)
                    eww(m, (uint16_t)(di + 8), (uint16_t)(erw(m, (uint16_t)(di + 8)) & 0xfffb));
                else
                    imgbuf_restore(m, erw(m, (uint16_t)(di + 0x15)));
                if (erw(m, (uint16_t)(di + 8)) & 8)
                    eww(m, (uint16_t)(di + 8), (uint16_t)(erw(m, (uint16_t)(di + 8)) & 0xfffe));
            }
        }
        /* Each group's dirty list emptied and its visible elements drawn, from
         * group 0; before a group above 0 is drawn, its invisible dirty
         * elements get bit 2 (nothing saved) and the visible ones have their
         * backgrounds saved. */
        for (bx = 0x408; bx < 0x508; bx = (uint16_t)(bx + 0x40)) {
            n = (uint16_t)(erw(m, bx) >> 1);
            if (bx != 0x408)
                for (k = 0; k < n; k++) {
                    uint16_t di = (uint16_t)(erw(m, (uint16_t)(bx + 2 + k * 2)) << 4);
                    if (!(erw(m, (uint16_t)(di + 8)) & 2))
                        eww(m, (uint16_t)(di + 8), (uint16_t)(erw(m, (uint16_t)(di + 8)) | 4));
                    else
                        imgbuf_capture(m, di);
                }
            eww(m, bx, 0);
            for (k = 0; k < n; k++) {
                uint16_t di = (uint16_t)(erw(m, (uint16_t)(bx + 2 + k * 2)) << 4);
                if (erw(m, (uint16_t)(di + 8)) & 2) elem_draw(m, di);
            }
        }
        cursor_show(m);
    }
    m->elem_any_dirty = 0;
}

uint16_t uw_motion_imgbuf_alloc(uw_motion *m, uint16_t w, uint16_t h) { return imgbuf_alloc(m, w, h); }
void uw_motion_imgbuf_capture(uw_motion *m, uint16_t handle, int x, int y, int w, int h) {
    imgbuf_capture_rect(m, handle, x, y, w, h);
}

void imgbuf_restore_rows(uw_motion *m, uint16_t handle, int x, int y, int w, int end, int srcy) {
    int b0 = x >> 2, nb = ((x + w - 1) >> 2) - b0 + 1, ri, i, pl, h = end - srcy;
    if (!m->screen) return;
    if (!m->vram || w <= 0 || h <= 0 || nb <= 0) {
        m->pixels_not_drawn++;
        return;
    }
    for (ri = 0; ri < h; ri++) {
        int sr = 199 - y + ri;
        for (i = 0; i < nb; i++) {
            uint32_t src = ((uint32_t)handle + (uint32_t)((srcy + ri) * nb + i)) * 4;
            for (pl = 0; pl < 4; pl++) {
                int sx = (b0 + i) * 4 + pl;
                if (sr < 0 || sr >= 200 || sx < 0 || sx >= 320 || src + pl >= 0x40000) continue;
                m->screen[sr * 320 + sx] = m->vram[src + pl];
                if (m->screen_written) m->screen_written[sr * 320 + sx] = 1;
            }
        }
    }
}
