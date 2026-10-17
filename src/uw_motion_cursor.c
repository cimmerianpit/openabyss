/* SPDX-License-Identifier: MIT */
/* the cursor, as far as the data segment and the screen
 * -- its save area, shapes and regions, the weapon composite drawn over
 * the view, view_present and the button latch of cursor_poll.
 *
 * One of src/uw_motion*.c: see uw_motion.h and uw_motion_int.h. */
#include "uw_motion_int.h"

/* ==== the cursor, as far as the data segment ==== */

/* The cursor's rectangle through gfx_fill_rect under colour 0x100 or 0x101
 * -- gfx_op_span_save and _restore, whole plane bytes packed a row after
 * another as an image buffer's are, at the save area gfx_colour_mode_areas
 * gives the entry: x from cursor_x - hot_x for cursor_w, and cursor_h + 1
 * rows from cursor_y + hot_y + 1 down, the row above the shape's top
 * included. Not made over a view presented where the port does not draw it
 * (view_unknown): cursor_blit_in_view saved that background from the
 * viewport. A restore not made leaves the rectangle unknown, what the port
 * drew there no longer compared. */
static void cursor_rect_copy(uw_motion *m, int restore) {
    uint8_t *ds = m->ds;
    uint16_t area = m->cursor_areas[restore ? 1 : 0];
    int16_t cx = rs(ds, CURSOR_X), cy = rs(ds, CURSOR_Y), w = rs(ds, CURSOR_W), h = rs(ds, CURSOR_H);
    int16_t hx = rs(ds, CURSOR_HOT_X), hy = rs(ds, CURSOR_HOT_Y);
    int16_t vx = rs(ds, CURSOR_VIEW_X), vy = rs(ds, CURSOR_VIEW_X + 2);
    int16_t vw = rs(ds, CURSOR_VIEW_X + 4), vh = rs(ds, CURSOR_VIEW_X + 6);
    /* over the view by cursor_overlaps_view's test (twice the half-extents) */
    int over_view = !(vx + vw < cx - w + hx || cx + w - hx < vx || cy + h - hy < vy - vh || vy < cy - h + hy);
    if (!m->screen) return;
    if (!area || !m->vram || (m->view_unknown && over_view)) {
        m->pixels_not_drawn++;
        if (restore && m->screen_written) {
            /* what the rectangle holds now is not known: nothing the port
             * drew there before stands to be compared */
            int x0 = ((cx - hx) >> 2) * 4, x1 = ((cx - hx + w - 1) >> 2) * 4 + 3, top = 199 - (cy + hy + 1), r, c;
            for (r = top; r <= top + h; r++)
                for (c = x0; c <= x1; c++)
                    if (r >= 0 && r < 200 && c >= 0 && c < 320) m->screen_written[r * 320 + c] = 0;
        }
        return;
    }
    imgbuf_copy(m, area, cx - hx, cy + hy + 1, w, h + 1, restore);
}

/* The cursor's rectangle saved as cursor_rect_copy saves it, but only where
 * it lies in the viewport, and from the viewport (`view`, `vw` x `vh`, row 0
 * its bottom, on the screen from x `vx` with its row 0 on row `vbottom`),
 * rows 1 up, the ones the screen shows: the rest of the save area, the
 * frame's, keeps what it held. */
static void cursor_rect_save_in_view(uw_motion *m, const uint8_t *view, int vx, int vbottom, int vw, int vh) {
    uint8_t *ds = m->ds;
    uint16_t area = m->cursor_areas[0];
    int16_t cx = rs(ds, CURSOR_X), cy = rs(ds, CURSOR_Y), w = rs(ds, CURSOR_W), h = rs(ds, CURSOR_H);
    int16_t hx = rs(ds, CURSOR_HOT_X), hy = rs(ds, CURSOR_HOT_Y);
    int x = cx - hx, y = cy + hy + 1, b0 = x >> 2, nb = ((x + w - 1) >> 2) - b0 + 1, ri, i, pl;
    if (!area || !m->vram) {
        m->pixels_not_drawn++;
        return;
    }
    for (ri = 0; ri <= h; ri++) {
        int vy = vbottom - (199 - y + ri);
        if (vy < 1 || vy >= vh) continue;
        for (i = 0; i < nb; i++) {
            uint32_t at = ((uint32_t)area + (uint32_t)(ri * nb + i)) * 4;
            for (pl = 0; pl < 4; pl++) {
                int sx = (b0 + i) * 4 + pl - vx;
                if (sx < 0 || sx >= vw || at + pl >= 0x40000) continue;
                m->vram[at + pl] = view[vy * vw + sx];
            }
        }
    }
}

/* cursor_erase: with the cursor drawn, the background restored
 * over its rectangle; whether it was drawn. */
static int cursor_erase(uw_motion *m) {
    if (!m->ds[CURSOR_ON_SCREEN]) return 0;
    cursor_rect_copy(m, 1);
    return 1;
}

/* cursor_draw: the background under the cursor's rectangle
 * saved, and the cursor marked drawn. */
