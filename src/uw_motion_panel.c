/* SPDX-License-Identifier: MIT */
/* the panels: panel_redraw_dispatch and its nine drawers (the flasks,
 * the compass dial, the dragons, the weapon's animation), the palette
 * cycle, panel_set_value, the flask and time status lines, the mode
 * icon and the action buttons.
 *
 * One of src/uw_motion*.c: see uw_motion.h and uw_motion_int.h. */
#include "uw_motion_int.h"

/* ---- panel_redraw_dispatch and the panel drawers, event 13 ---
 *
 * What the pass's third standing handler writes in the data segment. The
 * drawers draw through the screen-element manager, which keeps its
 * records in a segment of its own: elem_show, elem_move, elem_set_rect,
 * elem_hide, the crops and elem_flush write nothing here and are not
 * modelled. elem_alloc does write here -- the handle -- so a drawer that
 * still has to allocate its elements is counted. */

/* panel_draw_flask(el), elements 0 and 1: the drawn level steps
 * one unit toward panel_values[el]; at the target the dirty bit is cleared
 * unless the bubble flag is up, and a bubble advances its art from the
 * second id of the flask's triple to the third, then resets and drops the
 * flag. The vitality flask's triple follows the poison bits (record +0x5f &
 * 0x3c), and a change of triple slides the bubble art by 0x32 after
 * panel_flask_fill has redrawn the flask at its value. */
static void panel_draw_flask(uw_motion *m, int el) {
    uint8_t *ds = m->ds;
    uint16_t base, second, third, lvl, x, fill = (uint16_t)(0x08cf + el * 2), body = (uint16_t)(FLASK_BODY_ELEM + el * 2);
    int16_t diff;
    if (el == 0) {
        int poisoned = (ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x5f)] & 0x3c) != 0;
        base = poisoned ? 0x203e : 0x200c;
        second = poisoned ? 0x204b : 0x2019;
        third = poisoned ? 0x2056 : 0x2024;
        if (poisoned ? (int16_t)rw(ds, FLASK_ART) > 0x2018 && (int16_t)rw(ds, FLASK_ART) < 0x2025
                     : (int16_t)rw(ds, FLASK_ART) > 0x204a && (int16_t)rw(ds, FLASK_ART) < 0x2057) {
            /* panel_flask_fill(0): the empty flask drawn, then
             * each level up to the value shown and flushed in turn -- the
             * empty one first, or the levels between the value and what was
             * shown keep the colour they had */
            panel_flask_fill(m, 0);
            ww(ds, FLASK_ART, (uint16_t)(rw(ds, FLASK_ART) + (poisoned ? 0x32 : -0x32)));
        }
    } else if (el == 1) {
        base = 0x2025; second = 0x2032; third = 0x203d;
    } else {
        return;
    }
    x = rw(ds, (uint16_t)(0x079c + el * 2));
    if (!rw(ds, body)) {
        uint16_t h = elem_alloc(m, 0, 0, 0, 0);
        ww(ds, body, h);
        elem_set_rect(m, h, x, 0x4a, 0x18, 0x21);
        h = elem_alloc(m, 0, 0, 0, 0);
        ww(ds, (uint16_t)(0x090c + el * 2), h);
        elem_set_rect(m, h, 0, 0, 0x18, 4);
        h = elem_alloc(m, 0, 0, 0, 0);
        ww(ds, (uint16_t)(0x0914 + el * 2), h);
        elem_set_rect(m, h, x, 0x4a, 0x18, 4);
    }
    diff = (int16_t)(ds[(uint16_t)(PANEL_VALUES + el)] - ds[(uint16_t)(PANEL_FLASK_SHOWN + el)]);
    if (diff < 0) {
        uint16_t hgt;
        lvl = --ds[(uint16_t)(PANEL_FLASK_SHOWN + el)];
        hgt = rw(ds, (uint16_t)(0x07a2 + lvl * 2));
        elem_set_rect(m, rw(ds, body), x, (uint8_t)hgt, 0x18, (uint8_t)rw(ds, (uint16_t)(0x07be + lvl * 2)));
        elem_set_top_crop(m, rw(ds, body), (uint8_t)(0x4a - hgt));
        elem_show(m, rw(ds, body), 0x2057);
        if (lvl) {
            elem_move(m, rw(ds, fill), x, (uint8_t)rw(ds, (uint16_t)(0x07a0 + lvl * 2)));
            elem_show(m, rw(ds, fill), (uint16_t)(base + lvl - 1));
        }
    } else if (diff > 0) {
        lvl = ++ds[(uint16_t)(PANEL_FLASK_SHOWN + el)];
        elem_move(m, rw(ds, fill), x, (uint8_t)rw(ds, (uint16_t)(0x07a0 + lvl * 2)));
        elem_show(m, rw(ds, fill), (uint16_t)(base + lvl - 1));
    } else {
        if (!ds[(uint16_t)(FLASK_BUBBLE + el)]) ww(ds, PANEL_DIRTY, (uint16_t)(rw(ds, PANEL_DIRTY) & ~(1u << el)));
        lvl = ds[(uint16_t)(PANEL_FLASK_SHOWN + el)];
    }
    if (ds[(uint16_t)(FLASK_BUBBLE + el)] == 1 && lvl < 8 && lvl != 0) {
        if (rw(ds, (uint16_t)(FLASK_ART + el * 2)) == third) {
            ww(ds, (uint16_t)(FLASK_ART + el * 2), second);
            elem_show(m, rw(ds, fill), (uint16_t)(base + lvl - 1));
            ds[(uint16_t)(FLASK_BUBBLE + el)] = 0;
        } else {
            uint16_t cap = rw(ds, (uint16_t)(0x0914 + el * 2)), y = rw(ds, (uint16_t)(0x07a0 + lvl * 2));
            elem_move(m, rw(ds, (uint16_t)(0x090c + el * 2)), x, (uint8_t)y);
            ww(ds, (uint16_t)(FLASK_ART + el * 2), (uint16_t)(rw(ds, (uint16_t)(FLASK_ART + el * 2)) + 1));
            elem_show(m, rw(ds, (uint16_t)(0x090c + el * 2)), rw(ds, (uint16_t)(FLASK_ART + el * 2)));
            elem_set_top_crop(m, cap, (uint8_t)(0x4a - y));
            elem_move(m, cap, x, (uint8_t)y);
            elem_show_shaded(m, cap, 0x2058);
        }
    }
}

/* panel_dial_step, element 2, the compass: the drawn position
 * steps one place toward panel_compass_value the shorter way round sixteen;
 * at it, dirty bit 2 is cleared. */
