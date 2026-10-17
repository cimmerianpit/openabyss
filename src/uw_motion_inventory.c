/* SPDX-License-Identifier: MIT */
/* the inventory -- the panel and
 * paperdoll drawn, slots clicked, player_state_recalc and what the
 * player derives from what is worn, the Avatar's missile weapon, and
 * the drag of a thing between slots.
 *
 * One of src/uw_motion*.c: see uw_motion.h and uw_motion_int.h. */
#include "uw_motion_int.h"

/* ==== the inventory ==== */

/* An object as obj_deref_link returns it, far: its offset and the pool's
 * segment, or 0:0. */
void store_obj_far(uw_motion *m, uint16_t at, uint16_t o) {
    uint8_t *ds = m->ds;
    ww(ds, at, o);
    ww(ds, (uint16_t)(at + 2), !o ? 0 : o < rw(ds, STATIC_BASE) ? rw(ds, (uint16_t)(MOBILE_BASE + 2))
                                                                 : rw(ds, (uint16_t)(STATIC_BASE + 2)));
}

/* inventory_slot_object: the object in a slot, or 0. */
uint16_t inventory_slot_object(uw_motion *m, int16_t slot) {
    return obj_at(m, (uint16_t)((rw(m->ds, (uint16_t)(INVENTORY_SLOTS + slot * 2)) >> 6) & 0x3ff));
}

/* Item id `w0` against a class, subclass and type, each -1 for any. */
static int kind_matches(uint16_t w0, int16_t cls, int16_t sub, int16_t type) {
    return (cls < 0 || (int16_t)((w0 & 0x1c0) >> 6) == cls) && (sub < 0 || (int16_t)((w0 & 0x30) >> 4) == sub)
           && (type < 0 || (int16_t)(w0 & 0xf) == type);
}

/* object_chain_find_by_kind(cls, sub, type, &chain): the first
 * match along the chain through +4, looking into each non-quantity's
 * contents through +6. A match in the chain itself clears *chain; one found
 * deeper leaves *chain at its parent. */
static uint16_t object_chain_find_by_kind(uw_motion *m, int16_t cls, int16_t sub, int16_t type, uint16_t *chain) {
    uint8_t *ls = m->lseg;
    while (*chain) {
        uint16_t o = *chain, in, found;
        if (kind_matches(rw(ls, o), cls, sub, type)) {
            *chain = 0;
            return o;
        }
        if (!(rw(ls, o) & 0x8000) && (in = deref_link(m, (uint16_t)(o + 6))) != 0
            && (found = object_chain_find_by_kind(m, cls, sub, type, &in)) != 0) {
            if (in) *chain = in;
            return found;
        }
        *chain = deref_link(m, (uint16_t)(o + 4));
    }
    return 0;
}

/* inventory_find_by_kind(cls, sub, type, scope, &slot): slots
 * 0..10, then 11..18 unless scope is 1, then inside the slots' containers
 * unless scope is 2 or 3. The object, its slot out, or 0. */
uint16_t inventory_find_by_kind(uw_motion *m, int16_t cls, int16_t sub, int16_t type, int16_t scope,
                                       int16_t *slot) {
    uint8_t *ls = m->lseg;
    uint16_t objs[0x13];
    int16_t i;
    for (i = 0; i < 0x13; i++) {
        if (i > 10 && scope == 1) return 0;
        objs[i] = inventory_slot_object(m, i);
        if (objs[i] && kind_matches(rw(ls, objs[i]), cls, sub, type)) {
            *slot = i;
            return objs[i];
        }
    }
    if (scope == 2 || scope == 3) return 0;
    for (i = 0; i < 0x13; i++) {
        uint16_t in, found;
        if (!objs[i] || (rw(ls, objs[i]) & 0x8000)) continue;
        in = deref_link(m, (uint16_t)(objs[i] + 6));
        found = object_chain_find_by_kind(m, cls, sub, type, &in);
        if (found) {
            *slot = i;
            return found;
        }
    }
    return 0;
}

/* inventory_panel_init's buffers, guarded by 0x18a3 as the
 * original guards them: a background for each of the panel's elements
 * 6..0x16 out of inventory_panel_elements' draw rectangles, entry 0 (0x10 x
 * 0x0a at 299, 0x8d) and entry 1 (the container panel's 0x54 x 0x29 at
 * 0xec, 0x77), each captured off the page the panel's art is on. Elements
 * 10 and 0x0b take their rectangles five narrower, and element 10 five to
 * the right as well, as paperdoll_redraw takes them again. Without them
 * every imgbuf_restore over the panel would do nothing. */
void uw_motion_inventory_panel_init(uw_motion *m) {
    uint8_t *ds = m->ds;
    int16_t i;
    if (ds[INVENTORY_PANEL_INIT]) return;
    ds[INVENTORY_PANEL_INIT] = 1;
    m->span_variant = 1;
    paperdoll_load_body_art(m);
    for (i = 6; i <= 0x16; i++) {
        uint16_t e = (uint16_t)(INVENTORY_ELEM_PLACE + i * 0xe);
        ww(ds, (uint16_t)(INVENTORY_PANEL_BG + i * 2), imgbuf_alloc(m, ds[(uint16_t)(e + 4)], ds[(uint16_t)(e + 5)]));
    }
    ww(ds, INVENTORY_PANEL_BG, imgbuf_alloc(m, 0x10, 0xa));
    ww(ds, (uint16_t)(INVENTORY_PANEL_BG + 2), imgbuf_alloc(m, 0x54, 0x29));
    for (i = 6; i <= 0x16; i++) {
        uint16_t e = (uint16_t)(INVENTORY_ELEM_PLACE + i * 0xe);
        int16_t x = (int16_t)rw(ds, e);
        int w = ds[(uint16_t)(e + 4)];
        if (i == 10) { x = (int16_t)(x + 5); w -= 5; }
        else if (i == 0xb) { w -= 5; }
        imgbuf_capture_rect(m, rw(ds, (uint16_t)(INVENTORY_PANEL_BG + i * 2)), x,
                            (int16_t)rw(ds, (uint16_t)(e + 2)), w, ds[(uint16_t)(e + 5)]);
    }
    imgbuf_capture_rect(m, rw(ds, (uint16_t)(INVENTORY_PANEL_BG + 2)), 0xec, 0x77, 0x54, 0x29);
    imgbuf_capture_rect(m, rw(ds, INVENTORY_PANEL_BG), 299, 0x8d, 0x10, 0xa);
    m->span_variant = 0;
}

/* inventory_draw_weight_left(report): the capacity left, drawn
 * (pixels) only when it differs from the last drawn; 1 when it was and
 * `report` asked. */
int inventory_draw_weight_left(uw_motion *m, int report) {
    uint8_t *ds = m->ds;
    uint16_t left = (uint16_t)(rw(ds, CARRY_CAPACITY) - rw(ds, CARRIED_WEIGHT));
    if (rw(ds, WEIGHT_LEFT_DRAWN) == left) return 0;
    /* element 0's background put back, then the stones left -- tenths
     * divided by ten -- in colour 0xe0, centred on x 0x131 at y 0x8c in the
     * current font */
    imgbuf_restore(m, rw(ds, INVENTORY_PANEL_BG));
    ww(ds, WEIGHT_LEFT_DRAWN, left);
    if (m->screen) {
        char digits[8];
        int16_t v = (int16_t)((int16_t)left / 10);
        int nd = 0, k, neg = v < 0;
        uint16_t u = (uint16_t)(neg ? -v : v);
        do {
            digits[nd++] = (char)('0' + u % 10);
            u = (uint16_t)(u / 10);
        } while (u && nd < 6);
        if (neg) digits[nd++] = '-';
        for (k = 0; k < nd / 2; k++) {
            char t = digits[k];
            digits[k] = digits[nd - 1 - k];
            digits[nd - 1 - k] = t;
        }
        digits[nd] = '\0';
        uw_motion_draw_string(m, m->font, m->font_size, digits,
                              0x129 + 8 - uw_motion_string_width(m->font, m->font_size, digits) / 2, 0x8c, 0xe0);
    }
    return report != 0;
}

/* inventory_panel_redraw(from, to): the panel elements from..to
 * redrawn under a hidden cursor -- each slot's background and item art, a
 * stack's count in font4x5p.sys and font5x6p.sys back after, the weight
 * left. The drawing and the fonts' glyphs are the graphics module's; what
 * the data segment keeps is the cursor, the font flag and the weight. */
static void paperdoll_redraw(uw_motion *m);