static void cursor_draw(uw_motion *m) {
    cursor_rect_copy(m, 0);
    m->ds[CURSOR_ON_SCREEN] = 1;
}

/* cursor_blit: cursor_draw, then the shape's art at cursor_x -
 * hot_x, cursor_y + hot_y with colour 0 skipped, clipped -- by the show's
 * window clip, whose bottom test keeps only rows above the window's foot:
 * nothing of the cursor reaches the screen's last line. */
static void cursor_blit(uw_motion *m) {
    uint8_t *ds = m->ds;
    cursor_draw(m);
    m->span_variant = 1;
    m->clip_foot = 1;
    uw_motion_gr_draw_art(m, rw(ds, CURSOR_SHAPE), rs(ds, CURSOR_X) - rs(ds, CURSOR_HOT_X), rs(ds, CURSOR_Y) + rs(ds, CURSOR_HOT_Y));
    m->clip_foot = 0;
    m->span_variant = 0;
}

/* cursor_hide and cursor_show: the visible count
 * down and up. Down to 0 the cursor is erased and marked off the screen; up
 * to 1 cursor_blit draws it. */
void cursor_hide(uw_motion *m) {
    uint8_t *ds = m->ds;
    ww(ds, CURSOR_VISIBLE, (uint16_t)(rw(ds, CURSOR_VISIBLE) - 1));
    if (!rw(ds, CURSOR_VISIBLE) && cursor_erase(m)) ds[CURSOR_ON_SCREEN] = 0;
}

void cursor_show(uw_motion *m) {
    uint8_t *ds = m->ds;
    ww(ds, CURSOR_VISIBLE, (uint16_t)(rw(ds, CURSOR_VISIBLE) + 1));
    if (rw(ds, CURSOR_VISIBLE) == 1) cursor_blit(m);
}

/* weapon_swing_jitter(mode): mode 0 clears the running
 * remainder; 1 and 2 draw rt_rand, negated while the remainder
 * is not positive, and keep its remainder by 5 or by 10. The jitter-applied
 * byte is set whatever the mode. The quotient it returns
 * weapon_composite drops. */
static void weapon_swing_jitter(uw_motion *m, int mode) {
    uint8_t *ds = m->ds;
    if (mode == 0) {
        ww(ds, 0x0798, 0);
    } else if (mode == 1 || mode == 2) {
        int16_t r = (int16_t)rt_rand(m);
        if (rs(ds, 0x0798) < 1) r = (int16_t)-r;
        ww(ds, 0x0798, (uint16_t)(int16_t)(r % (mode == 1 ? 5 : 10)));
    } else {
        return;
    }
    ds[0x079a] = 1;
}

/* weapon_composite into the viewport `buf` (172 wide, row 0 the
 * view's bottom; `bmask` its known pixels): nothing unless the weapon's
 * animation state is not 6, its frame below 0x1c, an
 * animation loaded (a signed byte, not negative) and the overlay
 * allowed; then weapon_swing_jitter by the forward speed -- 0
 * standing, else speed * 2 / 799 + 1 -- and the frame's image blitted with
 * colour 0 skipped. The frame: the state's frame + 0x12 in states 3 and 5;
 * 0x1c less the jitter byte with the frame negative or in state 4; else
 * state * 9 + the frame, the remainder cleared. The image is WEAPONS.GR's
 * (1 - the record's +0x64 bit 0) * 0x70 + animation * 0x1c + frame, the 28
 * weapons_load_anim loaded, its nibbles coloured by weapon_colourmap,
 * the WEAPONS.CM map weapons_load_colourmap chose -- not the
 * image's auxiliary palette, which is the file's map 0 -- and its top row at
 * WEAPONS.DAT's y for the frame
 * counted up from the view's bottom row and running down, its left column
 * at the frame's x plus the remainder -- buttons1's
 * 099, the fist at rest.
 *
 * Then, weapon or not, the interface's three marks over the view:
 * gr_draw_art_by_id of art 0x107f at (62, 3) and 0x1080 and 0x1081 at (0,
 * 13) and (171, 13), 3DWIN.GR's first three images, each drawn rows up from
 * the y it is given with colour 0 skipped -- over the weapon, so the mark on
 * the view's left edge stays where a swing crosses it (swing4's frames 4
 * and 5, all but those five pixels of theirs on the screen). The renderer
 * draws the same marks into its frame (src/uw_scene.c), for the tools that
 * hold a render against a state's viewport without a present; over the
 * view alone the two draws are the same pixels. */
static void weapon_marks(uw_motion *m, uint8_t *buf, uint8_t *bmask) {
    static const uint16_t at[3][3] = { { 0x107f, 62, 3 }, { 0x1080, 0, 13 }, { 0x1081, 171, 13 } };
    const uint8_t *px;
    int k, w, h, i, j;
    for (k = 0; k < 3; k++) {
        px = art_image(m, at[k][0], &w, &h);
        if (!px) {
            m->pixels_not_drawn++;
            continue;
        }
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++) {
                int bx = at[k][1] + i, by = at[k][2] - j;
                if (bx < 0 || bx >= 172 || by < 0 || by >= 113 || !px[j * w + i]) continue;
                buf[by * 172 + bx] = px[j * w + i];
                bmask[by * 172 + bx] = 1;
            }
    }
}