static void panel_dial_step(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t cur = ds[PANEL_PREV_VALUES];
    int16_t diff = (int16_t)(ds[PANEL_VALUES + 2] - cur);
    if (diff == 0) {
        ww(ds, PANEL_DIRTY, (uint16_t)(rw(ds, PANEL_DIRTY) & 0xfffb));
        return;
    }
    if (diff < 0) diff = (int16_t)(diff + 0x10);
    cur = (uint16_t)((diff < 9 ? cur + 1 : cur - 1) & 0xf);
    elem_show(m, rw(ds, 0x08d3), (uint16_t)((cur & 3) + 0x2059));
    elem_move(m, rw(ds, 0x08d5), rw(ds, (uint16_t)(0x07dc + cur * 2)), (uint8_t)rw(ds, (uint16_t)(0x07fc + cur * 2)));
    elem_show(m, rw(ds, 0x08d5), (uint16_t)(cur + 0x205d));
    ds[PANEL_PREV_VALUES] = (uint8_t)cur;
}

/* panel_draw_element3: values 0..13; 9 animates frames 9..13
 * and keeps dirty bit 3, any other is drawn once and clears it (a 9 left
 * behind restarting the animation at 9). */
static void panel_draw_element3(uw_motion *m) {
    uint8_t *ds = m->ds;
    int8_t v = (int8_t)ds[PANEL_VALUES + 3];
    if (v < 0 || v >= 0xe) return;
    if (!rw(ds, ELEMENT3_ELEM)) {
        ww(ds, ELEMENT3_ELEM, elem_alloc(m, 0, 0, 0, 0));
        elem_set_rect(m, rw(ds, ELEMENT3_ELEM), 4, 0x3c, 1, 1);
    }
    if (v == 9) {
        uint16_t art = (uint16_t)(0x2098 + rw(ds, ELEMENT3_FRAME));
        ww(ds, ELEMENT3_FRAME, (uint16_t)(rw(ds, ELEMENT3_FRAME) + 1));
        elem_show(m, rw(ds, ELEMENT3_ELEM), art);
        if ((int16_t)rw(ds, ELEMENT3_FRAME) > 0xd) ww(ds, ELEMENT3_FRAME, 9);
        ww(ds, PANEL_DIRTY, (uint16_t)(rw(ds, PANEL_DIRTY) | 8));
    } else {
        if (rw(ds, ELEMENT3_LAST) == 9) ww(ds, ELEMENT3_FRAME, 9);
        elem_show(m, rw(ds, ELEMENT3_ELEM), (uint16_t)(0x2098 + v));
        ww(ds, PANEL_DIRTY, (uint16_t)(rw(ds, PANEL_DIRTY) & 0xfff7));
    }
    ww(ds, ELEMENT3_LAST, (uint16_t)(int16_t)v);
}

/* dragon_animate(el), elements 4 and 5, from the instructions:
 * a pending flick plays seven frames (the counter shared by both dragons);
 * an idle dragon with a value latches it as its state; then state 1 plays
 * its frames once more for every repeat left, 2 enters, loops and backs out,
 * 3 rears, holds six redraws and settles back to the still image. A phase
 * short of the end keeps the dirty bit. */
static void dragon_animate(uw_motion *m, int el) {
    uint8_t *ds = m->ds;
    int n = el - 4, state;
    uint16_t ph = (uint16_t)(DRAGON_PHASE + n * 2), fr = (uint16_t)(DRAGON_FRAME + n * 2), rep = (uint16_t)(DRAGON_REPEAT + n * 2);
    uint16_t de = (uint16_t)(DRAGON_ELEM + n * 2), rect;
    int16_t first, last;
    if (el != 4 && el != 5) return;
    if (!rw(ds, de)) ww(ds, de, elem_alloc(m, 3, 0x28, 0x18, 1));
    if (ds[(uint16_t)(DRAGON_FLICK_PENDING + n)] == 1) {
        uint16_t k = rw(ds, DRAGON_FLICK_FRAME);
        ww(ds, DRAGON_FLICK_FRAME, (uint16_t)(k + 1));
        elem_show(m, rw(ds, (uint16_t)(0x08df + n * 2)), rw(ds, (uint16_t)(0x0858 + n * 0xe + k * 2)));
        if ((int16_t)rw(ds, DRAGON_FLICK_FRAME) >= 7) {
            ww(ds, DRAGON_FLICK_FRAME, 0);
            ds[(uint16_t)(DRAGON_FLICK_PENDING + n)] = 0;
        }
    }
    if (!rw(ds, ph) && ds[(uint16_t)(PANEL_VALUES + el)]) {
        ds[(uint16_t)(PANEL_FLASK_SHOWN + el)] = ds[(uint16_t)(PANEL_VALUES + el)];
        ww(ds, ph, 1);
    }
    state = ds[(uint16_t)(PANEL_FLASK_SHOWN + el)];
    if (state > 3) return;
    if (state == 0) {
        if (!ds[(uint16_t)(DRAGON_FLICK_PENDING + n)])
            ww(ds, PANEL_DIRTY, (uint16_t)(rw(ds, PANEL_DIRTY) & ~(1u << el)));
        return;
    }
    rect = (uint16_t)((n * 6) + (state - 1) * 2);
    first = (int16_t)rw(ds, (uint16_t)(DRAGON_FIRST + (n * 3 + state - 1) * 2));
    last = (int16_t)rw(ds, (uint16_t)(DRAGON_LAST + (n * 3 + state - 1) * 2));
#define FRAME() ((int16_t)rw(ds, fr))
#define SET_FRAME(v) ww(ds, fr, (uint16_t)(v))
#define SHOW_POST(d) do { uint16_t f_ = rw(ds, fr); SET_FRAME(f_ + (d)); elem_show(m, rw(ds, de), f_); } while (0)
#define SET_RECT() elem_set_rect(m, rw(ds, de), rw(ds, (uint16_t)(0x0824 + rect)), (uint8_t)rw(ds, (uint16_t)(0x0830 + rect)), \
                                 rw(ds, (uint16_t)(0x083c + rect)), (uint8_t)rw(ds, (uint16_t)(0x0848 + rect)))