void inventory_panel_redraw(uw_motion *m, int16_t from, int16_t to) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    int counts = 0;
    int16_t e;
    uint16_t count[0x20];
    cursor_hide(m);
    m->span_variant = 1;
    for (e = from; e <= to; e++) {
        /* the element's saved background (inventory_panel_bg) */
        imgbuf_restore(m, rw(ds, (uint16_t)(INVENTORY_PANEL_BG + e * 2)));
        if (e >= 0 && e < 0x20) count[e] = 1;
        if (e < 0x15) {
            uint16_t o = inventory_slot_object(m, (int8_t)ds[(uint16_t)(INVENTORY_SEARCH_ORDER + e)]);
            uint16_t q;
            if (!o) continue;
            /* the item's art at the element's place (14 bytes an element) */
            uw_motion_gr_draw_art(m, (uint16_t)(rw(ls, o) & 0x1ff), (int16_t)rw(ds, (uint16_t)(INVENTORY_ELEM_PLACE + e * 0xe)),
                                  (int16_t)rw(ds, (uint16_t)(INVENTORY_ELEM_PLACE + 2 + e * 0xe)));
            q = (uint16_t)((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff);
            if ((rw(ls, o) & 0x8000) && !(q & 0x200) && q > 1) {
                counts = 1;
                if (e >= 0 && e < 0x20) count[e] = q;
            }
        } else {
            inventory_slot_click(m, e);
        }
    }
    m->span_variant = 0;
    if (counts) {
        ds[FONT_LOADED] = 1;                /* font_open(font4x5p.sys), then font5x6p.sys back: both set it */
        /* each stack's count in font4x5p.sys, colour 0x60, at the element's
         * x + 3 and y - 1 */
        for (e = from; e <= to; e++) {
            char digits[8];
            uint16_t v;
            int nd = 0, k;
            if (e < 0 || e >= 0x20 || count[e] <= 1) continue;
            for (v = count[e]; v && nd < 7; v = (uint16_t)(v / 10)) digits[nd++] = (char)('0' + v % 10);
            for (k = 0; k < nd / 2; k++) {
                char t = digits[k];
                digits[k] = digits[nd - 1 - k];
                digits[nd - 1 - k] = t;
            }
            digits[nd] = '\0';
            uw_motion_draw_string(m, m->font_small, m->font_small_size, digits,
                                  (int16_t)rw(ds, (uint16_t)(INVENTORY_ELEM_PLACE + e * 0xe)) + 3,
                                  (int16_t)rw(ds, (uint16_t)(INVENTORY_ELEM_PLACE + 2 + e * 0xe)) - 1, 0x60);
        }
    }
    inventory_draw_weight_left(m, 0);
    cursor_show(m);
}

/* inventory_slot_click(element): an inventory panel element
 * redrawn, when the inventory is the panel shown -- the paperdoll below 6, a
 * slot below 0x15, else an arrow button: the element's background put back
 * and, while the container can page that way, art 0x101b or 0x101c with colour 0 skipped. */
void inventory_slot_click(uw_motion *m, int16_t element) {
    uint8_t *ds = m->ds;
    uint16_t art = 0;
    if (ds[PANEL_MODE]) return;
    if (element < 6) {
        paperdoll_redraw(m);
    } else if (element < 0x15) {
        inventory_panel_redraw(m, element, element);
    } else {
        imgbuf_restore(m, rw(ds, (uint16_t)(INVENTORY_PANEL_BG + element * 2)));
        if (element == 0x15 ? ds[CONTAINER_CAN_PAGE_FORWARD] : ds[CONTAINER_CAN_PAGE_BACK])
            art = element == 0x15 ? 0x101b : 0x101c;
        if (art) {
            m->span_variant = 1;
            uw_motion_gr_draw_art(m, art, (int16_t)rw(ds, (uint16_t)(INVENTORY_ELEM_PLACE + element * 0xe)),
                                  (int16_t)rw(ds, (uint16_t)(INVENTORY_ELEM_PLACE + 2 + element * 0xe)));
            m->span_variant = 0;
        }
    }
}

/* paperdoll_load_body_art: BODIES.GR's image for the Avatar
 * loaded at art 0x2091 (uw_motion_gr_draw_art takes it from the record) and
 * the five worn slots' art forgotten, so the redraw loads each again. */
void paperdoll_load_body_art(uw_motion *m) {
    int k;
    for (k = 1; k < 6; k++) m->ds[(uint16_t)(PAPERDOLL_ART_ITEM + k)] = 0;
}

/* paperdoll_load_armour_art(slot, index): "armor_f" or
 * "armor_m" by the Avatar's sex, image `index` loaded at art 0x2091 + slot.
 * The loading, gr_load_one_at_art_id, is art_image's: it takes
 * the image from the same file by the item and tier this remembers
 * (src/uw_motion_elem.c) when the slot is drawn. */
static void paperdoll_load_armour_art(uw_motion *m) {
    uint8_t *ds = m->ds;
    ds[(uint16_t)(PAPERDOLL_ART_NAME + 6)] = ((ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x64)] >> 1) & 1) ? 'f' : 'm';
}

/* imgbuf_capture's rectangle (x, y the top row, w, h) into a handle's save
 * area, recorded in its heap record. */