static void weapon_draw(uw_motion *m, uint8_t *buf, uint8_t *bmask) {
    uint8_t *ds = m->ds;
    uint8_t state = ds[0x35ea];
    int16_t frame = rs(ds, 0x0796), speed = rs(ds, MOVEMENT_SPEED), si;
    const uint8_t *px;
    int w, h, i, j, x, y;
    if (state == 6 || frame >= 0x1c || (int8_t)ds[0x0794] < 0 || !ds[0x094b]) return;
    weapon_swing_jitter(m, speed ? (int16_t)(speed * 2) / 799 + 1 : 0);
    if (state == 3 || state == 5) {
        si = (int16_t)(frame + 0x12);
    } else if (frame < 0 || state == 4) {
        si = (int16_t)(0x1c - ds[0x079a]);      /* weapon_jitter_applied is a byte */
    } else {
        si = (int16_t)(state * 9 + frame);
        ww(ds, 0x0798, 0);
    }
    if (si < 0 || si >= 0x1c || !m->gr_file) {
        m->pixels_not_drawn++;              /* a frame past the 28 loaded, or no images given */
        return;
    }
    px = m->gr_file(m->art_user, "WEAPONS#raw",
                    (1 - (ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x64)] & 1)) * 0x70 + (int8_t)ds[0x0794] * 0x1c + si, &w, &h);
    if (!px) {
        m->pixels_not_drawn++;
        return;
    }
    x = ds[(uint16_t)(0x35aa + si)] + rs(ds, 0x0798);
    y = ds[(uint16_t)(0x35c6 + si)];
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            int bx = x + i, by = y - j;
            uint8_t v = ds[(uint16_t)(0x591e + (px[j * w + i] & 0xf))];
            if (bx < 0 || bx >= 172 || by < 0 || by >= 113 || !v) continue;
            buf[by * 172 + bx] = v;
            bmask[by * 172 + bx] = 1;
        }
}

static void weapon_composite(uw_motion *m, uint8_t *buf, uint8_t *bmask) {
    weapon_draw(m, buf, bmask);
    weapon_marks(m, buf, bmask);
}

void uw_motion_weapon_composite(uw_motion *m, uint8_t *buf, uint8_t *bmask) {
    weapon_composite(m, buf, bmask);
}

/* view_present after gfx_execute_draw_list: `view` is the
 * viewport as the list left it, 172 x 113 with row 0 the view's bottom, and
 * `mask` the pixels it wrote; weapon_composite when view_viewport_dirty
 * is set. cursor_overlaps_view: the
 * cursor's rectangle, twice its half-extents, against the view's
 * (cursor_view_x) -- clear of it, mode 0; wholly inside, mode 2;
 * across an edge, mode 1 and the cursor hidden on the screen unless the
 * event's mouse state is 1. Then with the cursor shown once,
 * cursor_blit_in_view: in mode 2 the rectangle under the cursor
 * saved from the viewport, in mode 1 with the mouse state 1 the part of it
 * inside the viewport saved too, the rest of the save area left as it was,
 * and the shape drawn into the viewport, clipped to
 * it; shown more than once, the count down by one. The viewport onto the
 * screen at x 52, its rows 1..112 on rows 130..19 (gfx_blit_planar_to_screen), and
 * cursor_show_unclipped: in mode 1 with the mouse state not 1,
 * cursor_show. `mask` says what of the viewport to blit, and its callers
 * pass all of it: gfx_execute_draw_list clears the work buffer before it
 * runs the list (gfx_clear_work_buffer), so a pixel no face
 * wrote is black rather than what the viewport held before. */