#define DONE() do { ds[(uint16_t)(PANEL_FLASK_SHOWN + el)] = 0; ww(ds, ph, 0); } while (0)
    switch (state) {
    case 1:
        switch (rw(ds, ph)) {
        case 1:
            ds[(uint16_t)(PANEL_VALUES + el)] = 0;
            SET_RECT();
            SET_FRAME(first);
            ww(ds, rep, 1);
            ww(ds, ph, 3);
            /* FALLTHROUGH */ /* to phase 3 */
        case 3:
            SHOW_POST(1);
            if (last < FRAME()) {
                SET_FRAME(first);
                ww(ds, rep, (uint16_t)(rw(ds, rep) - 1));
            }
            if (ds[(uint16_t)(PANEL_VALUES + el)] || !rw(ds, rep)) ww(ds, ph, 4);
            break;
        case 4:
            SHOW_POST(1);
            if (last < FRAME()) ww(ds, ph, 5);
            break;
        case 5:
            elem_hide(m, rw(ds, de));
            DONE();
            break;
        default:
            break;
        }
        break;
    case 2:
        switch (rw(ds, ph)) {
        case 1:
            ds[(uint16_t)(PANEL_VALUES + el)] = 0;
            SET_RECT();
            SET_FRAME(first);
            ww(ds, rep, 3);
            ww(ds, ph, 2);
            /* FALLTHROUGH */ /* to phase 2 */
        case 2:
            SHOW_POST(1);
            if (first + 1 < FRAME()) ww(ds, ph, 3);
            break;
        case 3:
            SHOW_POST(1);
            if (last < FRAME()) {
                SET_FRAME(first + 2);
                ww(ds, rep, (uint16_t)(rw(ds, rep) - 1));
            }
            if (ds[(uint16_t)(PANEL_VALUES + el)] || !rw(ds, rep)) ww(ds, ph, 4);
            break;
        case 4:
            SHOW_POST(-1);
            if (first > FRAME()) ww(ds, ph, 5);
            break;
        case 5:
            elem_hide(m, rw(ds, de));
            DONE();
            break;
        default:
            break;
        }
        break;
    default:                                                            /* 3 */
        switch (rw(ds, ph)) {
        case 1:
            ds[(uint16_t)(PANEL_VALUES + el)] = 0;
            elem_hide(m, rw(ds, (uint16_t)(DRAGON_STILL_ELEM + n * 2)));
            SET_RECT();
            SET_FRAME(first);
            ww(ds, rep, 6);
            ww(ds, ph, 2);
            /* FALLTHROUGH */ /* to phase 2 */
        case 2:
            SHOW_POST(1);
            if (last < FRAME()) {
                SET_FRAME(FRAME() - 1);
                ww(ds, ph, 3);
            }
            break;
        case 3:
            if (ds[(uint16_t)(PANEL_VALUES + el)]) {
                ww(ds, ph, 4);
            } else {
                ww(ds, rep, (uint16_t)(rw(ds, rep) - 1));
                if (!rw(ds, rep)) ww(ds, ph, 4);
            }
            break;
        case 4:
            SET_FRAME(FRAME() - 1);
            elem_show(m, rw(ds, de), rw(ds, fr));
            if (FRAME() == first) ww(ds, ph, 5);
            break;
        case 5:
            elem_hide(m, rw(ds, de));
            elem_show(m, rw(ds, (uint16_t)(DRAGON_STILL_ELEM + n * 2)), rw(ds, (uint16_t)(0x0878 + n * 2)));
            DONE();
            break;
        default:
            break;
        }
        break;
    }
#undef FRAME
#undef SET_FRAME
#undef SHOW_POST
#undef SET_RECT
#undef DONE
}

/* panel_view_switch, element 6: while the panel shown is not
 * the one wanted, panel_view_switch_begin once -- with the target and the
 * panel's rectangle -- and panel_mode 4 while the card turns, then
 * panel_view_switch_step each redraw, and the view is committed when the
 * step says it is over (src/uw_motion_panelview.c). */
static void panel_view_switch(uw_motion *m) {
    uint8_t *ds = m->ds;
    if (ds[PANEL_MODE_SHOWN] == ds[PANEL_MODE_WANTED]) return;
    if (!ds[VIEW_SWITCHING]) {
        ds[VIEW_SWITCHING] = 1;
        panel_view_switch_begin(m, ds[PANEL_MODE_WANTED]);
        ds[PANEL_MODE] = 4;
    }
    if (!panel_view_switch_step(m)) return;
    ds[PANEL_MODE_SHOWN] = ds[PANEL_MODE_WANTED];
    ds[PANEL_MODE] = ds[PANEL_MODE_WANTED];
    ds[VIEW_SWITCHING] = 0;
    ww(ds, PANEL_DIRTY, (uint16_t)(rw(ds, PANEL_DIRTY) & 0xffbf));
}

/* panel_draw_element7: a five-step animation of the value,
 * holding up to sixteen redraws at step 3 (the hold count a byte), then
 * back to rest with dirty bit 7 cleared. */
static void panel_draw_element7(uw_motion *m) {
    uint8_t *ds = m->ds;
    if (ds[ELEMENT7_SHOWN] == ds[PANEL_VALUES + 7]) {
        ds[PANEL_VALUES + 7] = (uint8_t)(ds[PANEL_VALUES + 7] + 4);
        ww(ds, ELEMENT7_STEP, 2);
        ds[ELEMENT7_HOLD] = 0;
    } else if ((uint16_t)ds[ELEMENT7_SHOWN] != (uint16_t)(ds[PANEL_VALUES + 7] - 4)) {
        if (ds[ELEMENT7_HOLD]) {
            ww(ds, ELEMENT7_STEP, 2);
            ds[ELEMENT7_HOLD] = 0;
        }
        ds[ELEMENT7_SHOWN] = ds[PANEL_VALUES + 7];
        ds[PANEL_VALUES + 7] = (uint8_t)(ds[PANEL_VALUES + 7] + 4);
    }
    if (rw(ds, ELEMENT7_STEP) == 3 && ds[ELEMENT7_HOLD] < 0x10) {
        ds[ELEMENT7_HOLD]++;
    } else {
        uint16_t step = rw(ds, ELEMENT7_STEP);
        ww(ds, ELEMENT7_STEP, (uint16_t)(step + 1));
        elem_show(m, rw(ds, ELEMENT7_ELEM), (uint16_t)(rw(ds, (uint16_t)(0x08a9 + step * 2)) + (ds[ELEMENT7_SHOWN] - 1) * 3));
    }
    if ((int16_t)rw(ds, ELEMENT7_STEP) > 5) {
        ds[ELEMENT7_HOLD] = 0;
        ww(ds, ELEMENT7_STEP, 0);
        ds[ELEMENT7_SHOWN] = 0;
        elem_show(m, rw(ds, ELEMENT7_ELEM), 0x20a6);
        ww(ds, PANEL_DIRTY, (uint16_t)(rw(ds, PANEL_DIRTY) & 0xff7f));
    }
}

/* weapons_load_anim: the weapon wanted made the one
 * loaded, and for one of the four kinds its 28 WEAPONS.GR frames --
 * the hand's half of the file by the record's +0x64 bit 0, first, then
 * 0x1c a kind -- loaded through panel_ems_alloc, a bump cursor from 0x8000
 * of the EMS page frame (weapon_anim_buffer_ptr, 08a4), and
 * weapons_record_frame_offset, which keeps each frame's start as the
 * running sum of the entries' lengths (weapon_frame_offsets, 35ec, reset
 * at the first); then WEAPONS.DAT's two 0x1c-byte halves at the same
 * record, the frames' x at 35aa and y at 35c6. The frames' pixels the port
 * takes from WEAPONS.GR as weapon_composite draws them. 1 when it loaded
 * (or had nothing to load). */