void imgbuf_capture_rect(uw_motion *m, uint16_t handle, int x, int y, int w, int h) {
    uint16_t at = imgbuf_record(m, handle);
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

/* paperdoll_redraw, when the inventory is the panel shown: under
 * a hidden cursor the body (art 0x2091) at element 0's place, opaque, then
 * each worn slot's armour (elements 1..5) with colour 0 skipped -- the art
 * loaded again when the item's low five bits or its wear tier (quality >> 4,
 * 3 from item 0xf up) differ from what was last loaded; the two
 * hands' backgrounds captured again over the body (elements 11 and 10, five
 * pixels narrower, 10's five pixels in), the hands redrawn when either holds
 * something, and the weight left, font5x6p opened again after a draw. While
 * the panel is switched in the drawing is on the other page,
 * brought over before and after by gfx_fill_rect's page copy (colour 0x106)
 * of the panel; drawn straight onto the screen here, which comes to the same
 * pixels. */
static void paperdoll_redraw(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    int i;
    if (ds[PANEL_MODE]) return;
    cursor_hide(m);
    uw_motion_gr_draw_art(m, 0x2091, (int16_t)rw(ds, INVENTORY_ELEM_PLACE), (int16_t)rw(ds, INVENTORY_ELEM_PLACE + 2));
    m->span_variant = 1;
    for (i = 1; i < 6; i++) {
        uint16_t o = inventory_slot_object(m, (int8_t)ds[(uint16_t)(INVENTORY_SEARCH_ORDER + i)]);
        uint16_t item, tier;
        if (!o) continue;
        item = (uint16_t)(rw(ls, o) & 0x1f);
        tier = item < 0xf ? (uint16_t)((ls[(uint16_t)(o + 4)] & 0x3f) >> 4) : 3;
        if ((int)item + 1 != (int8_t)ds[(uint16_t)(PAPERDOLL_ART_ITEM + i)]
            || (int)tier + 1 != (int8_t)ds[(uint16_t)(PAPERDOLL_ART_TIER + i)]) {
            ds[(uint16_t)(PAPERDOLL_ART_ITEM + i)] = (uint8_t)(item + 1);
            ds[(uint16_t)(PAPERDOLL_ART_TIER + i)] = (uint8_t)(tier + 1);
            paperdoll_load_armour_art(m);
        }
        uw_motion_gr_draw_art(m, (uint16_t)(0x2091 + i), (int16_t)rw(ds, (uint16_t)(INVENTORY_ELEM_PLACE + i * 0xe)),
                              (int16_t)rw(ds, (uint16_t)(INVENTORY_ELEM_PLACE + 2 + i * 0xe)));
    }
    m->span_variant = 0;
    imgbuf_capture_rect(m, rw(ds, (uint16_t)(INVENTORY_PANEL_BG + 0xb * 2)),
                        (int16_t)rw(ds, (uint16_t)(INVENTORY_ELEM_PLACE + 0xb * 0xe)),
                        (int16_t)rw(ds, (uint16_t)(INVENTORY_ELEM_PLACE + 2 + 0xb * 0xe)),
                        ds[(uint16_t)(INVENTORY_ELEM_PLACE + 4 + 0xb * 0xe)] - 5, ds[(uint16_t)(INVENTORY_ELEM_PLACE + 5 + 0xb * 0xe)]);
    imgbuf_capture_rect(m, rw(ds, (uint16_t)(INVENTORY_PANEL_BG + 0xa * 2)),
                        (int16_t)rw(ds, (uint16_t)(INVENTORY_ELEM_PLACE + 0xa * 0xe)) + 5,
                        (int16_t)rw(ds, (uint16_t)(INVENTORY_ELEM_PLACE + 2 + 0xa * 0xe)),
                        ds[(uint16_t)(INVENTORY_ELEM_PLACE + 4 + 0xa * 0xe)] - 5, ds[(uint16_t)(INVENTORY_ELEM_PLACE + 5 + 0xa * 0xe)]);
    if ((rw(ds, (uint16_t)(INVENTORY_SLOTS + 9 * 2)) >> 6) || (rw(ds, (uint16_t)(INVENTORY_SLOTS + 10 * 2)) >> 6))
        inventory_panel_redraw(m, 10, 0xb);
    if (inventory_draw_weight_left(m, 1)) ds[FONT_LOADED] = 1;   /* font_open(font5x6p.sys) */
    cursor_show(m);
}

/* inventory_panel_container_button, when the inventory is the
 * panel shown: the weight left forgotten (-1), INV.GR's
 * open-container art 0x2097 at (0xec, 0x77) when a container is open, and
 * elements 6..0x16 redrawn. With none open it calls imgbuf_restore(1),
 * and 1 is no buffer's handle -- handles are save areas' plane
 * addresses -- so nothing is put back. */
void inventory_panel_container_button(uw_motion *m) {
    uint8_t *ds = m->ds;
    if (ds[PANEL_MODE]) return;
    ww(ds, WEIGHT_LEFT_DRAWN, 0xffff);
    if (rw(ds, CONTAINER_STACK_TOP) || rw(ds, (uint16_t)(CONTAINER_STACK_TOP + 2)))
        uw_motion_gr_draw_art(m, 0x2097, 0xec, 0x77);
    inventory_panel_redraw(m, 6, 0x16);
}

void uw_motion_print_ds_string(uw_motion *m, uint16_t at) {
    scroll_print(m, (const char *)m->ds + at);
}

void uw_motion_dungeon_refresh_inventory(uw_motion *m) {
    uint8_t *ds = m->ds;
    if (ds[PANEL_MODE] && rw(ds, (uint16_t)(rw(ds, 0x00e2) + 8)) != 4) return;
    paperdoll_load_body_art(m);
    paperdoll_redraw(m);
    inventory_panel_container_button(m);
}

/* inventory_click_slot_index(slot): a slot's panel element
 * redrawn. */
void inventory_click_slot_index(uw_motion *m, int16_t slot) {
    inventory_slot_click(m, (int8_t)m->ds[(uint16_t)(INVENTORY_CLICK_ORDER + slot)]);
}

/* inventory_unlink_object(cls, sub, type, slot, count): the
 * object in a slot, or of that kind inside it, out of its parent's contents
 * -- `count` of a larger stack split off first, the rest a copy put after
 * it -- and its weight off the player's. The parent is the Avatar, or for a
 * slot above 0x12 the open container (the top node's +8). The slot takes the
 * copy (or empties) when the parent is the Avatar or the slot is a page slot
 * (0x14..). Unlinked from the open container itself, a page slot still
 * naming it takes the copy too and every open container's running weight
 * (+0xa) loses its weight. */
uint16_t inventory_unlink_object(uw_motion *m, int16_t cls, int16_t sub, int16_t type, int16_t slot,
                                        int16_t count) {
    uint8_t *ds = m->ds, *ls = m->lseg, *top = NULL;
    uw_objpool pool;
    uint16_t o = inventory_slot_object(m, slot), parent, copy = 0, w, idx;
    int16_t si;
    int open = (rw(ds, CONTAINER_STACK_TOP) | rw(ds, (uint16_t)(CONTAINER_STACK_TOP + 2))) != 0;
    if (!o) return 0;
    if (open) top = far_bytes(m, rw(ds, CONTAINER_STACK_TOP), rw(ds, (uint16_t)(CONTAINER_STACK_TOP + 2)), 0xc);
    if (slot > 0x12) {
        if (!top) {
            UW_NOT_CARRIED(m->not_carried);
            return 0;
        }
        parent = obj_at(m, (uint16_t)((rw(top, 8) >> 6) & 0x3ff));
    } else {
        parent = rw(ds, TRACKED_OBJECT);
    }
    if ((cls >= 0 || sub >= 0 || type >= 0) && !kind_matches(rw(ls, o), cls, sub, type)) {
        o = object_chain_find_by_kind(m, cls, sub, type, &parent);
        if (!o) return 0;
    }
    if (count) {
        uint16_t q = (uint16_t)((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff);
        if ((rw(ls, o) & 0x8000) && !(q & 0x200) && (int16_t)q > 1 && count < (int16_t)q) {
            pool_from_ds(m, &pool);
            copy = uw_obj_alloc(&pool, 0);
            pool_to_ds(m, &pool);
            if (!copy) {
                UW_NOT_CARRIED(m->not_carried);
                return 0;
            }
            memmove(ls + copy, ls + o, 8);  /* struct_copy_far */
            ww(ls, (uint16_t)(copy + 6), (uint16_t)((rw(ls, (uint16_t)(copy + 6)) & 0x3f) | ((q - count) & 0x3ff) << 6));
            ww(ls, (uint16_t)(o + 6), (uint16_t)((rw(ls, (uint16_t)(o + 6)) & 0x3f) | (count & 0x3ff) << 6));
            pool_from_ds(m, &pool);
            uw_object_list_insert(&pool, (uint16_t)(o + 4), copy);
            pool_to_ds(m, &pool);
        }
    }
    if (parent == rw(ds, TRACKED_OBJECT) || slot >= 0x14)
        ww(ds, (uint16_t)(INVENTORY_SLOTS + slot * 2), (uint16_t)((rw(ds, (uint16_t)(INVENTORY_SLOTS + slot * 2)) & 0x3f)
                                                                | (obj_index_of(m, copy) & 0x3ff) << 6));
    pool_from_ds(m, &pool);
    uw_object_list_remove(&pool, (uint16_t)(parent + 6), o);
    pool_to_ds(m, &pool);
    w = object_weight(m, o);
    ww(ds, CARRIED_WEIGHT, (uint16_t)(rw(ds, CARRIED_WEIGHT) - w));
    if (!open) return o;
    if (!top) {
        UW_NOT_CARRIED(m->not_carried);
        return o;
    }
    if (obj_index_of(m, parent) != ((rw(top, 8) >> 6) & 0x3ff)) return o;
    idx = obj_index_of(m, o);
    for (si = 0x14; si <= 0x1b; si++) {
        uint16_t at = (uint16_t)(INVENTORY_SLOTS + si * 2);
        if (((rw(ds, at) >> 6) & 0x3ff) != idx) continue;
        ww(ds, at, (uint16_t)((rw(ds, at) & 0x3f) | (obj_index_of(m, copy) & 0x3ff) << 6));
        container_stack_less_weight(m, w);
        break;
    }
    return o;
}

/* inventory_take_one_from_slot(cls, sub, type, slot): one of
 * the slot's object taken (inventory_take_object: unlinked and
 * player_state_recalc), and the slot redrawn -- when the slot holds a
 * container and one is open, the page refilled and container_view_refresh
 * instead. */
static uint16_t inventory_take_one_from_slot(uw_motion *m, int16_t cls, int16_t sub, int16_t type, int16_t slot) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t o = inventory_unlink_object(m, cls, sub, type, slot, 1), c;
    player_state_recalc(m);
    c = inventory_slot_object(m, slot);
    if (!c || ((rw(ls, c) & 0x1c0) >> 6) != 2 || (rw(ls, c) & 0x30)
        || !(rw(ds, CONTAINER_STACK_TOP) | rw(ds, (uint16_t)(CONTAINER_STACK_TOP + 2))))
        inventory_slot_click(m, (int8_t)ds[(uint16_t)(INVENTORY_CLICK_ORDER + slot)]);
    else {
        container_page_fill(m);
        container_view_refresh(m);
    }
    return o;
}

/* ==== player_state_recalc: what the player derives from the
 * equipment and the active spells, from the instructions ==== */

/* armour_value: nothing for a weapon (class 0, subclass 0 or 1);
 * else the protection scaled by the quality in 64ths, plus one. */
static uint16_t armour_value(uw_motion *m, uint16_t o) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w0 = rw(ls, o);
    if (!((w0 & 0x1c0) >> 6) && ((w0 & 0x30) >> 4) < 2) return 0;
    return (uint16_t)((uint16_t)((ls[(uint16_t)(o + 4)] & 0x3f) * ds[(uint16_t)(ARMOUR_PROPS + ((w0 & 0x1ff) - 0x20) * 4)]) >> 6) + 1;
}

/* weapon_set_anim(set): the weapon animation wanted, and panel
 * element 8 dirtied for a set 0..3 or one not loaded. */
static void weapon_set_anim(uw_motion *m, int8_t set) {
    uint8_t *ds = m->ds;
    ds[WEAPON_ANIM_WANTED] = (uint8_t)set;
    if ((set >= 0 && set <= 3) || ds[WEAPON_ANIM_LOADED] != ds[WEAPON_ANIM_WANTED])
        ww(ds, PANEL_DIRTY_1, (uint16_t)(rw(ds, PANEL_DIRTY_1) | 0x100));
}

/* player_derived_state_reset: the noise base and level from
 * Sneak (rec +0x2e), 13 - Sneak / 3 and 15 - Sneak / 5, the flags the enchantments set cleared, 0x286 back to 0x90 (0x190
 * with 0x1b00). */