void uw_motion_view_present(uw_motion *m, const uint8_t *view, const uint8_t *mask) {
    enum { VIEW_MODE = 0x25c8, VX = 52, VTOP = 19, VW = 172, VH = 113 };
    uint8_t *ds = m->ds;
    int16_t cx = rs(ds, CURSOR_X), cy = rs(ds, CURSOR_Y), w = rs(ds, CURSOR_W), h = rs(ds, CURSOR_H);
    int16_t hx = rs(ds, CURSOR_HOT_X), hy = rs(ds, CURSOR_HOT_Y);
    int16_t vx = rs(ds, CURSOR_VIEW_X), vy = rs(ds, CURSOR_VIEW_X + 2);
    int16_t vw = rs(ds, CURSOR_VIEW_X + 4), vh = rs(ds, CURSOR_VIEW_X + 6);
    int16_t x1 = (int16_t)(cx - w + hx), x2 = (int16_t)(cx + w - hx);
    int16_t y1 = (int16_t)(cy + h - hy), y2 = (int16_t)(cy - h + hy);
    uint16_t mouse = rw(ds, (uint16_t)(rw(ds, 0x00e2) + 8));
    static uint8_t buf[VW * VH];
    static uint8_t bmask[VW * VH];
    int in_view = 0, x, y;
    if (!m->screen) return;
    /* the viewport: row 0 the bottom */
    for (y = 0; y < VH; y++)
        for (x = 0; x < VW; x++) {
            buf[y * VW + x] = view[y * 320 + x];
            bmask[y * VW + x] = mask[y * 320 + x];
        }
    if (ds[VIEW_VIEWPORT_DIRTY]) weapon_composite(m, buf, bmask);
    if (vx + vw < x1 || x2 < vx || y1 < vy - vh || vy < y2) {
        ds[VIEW_MODE] = 0;
    } else {
        if (vx < x1 && x2 < vx + vw && y1 < vy && vy - vh < y2) {
            ds[VIEW_MODE] = 2;
        } else {
            ds[VIEW_MODE] = 1;
            if (mouse != 1) cursor_hide(m);
        }
        if (rs(ds, CURSOR_VISIBLE) == 1) in_view = 1;
        else if (rs(ds, CURSOR_VISIBLE) > 1) ww(ds, CURSOR_VISIBLE, (uint16_t)(rw(ds, CURSOR_VISIBLE) - 1));
    }
    /* the blit: the written pixels of the viewport's 112 rows above its
     * bottom one onto the screen's 130..19 -- row 131 is the frame's */
    for (y = 1; y < VH; y++)
        for (x = 0; x < VW; x++)
            if (bmask[y * VW + x]) {
                m->screen[(VTOP + VH - 1 - y) * 320 + VX + x] = buf[y * VW + x];
                if (m->screen_written) m->screen_written[(VTOP + VH - 1 - y) * 320 + VX + x] = 1;
            }
    if (in_view) {
        /* cursor_blit_in_view, on the screen the viewport now is: the same
         * place as cursor_blit's, clipped to the view */
        const uint8_t *px;
        int aw, ah, i, j;
        if (ds[VIEW_MODE] == 2) cursor_rect_copy(m, 0);
        else if (mouse == 1) cursor_rect_save_in_view(m, buf, VX, VTOP + VH - 1, VW, VH);
        if (ds[VIEW_MODE] == 2 || mouse == 1) ds[CURSOR_ON_SCREEN] = 1;
        px = art_image(m, rw(ds, CURSOR_SHAPE), &aw, &ah);
        if (!px) {
            m->pixels_not_drawn++;
        } else {
            for (j = 0; j < ah; j++)
                for (i = 0; i < aw; i++) {
                    int sx = cx - hx + i, sy = 199 - (cy + hy) + j;
                    if (sx < VX || sx >= VX + VW || sy < VTOP || sy >= VTOP + VH - 1 || !px[j * aw + i]) continue;
                    m->screen[sy * 320 + sx] = px[j * aw + i];
                    if (m->screen_written) m->screen_written[sy * 320 + sx] = 1;
                }
        }
    }
    if (ds[VIEW_MODE] == 1 && mouse != 1) cursor_show(m);
}

/* cursor_set_shape(id): the size from the image's header --
 * `art_size`, for the art ids 0x1000.. gr_load_all hands out, and
 * `obj_art_size` for an object's below -- the hot
 * point its centre, (w / 2 - 1, h / 2), and the shape recorded twice, the
 * cursor erased before and, when it was drawn, its background saved again
 * after -- but not the new shape drawn. gr_bitmap_ptr's EMS page
 * is the graphics module's. */
void cursor_set_shape(uw_motion *m, uint16_t shape) {
    uint8_t *ds = m->ds;
    uint16_t k = (uint16_t)(shape - 0x1000);
    cursor_erase(m);
    if (shape >= 0x1000 && shape < 0x2000 && m->art_size && k < m->art_size_count) {
        ww(ds, CURSOR_W, m->art_size[k * 2]);
        ww(ds, CURSOR_H, m->art_size[k * 2 + 1]);
    } else if (shape < 0x1000 && m->obj_art_size && shape < m->obj_art_size_count
               && m->obj_art_size[shape * 2]) {
        ww(ds, CURSOR_W, m->obj_art_size[shape * 2]);
        ww(ds, CURSOR_H, m->obj_art_size[shape * 2 + 1]);
    } else {
        UW_NOT_CARRIED(m->not_carried);
    }
    ww(ds, CURSOR_HOT_X, (uint16_t)((int16_t)rw(ds, CURSOR_W) / 2 - 1));
    ww(ds, CURSOR_HOT_Y, (uint16_t)((int16_t)rw(ds, CURSOR_H) / 2));
    ww(ds, CURSOR_SHAPE, shape);
    ww(ds, CURSOR_SHAPE_COPY, shape);
    if (ds[CURSOR_ON_SCREEN]) cursor_draw(m);   /* the background saved again, the shape not drawn */
}

/* cursor_region_update: with nothing pushed, and the cursor
 * outside the region it last matched, the first of the regions holding it
 * becomes the cache and gives the shape; none, and a cache is cleared for
 * the default shape 0x106c. */