static int weapons_load_anim(uw_motion *m) {
    uint8_t *ds = m->ds;
    int8_t want = (int8_t)ds[WEAPON_ART_WANTED];
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), off = 0x8000;
    size_t first, count, at, k;
    int ok = 1;
    if (ds[WEAPON_ART_LOADED] == ds[WEAPON_ART_WANTED]) return 1;
    ds[WEAPON_ART_LOADED] = ds[WEAPON_ART_WANTED];
    if (want < 0 || want >= 4) return 0;
    if (!m->weapons_gr || !m->weapons_dat) {
        UW_NOT_CARRIED(m->not_carried);
        return 0;
    }
    ww(ds, 0x08a4, 0);
    ww(ds, 0x08a6, 0);
    first = (size_t)(1 - (ds[(uint16_t)(rec + 0x64)] & 1)) * 0x70 + (size_t)want * 0x1c;
    count = m->weapons_gr_size >= 3 ? (size_t)(m->weapons_gr[1] | m->weapons_gr[2] << 8) : 0;
    for (k = 0; k < 0x1c && first + k < count && 3 + (first + k + 2) * 4 <= m->weapons_gr_size; k++) {
        const uint8_t *o = m->weapons_gr + 3 + (first + k) * 4;
        uint32_t a = (uint32_t)(o[0] | o[1] << 8 | o[2] << 16 | (uint32_t)o[3] << 24);
        uint32_t b = (uint32_t)(o[4] | o[5] << 8 | o[6] << 16 | (uint32_t)o[7] << 24);
        uint16_t len = (uint16_t)(b - a);
        if (k == 0) {
            ww(ds, 0x08a4, 0x8000);         /* panel_ems_alloc's first call: page 2 at 0x8000 */
            ww(ds, 0x08a6, rw(ds, 0x24ba)); /* ems_page_frame_seg */
            ww(ds, 0x35ec, 0);
        }
        off = (uint16_t)(off + len);
        ww(ds, 0x08a4, off);
        ww(ds, (uint16_t)(0x35ee + k * 2), (uint16_t)(rw(ds, (uint16_t)(0x35ec + k * 2)) + len));
    }
    if (k < 0x1c) ok = 0;
    /* gr_open's entry count, which gr_close leaves (the offset table's
     * heap pointer beside it at 573a is the allocator's) */
    ww(ds, 0x573c, (uint16_t)count);
    at = (size_t)(1 - (ds[(uint16_t)(rec + 0x64)] & 1)) * 0xe0 + (size_t)want * 0x38;
    if (at + 0x38 <= m->weapons_dat_size) {
        memcpy(ds + 0x35aa, m->weapons_dat + at, 0x1c);
        memcpy(ds + 0x35c6, m->weapons_dat + at + 0x1c, 0x1c);
    } else ok = 0;
    return ok;
}

/* weapon_anim_step, element 8, from the instructions: the
 * first-person weapon's state machine, post_event(2) first. The wanted state
 * (capped at 6) is reconciled with the current one, then the state steps:
 * raising (5) and lowering (3) count the frame to rest (6) and ready (4);
 * rest and ready park at frame -1 and clear dirty bit 8, or with new art due
 * load it (weapons_load_anim) or go to rest first; a swing counts
 * frames 0..9 -- 0 clears weapon_anim_struck, 3 sets it and clears the bit,
 * 9 returns to ready. */
static void weapon_anim_step(uw_motion *m) {
    uint8_t *ds = m->ds;
    ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));      /* post_event(2) */
    if (ds[WEAPON_WANTED_STATE] > 6) ds[WEAPON_WANTED_STATE] = 6;
    if (ds[WEAPON_WANTED_STATE] == 6) {
        if (ds[WEAPON_STATE] != 6 && ds[WEAPON_STATE] != 5) {
            ds[WEAPON_STATE] = 5;
            ww(ds, WEAPON_FRAME, 0xffff);
        }
    } else if (ds[WEAPON_WANTED_STATE] == 4) {
        if (ds[WEAPON_STATE] == 6 || ds[WEAPON_STATE] == 5) {
            ds[WEAPON_STATE] = 3;
            ww(ds, WEAPON_FRAME, 3);
        } else if (ds[WEAPON_STRUCK]) {
            ds[WEAPON_STATE] = 4;
            ww(ds, WEAPON_FRAME, 0xffff);
        } else if (ds[WEAPON_STATE] <= 2) {
            ww(ds, WEAPON_FRAME, (uint16_t)(rw(ds, WEAPON_FRAME) - 2));
            if ((int16_t)rw(ds, WEAPON_FRAME) < -1) ds[WEAPON_STATE] = 4;
        } else if (ds[WEAPON_STATE] == 4 && ds[WEAPON_ART_WANTED] == ds[WEAPON_ART_LOADED]) {
            ww(ds, PANEL_DIRTY_1, (uint16_t)(rw(ds, PANEL_DIRTY_1) & 0xfeff));
            return;
        }
    } else {
        ds[WEAPON_STATE] = ds[WEAPON_WANTED_STATE];
    }
    switch (ds[WEAPON_STATE]) {
    case 3:
        ww(ds, WEAPON_FRAME, (uint16_t)(rw(ds, WEAPON_FRAME) - 1));
        if ((int16_t)rw(ds, WEAPON_FRAME) < 0) ds[WEAPON_STATE] = 4;
        break;
    case 4:
        if (ds[WEAPON_ART_WANTED] != ds[WEAPON_ART_LOADED]) {
            ds[WEAPON_WANTED_STATE] = 6;
            ds[WEAPON_RESUME] = 4;
        } else {
            ww(ds, WEAPON_FRAME, 0xffff);
            ww(ds, PANEL_DIRTY_1, (uint16_t)(rw(ds, PANEL_DIRTY_1) & 0xfeff));
        }
        break;
    case 5:
        ww(ds, WEAPON_FRAME, (uint16_t)(rw(ds, WEAPON_FRAME) + 1));
        if ((int16_t)rw(ds, WEAPON_FRAME) > 2) ds[WEAPON_STATE] = 6;
        break;
    case 6:
        if (ds[WEAPON_ART_WANTED] != ds[WEAPON_ART_LOADED]) {
            weapons_load_anim(m);               /* its answer unread */
            ds[WEAPON_WANTED_STATE] = ds[WEAPON_RESUME];
            ds[WEAPON_RESUME] = 6;
        } else {
            ww(ds, WEAPON_FRAME, 0xffff);
            ww(ds, PANEL_DIRTY_1, (uint16_t)(rw(ds, PANEL_DIRTY_1) & 0xfeff));
        }
        break;
    default: {
        uint16_t f = (uint16_t)(rw(ds, WEAPON_FRAME) + 1);
        ww(ds, WEAPON_FRAME, f);
        if (f == 0) {
            ds[WEAPON_STRUCK] = 0;
        } else if (f == 3) {
            ww(ds, PANEL_DIRTY_1, (uint16_t)(rw(ds, PANEL_DIRTY_1) & 0xfeff));
            ds[WEAPON_STRUCK] = 1;
        } else if (f == 9) {
            ds[WEAPON_STATE] = 4;
            ds[WEAPON_WANTED_STATE] = 4;
            ww(ds, WEAPON_FRAME, 0xffff);
            ww(ds, PANEL_DIRTY_1, (uint16_t)(rw(ds, PANEL_DIRTY_1) & 0xfeff));
        }
        break;
    }
    }
}