static void player_derived_state_reset(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    ds[0x60eb] = 0;
    ds[NOISE_BASE] = (uint8_t)(0xd - ds[(uint16_t)(rec + 0x2e)] / 3);
    ds[NOISE_LEVEL] = (uint8_t)(0xf - ds[(uint16_t)(rec + 0x2e)] / 5);
    ds[BLOCK_FLAGS] = 0;
    memset(ds + CRITTER_ARMOUR_ADDS, 0, 4);
    ds[LEVEL7_FLOOR_VARIANT] = 0;
    ds[DRAGON_BOOTS_WORN] = 0;
    ds[TIME_FROZEN] = 0;
    ds[0x285] = 0;
    ds[0x284] = 0;
    ds[0x283] = 0;
    ww(ds, 0x286, ds[0x1b00] ? 0x190 : 0x90);
    ds[REGEN_FLAGS] = 0;
}

/* enchantment_apply(kind, value, &mask, slot), from the
 * instructions: one enchantment, a spell on the player (slot -1) or a worn
 * item's (its slot). 0 the light's brightness raised to `value` (the slot
 * nibble cleared), 1 a motion spell bit (Leap .. Fly), 2 the mask's high nibble raised,
 * 3 the armour adds (value 1), a mask bit (2..4) or 0x60eb's (5..9), 9
 * effect_nonlethal_damage on the Avatar by the value, 11 flags by value, 12 a worn item's
 * protection -- to the armour adds by region, or with bit 3 to the critter
 * row -- and 13 level 7's floor. Returns 0 on every arm. */
static int enchantment_apply(uw_motion *m, uint8_t kind, uint8_t value, uint16_t *mask, int16_t slot) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    switch (kind) {
    case 0:
        if (((ds[(uint16_t)(rec + 0x63)] & 0xf0) >> 4) < value) ds[(uint16_t)(rec + 0x63)] = (uint8_t)(value << 4);
        break;
    case 1:
        ds[BLOCK_FLAGS] |= (uint8_t)(1u << ((value - 1) & 0x1f));
        break;
    case 2:
        if ((*mask >> 4) < value) *mask = (uint16_t)((value << 4) + (*mask & 0xf));
        break;
    case 3:
        if (value == 1) {
            int k;
            for (k = 0; k < 4; k++) ds[CRITTER_ARMOUR_ADDS + k] = (uint8_t)(ds[CRITTER_ARMOUR_ADDS + k] + 3);
        } else if (value <= 4) {
            *mask |= (uint16_t)(1u << ((value - 1) & 0x1f));
        } else if (value <= 9) {
            ds[0x60eb] |= ds[(uint16_t)(0x1b00 + value)];
        }
        break;
    case 9:
        effect_nonlethal_damage(m, rw(ds, TRACKED_OBJECT), (int16_t)value);
        break;
    case 11:
        if (value == 0) ds[TIME_FROZEN] = 1;
        else if (value == 1) ds[0x284] = 1;
        else if (value == 2) ds[0x283] = 1;
        else if (value == 3) ww(ds, 0x286, 0);
        else if (value == 14) ds[REGEN_FLAGS] |= 1;
        else if (value == 15) ds[REGEN_FLAGS] |= 2;
        break;
    case 12:
        if (slot >= 0) {
            int16_t w[2];
            uint16_t crow = rw(ds, CRITTER_ROW_PTR);
            int k;
            w[0] = rs(ds, 0x1b0a);
            w[1] = rs(ds, 0x1b0c);
            if (slot > 4) {
                w[0] = 0;
                w[1] = 1;
            } else {
                w[0] = (int8_t)ds[(uint16_t)(ARMOUR_REGION_BY_SLOT + slot)];
            }
            for (k = 0; k < 2 && w[k] != -1; k++) {
                uint8_t add = 0;
                if (value & 8)
                    add = (uint8_t)((value & 7) + 1);
                else
                    ds[(uint16_t)(CRITTER_ARMOUR_ADDS + w[k])] = (uint8_t)((value & 7) + ds[(uint16_t)(CRITTER_ARMOUR_ADDS + w[k])] + 1);
                ds[(uint16_t)(crow + w[0])] = (uint8_t)(ds[(uint16_t)(crow + w[0])] + add);
            }
        }
        break;
    case 13:
        if (value == 4) ds[LEVEL7_FLOOR_VARIANT] = 1;
        break;
    default:
        break;
    }
    return 0;
}

/* inventory_slot_is_vulnerable(id, slot): the worn slots 0..4,
 * the rings 9 and 10, and a shield (class 0 subclass 2 or 3, type 11..15)
 * in the off hand, 7 + the handedness bit. */
static int inventory_slot_is_vulnerable(uw_motion *m, uint16_t id, int16_t slot) {
    uint8_t *ds = m->ds;
    if (slot >= 0 && slot <= 4) return 1;
    if (slot == 10 || slot == 9) return 1;
    return (ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x64)] & 1) + 7 == slot && !(id >> 6)
           && ((id & 0x30) >> 4) > 1 && (id & 0xf) > 10;
}

/* inventory_damage_slot(slot, dmg, type, mode, debris), from
 * the instructions: what the player is carrying, damaged. Mode 0 restricts
 * it to item ids whose bits 4..8 are clear, mode 1 asks
 * inventory_slot_is_vulnerable, mode 2 hits anything; an empty slot or a
 * refusal is -2. Then apply_damage(obj, 0, -1, -1, dmg, type), and the
 * quality word says what happened:
 *   unchanged  -1, and nothing printed
 *   reduced     0, message " damaged.\n"
 *   destroyed   1, message " destroyed.\n" -- one off the stack
 *               (inventory_remove_quantity of 1), the object removed and
 *               the state recalculated, and with `debris` a piece of it
 *               (item 0xd5 or 0xd6, rand() * 2 / 0x8000) dropped at the
 *               Avatar's feet with a spread of six.
 * The line is "Your " + the object's name with NO article and NO plural --
 * the instructions push two zero words after the object -- then " were" if
 * that name ends in an s and " was" if it does not, then the message; and
 * the slot is redrawn.
 *
 * The object is compared with the tracked one and, if it IS the Avatar,
 * its item id is rewritten to 0xf before the name is taken. No slot holds
 * the Avatar, so nothing reaches it; it is carried because it is there. */
int16_t inventory_damage_slot(uw_motion *m, int16_t slot, uint8_t dmg, uint8_t type, int16_t mode,
                              int debris) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t o = inventory_slot_object(m, slot), id, text;
    uint8_t quality;
    char buf[0x32], part[0x20];
    int16_t r;
    size_t n;
    if (!o) return -2;
    id = (uint16_t)(rw(ls, o) & 0x1ff);
    if (mode != 2) {
        if (mode == 0) { if (rw(ls, o) & 0x1f0) return -2; }
        else if (!inventory_slot_is_vulnerable(m, id, slot)) return -2;
    }
    quality = (uint8_t)(ls[(uint16_t)(o + 4)] & 0x3f);
    if (!apply_damage(m, o, 0, -1, -1, dmg, type)) {
        if ((ls[(uint16_t)(o + 4)] & 0x3f) == quality) return -1;
        text = 0x1701;
        r = 0;
    } else {
        if (debris) {
            uint16_t piece = create_object(m, (uint16_t)(0xd5 + (uint16_t)(((int32_t)rt_rand(m) * 2) / 0x8000)), 0);
            uw_motion_object_place_at_own_coords(m, rw(ds, TRACKED_OBJECT), piece, 6, 0,
                                                 (uint16_t)(FRAME_BP - 0x80));
        }
        inventory_remove_quantity(m, o, 1);
        object_remove(m, 0, o, 1);
        player_state_recalc(m);
        text = 0x16f4;
        r = 1;
    }
    buf[0] = 0;
    strncat(buf, ds_text(m, 0x170c, part, sizeof part), sizeof buf - 1);
    if (o == rw(ds, TRACKED_OBJECT)) ww(ls, o, (uint16_t)((rw(ls, o) & 0xfe00) | 0xf));
    n = strlen(buf);
    format_object_name(m, buf + n, sizeof buf - n, rw(ls, o), ls[(uint16_t)(o + 0x1a)], 0, 0);
    n = strlen(buf);
    strncat(buf, ds_text(m, (uint16_t)(n && buf[n - 1] == 's' ? 0x1712 : 0x1718), part, sizeof part),
            sizeof buf - strlen(buf) - 1);
    strncat(buf, ds_text(m, text, part, sizeof part), sizeof buf - strlen(buf) - 1);
    scroll_print(m, buf);
    inventory_click_slot_index(m, slot);
    return r;
}

/* apply_level7_floor_variant(on): on level 7 the fifth floor
 * texture swapped to 0xc or back to 0xe -- apply_maze_texture,
 * which reloads F32.TR and F16.TR through floor_texture_assign so the new
 * entry takes at once: the host's scene, asked -- and rec +0x62 bit 4
 * remembers it. */