static void cursor_region_update(uw_motion *m) {
    uint8_t *ds = m->ds;
    int16_t x = rs(ds, CURSOR_X), y = rs(ds, CURSOR_Y), n = rs(ds, CURSOR_REGIONS), i;
    if ((int8_t)ds[CURSOR_DEPTH] > 0) return;
    if (rs(ds, CURSOR_CACHE_X1) != -1 && x >= rs(ds, CURSOR_CACHE_X1) && x <= rs(ds, CURSOR_CACHE_X2)
        && y >= rs(ds, CURSOR_CACHE_Y1) && y <= rs(ds, CURSOR_CACHE_Y2))
        return;
    for (i = 0; i < n; i++) {
        uint16_t at = (uint16_t)(i * 2);
        if (rs(ds, (uint16_t)(CURSOR_REGION_X1 + at)) <= x && rs(ds, (uint16_t)(CURSOR_REGION_X2 + at)) >= x
            && rs(ds, (uint16_t)(CURSOR_REGION_Y1 + at)) <= y && rs(ds, (uint16_t)(CURSOR_REGION_Y2 + at)) >= y) {
            ww(ds, CURSOR_CACHE_X1, rw(ds, (uint16_t)(CURSOR_REGION_X1 + at)));
            ww(ds, CURSOR_CACHE_X2, rw(ds, (uint16_t)(CURSOR_REGION_X2 + at)));
            ww(ds, CURSOR_CACHE_Y1, rw(ds, (uint16_t)(CURSOR_REGION_Y1 + at)));
            ww(ds, CURSOR_CACHE_Y2, rw(ds, (uint16_t)(CURSOR_REGION_Y2 + at)));
            cursor_set_shape(m, rw(ds, (uint16_t)(CURSOR_REGION_SHAPE + at)));
            break;
        }
    }
    if (rs(ds, CURSOR_CACHE_X1) != -1 && i == n) {
        ww(ds, CURSOR_CACHE_X1, 0xffff);
        cursor_set_shape(m, 0x106c);
    }
}

/* cursor_region_add(x1, y1, x2, y2, shape) -> slot: the first
 * of the twenty whose x1 is not the free marker 10000, written, the count
 * (only a high-water mark) raised past it, and the shape looked up again;
 * -1 when all twenty are taken. */
int16_t cursor_region_add(uw_motion *m, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t shape) {
    uint8_t *ds = m->ds;
    int16_t i;
    for (i = 0; i < 20 && rw(ds, (uint16_t)(CURSOR_REGION_X1 + i * 2)) != 10000; i++) { }
    if (i == 20) return -1;
    ww(ds, (uint16_t)(CURSOR_REGION_X1 + i * 2), (uint16_t)x1);
    ww(ds, (uint16_t)(CURSOR_REGION_X2 + i * 2), (uint16_t)x2);
    ww(ds, (uint16_t)(CURSOR_REGION_Y2 + i * 2), (uint16_t)y2);
    ww(ds, (uint16_t)(CURSOR_REGION_Y1 + i * 2), (uint16_t)y1);
    ww(ds, (uint16_t)(CURSOR_REGION_SHAPE + i * 2), shape);
    if (rs(ds, CURSOR_REGIONS) <= i) ww(ds, CURSOR_REGIONS, (uint16_t)(i + 1));
    cursor_region_update(m);
    return i;
}

/* cursor_region_remove(slot): below the count, the slot's x1
 * the free marker, the cache's x1 too when the slot's shape is the one up
 * (10000, not the -1 an empty cache holds, so the next look searches), and
 * -- only for the last slot -- the count lowered past the free slots behind
 * it; then the shape looked up again. */
void cursor_region_remove(uw_motion *m, int16_t slot) {
    uint8_t *ds = m->ds;
    int16_t i;
    if (slot >= rs(ds, CURSOR_REGIONS)) return;
    ww(ds, (uint16_t)(CURSOR_REGION_X1 + slot * 2), 10000);
    if (rw(ds, (uint16_t)(CURSOR_REGION_SHAPE + slot * 2)) == rw(ds, CURSOR_SHAPE)) ww(ds, CURSOR_CACHE_X1, 10000);
    if (rs(ds, CURSOR_REGIONS) - 1 == slot) {
        for (i = (int16_t)(rs(ds, CURSOR_REGIONS) - 2); i >= 0 && rw(ds, (uint16_t)(CURSOR_REGION_X1 + i * 2)) == 10000; i--) { }
        ww(ds, CURSOR_REGIONS, (uint16_t)(i + 1));
    }
    cursor_region_update(m);
}

/* cursor_shape_push(shape): unless three are pushed already,
 * the shape shown saved on the stack and `shape` put up, hidden around. */
void cursor_shape_push(uw_motion *m, uint16_t shape) {
    uint8_t *ds = m->ds;
    if (ds[CURSOR_DEPTH] == 3) return;
    cursor_hide(m);
    ww(ds, (uint16_t)(CURSOR_STACK + (int8_t)ds[CURSOR_DEPTH] * 2), rw(ds, CURSOR_SHAPE_COPY));
    ds[CURSOR_DEPTH]++;
    cursor_set_shape(m, shape);
    cursor_show(m);
}

/* cursor_shape_pop(flags): the shape saved put back, the
 * regions let take the pointer again; below the bottom the stack holds the
 * default 0x106c. Bit 0 hides first, bit 1 shows after. */