static void panel_draw(uw_motion *m, int el) {
    switch (el) {
    case 0: case 1: panel_draw_flask(m, el); break;
    case 2: panel_dial_step(m); break;
    case 3: panel_draw_element3(m); break;
    case 4: case 5: dragon_animate(m, el); break;
    case 6: panel_view_switch(m); break;
    case 7: panel_draw_element7(m); break;
    default: weapon_anim_step(m); break;
    }
}

void uw_motion_panels_redraw(uw_motion *m, uint32_t clock, uint16_t stop) {
    uint8_t *ds = m->ds, now = (uint8_t)clock;
    int drawn = 0, el;
    if (rw(ds, PANEL_DIRTY_2)) {
        for (el = 0; el < 9; el++)
            if (rw(ds, PANEL_DIRTY_2) & (1u << el)) {
                panel_draw(m, el);
                ww(ds, PANEL_DIRTY_2, (uint16_t)(rw(ds, PANEL_DIRTY_2) & ~(1u << el)));
                drawn = 1;
            }
    }
    if ((now >> 5) != (ds[PANEL_LAST_CLOCK] >> 5)) {
        for (el = 0; el < 9; el++)
            if (rw(ds, PANEL_DIRTY_1) & (1u << el)) panel_draw(m, el);
        drawn = 1;
    }
    if ((now >> 6) != (ds[PANEL_LAST_CLOCK] >> 6)) {
        int k;
        for (k = 0; k < 2; k++) {
            /* Flask bubbles, then the dragons' flicks -- whose dirty bit is
             * (r + 4) & 1, the instructions' `add cl,4; and cl,1`: element 0
             * or 1 marked, the dragon's own flag set for its next redraw. */
            int16_t r = (int16_t)rt_rand(m);
            uint16_t flag = (uint16_t)((k ? DRAGON_FLICK_PENDING : FLASK_BUBBLE) + (r & 1));
            if (r < 0x666 && !ds[flag]) {
                ds[flag] = 1;
                ww(ds, PANEL_DIRTY, (uint16_t)(rw(ds, PANEL_DIRTY) | (1u << ((k ? r + 4 : r) & 1))));
            }
        }
        for (el = 0; el < 9; el++)
            if (rw(ds, PANEL_DIRTY) & (1u << el)) panel_draw(m, el);
        drawn = 1;
    }
    if (stop && stop != 0x06e2) UW_NOT_CARRIED(m->not_carried);
    if (drawn && !stop) {
        elem_flush(m);
        ds[PANEL_LAST_CLOCK] = now;
    }
}

/* panel_set_value, from the instructions: a status panel element's
 * value and its dirty bit. The flasks store twelfths of their maximum; the
 * compass only a change; the power bar's dirty bit is its own word unless it
 * is 9; a dragon takes whichever of elements 4 and 5 is free (a coin toss when
 * both are); the inventory element waits out panel mode 4; the weapon element
 * waits for its animation to load. */
void panel_set_value(uw_motion *m, int8_t e, uint16_t v) {
    uint8_t *ds = m->ds;
    uint16_t max;
    switch (e) {
    case 0: case 1:
        max = e == 1 ? ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x38)] : ds[(uint16_t)(rw(ds, CRITTER_ROW_PTR) + 4)];
        ds[(uint16_t)(PANEL_VALUES + e)] = max ? (uint8_t)((int16_t)(uint16_t)(v * 12) / (int16_t)max) : 0;
        if (ds[(uint16_t)(PANEL_VALUES + e)] >= 12) ds[(uint16_t)(PANEL_VALUES + e)] = 12;
        ww(ds, PANEL_DIRTY, (uint16_t)(rw(ds, PANEL_DIRTY) | (1 << e)));
        return;
    case 2:
        if (ds[(uint16_t)(PANEL_PREV + 2)] == v) return;
        ww(ds, PANEL_DIRTY, (uint16_t)(rw(ds, PANEL_DIRTY) | 4));
        ds[(uint16_t)(PANEL_VALUES + 2)] = (uint8_t)v;
        return;
    case 3:
        if (v != 9) {
            ww(ds, PANEL_DIRTY_2, (uint16_t)(rw(ds, PANEL_DIRTY_2) | 8));
            ds[(uint16_t)(PANEL_VALUES + 3)] = (uint8_t)v;
            return;
        }
        break;
    case 4: {
        uint8_t v4 = ds[(uint16_t)(PANEL_VALUES + 4)], v5 = ds[(uint16_t)(PANEL_VALUES + 5)];
        uint8_t p2 = ds[(uint16_t)(PANEL_PREV + 4)], p3 = ds[(uint16_t)(PANEL_PREV + 5)];
        if (v4 == v || v5 == v || p2 == v || p3 == v) return;
        if (!p2 && !p3) {
            e = (int8_t)(e + (rt_rand(m) & 1));
        } else if (p2) {
            if (!p3) e++;
            else if (!v4 && !v5) e = (int8_t)(e + (rt_rand(m) & 1));
            else if (v4 && !v5) e++;
        }
        ds[(uint16_t)(PANEL_VALUES + e)] = (uint8_t)v;
        ww(ds, PANEL_DIRTY, (uint16_t)(rw(ds, PANEL_DIRTY) | (1 << e)));
        return;
    }
    case 6:
        if (ds[PANELS_MODE] == 4) return;
        break;
    case 8:
        if (ds[WEAPON_ANIM_WANTED] != ds[WEAPON_ANIM_LOADED] && ds[(uint16_t)(PANEL_VALUES + 8)] == 6) return;
        ww(ds, PANEL_DIRTY_1, (uint16_t)(rw(ds, PANEL_DIRTY_1) | 0x100));
        ds[(uint16_t)(PANEL_VALUES + 8)] = (uint8_t)v;
        return;
    default:
        break;
    }
    ww(ds, PANEL_DIRTY, (uint16_t)(rw(ds, PANEL_DIRTY) | (1 << (e & 0x1f))));
    ds[(uint16_t)(PANEL_VALUES + e)] = (uint8_t)v;
}