void apply_level7_floor_variant(uw_motion *m, uint8_t on) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    if (rw(ds, CURRENT_LEVEL_WORD) == 7) {
        int8_t t = -1;
        if (on && rw(ds, FLOOR_TEXTURE_4) != 0xc) t = 0xc;
        if (!on && rw(ds, FLOOR_TEXTURE_4) == 0xc) t = 0xe;
        if (t >= 0) {
            ww(ds, FLOOR_TEXTURE_4, (uint16_t)t);
            m->render.floors = 1;
        }
    }
    ds[(uint16_t)(rec + 0x62)] = (uint8_t)((ds[(uint16_t)(rec + 0x62)] & 0xef) | (on & 1) << 4);
}

/* panel_draw_active_spells(icons): in the dungeon (the event's
 * +8 is 1) the three spell icon elements, made on the first call, shown
 * with art 0x20c0 + icon or hidden for 0x15 and above, and flushed. */
static void panel_draw_active_spells(uw_motion *m, const uint8_t *icons) {
    uint8_t *ds = m->ds;
    int k;
    if (rw(ds, (uint16_t)(rw(ds, 0x00e2) + 8)) != 1) return;
    if (!rw(ds, SPELL_ELEMS)) {
        for (k = 0; k < 3; k++) {
            ww(ds, (uint16_t)(SPELL_ELEMS + k * 2), elem_alloc(m, 1, 0x10, 0x12, 1));
            elem_set_rect(m, rw(ds, (uint16_t)(SPELL_ELEMS + k * 2)), rw(ds, (uint16_t)(SPELL_ELEM_X + k * 2)), 0x3f,
                          0x10, 0x12);
        }
    }
    for (k = 0; k < 3; k++) {
        if (icons[k] < 0x15)
            elem_show(m, rw(ds, (uint16_t)(SPELL_ELEMS + k * 2)), (uint16_t)(icons[k] + 0x20c0));
        else
            elem_hide(m, rw(ds, (uint16_t)(SPELL_ELEMS + k * 2)));
    }
    elem_flush(m);
}

/* apply_active_spell_effects(mask): the mask's bits 1..3 take
 * the noise bases down (by up to 16, 5 and 16), bits 8..11 -- the loop has
 * shifted the mask four places before it adds its "high nibble" -- go onto
 * the critter row's first four bytes; then level 7's floor, and the spell
 * icons (active_spell_icons) drawn. */
static void apply_active_spell_effects(uw_motion *m, uint16_t mask) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), crow = rw(ds, CRITTER_ROW_PTR), n;
    uint8_t icons[3];
    int k;
    for (k = 0; k < 4; k++, mask >>= 1) {
        uint8_t cut;
        if (!(mask & 1)) continue;
        if (k == 1) {
            cut = ds[NOISE_BASE] > 0x10 ? 0x10 : ds[NOISE_BASE];
            ds[NOISE_BASE] = (uint8_t)(ds[NOISE_BASE] - cut);
        } else if (k == 2 || k == 3) {
            uint8_t cap = k == 2 ? 5 : 0x10;
            cut = ds[NOISE_LEVEL] > cap ? cap : ds[NOISE_LEVEL];
            ds[NOISE_LEVEL] = (uint8_t)(ds[NOISE_LEVEL] - cut);
        }
    }
    for (k = 0; k < 4; k++) ds[(uint16_t)(crow + k)] = (uint8_t)(ds[(uint16_t)(crow + k)] + ((mask >> 4) & 0xf));
    apply_level7_floor_variant(m, ds[LEVEL7_FLOOR_VARIANT]);
    memset(icons, 0x15, 3);
    n = (uint16_t)((rw(ds, (uint16_t)(rec + 0x5f)) >> 6) & 0xf);
    for (k = 0; k < 3 && (uint16_t)k < n; k++) {
        uint16_t e = rw(ds, (uint16_t)(rec + 0x3e + k * 2));
        icons[k] = (uint8_t)(ds[(uint16_t)(SPELL_ICON_BASE + (e & 0xf))] + ((e & 0xf0) >> 4));
    }
    if (n > 3) UW_NOT_CARRIED(m->not_carried);
    panel_draw_active_spells(m, icons);
}

/* shade_set_level(level): a light level other than the one
 * set takes its SHADES.DAT record -- the distance ramp's slope (at least 1),
 * addend and floor for the renderer (the graphics module's),
 * the light radius and the two detail distances (0x549, 0x547)
 * -- rebuilds view_light_map from them and posts the view's refresh. Light
 * level 5 (the carried light's, not the dungeon's) swaps MONO.DAT in for
 * LIGHT.DAT, and back (counted: the renderer's far buffer). */
void shade_set_level(uw_motion *m, uint8_t level) {
    uint8_t *ds = m->ds;
    uint8_t lprm[6];
    const uint8_t *rec;
    int16_t slope;
    if (ds[SHADE_LEVEL] == level) return;
    /* into or out of level 5, the ramp read again -- MONO.DAT or LIGHT.DAT
     * into the far buffer -- which undoes a blanked one too;
     * the host's scene takes the file by the level itself */
    if (ds[SHADE_LEVEL] == 5 || level == 5) m->render.shade = 2;
    ds[SHADE_LEVEL] = level;
    if (!m->shades || level >= 8) {
        UW_NOT_CARRIED(m->not_carried);
        return;
    }
    rec = m->shades + level * 12;
    slope = (int16_t)(rec[0] | rec[1] << 8);
    if (slope <= 1) slope = 1;
    lprm[0] = (uint8_t)slope;
    lprm[1] = (uint8_t)(slope >> 8);
    memcpy(lprm + 2, rec + 2, 4);
    ww(ds, 0x735a, (uint16_t)(rec[6] | rec[7] << 8));
    ww(ds, 0x549, (uint16_t)(rec[8] | rec[9] << 8));
    ww(ds, 0x547, (uint16_t)(rec[10] | rec[11] << 8));
    uw_light_map_build(ds, rs(ds, 0x735a), lprm);   /* view_build_light_map */
    ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));   /* post_event(2) */
}

/* view_effect_warp(on): the swimming view -- the six texture
 * records' masks (5723:[0xb002] + 2 + 8i, which is 0xb07e + 8i + 6) at
 * rest just under the top of each size's range, and switched on six
 * draws that can only pull them down; view_warp_on keeps a repeat free.
 * The masks are the host's scene's, asked. */
static void view_effect_warp(uw_motion *m, uint8_t on) {
    static const uint16_t rest[6] = { 0x0f0, 0x0f0, 0x3e0, 0x3e0, 0xfc0, 0xfc0 };
    static const uint16_t range[6] = { 0x0ff, 0x0ff, 0x3ff, 0x3ff, 0xfff, 0xfff };
    uint8_t *ds = m->ds;
    int k;
    if (ds[VIEW_WARP_ON] == on) return;
    ds[VIEW_WARP_ON] = on;
    for (k = 0; k < 6; k++) m->render.mask[k] = on ? (uint16_t)(rt_rand(m) & range[k]) : rest[k];
    m->render.masks = 1;
}

/* impairment_effect_set(on), from the instructions: switching
 * the confusion on picks one of three distortions -- forced to the first
 * once by 0x1b04 -- the rasteriser's warp, one of PALS.DAT's eight
 * palettes (palette_load(rand & 7)), the shading ramp blanked
 * (shade_reload_or_scramble(1)); off undoes the one remembered, the warp's
 * rest, palette_load(0), the ramp read again. */
static void impairment_effect_set(uw_motion *m, int on) {
    uint8_t *ds = m->ds;
    if (on) {
        if ((int8_t)ds[IMPAIRMENT_EFFECT] >= 0) return;
        if (ds[IMPAIRMENT_FORCED]) {
            ds[IMPAIRMENT_EFFECT] = 0;
            ds[IMPAIRMENT_FORCED] = 0;
        } else {
            ds[IMPAIRMENT_EFFECT] = (uint8_t)(rt_rand(m) % 3);
        }
        switch (ds[IMPAIRMENT_EFFECT]) {
        case 0: view_effect_warp(m, 1); break;
        case 1: m->render.palette = (int)(rt_rand(m) & 7) + 1; break;
        case 2: m->render.shade = 1; break;
        default: break;
        }
    } else if ((int8_t)ds[IMPAIRMENT_EFFECT] >= 0) {
        switch (ds[IMPAIRMENT_EFFECT]) {
        case 0: view_effect_warp(m, 0); break;
        case 1: m->render.palette = 0 + 1; break;
        case 2: m->render.shade = 2; break;
        default: break;
        }
        ds[IMPAIRMENT_EFFECT] = 0xff;
    }
}

/* player_state_recalc: armour by region from the five worn slots and a
 * shield in the off hand; the weapon hand's animation set and the attack
 * skill it adds to critter row +0x12; the brightest light among the four
 * light slots and the cursor into rec +0x63; the active spells' and the
 * worn items' enchantments; the noise bases, level 7's floor and the spell
 * icons; the shading level; the confusion; and the movement state re-derived with
 * force and the mode reapplied. */