void cursor_shape_pop(uw_motion *m, uint16_t flags) {
    uint8_t *ds = m->ds;
    if (flags & 1) cursor_hide(m);
    ds[CURSOR_DEPTH]--;
    if ((int8_t)ds[CURSOR_DEPTH] < 0) {
        ww(ds, CURSOR_STACK, 0x106c);
        ds[CURSOR_DEPTH] = 0;
    }
    cursor_set_shape(m, rw(ds, (uint16_t)(CURSOR_STACK + (int8_t)ds[CURSOR_DEPTH] * 2)));
    cursor_region_update(m);
    if (flags & 2) cursor_show(m);
}

/* cursor_overlaps_rect(x1, y1, x2, y2): does the cursor's
 * sprite touch the rectangle -- grown by half the sprite's size? */
static int cursor_overlaps_rect(uw_motion *m, int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
    uint8_t *ds = m->ds;
    int16_t hh = (int16_t)((rs(ds, CURSOR_H) + 1) >> 1), hw = (int16_t)((rs(ds, CURSOR_W) + 1) >> 1);
    int16_t cx = rs(ds, CURSOR_X), cy = rs(ds, CURSOR_Y);
    return !(y1 - hh > cy || y2 + hh < cy || x1 - hw > cx || x2 + hw < cx);
}

/* cursor_over_view_rect: the cursor over the 3-D view. */
int cursor_over_view_rect(uw_motion *m) {
    uint8_t *ds = m->ds;
    int16_t x1 = rs(ds, CURSOR_VIEW_X), y2 = rs(ds, (uint16_t)(CURSOR_VIEW_X + 2));
    return cursor_overlaps_rect(m, x1, (int16_t)(y2 - rs(ds, (uint16_t)(CURSOR_VIEW_X + 6))),
                                (int16_t)(x1 + rs(ds, (uint16_t)(CURSOR_VIEW_X + 4))), y2);
}

/* projectile_aim_from_cursor: the cursor clamped into the view
 * (0x34.. by 0xac, 0x43.. by 0x71) as the missile's aim across and up, the
 * view's pitch in the second. Nonzero when not too high in the view. */
int projectile_aim_from_cursor(uw_motion *m) {
    uint8_t *ds = m->ds;
    int16_t x = (int16_t)(rs(ds, CURSOR_X) - 0x34), y = (int16_t)(rs(ds, CURSOR_Y) - 0x43);
    if (x > 0xac) x = 0xac;
    else if (x < 0) x = 0;
    if (y > 0x71) y = 0x71;
    else if (y < 0) y = 0;
    ww(ds, PROJ_AIM_X, (uint16_t)((int16_t)((x - 0x56) * 5) / 0xd + 1));
    ww(ds, PROJ_AIM_Z, (uint16_t)(rs(ds, VIEW_PITCH) / 0x300));
    ww(ds, PROJ_AIM_Z, (uint16_t)(rw(ds, PROJ_AIM_Z) + (int16_t)(y - 0x38) / 6));
    return y >= 0x25;
}

/* cursor_poll(0), view_render's call between the traversal and
 * the draw-list build: cursor_update_position (the port's moves are
 * uw_motion_cursor_move's), then the button latch -- none down clears
 * mouse_button_released, so a release wait's -1 is gone by the next render
 * (seed1); one down with no event pending latches the cursor's position as a
 * click (click_latch_x/_y, mouse_pending_event). mouse_button_mask's keyboard
 * fallback -- keypad Ins and Del as the buttons -- is the host's, in
 * `buttons`. */
void uw_motion_cursor_poll(uw_motion *m) {
    uint8_t *ds = m->ds;
    if (!m->buttons) {
        ww(ds, 0x011b, 0);
    } else if (rw(ds, MOUSE_PENDING) == 0xffff) {
        ww(ds, 0x0117, rw(ds, CURSOR_X));
        ww(ds, 0x0119, rw(ds, CURSOR_Y));
        ww(ds, MOUSE_PENDING, m->buttons);
    }
}

/* cursor_glide_to(x, y): the keyboard's glide aimed at (x, y)
 * clamped into the cursor's bounds, its speed 1, and a step's signs toward
 * it -- y's inverted, the screen's y running up. */
static void cursor_glide_to(uw_motion *m, int16_t x, int16_t y) {
    uint8_t *ds = m->ds;
    int16_t tx = rs(ds, CURSOR_BOUND_X0), ty = rs(ds, CURSOR_BOUND_Y0);
    ww(ds, GLIDE_SPEED, 1);
    if (x >= tx) tx = x > rs(ds, CURSOR_BOUND_X1) ? rs(ds, CURSOR_BOUND_X1) : x;
    if (y >= ty) ty = y > rs(ds, CURSOR_BOUND_Y1) ? rs(ds, CURSOR_BOUND_Y1) : y;
    ww(ds, GLIDE_TARGET_X, (uint16_t)tx);
    ww(ds, GLIDE_TARGET_Y, (uint16_t)ty);
    ww(ds, GLIDE_STEP_X, rs(ds, CURSOR_X) < tx ? 1 : 0xffff);
    ww(ds, GLIDE_STEP_Y, rs(ds, CURSOR_Y) < ty ? 0xffff : 1);
}