/* palette_rotate_range: `count` RGB triples from `start` of the
 * working palette turned one place -- up with `dir` set, down without -- the
 * wrapped triple kept in a data-segment buffer. */
static void palette_rotate_range(uw_motion *m, uint8_t start, uint8_t count, int dir);
void uw_motion_palette_rotate_range(uw_motion *m, uint8_t start, uint8_t count, int dir) {
    palette_rotate_range(m, start, count, dir);
}
static void palette_rotate_range(uw_motion *m, uint8_t start, uint8_t count, int dir) {
    uint8_t *ds = m->ds, *p = m->palette;
    int16_t step = (int16_t)(dir ? 3 : -3), at = (int16_t)(start * 3);
    int c, k;
    if (!p) {
        UW_NOT_CARRIED(m->not_carried);
        return;
    }
    if (dir) at = (int16_t)(at + (count - 1) * 3);
    ds[PALETTE_SAVED] = p[at];
    ds[PALETTE_SAVED + 1] = p[at + 1];
    ds[PALETTE_SAVED + 2] = p[at + 2];
    for (c = 0; c < count - 1; c++) {
        for (k = 0; k < 3; k++) p[at + k] = p[at - step + k];
        at = (int16_t)(at - step);
    }
    p[at] = ds[PALETTE_SAVED];
    p[at + 1] = ds[PALETTE_SAVED + 1];
    p[at + 2] = ds[PALETTE_SAVED + 2];
}

/* palette_cycle_liquids: on a change of the clock byte's top three
 * bits, and of their top two, the water and fire ranges turn and go to the
 * VGA (vga_set_palette writes the hardware only). */
void palette_cycle(uw_motion *m, uint8_t clock) {
    uint8_t *ds = m->ds;
    if ((clock >> 5) == ds[PALETTE_PHASE]) return;
    ds[PALETTE_PHASE] = (uint8_t)(clock >> 5);
    if ((ds[PALETTE_PHASE] >> 1) == ds[PALETTE_PHASE_HALF]) return;
    palette_rotate_range(m, 0x30, 4, 0);
    palette_rotate_range(m, 0x34, 4, 0);
    palette_rotate_range(m, 0x38, 4, 0);
    palette_rotate_range(m, 0x3c, 4, 0);
    palette_rotate_range(m, 0x10, 5, 1);
    palette_rotate_range(m, 0x15, 3, 1);
    ds[PALETTE_PHASE_HALF] = (uint8_t)(ds[PALETTE_PHASE] >> 1);
}

/* panel_set_mode_icon: the mode icon, element 6, shown (2) when
 * the inventory is the panel, hidden (0) otherwise, and left alone in panel
 * mode 4. */
void panel_set_mode_icon(uw_motion *m) {
    uint8_t mode = m->ds[PANEL_MODE];
    if (mode == 4) return;
    panel_set_value(m, 6, mode == 0 ? 2 : 0);
}