void player_state_recalc(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), crow = rw(ds, CRITTER_ROW_PTR), o, w0, mask = 0;
    int16_t i, code = 2, bright = 0, bright_slot = 0;
    int8_t set = 3;
    uint8_t shade;
    for (i = 0; i < 4; i++) ds[(uint16_t)(crow + i)] = 0;
    for (i = 0; i < 5; i++) {
        o = inventory_slot_object(m, i);
        if (o) {
            uint16_t r = (uint16_t)(crow + (int8_t)ds[(uint16_t)(ARMOUR_REGION_BY_SLOT + i)]);
            ds[r] = (uint8_t)(ds[r] + armour_value(m, o));
        }
    }
    o = inventory_slot_object(m, (int16_t)((ds[(uint16_t)(rec + 0x64)] & 1) + 7));
    if (o && !((rw(ls, o) & 0x1c0) >> 6) && ((rw(ls, o) & 0x30) >> 4) == 3 && (rw(ls, o) & 0xf) >= 0xb) {
        uint16_t a = armour_value(m, o);
        ds[crow] = (uint8_t)(ds[crow] + a);
        ds[(uint16_t)(crow + 1)] = (uint8_t)(ds[(uint16_t)(crow + 1)] + a);
    }
    ds[(uint16_t)(crow + 0x12)] = ds[(uint16_t)(rec + 0x22)];
    o = inventory_slot_object(m, (int16_t)(8 - (ds[(uint16_t)(rec + 0x64)] & 1)));
    store_obj_far(m, PROPS_OBJECT, o);
    if (o && !(((w0 = rw(ls, o)) & 0x1c0) >> 6) && ((w0 & 0x30) >> 4) < 2) {
        if (!((w0 & 0x30) >> 4)) {
            code = ds[(uint16_t)(MELEE_WEAPON_PROPS + 6 + (w0 & 0xf) * 8)];
            if (code < 3) code = 3;
            else if (code > 5) code = 5;
            set = (int8_t)(code - 3);
        } else if ((w0 & 0xf) > 7) {
            set = -1;
        }
    }
    weapon_set_anim(m, set);
    ds[(uint16_t)(crow + 0x12)] = (uint8_t)(ds[(uint16_t)(crow + 0x12)] + (ds[(uint16_t)(rec + code + 0x21)] >> 1));
    player_derived_state_reset(m);
    for (i = 0; i < 5; i++) {
        if (i == 4) {
            o = rw(ds, CURSOR_OBJECT);
            ww(ds, PROPS_OBJECT, o);
            ww(ds, (uint16_t)(PROPS_OBJECT + 2), rw(ds, (uint16_t)(CURSOR_OBJECT + 2)));
        } else {
            o = inventory_slot_object(m, (int8_t)ds[(uint16_t)(LIGHT_SLOT_INDICES + i)]);
            store_obj_far(m, PROPS_OBJECT, o);
        }
        if (o && ((rw(ls, o) & 0x1f0) >> 4) == 9 && (rw(ls, o) & 0xf) >= 4 && (rw(ls, o) & 0xf) < 8) {
            uint8_t b = ds[(uint16_t)(LIGHT_BURN_RATES + (rw(ls, o) & 0xf) * 2 + 1)];
            if (b > bright) {
                bright = b;
                bright_slot = i;
            }
        }
    }
    ds[(uint16_t)(rec + 0x63)] = (uint8_t)((bright << 4) + bright_slot);
    for (i = 0; (uint16_t)i < ((rw(ds, (uint16_t)(rec + 0x5f)) >> 6) & 0xf); i++) {
        uint16_t e = rw(ds, (uint16_t)(rec + 0x3e + i * 2));
        enchantment_apply(m, (uint8_t)(e & 0xf), (uint8_t)((e & 0xf0) >> 4), &mask, -1);
    }
    for (i = 0; i <= 10; i++) {
        int16_t effect = 0, magnitude = 0;
        int special = 0;
        o = inventory_slot_object(m, i);
        store_obj_far(m, PROPS_OBJECT, o);
        if (!o || !inventory_slot_is_vulnerable(m, (uint16_t)(rw(ls, o) & 0x1ff), i)) continue;
        if (item_enchantment(m, o, &effect, &magnitude, &special) && !special) {
            /* item_clear_broken_flag when it applies: never, it returns 0 */
            enchantment_apply(m, (uint8_t)effect, (uint8_t)magnitude, &mask, i);
        } else if ((rw(ls, o) & 0x1ff) == 0x2f) {
            ds[DRAGON_BOOTS_WORN] = 1;
        }
    }
    apply_active_spell_effects(m, mask);
    shade = (uint8_t)(ds[0x284] ? 6 : (ds[(uint16_t)(rec + 0x63)] & 0xf0) >> 4);
    shade_set_level(m, shade);
    impairment_effect_set(m, ((ds[(uint16_t)(rec + 0x61)] >> 2) & 3) != 0);
    set_movement_state(m, ds[COLLISION_FACE], 1);   /* player_movement_state_refresh */
    ds[PLAYER_IN_LIQUID] = 1;
    apply_movement_mode(m, (int8_t)-1);
}

/* ==== the Avatar's missile weapon ==== */

/* format_article_plural(name, article, plural), in place: a
 * block 4 name is "singular&plural" and "article_noun". The plural form is
 * the part after '&', or the whole with an 's' appended; the singular stops
 * at '&'. Then the article's '_' becomes a space, or without `article` the
 * name starts after it. */
char *format_article_plural(char *s, int article, int plural) {
    char *amp = strchr(s, '&'), *us;
    if (plural) {
        if (amp) s = amp + 1;
        else strcat(s, "s");
    } else if (amp) {
        *amp = '\0';
    }
    us = strchr(s, '_');
    if (us) {
        if (article) *us = ' ';
        else s = us + 1;
    }
    return s;
}

/* format_object_name(dest, obj, article, plural), over an
 * object's word 0 and its byte +0x1a: a class 1 object whose byte is
 * 1..0xef takes block 7's name 0x10 on; anything else its block 4 name
 * through format_article_plural. 0, dest untouched, for an empty name or
 * none given. */
int format_object_name(uw_motion *m, char *dest, size_t cap, uint16_t w0, uint8_t who, int article, int plural) {
    char text[0x100], *name = text;
    if (!m->strings) return 0;
    if (((w0 & 0x1c0) >> 6) == 1 && who > 0 && who < 0xf0) {
        if (uw_strings_by_id(m->strings, (uint16_t)((who + 0x10) | 0xe00), text, (int)sizeof text - 1) < 0 || !*text) return 0;
    } else {
        if (uw_strings_by_id(m->strings, (uint16_t)((w0 & 0x1ff) | 0x800), text, (int)sizeof text - 2) < 0 || !*text) return 0;
        name = format_article_plural(text, article, plural);
    }
    {
        size_t n = strlen(name);
        if (!cap) return 0;
        if (n >= cap) n = cap - 1;
        memcpy(dest, name, n);
        dest[n] = '\0';
    }
    return 1;
}

/* scroll_print of a C string, when the text windows are given. */
void scroll_print(uw_motion *m, const char *text) {
    if (m->scroll) uw_scroll_print(m->scroll, text);
    else UW_NOT_CARRIED(m->not_carried);
}

/* combat_check_for_ammo(launcher): the slot holding the
 * launcher's ammunition, searched to the containers; without, -1 and "Sorry,
 * you have no ", the ammunition's plural name without its article and ".\n"
 * printed one after another. The name is of an object built on the stack,
 * word 0 the ammunition's item id (0x10 on) over whatever the frame held --
 * class 0 for every item id below 0x40, so the byte +0x1a is never read;
 * "UNNAMED" for an empty name. */
int16_t combat_check_for_ammo(uw_motion *m, uint16_t launcher) {
    int16_t slot = -1;
    int8_t ammo = (int8_t)m->ds[(uint16_t)(MISSILE_PROPS_ROW + 2 + launcher * 3)];
    char name[0x40];
    if (inventory_find_by_kind(m, 0, 1, ammo, 4, &slot)) return slot;
    scroll_print(m, "Sorry, you have no ");
    if (!format_object_name(m, name, sizeof name, (uint16_t)((ammo + 0x10) & 0x1ff), 0, 0, 1)) strcpy(name, "UNNAMED");
    scroll_print(m, name);
    scroll_print(m, ".\n");
    return -1;
}

/* missile_release(launcher), from the instructions: the
 * ammunition's missile launched from the player's tile, aimed by the cursor;
 * one taken from its slot gives the missile its word 0 bits 15, 13 and
 * 9..12, its count word, quality and owner and, unless it is class 5 or a
 * props +9 kind 2, its heading, and is freed. No room prints message 0xfe
 * (counted). The bow and the crossbow (9, 10) play sound 9. `bp` is the
 * function's frame. */