/* cursor_key_move(key), which
 * dungeon_mode_setup binds to twelve codes: the keypad's eight send the
 * cursor to the edge or corner they point at -- 0x8c (0, 199), 0x8d y 199,
 * 0x8e (0x13f, 199), 0x8f x 0, 0x91 x 0x13f, 0x92 (0, 0), 0x93 y 0, 0x94
 * (0x13f, 0) -- the glide ending when their scan code (0x47..0x51) is let
 * go; Tab (9) and its shifted code (0x4a3), refused while a glide runs,
 * cycle the cursor at y 0x82 between x 0x82, 0x118 and 0x14, forward and
 * back by the screen's third it is in. The same key again while its glide
 * runs speeds it by 1, to 0x28. */
void uw_motion_cursor_key(uw_motion *m, uint16_t code) {
    cursor_key_move(m, (int16_t)code);
}

void cursor_key_move(uw_motion *m, int16_t key) {
    uint8_t *ds = m->ds;
    int16_t x = rs(ds, CURSOR_X), y = rs(ds, CURSOR_Y), cx = x, watch = 0;
    switch (key) {
    case 0x8c: x = 0; y = 199; watch = 0x47; break;
    case 0x8d: y = 199; watch = 0x48; break;
    case 0x8e: x = 0x13f; y = 199; watch = 0x49; break;
    case 0x8f: x = 0; watch = 0x4b; break;
    case 0x91: x = 0x13f; watch = 0x4d; break;
    case 0x92: x = 0; y = 0; watch = 0x4f; break;
    case 0x93: y = 0; watch = 0x50; break;
    case 0x94: x = 0x13f; y = 0; watch = 0x51; break;
    case 9:
    case 0x4a3:
        if (rs(ds, GLIDE_TARGET_X) >= 0) return;
        y = 0x82;
        if (key == 9) x = cx < 0x2e ? 0x82 : cx < 0xe1 ? 0x118 : 0x14;
        else          x = cx < 0x2e ? 0x118 : cx < 0xe1 ? 0x14 : 0x82;
        break;
    default: return;
    }
    if (watch == rs(ds, GLIDE_KEY) && rs(ds, GLIDE_TARGET_X) >= 0) {
        ww(ds, GLIDE_SPEED, (uint16_t)(rs(ds, GLIDE_SPEED) + 1));
        if (rs(ds, GLIDE_SPEED) > 0x28) ww(ds, GLIDE_SPEED, 0x28);
    } else {
        ww(ds, GLIDE_KEY, (uint16_t)watch);
        if (x != rs(ds, CURSOR_X) || y != rs(ds, CURSOR_Y)) cursor_glide_to(m, x, y);
    }
}

/* cursor_update_position with no motion from the mouse: the
 * glide, while its target is set, rate-limited to a step every ten ticks of
 * the clock -- with no key to watch its speed rising by 8 a step to 0x28,
 * with one let go halving each step until it stops. A step takes `speed`
 * single-pixel moves along each axis, an axis latching when it comes within
 * five pixels of its target; then the cursor erased, moved, clamped to its
 * bounds, the glide over when it has arrived exactly, cursor_region_update
 * and cursor_blit while it is shown. The poll's clip opened to the whole
 * screen around the redraw is the port's drawing's already. */