/* A number as itoa writes it, base 10. */
void itoa10(int v, char *out) {
    char digits[8];
    int n = 0, k = 0;
    unsigned u = (unsigned)(v < 0 ? -v : v);
    do {
        digits[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u && n < 7);
    if (v < 0) out[k++] = '-';
    while (n) out[k++] = digits[--n];
    out[k] = '\0';
}

/* print_flask_status, the click on the two flasks: the
 * event's position relative to the hotspot at +0 and +2. Between the flasks
 * -- x 0x1a..0x27 -- a click below y 0xe does nothing and above it shows the
 * mode icon (panel_set_mode_icon). On a flask below y 0x1f: message 0x59
 * ("Your current vitality is ") with the tracked object's hit points and
 * the critter row's maximum left of x 0x1e -- the poison message first when
 * the record's +0x5f bits 2..5 are set, "You are ", the (level - 1) / 3rd of
 * "barely", "mildly", "badly", " poisoned." -- or 0x5a with the record's mana
 * and maximum right of it; " out of " and "\n" between and after, one print,
 * then input_wait_button_release(1). */
void print_flask_status(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t ev = rw(ds, 0x00e2), rec = rw(ds, PLAYER_RECORD_PTR);
    int16_t x = rs(ds, ev), y = rs(ds, (uint16_t)(ev + 2));
    char text[0x100], a[10], b[10];
    uint8_t poison;
    if (x > 0x19 && x < 0x28) {
        if (y > 0xd) panel_set_mode_icon(m);
        return;
    }
    if (y > 0x1e) return;
    if (!m->strings || uw_strings_by_id(m->strings, (uint16_t)(((x > 0x1e) + 0x59) | 0x200), text, 0x78) < 0) text[0] = '\0';
    if (x < 0x1e) {
        itoa10(ls[(uint16_t)(rw(ds, TRACKED_OBJECT) + 8)], a);
        itoa10(ds[(uint16_t)(rw(ds, CRITTER_ROW_PTR) + 4)], b);
        poison = (uint8_t)((ds[(uint16_t)(rec + 0x5f)] >> 2) & 0xf);
        if (poison) print_message_parts(m, 0x5b, (int16_t)((poison - 1) / 3 + 0x54), 0x5c);
    } else {
        itoa10(ds[(uint16_t)(rec + 0x37)], a);
        itoa10(ds[(uint16_t)(rec + 0x38)], b);
    }
    strcat(text, a);
    strcat(text, " out of ");
    strcat(text, b);
    strcat(text, "\n");
    scroll_print(m, text);
    input_wait_button_release(m, 1);
}

/* print_time_status, the click on the compass: "\n", message
 * 0x40 with the hunger's thirtieth over 0x68 and 0x67, 0x76 less the fatigue's
 * 23rd (at most 5), ".\n", 0x41 with the level's name (0x19a on) and 0x42;
 * the days played -- the record's 32-bit clock over 0x1c2000 -- as 0x43 with
 * its twelfth over 0x19b and 0x44 below 101 twelfths, 0x45 past; then 0x46
 * with the remainder's month over 0x47 and 0x53, and
 * input_wait_button_release(1). */
void print_time_status(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    uint32_t clock = (uint32_t)rw(ds, (uint16_t)(rec + 0xce)) | ((uint32_t)rw(ds, (uint16_t)(rec + 0xd0)) << 16);
    int16_t q = (int16_t)(clock / 0x1c2000u);
    uint16_t fatigue = (uint16_t)(ds[(uint16_t)(rec + 0x3a)] / 0x17);
    scroll_print(m, "\n");
    print_message_parts(m, 0x40, (int16_t)(ds[(uint16_t)(rec + 0x39)] / 0x1e + 0x68), 0x67);
    if (fatigue > 5) fatigue = 5;
    print_message(m, (uint16_t)(0x76 - fatigue));
    scroll_print(m, ".\n");
    print_message_parts(m, 0x41, (int16_t)(rs(ds, CURRENT_LEVEL_WORD) + 0x19a), 0x42);
    if (q / 0xc < 0x65) print_message_parts(m, 0x43, (int16_t)(q / 0xc + 0x19b), 0x44);
    else print_message(m, 0x45);
    print_message_parts(m, 0x46, (int16_t)(q % 0xc + 0x47), 0x53);
    input_wait_button_release(m, 1);
}

/* panel_button_draw_up and _down(mode): action button
 * `mode` drawn unpressed or pressed -- art 0x200a or 0x200b less 2 a mode
 * past the first -- at panel_button_x and _y, under a
 * hidden cursor. */
void panel_button_draw(uw_motion *m, int16_t mode, int down) {
    uint8_t *ds = m->ds;
    cursor_hide(m);
    uw_motion_gr_draw_art(m, (uint16_t)(0x200a + (down ? 1 : 0) - 2 * (mode - 1)),
                          rs(ds, (uint16_t)(0x026a + (mode - 1) * 2)), rs(ds, (uint16_t)(0x0276 + (mode - 1) * 2)));
    cursor_show(m);
}

/* weapon_draw, from the instructions: unless the weapon is drawn
 * already (the record's +0x5f bit 1) or the Avatar is in water (+0xb8 bit
 * 0), a mode of 1, 3 or 4 pops its cursor, a mode's button is drawn up, and
 * fight becomes the mode -- the stance set, panel element 8 to 4, its button
 * drawn down -- with the combat theme (8) wanted unless a track 5..7 plays. */
static void weapon_draw(uw_motion *m) {
    enum { ACTION_MODE = 0x268c };
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    int16_t mode = rs(ds, ACTION_MODE);
    if ((ds[(uint16_t)(rec + 0x5f)] & 2) || (ds[(uint16_t)(rec + 0xb8)] & 1)) return;
    if (mode == 1 || mode == 3 || mode == 4) cursor_shape_pop(m, 3);
    if (rs(ds, ACTION_MODE)) panel_button_draw(m, rs(ds, ACTION_MODE), 0);
    ww(ds, ACTION_MODE, 2);
    ds[(uint16_t)(rec + 0x5f)] |= 2;
    panel_set_value(m, 8, 4);
    panel_button_draw(m, 2, 1);
    if (ds[MUSIC_TRACK_PLAYING] < 5 || ds[MUSIC_TRACK_PLAYING] > 7) ds[MUSIC_TRACK_WANTED] = 8;
}

/* weapon_stow, from the instructions: with the weapon drawn,
 * panel element 8 to 6, the stance cleared, the fight button drawn up unless
 * the options panel is open, no mode, combat_reset and
 * pick_level_theme_music. */
void weapon_stow(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    if (!(ds[(uint16_t)(rec + 0x5f)] & 2)) return;
    panel_set_value(m, 8, 6);
    ds[(uint16_t)(rec + 0x5f)] &= 0xfd;
    if (!rw(ds, 0x1a30)) panel_button_draw(m, 2, 0);
    ww(ds, 0x268c, 0);
    combat_reset(m);
    ds[MUSIC_TRACK_WANTED] = (uint8_t)(rt_rand(m) % 3 + 2);   /* pick_level_theme_music */
}

/* weapon_toggle: drawn, stowed; stowed, drawn. */
void weapon_toggle(uw_motion *m) {
    if (m->ds[(uint16_t)(rw(m->ds, PLAYER_RECORD_PTR) + 0x5f)] & 2) weapon_stow(m);
    else weapon_draw(m);
}

/* panel_button_click(button), the action buttons, from the
 * instructions -- nothing while action_state is set. Button -1 is
 * the click's: with the options panel open options_click_at_xy
 * (counted) and on with -1; otherwise the event's y + 2 over 18, none past 5.
 * Button 5 opens the options (options_menu_loop, counted). Any other: panel
 * element 8 to 6, the record's combat stance (+0x5f bit 1) cleared, the place
 * cursor popped for a mode of 1, 3 or 4; the button's mode (button + 1)
 * again clears action_mode and draws it up; another draws the old up
 * and becomes the mode, drawn down -- fight (2) only on dry land (+0xb8 bit
 * 0), setting the stance, element 8 to 4 and theme 8 unless a track 5..7
 * plays, and no mode while swimming. Out of the stance with track 8 playing,
 * pick_level_theme_music. Then input_wait_button_release(1) -- and after it,
 * `after` set, a mode of 1, 3 or 4 pushes the place cursor 0x1077. */
void panel_button_click(uw_motion *m, int16_t button, int after) {
    enum { ACTION_MODE = 0x268c, OPTIONS_ACTIVE = 0x1a30 };
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), ev = rw(ds, 0x00e2);
    int16_t mode = rs(ds, ACTION_MODE);
    if (after) {
        if (mode == 1 || mode == 3 || mode == 4) cursor_shape_push(m, 0x1077);
        return;
    }
    if (rw(ds, ACTION_STATE_WORD)) return;
    if (button == -1) {
        if (rw(ds, OPTIONS_ACTIVE)) {
            UW_NOT_CARRIED(m->not_carried);
        } else {
            button = (int16_t)((rs(ds, (uint16_t)(ev + 2)) + 2) / 0x12);
            if (button > 5) return;
        }
    }
    if (button == 5) {
        /* options_menu_loop(1): its opening awaits the button's
         * release; the panel is the shell's (src/uw_options.c), opened at
         * the wait's end */
        m->options_open = 1;
        input_wait_button_release(m, 1);
        return;
    }
    panel_set_value(m, 8, 6);
    ds[(uint16_t)(rec + 0x5f)] &= 0xfd;
    if (mode == 1 || mode == 3 || mode == 4) cursor_shape_pop(m, 3);
    button++;
    if (button == mode) {
        panel_button_draw(m, mode, 0);
        ww(ds, ACTION_MODE, 0);
    } else {
        if (mode) panel_button_draw(m, mode, 0);
        ww(ds, ACTION_MODE, (uint16_t)button);
        if (button == 2) {
            if (!(ds[(uint16_t)(rec + 0xb8)] & 1)) {
                ds[(uint16_t)(rec + 0x5f)] |= 2;
                panel_set_value(m, 8, 4);
                panel_button_draw(m, 2, 1);
                if (ds[MUSIC_TRACK_PLAYING] < 5 || ds[MUSIC_TRACK_PLAYING] > 7) ds[MUSIC_TRACK_WANTED] = 8;
            } else {
                ww(ds, ACTION_MODE, 0);
            }
        } else {
            panel_button_draw(m, button, 1);
        }
    }
    if (!(ds[(uint16_t)(rec + 0x5f)] & 2) && ds[MUSIC_TRACK_PLAYING] == 8)
        ds[MUSIC_TRACK_WANTED] = (uint8_t)(rt_rand(m) % 3 + 2);   /* pick_level_theme_music */
    input_wait_button_release(m, 1);
}