void missile_release(uw_motion *m, uint16_t launcher, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    int16_t slot = combat_check_for_ammo(m, launcher), row;
    uint16_t pl = rw(ds, TRACKED_OBJECT), p, t;
    if (slot < 0) return;
    row = (int8_t)ds[(uint16_t)(MISSILE_PROPS_ROW + 2 + launcher * 3)];
    ww(ds, PROJ_SPEED, ds[(uint16_t)(MISSILE_PROPS + row * 3)]);
    ww(ds, PROJ_ITEM, (uint16_t)(row + 0x10));
    ww(ds, PROJ_TARGET_X, (uint16_t)(rw(ls, (uint16_t)(pl + 0x16)) >> 10));
    ww(ds, PROJ_TARGET_Y, (uint16_t)((rw(ls, (uint16_t)(pl + 0x16)) & 0x3f0) >> 4));
    ww(ds, PROJ_HEADING, 1);
    ww(ds, PROJ_FIRER, pl);
    ww(ds, (uint16_t)(PROJ_FIRER + 2), rw(ds, (uint16_t)(TRACKED_OBJECT + 2)));
    projectile_aim_from_cursor(m);
    p = launch_projectile(m, (uint16_t)(bp - 0xa - 4 - 4 - 2));
    if (!p) {
        print_message(m, 0xfe);             /* "You need more space to fire that weapon." */
    } else {
        t = inventory_take_one_from_slot(m, 0, 1, row, slot);
        if (!t) {
            UW_NOT_CARRIED(m->not_carried);
        } else {
            projectile_take_fields(m, p, t);
            uw_motion_obj_free(m, t);
        }
    }
    if (launcher == 9 || launcher == 10) play_sound_effect(m, 9, 0x40, 0);   /* the bow's twang */
}

/* A launched missile made the thing it was launched from, as missile_release
 * and throw_object both copy it: the quantity bit, the count, word 0 bits
 * 9..12 and 13, the +4 low six bits into +8 and the owner; the heading into
 * +0x1a but for class 5 or a property +9 low pair of 2. */
void projectile_take_fields(uw_motion *m, uint16_t p, uint16_t t) {
    uint8_t *ls = m->lseg;
    ww(ls, p, (uint16_t)((rw(ls, p) & 0x7fff) | (rw(ls, t) & 0x8000)));
    ww(ls, (uint16_t)(p + 6), (uint16_t)((rw(ls, (uint16_t)(p + 6)) & 0x3f) | (rw(ls, (uint16_t)(t + 6)) & 0xffc0)));
    ww(ls, p, (uint16_t)((rw(ls, p) & 0xe1ff) | (rw(ls, t) & 0x1e00)));
    ls[(uint16_t)(p + 8)] = (uint8_t)(ls[(uint16_t)(t + 4)] & 0x3f);
    ls[(uint16_t)(p + 6)] = (uint8_t)((ls[(uint16_t)(p + 6)] & 0xc0) | (ls[(uint16_t)(t + 6)] & 0x3f));
    ww(ls, p, (uint16_t)((rw(ls, p) & 0xdfff) | (rw(ls, t) & 0x2000)));
    if (((rw(ls, t) & 0x1c0) >> 6) != 5 && (prop(m, obj_id(m, t), 9) & 3) != 2)
        ls[(uint16_t)(p + 0x1a)] = (uint8_t)((rw(ls, (uint16_t)(t + 2)) & 0x380) >> 7);
}

/* throw_object(obj, verbose): from the Avatar's tile, for a
 * click (event type 1) aimed high enough in the view
 * (projectile_aim_from_cursor), a missile of the thing's item id at speed
 * 0xf -- launch_projectile -- takes its fields and the thing is freed. Not
 * launched, it is set down ahead of the Avatar at its height: its fine
 * position the Avatar's, moved along its heading by the two radii and one
 * -- and, when item_fits_in_tile finds room there, three more, where it is
 * asked again and its answer stands -- into the tile's chain
 * -- a lit light (0x94..0x96) put out -- and placed_object_collision; with
 * no room "There is no space to drop that." and a sound. 1 when it left the
 * hand. `bp` is its frame; its locals x and y at bp - 0xa and - 0xc. */
static int throw_object(uw_motion *m, uint16_t obj, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t pl = rw(ds, TRACKED_OBJECT), p, id, heading;
    uint16_t at_x = (uint16_t)(bp - 0xa), at_y = (uint16_t)(bp - 0xc);
    uint8_t radius;
    int fits;
    ww(ds, PROJ_TARGET_X, (uint16_t)(rw(ls, (uint16_t)(pl + 0x16)) >> 10));
    ww(ds, PROJ_TARGET_Y, (uint16_t)((rw(ls, (uint16_t)(pl + 0x16)) & 0x3f0) >> 4));
    if (rw(ds, (uint16_t)(rw(ds, 0x00e2) + 8)) == 1 && projectile_aim_from_cursor(m)) {
        ww(ds, PROJ_HEADING, 1);
        ww(ds, PROJ_FIRER, pl);
        ww(ds, (uint16_t)(PROJ_FIRER + 2), rw(ds, (uint16_t)(TRACKED_OBJECT + 2)));
        ww(ds, PROJ_ITEM, (uint16_t)(rw(ls, obj) & 0x1ff));
        ww(ds, PROJ_SPEED, 0xf);
        p = launch_projectile(m, (uint16_t)(bp - 0xe - 4 - 4 - 2));
        if (p) {
            projectile_take_fields(m, p, obj);
            uw_motion_obj_free(m, obj);
            return 1;
        }
    }
    ww(ds, at_x, (uint16_t)((rw(ds, PROJ_TARGET_X) << 3) + (rw(ls, (uint16_t)(pl + 2)) >> 13)));
    ww(ds, at_y, (uint16_t)((rw(ds, PROJ_TARGET_Y) << 3) + ((rw(ls, (uint16_t)(pl + 2)) & 0x1c00) >> 10)));
    ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0xff80) | (rw(ls, (uint16_t)(pl + 2)) & 0x7f)));
    id = (uint16_t)(rw(ls, obj) & 0x1ff);
    radius = (uint8_t)((prop(m, obj_id(m, pl), 1) & 7) + (prop(m, id, 1) & 7) + 1);
    heading = (uint16_t)((((rw(ls, (uint16_t)(pl + 2)) & 0x380) >> 7) << 5) + (ls[(uint16_t)(pl + 0x18)] & 0x1f));
    angle_to_offset(m, heading, radius, at_x, at_y);
    fits = item_fits_in_tile(m, id, 0, rs(ds, at_x), rs(ds, at_y), (int16_t)(rw(ls, (uint16_t)(pl + 2)) & 0x7f), 1,
                             radius, (uint16_t)(bp - 0x12 - 14 - 4 - 2));
    if (fits) {
        angle_to_offset(m, heading, 3, at_x, at_y);
        fits = item_fits_in_tile(m, id, 0, rs(ds, at_x), rs(ds, at_y), (int16_t)(rw(ls, (uint16_t)(pl + 2)) & 0x7f), 1,
                                 radius, (uint16_t)(bp - 0x12 - 14 - 4 - 2));
    }
    if (fits) {
        uint16_t tx = (uint16_t)(rs(ds, at_x) >> 3), ty = (uint16_t)(rs(ds, at_y) >> 3);
        uw_objpool pool;
        ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0x1fff) | (uint16_t)(rw(ds, at_x) << 13)));
        ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0xe3ff) | ((rw(ds, at_y) & 7) << 10)));
        pool_from_ds(m, &pool);
        uw_object_list_append(&pool, (uint16_t)(tile_ptr(m, tx, ty) + 2), obj);
        pool_to_ds(m, &pool);
        if ((rw(ls, obj) & 0x1f0) == 0x90 && (rw(ls, obj) & 0xf) > 3 && (rw(ls, obj) & 0xf) < 7)
            ww(ls, obj, (uint16_t)((rw(ls, obj) & 0xfff0) | (((rw(ls, obj) & 0xf) - 4) & 0xf)));
        placed_object_collision(m, obj, tx, ty, 1, (uint16_t)(bp - 0x12 - 10 - 4 - 2));
        return 1;
    }
    print_message(m, 0xfd);                 /* "There is no space to drop that." */
    play_sound_effect(m, 0xf, 0x40, -10);
    return 0;
}

/* inventory_panel_activate(0x17), the view's element: a held
 * thing thrown (throw_object); gone from the hand, a moonstone within sets
 * the record's +0x5e low nibble to the level, held_object_ptr is cleared and
 * player_state_recalc runs; and something held on entry and not now pops the
 * cursor's shape with action_state 0. */
void inventory_panel_activate_view(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t held = rw(ds, 0x5b06);
    int was = held || rw(ds, 0x5b08);
    /* the frame: view_action_dispatch's at 0x9582 past its word, the
     * element pushed, the far call and ours, eight bytes and two registers,
     * then throw_object's three arguments, far call and its own */
    if (was && throw_object(m, held, (uint16_t)(0x9582 - 2 - 2 - 4 - 2 - 8 - 4 - 6 - 4 - 2))) {
        if (object_tree_contains_id(m, held, 0x126)) {
            uint16_t rec = (uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x5e);
            ds[rec] = (uint8_t)((ds[rec] & 0xf0) | (ds[0x7278] & 0xf));
        }
        ww(ds, 0x5b06, 0);
        ww(ds, 0x5b08, 0);
        player_state_recalc(m);
    }
    if (was && !rw(ds, 0x5b06) && !rw(ds, 0x5b08)) {
        cursor_shape_pop(m, 3);
        ww(ds, ACTION_STATE_WORD, 0);
    }
}