void uw_motion_cursor_glide(uw_motion *m) {
    uint8_t *ds = m->ds;
    int16_t dx = 0, dy = 0, n, key = rs(ds, GLIDE_KEY);
    uint32_t last = (uint32_t)rw(ds, GLIDE_LAST_CLOCK) | (uint32_t)rw(ds, (uint16_t)(GLIDE_LAST_CLOCK + 2)) << 16;
    unsigned axes = 3;
    if (rs(ds, GLIDE_TARGET_X) < 0) return;
    if (key == 0) {
        if (m->clock - last < 10) return;
        ww(ds, GLIDE_SPEED, (uint16_t)(rs(ds, GLIDE_SPEED) + 8));
        if (rs(ds, GLIDE_SPEED) > 0x28) ww(ds, GLIDE_SPEED, 0x28);
    } else if (m->keys && !m->keys[(uint16_t)(rw(ds, KEY_STATE_PTR) + key)]) {
        ww(ds, GLIDE_SPEED, (uint16_t)(rs(ds, GLIDE_SPEED) >> 1));
        if (!rs(ds, GLIDE_SPEED)) {
            ww(ds, GLIDE_TARGET_X, 0xffff);
            ww(ds, GLIDE_KEY, 0);
            return;
        }
    } else if (m->clock - last < 10) {
        return;
    }
    ww(ds, GLIDE_LAST_CLOCK, (uint16_t)m->clock);
    ww(ds, (uint16_t)(GLIDE_LAST_CLOCK + 2), (uint16_t)(m->clock >> 16));
    for (n = rs(ds, GLIDE_SPEED); n != 0 && axes; n--) {
        int16_t tx = rs(ds, GLIDE_TARGET_X), ty = rs(ds, GLIDE_TARGET_Y), cx = rs(ds, CURSOR_X), cy = rs(ds, CURSOR_Y);
        if (tx - 5 < cx + dx && cx + dx < tx + 5 && (axes & 1)) { axes ^= 1; dx = (int16_t)(tx - cx); }
        else if (axes & 1) dx = (int16_t)(dx + rs(ds, GLIDE_STEP_X));
        if (ty - 5 < cy - dy && cy - dy < ty + 5 && (axes & 2)) { axes ^= 2; dy = (int16_t)(cy - ty); }
        else if (axes & 2) dy = (int16_t)(dy + rs(ds, GLIDE_STEP_Y));
    }
    if (cursor_erase(m)) ds[CURSOR_ON_SCREEN] = 0;
    {
        int16_t x = (int16_t)(rs(ds, CURSOR_X) + dx), y = (int16_t)(rs(ds, CURSOR_Y) - dy);
        if (x < rs(ds, CURSOR_BOUND_X0)) x = rs(ds, CURSOR_BOUND_X0);
        else if (x > rs(ds, CURSOR_BOUND_X1)) x = rs(ds, CURSOR_BOUND_X1);
        if (y < rs(ds, CURSOR_BOUND_Y0)) y = rs(ds, CURSOR_BOUND_Y0);
        else if (y > rs(ds, CURSOR_BOUND_Y1)) y = rs(ds, CURSOR_BOUND_Y1);
        ww(ds, CURSOR_X, (uint16_t)x);
        ww(ds, CURSOR_Y, (uint16_t)y);
        if (rs(ds, GLIDE_TARGET_X) == x && rs(ds, GLIDE_TARGET_Y) == y) {
            ww(ds, GLIDE_KEY, 0);
            ww(ds, GLIDE_TARGET_X, 0xffff);
        }
    }
    cursor_region_update(m);
    if (rs(ds, CURSOR_VISIBLE) > 0) cursor_blit(m);
}

/* cursor_update_position's move to a position the mouse
 * reported: the glide cancelled, the cursor erased where it was (and marked
 * off the screen), the new position clamped to the cursor's bounds
 * -- the view's while the mouse walks (movement_input_update)
 * -- cursor_region_update, and cursor_blit while it is shown. */
void uw_motion_cursor_move(uw_motion *m, int16_t x, int16_t y) {
    uint8_t *ds = m->ds;
    if (x < rs(ds, CURSOR_BOUND_X0)) x = rs(ds, CURSOR_BOUND_X0);
    else if (x > rs(ds, CURSOR_BOUND_X1)) x = rs(ds, CURSOR_BOUND_X1);
    if (y < rs(ds, CURSOR_BOUND_Y0)) y = rs(ds, CURSOR_BOUND_Y0);
    else if (y > rs(ds, CURSOR_BOUND_Y1)) y = rs(ds, CURSOR_BOUND_Y1);
    ww(ds, GLIDE_TARGET_X, 0xffff);
    if (cursor_erase(m)) ds[CURSOR_ON_SCREEN] = 0;
    ww(ds, CURSOR_X, (uint16_t)x);
    ww(ds, CURSOR_Y, (uint16_t)y);
    cursor_region_update(m);
    if (rs(ds, CURSOR_VISIBLE) > 0) cursor_blit(m);
}

void uw_motion_cursor_hide(uw_motion *m) { cursor_hide(m); }
void uw_motion_imgbuf_restore(uw_motion *m, uint16_t handle) { imgbuf_restore(m, handle); }
void uw_motion_cursor_show(uw_motion *m) { cursor_show(m); }
void uw_motion_cursor_shape_push(uw_motion *m, uint16_t shape) { cursor_shape_push(m, shape); }
void uw_motion_cursor_shape_pop(uw_motion *m, uint16_t flags) { cursor_shape_pop(m, flags); }

void uw_motion_cursor_bounds(uw_motion *m, int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    uint8_t *ds = m->ds;
    ww(ds, CURSOR_BOUND_X0, (uint16_t)x0);
    ww(ds, CURSOR_BOUND_Y0, (uint16_t)y0);
    ww(ds, CURSOR_BOUND_X1, (uint16_t)x1);
    ww(ds, CURSOR_BOUND_Y1, (uint16_t)y1);
}
void uw_motion_cursor_reset_bounds(uw_motion *m) { cursor_reset_bounds(m); }

/* cursor_move_to(x, y), mouse_putxy: hidden, the regions updated where it
 * stands -- before the move -- the position written unclamped, shown. */
void uw_motion_cursor_move_to(uw_motion *m, int16_t x, int16_t y) {
    cursor_hide(m);
    cursor_region_update(m);
    ww(m->ds, CURSOR_X, (uint16_t)x);
    ww(m->ds, CURSOR_Y, (uint16_t)y);
    cursor_show(m);
}
int uw_motion_cursor_overlaps_rect(uw_motion *m, int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
    return cursor_overlaps_rect(m, x1, y1, x2, y2);
}