void uw_motion_panel_set_value(uw_motion *m, int8_t element, uint16_t value) { panel_set_value(m, element, value); }

/* ==== panel_build_elements, for the boot ==== */

/* panel_flask_fill(which): the empty flask's art 0x2057 at the
 * flask's x and y 0x4a, then each level to the value, the fill element
 * moved to the row's y and shown as base + i and flushed in turn; the value
 * shown recorded. The vitality flask's base is 0x203e, the green one,
 * while poisoned (rec +0x5f bits 2..5, the poison level), else 0x200c;
 * mana's 0x2025. */
void panel_flask_fill(uw_motion *m, int which) {
    uint8_t *ds = m->ds;
    uint16_t base, x = rw(ds, (uint16_t)(0x079c + which * 2)), fill = rw(ds, (uint16_t)(0x08cf + which * 2));
    int i;
    if (which == 0) base = (ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x5f)] & 0x3c) ? 0x203e : 0x200c;
    else if (which == 1) base = 0x2025;
    else return;
    m->span_variant = 1;
    uw_motion_gr_draw_art(m, 0x2057, x, 0x4a);
    m->span_variant = 0;
    for (i = 0; i < ds[(uint16_t)(PANEL_VALUES + which)]; i++) {
        elem_move(m, fill, x, (uint8_t)rw(ds, (uint16_t)(0x07a2 + i * 2)));
        elem_show(m, fill, (uint16_t)(base + i));
        elem_flush(m);
    }
    ds[(uint16_t)(PANEL_FLASK_SHOWN + which)] = ds[(uint16_t)(PANEL_VALUES + which)];
}

/* panel_draw_compass: the rose as (value & 3) + 0x2059, the
 * needle moved to its direction's place (the tables at 0x07dc and 0x07fc)
 * and shown as value + 0x205d. */
void panel_draw_compass(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t v = ds[0x362a];
    elem_show(m, rw(ds, 0x08d3), (uint16_t)((v & 3) + 0x2059));
    elem_move(m, rw(ds, 0x08d5), rw(ds, (uint16_t)(0x07dc + v * 2)), (uint8_t)rw(ds, (uint16_t)(0x07fc + v * 2)));
    elem_show(m, rw(ds, 0x08d5), (uint16_t)(v + 0x205d));
    elem_flush(m);
}

/* panel_draw_runes(runes): three 16 x 16 elements of group 1
 * at the x table 0x08b7, y 0x3d, allocated once with the span variant;
 * each rune under 0x18 shown as rune + 0xe8, else hidden; flushed. */
void panel_draw_runes_at(uw_motion *m, uint16_t runes) {
    uint8_t *ds = m->ds;
    int k;
    if (!rw(ds, 0x093e)) {
        for (k = 0; k < 3; k++) {
            uint16_t h = elem_alloc(m, 1, 0x10, 0x10, 1);
            ww(ds, (uint16_t)(0x093e + k * 2), h);
            elem_set_rect(m, h, rw(ds, (uint16_t)(0x08b7 + k * 2)), 0x3d, 0x10, 0x10);
        }
    }
    for (k = 0; k < 3; k++) {
        uint8_t r = ds[(uint16_t)(runes + k)];
        if (r < 0x18) elem_show(m, rw(ds, (uint16_t)(0x093e + k * 2)), (uint16_t)(r + 0xe8));
        else elem_hide(m, rw(ds, (uint16_t)(0x093e + k * 2)));
    }
    elem_flush(m);
}

void panel_build_elements(uw_motion *m) {
    uint8_t *ds = m->ds;
    int i;
    if (!ds[0x0907]) {
        for (i = 0; i < 2; i++) {
            uint16_t h = elem_alloc(m, 0, 0, 0, 0);
            ww(ds, (uint16_t)(0x08cf + i * 2), h);
            elem_set_rect(m, h, 0, 0, 0x18, 4);
            h = elem_alloc(m, 2, 0xd, 10, 0);
            ww(ds, (uint16_t)(0x08d7 + i * 2), h);
            elem_set_rect(m, h, rw(ds, (uint16_t)(0x081c + i * 2)), 0x41, 0xd, 10);
            h = elem_alloc(m, 2, 0x25, 0x17, 1);
            ww(ds, (uint16_t)(0x08db + i * 2), h);
            elem_set_rect(m, h, rw(ds, (uint16_t)(0x0820 + i * 2)), 0x36, 0x25, 0x17);
            h = elem_alloc(m, 0, 0, 0, 0);
            ww(ds, (uint16_t)(0x08df + i * 2), h);
            elem_set_rect(m, h, rw(ds, (uint16_t)(0x0854 + i * 2)), 0x86, 0xc, 0x1c);
            ds[(uint16_t)(PANEL_PREV_VALUES + 2 + i)] = 0;
            ds[(uint16_t)(0x362c + i)] = 0;
        }
        ww(ds, 0x08d3, elem_alloc(m, 0, 0, 0, 0));
        elem_set_rect(m, rw(ds, 0x08d3), 0x70, 0x44, 0x38, 0x20);
        ww(ds, 0x08d5, elem_alloc(m, 0, 0, 0, 0));
        elem_set_rect(m, rw(ds, 0x08d5), rw(ds, 0x07dc), (uint8_t)rw(ds, 0x07fc), 3, 4);
        ww(ds, ELEMENT7_ELEM, elem_alloc(m, 0, 0, 0, 0));
        elem_set_rect(m, rw(ds, ELEMENT7_ELEM), 0x80, 0xc3, 1, 1);
        ds[0x35ea] = 6;                       /* weapon_anim_state */
        ds[0x3630] = 6;
        ds[0x0907] = 1;
    }
    for (i = 0; i < 2; i++) {
        panel_flask_fill(m, i);
        elem_show(m, rw(ds, (uint16_t)(0x08d7 + i * 2)), rw(ds, (uint16_t)(0x0874 + i * 2)));
        elem_show(m, rw(ds, (uint16_t)(0x08db + i * 2)), rw(ds, (uint16_t)(0x0878 + i * 2)));
        elem_show(m, rw(ds, (uint16_t)(0x08df + i * 2)), (uint16_t)(0x207b + (i ? 0x12 : 0)));
    }
    panel_draw_compass(m);
    elem_show(m, rw(ds, ELEMENT7_ELEM), 0x20a6);
    panel_draw_runes_at(m, (uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x47));
    elem_flush(m);
}