/* ==== the pass's input: input_tick's mouse arm ==== */

/* print_message_parts(a, b, c): block 1's messages a, b and c --
 * a negative one left out -- as one scroll_print. */
void print_message_parts(uw_motion *m, int16_t a, int16_t b, int16_t c) {
    char text[0x300], part[0x100];
    int16_t ids[3];
    int k;
    ids[0] = a; ids[1] = b; ids[2] = c;
    text[0] = '\0';
    if (!m->strings) {
        UW_NOT_CARRIED(m->not_carried);
        return;
    }
    for (k = 0; k < 3; k++) {
        if (k && ids[k] < 0) continue;
        if (uw_strings_by_id(m->strings, (uint16_t)(ids[k] | 0x200), part, (int)sizeof part) < 0) part[0] = '\0';
        if (strlen(text) + strlen(part) < sizeof text) strcat(text, part);
    }
    scroll_print(m, text);
}

/* paperdoll_click on from cursor_wait_for_drag's answer of a drag,
 * the pass its loop saw the cursor move more than six:
 * the element and slot again from the event's position; a stack of more
 * than one asks how many (inventory_split_stack, not carried), a container
 * cannot go onto the barter table (message 0xba) nor be lifted while open
 * (its +8 walked from the stack's root along +0); then
 * inventory_pick_up_from_slot, the slot's element redrawn -- from a page
 * slot, every open container's running weight less the thing's, the page
 * refilled and container_view_refresh -- and, something now held, the
 * release wait begun. */
void uw_motion_inventory_drag(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t ev = rw(ds, 0x00e2), o, w0, q;
    int16_t region = inventory_panel_hit_test(m, (int16_t)(rs(ds, ev) + 0xf0), (int16_t)(rs(ds, (uint16_t)(ev + 2)) + 0x52));
    int16_t slot;
    mouse_sample_buttons(m);                /* the loop's last sample, which the drag ends */
    if (region <= 0 || region >= 0x15) {
        UW_NOT_CARRIED(m->not_carried);
        return;
    }
    slot = (int8_t)ds[(uint16_t)(0x1888 + region)];
    o = inventory_slot_object(m, slot);
    if (slot == -1 || !o) {
        UW_NOT_CARRIED(m->not_carried);
        return;
    }
    w0 = rw(ls, o);
    q = (uint16_t)((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff);
    if ((w0 & 0x8000) && !(q & 0x200)) {
        if (q != 1) {
            stack_ask(m, o, 2, slot);
            return;
        }
    }
    inventory_drag_take(m, slot, o, 0, 0);
}

/* The lift past the stack's question: a container's guards, then
 * inventory_pick_up_from_slot (the rest of a split stack staying in the
 * slot) and, something held, the release wait -- or when the question was
 * asked and answered, action state 1 and what was taken left on the cursor
 * for the next click to place. paperdoll_click tests inventory_split_stack's
 * result for null, not for a split: the whole stack taken is
 * the object itself, and stays on the cursor as a part would. */
void inventory_drag_take(uw_motion *m, int16_t slot, uint16_t o, int split, int answered) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w0 = rw(ls, o);
    if ((w0 & 0x8000) && !((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x200)) {
        /* a quantity: no container guard */
    } else if (((w0 & 0x1c0) >> 6) == 2 && !(w0 & 0x30)) {
        if (rw(ds, (uint16_t)(rw(ds, 0x00e2) + 8)) == 4 && (w0 & 0xf) != 0xf) {
            print_message(m, 0xba);
            return;
        }
        {
            uint16_t off = rw(ds, 0x1722), sg = rw(ds, 0x1724);
            while (off | sg) {
                uint8_t *node = far_bytes(m, off, sg, 0xc);
                if (!node) {
                    UW_NOT_CARRIED(m->not_carried);
                    return;
                }
                if (obj_at(m, (uint16_t)((rw(node, 8) >> 6) & 0x3ff)) == o) return;
                off = rw(node, 0);
                sg = rw(node, 2);
            }
        }
    }
    inventory_pick_up_from_slot(m, slot, split);
    if (slot > 0x13) {
        container_stack_less_weight(m, object_weight(m, o));
        container_page_fill(m);
        container_view_refresh(m);
    } else {
        inventory_slot_click(m, (int8_t)ds[(uint16_t)(INVENTORY_CLICK_ORDER + slot)]);
    }
    if (!(rw(ds, 0x5b06) || rw(ds, 0x5b08))) return;
    if (answered) {
        ww(ds, ACTION_STATE_WORD, 1);
        return;
    }
    input_wait_button_release(m, 1);
}

/* paperdoll_click on from cursor_wait_for_drag's answer of no drag,
 * the button come up before the cursor moved six: the
 * release wait, which ends at its first poll, the element under the cursor,
 * and the rest with nothing held on entry -- its argument read off its
 * frame. */
void uw_motion_inventory_click_end(uw_motion *m) {
    uint8_t *ds = m->ds;
    int16_t region;
    input_wait_button_release(m, 1);
    region = inventory_panel_hit_test(m, rs(ds, CURSOR_X), rs(ds, CURSOR_Y));
    paperdoll_click_rest(m, m->drag_param_set ? m->drag_param : rs(ds, 0x9574), region, 0);
}

/* paperdoll_click past the release wait a lift began: the
 * element under the cursor where the button came up, and the rest -- its
 * argument read off its frame, the lifted thing held. */
void uw_motion_inventory_release(uw_motion *m) {
    uint8_t *ds = m->ds;
    int16_t region = inventory_panel_hit_test(m, rs(ds, CURSOR_X), rs(ds, CURSOR_Y));
    paperdoll_click_rest(m, m->drag_param_set ? m->drag_param : rs(ds, 0x9574), region, 1);
}

/* inventory_click_dispatch under panel_inventory_click,
 * the inventory panel's hotspot while panel_mode is 0 (the rune bag's and
 * the stats' not carried): the pick cleared; with action_state 1
 * paperdoll_click(4), with 0 paperdoll_click(0) -- but a right click
 * (the event's flags 2) gets -2, the look, unless use mode is on outside
 * a conversation -- and with a use aimed (2), a page arrow is paperdoll_click(1)
 * and a slot's thing, the pick (inventory_click_take), gets the pending
 * handler (as used on the inventory) and the release wait; nothing there
 * drops the use: the cursor's shape popped, the hand emptied, action_state
 * 0. */
void panel_inventory_click(uw_motion *m) {
    uint8_t *ds = m->ds;
    if (ds[PANEL_MODE] == 1) { rune_bag_click(m); return; }
    if (ds[PANEL_MODE] == 2) { stats_skill_scroll_click(m); return; }
    if (ds[PANEL_MODE]) return;             /* mid-flip (mode 4): no arm takes it */
    ww(ds, CURSOR_PICK_OBJECT, 0);
    ww(ds, (uint16_t)(CURSOR_PICK_OBJECT + 2), 0);
    if (rw(ds, ACTION_STATE_WORD) == 1) {
        paperdoll_click(m, 4);
    } else if (rw(ds, ACTION_STATE_WORD) == 0) {
        uint16_t ev = rw(ds, 0x00e2), flags = rw(ds, (uint16_t)(ev + 6));
        if (rw(ds, 0x5b06) || rw(ds, 0x5b08)) {
            paperdoll_click(m, 0);
        } else if (flags == 1 || flags == 3) {
            paperdoll_click(m, 0);
        } else if (flags == 2) {
            paperdoll_click(m, (int16_t)(rw(ds, 0x268c) == 1 && rw(ds, (uint16_t)(ev + 8)) != 4 ? 0 : -2));
        }
    } else if (rw(ds, ACTION_STATE_WORD) == 2) {
        uint16_t ev = rw(ds, 0x00e2), o;
        int16_t region = inventory_panel_hit_test(m, (int16_t)(rs(ds, ev) + 0xf0), (int16_t)(rs(ds, (uint16_t)(ev + 2)) + 0x52));
        if (region == 0x15 || region == 0x16) {
            paperdoll_click(m, 1);
            return;
        }
        o = inventory_click_take(m);
        store_obj_far(m, CURSOR_PICK_OBJECT, o);
        if (o) {
            pending_action_run(m, o, 1);
            input_wait_button_release(m, 1);
        } else {
            cursor_shape_pop(m, 3);
            ww(ds, 0x5b06, 0);
            ww(ds, 0x5b08, 0);
            ww(ds, ACTION_STATE_WORD, 0);
        }
    }
}
