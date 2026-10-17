/* SPDX-License-Identifier: MIT */
/* placing a thing in the pack: inventory_place_object and its slots,
 * stacking, the recipes of combine, container_insert_object, the slot
 * swap and combine, and the take from a slot.
 *
 * One of src/uw_motion*.c: see uw_motion.h and uw_motion_int.h. */
#include "uw_motion_int.h"

/* inventory_panel_hit_test(x, y): in a conversation (event type
 * 4) the barter area, 0x18; otherwise the live view, 0x17; else the first of
 * the panel's elements 0..0x16 (fourteen bytes each: x0, y1, x1, y0)
 * holding the point; -1 for none. */
int16_t inventory_panel_hit_test(uw_motion *m, int16_t x, int16_t y) {
    uint8_t *ds = m->ds;
    int16_t i;
    if (rw(ds, (uint16_t)(rw(ds, 0x00e2) + 8)) == 4) {
        if (x > 0x8b && x < 0xc1 && y > 0x98 && y < 0xbe) return 0x18;
    } else if (rs(ds, 0x7282) < x && x < rs(ds, 0x7282) + rs(ds, 0x7364)
               && rs(ds, 0x735e) < y && y < rs(ds, 0x735e) + rs(ds, 0x7280)) {
        return 0x17;
    }
    for (i = 0; i <= 0x16; i++) {
        uint16_t e = (uint16_t)(i * 0xe);
        if (rs(ds, (uint16_t)(0x172a + e)) <= x && x <= rs(ds, (uint16_t)(0x172e + e))
            && rs(ds, (uint16_t)(0x1730 + e)) <= y && y <= rs(ds, (uint16_t)(0x172c + e)))
            return i;
    }
    return -1;
}

/* inventory_place_object's container arm, for a backpack
 * slot holding container `dest` (ids 0x80..0x8f): the
 * thing's weight with the container's contents' (object_chain_weight) over
 * its capacity (container_props, by its low nibble, three bytes;
 * 0 none) is "The ", its name ("UNNAMED") and " is too full.\n", and 0.
 * What it accepts, the word after: negative, anything the properties let be
 * carried (+3 bit 5); below 0x200 that item id alone, else message 0xf8 and
 * 0; 0x200 a rune (class 3 subclass 3, or subclass 2 above nibble 7), else
 * message 0xf7; 0x201 ammunition (class 0 subclass 1 below nibble 3), 0x202
 * a scroll (class 4 subclass 3 from nibble 8), 0x203 food (class 2
 * subclass 3 or ids 0xce, 0xcf, 0x92, 0x125, 0x11b, 0xd9), else message
 * 0xf8 and 0. For a page slot the open containers' running weights (+0xa)
 * are held to their capacities too, each with the thing's weight added, and
 * one over is the same message about `dest`. */
static int16_t inventory_place_in_container(uw_motion *m, uint16_t obj, uint16_t dest, int16_t slot) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w0 = rw(ls, obj), id = (uint16_t)(w0 & 0x1ff), dn = (uint16_t)(rw(ls, dest) & 0xf);
    uint16_t cls = (uint16_t)((w0 & 0x1c0) >> 6), sub = (uint16_t)((w0 & 0x30) >> 4), nib = (uint16_t)(w0 & 0xf);
    uint16_t w = object_weight(m, obj);
    uint8_t cap;
    int16_t acc;
    int ok, fits = 1;
    if (slot > 0x13) {
        uint16_t off = rw(ds, CONTAINER_STACK_TOP), sg = rw(ds, (uint16_t)(CONTAINER_STACK_TOP + 2));
        while (off | sg) {
            uint8_t *node = far_bytes(m, off, sg, 0xc);
            uint8_t c;
            if (!node) {
                UW_NOT_CARRIED(m->not_carried);
                return 0;
            }
            c = ds[(uint16_t)(0x5b0a + (rw(ls, obj_at(m, (uint16_t)((rw(node, 8) >> 6) & 0x3ff))) & 0xf) * 3)];
            if (c && (int16_t)(rw(node, 0xa) + w) > (int16_t)c) fits = 0;
            off = rw(node, 4);
            sg = rw(node, 6);
        }
    }
    object_chain_weight(m, (uint16_t)(dest + 6), &w);
    cap = ds[(uint16_t)(0x5b0a + dn * 3)];
    if (!fits || (cap && (int16_t)w > (int16_t)cap)) {
        char buf[0x32], part[0x20];
        if (!format_object_name(m, buf, sizeof buf, rw(ls, dest), ls[(uint16_t)(dest + 0x1a)], 0, 0))
            ds_text(m, 0x18d3, buf, sizeof buf);                        /* "UNNAMED" */
        scroll_print(m, ds_text(m, 0x18db, part, sizeof part));          /* "The " */
        scroll_print(m, buf);
        scroll_print(m, ds_text(m, 0x18e0, part, sizeof part));          /* " is too full.\n" */
        return 0;
    }
    acc = rs(ds, (uint16_t)(0x5b0b + dn * 3));
    if (acc < 0) return (int16_t)((prop(m, id, 3) >> 5) & 1);
    if (acc < 0x200) {
        if (id != (uint16_t)acc) print_message(m, 0xf8);
        return id == (uint16_t)acc;
    }
    switch (acc - 0x200) {
    case 0:
        if (cls == 3 && (sub == 3 || (sub == 2 && nib > 7))) return 1;
        print_message(m, 0xf7);             /* "You can only put runes in the rune bag." */
        return 0;
    case 1: ok = cls == 0 && sub == 1 && nib < 3; break;
    case 2: ok = cls == 4 && sub == 3 && nib >= 8; break;
    case 3:
        ok = (cls == 2 && sub == 3) || id == 0xce || id == 0xcf || id == 0x92 || id == 0x125 || id == 0x11b || id == 0xd9;
        break;
    default: ok = 0;
    }
    if (!ok) print_message(m, 0xf8);
    return (int16_t)ok;
}

/* inventory_place_object(obj, slot), as far
 * as the paperdoll's five slots and the backpack's (5..0x12 but the rings' 9
 * and 10 and the weapon hand for a weapon) take it: the object made
 * the class accessors' current object. A paperdoll slot (0..4)
 * takes armour only -- class 0 from subclass 2, ids 0x20..0x3f, whose
 * armour properties' (obj_props_for_object's row) +3 names the
 * slot by a table: the head 8, the chest 1, the hands 4, the
 * legs 3, the feet 5; anything else is refused (0) but on the head, where
 * use_object(player, it, 0) eats or drinks it and a use answers -1. A ring
 * slot (9, 10) takes a ring only -- class 0 from subclass 2 with armour
 * property +3 of 9. A melee weapon (class 0 subclass 0) into the weapon
 * hand (8 less the record's +0x64 bit 0) is refused over one of its own id,
 * or as a stack of more than one, and goes on to the tests below. A lit light
 * (class 2 subclass 1, nibble 4..7) is taken by a light slot;
 * anywhere else it is put out (nibble less four) and placed as that, lit
 * again when refused. A negative slot takes nothing. Slot 0x13,
 * the open container's own: with the root alone open, 1 while a backpack
 * slot (0xb..0x12) is empty, else message 0x102 and 0; nested, the container
 * one out is the destination. A page slot's destination is the container in
 * it, or else the open one. A container destination (ids 0x80..0x8f)
 * answers through inventory_place_in_container; otherwise -- the slot empty
 * or holding anything else -- the item's properties +3 bit 5, whether it may
 * be carried. */
static int16_t inventory_place_object(uw_motion *m, uint16_t obj, int16_t slot) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w0 = rw(ls, obj), id = (uint16_t)(w0 & 0x1ff), in;
    uint16_t cls = (uint16_t)((w0 & 0x1c0) >> 6), sub = (uint16_t)((w0 & 0x30) >> 4), nib = (uint16_t)(w0 & 0xf);
    ww(ds, 0x5b6a, obj);
    ww(ds, 0x5b6c, rw(ds, 0x5b08));         /* the held thing's segment: its one caller's */
    if (slot >= 0 && slot < 5) {
        static const uint8_t worn_at[5] = { 8, 1, 4, 3, 5 };
        if (cls != 0) {
            if (slot != 0) return 0;
            return use_object(m, rw(ds, TRACKED_OBJECT), obj, 0) > 0 ? -1 : 0;
        }
        if (sub < 2) return 0;
        return ds[(uint16_t)(0x59f2 + (id - 0x20) * 4 + 3)] == worn_at[slot];
    }
    if (slot == 0x13) {
        uint8_t *top, *prev;
        int16_t i;
        if (!(rw(ds, CONTAINER_STACK_TOP) | rw(ds, (uint16_t)(CONTAINER_STACK_TOP + 2)))) return 0;
        top = far_bytes(m, rw(ds, CONTAINER_STACK_TOP), rw(ds, (uint16_t)(CONTAINER_STACK_TOP + 2)), 0xc);
        if (!top) {
            UW_NOT_CARRIED(m->not_carried);
            return 0;
        }
        if (!(rw(top, 4) | rw(top, 6))) {
            for (i = 0xb; i <= 0x12; i++)
                if (!((rw(ds, (uint16_t)(INVENTORY_SLOTS + i * 2)) >> 6) & 0x3ff)) break;
            if (i > 0x12) print_message(m, 0x102);
            return i <= 0x12;
        }
        prev = far_bytes(m, rw(top, 4), rw(top, 6), 0xc);
        if (!prev) {
            UW_NOT_CARRIED(m->not_carried);
            return 0;
        }
        in = obj_at(m, (uint16_t)((rw(prev, 8) >> 6) & 0x3ff));
    } else if (slot > 0x13) {
        in = inventory_slot_object(m, slot);
        if (!in || ((rw(ls, in) & 0x1f0) >> 4) != 8) in = inventory_slot_object(m, 0x13);
    } else {
        in = inventory_slot_object(m, slot);
    }
    if (slot < 5) return 0;
    if (slot == 9 || slot == 10) {
        if (cls != 0 || sub < 2) return 0;
        return ds[(uint16_t)(0x59f2 + (id - 0x20) * 4 + 3)] == 9;
    }
    if ((8 - (ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x64)] & 1)) == slot && cls == 0 && sub == 0) {
        uint16_t q = (uint16_t)((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x3ff);
        if (in && (rw(ls, in) & 0x1ff) == id) return 0;
        if ((w0 & 0x8000) && !(q & 0x200) && q > 1) return 0;
    } else if (cls == 2 && sub == 1 && nib >= 4 && nib < 8) {
        int16_t i;
        for (i = 0; i < 4; i++)
            if ((int8_t)ds[(uint16_t)(0x171e + i)] == slot) return 1;
        ww(ls, obj, (uint16_t)((rw(ls, obj) & 0xfff0) | ((nib - 4) & 0xf)));
        if (inventory_place_object(m, obj, slot)) return 1;
        ww(ls, obj, (uint16_t)((rw(ls, obj) & 0xfff0) | nib));
        return 0;
    }
    if (in && ((rw(ls, in) & 0x1f0) >> 4) == 8) return inventory_place_in_container(m, obj, in, slot);
    return (int16_t)((prop(m, id, 3) >> 5) & 1);
}

/* inventory_add_object(obj, slot): inventory_place_object's
 * answer (slot -1 needs none); placed, the thing's index into the slot's
 * link (not for slot -1), onto the end of the parent's contents and its
 * weight onto player_carried_weight. The parent is the Avatar, or for a slot
 * above 0x12 the open container (the top node's +8), when every open
 * container's running weight (+0xa) gains the weight too. player_state_recalc
 * runs either way. */
int inventory_add_object(uw_motion *m, uint16_t obj, int16_t slot) {
    uint8_t *ds = m->ds;
    int ok = 0;
    if (slot == -1 || inventory_place_object(m, obj, slot) >= 1) {
        uint16_t w = object_weight(m, obj), link = (uint16_t)(rw(ds, TRACKED_OBJECT) + 6);
        uw_objpool pool;
        if (slot > 0x12) {
            uint16_t off = rw(ds, CONTAINER_STACK_TOP), sg = rw(ds, (uint16_t)(CONTAINER_STACK_TOP + 2));
            uint8_t *top = far_bytes(m, off, sg, 0xc);
            if (!top) {
                UW_NOT_CARRIED(m->not_carried);
                player_state_recalc(m);
                return 0;
            }
            link = (uint16_t)(obj_at(m, (uint16_t)((rw(top, 8) >> 6) & 0x3ff)) + 6);
            container_stack_less_weight(m, (uint16_t)-w);
        }
        if (slot >= 0) {
            uint16_t at = (uint16_t)(INVENTORY_SLOTS + slot * 2);
            ww(ds, at, (uint16_t)((rw(ds, at) & 0x3f) | (obj_index_of(m, obj) << 6)));
        }
        pool_from_ds(m, &pool);
        uw_object_list_append(&pool, link, obj);
        pool_to_ds(m, &pool);
        ww(ds, CARRIED_WEIGHT, (uint16_t)(rw(ds, CARRIED_WEIGHT) + w));
        ok = 1;
    }
    player_state_recalc(m);
    return ok;
}

/* inventory_place_in_slot(obj, slot): not slot 0x13; placed by
 * inventory_add_object, the slot's element redrawn -- a page slot's page
 * refilled and container_view_refresh instead. 1 when placed. */
static int inventory_place_in_slot(uw_motion *m, uint16_t obj, int16_t slot) {
    uint8_t *ds = m->ds;
    if (slot == 0x13 || !inventory_add_object(m, obj, slot)) return 0;
    if (slot < 0x13) {
        inventory_slot_click(m, (int8_t)ds[(uint16_t)(0x186c + slot)]);
    } else {
        container_page_fill(m);
        container_view_refresh(m);
    }
    return 1;
}

/* objects_can_stack(a, b), from the instructions: the same item
 * id; neither with contents or a special link (not a quantity with a link,
 * or word 3's bit 15); not a single item or a container by properties +3
 * bits 6..7 (1, 3); for ids 0x100..0x10f the same +6 low six bits; fewer
 * than 999 together; and then ids 0x10..0x12 always, the rest with their
 * qualities in the same sixteen, both non-zero or equal. */
static int objects_can_stack(uw_motion *m, uint16_t a, uint16_t b) {
    uint8_t *ls = m->lseg;
    uint16_t wa = rw(ls, a), wb = rw(ls, b), qa, qb;
    uint16_t la = (uint16_t)((rw(ls, (uint16_t)(a + 6)) >> 6) & 0x3ff), lb = (uint16_t)((rw(ls, (uint16_t)(b + 6)) >> 6) & 0x3ff);
    uint8_t kind = (uint8_t)((prop(m, (uint16_t)(wa & 0x1ff), 3) >> 6) & 3);
    if ((wa & 0x1ff) != (wb & 0x1ff)) return 0;
    if ((!(wa & 0x8000) && la) || (!(wb & 0x8000) && lb)) return 0;
    if ((la & 0x200) || (lb & 0x200) || kind == 1 || kind == 3) return 0;
    if (((wa & 0x1f0) >> 4) == 0x10 && (ls[(uint16_t)(a + 6)] & 0x3f) != (ls[(uint16_t)(b + 6)] & 0x3f)) return 0;
    if (la + lb >= 0x3e7) return 0;
    if ((wa & 0x1ff) >= 0x10 && (wa & 0x1ff) <= 0x12) return 1;
    qa = (uint16_t)(ls[(uint16_t)(a + 4)] & 0x3f);
    qb = (uint16_t)(ls[(uint16_t)(b + 4)] & 0x3f);
    if ((qa >> 4) != (qb >> 4)) return 0;
    return (qa && qb) || qa == qb;
}

/* combine_find_recipe(a, b), from the instructions: -1 when
 * either is a stack of more than one or has contents or a special link;
 * otherwise the first of the ten recipes (three words: two
 * ingredients' item ids, the product's) whose ingredients are the two, in
 * either order, with bit 15 on one of the ingredient words, or -1. Its three
 * debug prints are left out. */
static int16_t combine_find_recipe(uw_motion *m, uint16_t a, uint16_t b) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t o[2], ia, ib;
    int16_t i, k;
    o[0] = a;
    o[1] = b;
    for (k = 0; k < 2; k++) {
        uint16_t w0 = rw(ls, o[k]), l = (uint16_t)((rw(ls, (uint16_t)(o[k] + 6)) >> 6) & 0x3ff);
        if ((w0 & 0x8000) ? l > 1 : l != 0) return -1;
    }
    ia = (uint16_t)(rw(ls, a) & 0x1ff);
    ib = (uint16_t)(rw(ls, b) & 0x1ff);
    for (i = 0; i < 10; i++) {
        uint16_t r0 = rw(ds, (uint16_t)(0x48c6 + i * 6)), r1 = rw(ds, (uint16_t)(0x48c8 + i * 6));
        if (!((r0 | r1) & 0x8000)) continue;
        if (((r0 & 0x1ff) == ia && (r1 & 0x1ff) == ib) || ((r0 & 0x1ff) == ib && (r1 & 0x1ff) == ia)) return i;
    }
    return -1;
}

/* combine_ingredient_consumed(obj, recipe): the recipe's
 * ingredient word naming the object's item id -- the first, else the second
 * -- has bit 15 when the combination uses it up. */
static int combine_ingredient_consumed(uw_motion *m, uint16_t obj, int16_t recipe) {
    uint16_t w = rw(m->ds, (uint16_t)(0x48c6 + recipe * 6));
    if ((w & 0x1ff) != (rw(m->lseg, obj) & 0x1ff)) w = rw(m->ds, (uint16_t)(0x48c8 + recipe * 6));
    return (w & 0x8000) != 0;
}

/* container_insert_object(obj, slot), from the instructions:
 * refused by inventory_place_object, 0. The destination is the slot's
 * container -- for slot 0x13, the open container's own, the Avatar when the
 * root alone is open (and then the first empty backpack slot 0xb..0x12 takes
 * it; none, 0) or else the container one out. The rune bag (0x8f) asks
 * 5a0b's rune test (rune_bag_add). Otherwise its weight onto the carried
 * weight, and onto the running weight (+0xa) of every open container from
 * the one out (slot 0x13) or the top (a page slot) back; then along the
 * destination's contents the first thing it stacks with takes it -- a
 * quantity of 1 made first, the counts added, the qualities averaged, the
 * thing freed -- or, none, it goes onto the end of the contents (and into
 * that backpack slot). The container open in the panel (container_stack_top's
 * +8) refills its page and redraws elements 0x0c..0x13; any other redraws the
 * weight left, the font reopened when it did. A lit light put in is put out
 * (0x94..0x97 less four). 1.
 * With no container open, the original compares the index with a word of
 * the interrupt table's; the port takes it as no match. */
static int container_insert_object(uw_motion *m, uint16_t obj, int16_t slot) {
    uint8_t *ds = m->ds, *ls = m->lseg, *top = NULL, *node = NULL;
    uw_objpool pool;
    uint16_t dest, c, w;
    int16_t into = -1;
    if (!inventory_place_object(m, obj, slot)) return 0;
    if (rw(ds, CONTAINER_STACK_TOP) | rw(ds, (uint16_t)(CONTAINER_STACK_TOP + 2))) {
        top = far_bytes(m, rw(ds, CONTAINER_STACK_TOP), rw(ds, (uint16_t)(CONTAINER_STACK_TOP + 2)), 0xc);
        if (!top) {
            UW_NOT_CARRIED(m->not_carried);
            return 0;
        }
    }
    if (slot == 0x13) {
        if (!top) {
            UW_NOT_CARRIED(m->not_carried);
            return 0;
        }
        if (!(rw(top, 4) | rw(top, 6))) {
            for (into = 0xb; into <= 0x12; into++)
                if (!((rw(ds, (uint16_t)(INVENTORY_SLOTS + into * 2)) >> 6) & 0x3ff)) break;
            if (into > 0x12) return 0;
            dest = rw(ds, TRACKED_OBJECT);
        } else {
            node = far_bytes(m, rw(top, 4), rw(top, 6), 0xc);
            if (!node) {
                UW_NOT_CARRIED(m->not_carried);
                return 0;
            }
            dest = obj_at(m, (uint16_t)((rw(node, 8) >> 6) & 0x3ff));
        }
    } else {
        dest = inventory_slot_object(m, slot);
        if (slot > 0x13) node = top;
    }
    if ((rw(ls, dest) & 0x1ff) == 0x8f) {
        /* the rune bag (5a0b's rune_bag_add): the rune's bit set and the
         * stone freed, or "You can only put runes in the rune bag." */
        if (rune_bag_add(m, obj)) return 1;
        print_message(m, 0xf7);
        return 0;
    }
    w = object_weight(m, obj);
    ww(ds, CARRIED_WEIGHT, (uint16_t)(rw(ds, CARRIED_WEIGHT) + w));
    while (node) {
        ww(node, 0xa, (uint16_t)(rw(node, 0xa) + w));
        if (!(rw(node, 4) | rw(node, 6))) break;
        node = far_bytes(m, rw(node, 4), rw(node, 6), 0xc);
        if (!node) UW_NOT_CARRIED(m->not_carried);
    }
    for (c = deref_link(m, (uint16_t)(dest + 6)); c; c = deref_link(m, (uint16_t)(c + 4))) {
        int16_t count;
        if (!objects_can_stack(m, obj, c)) continue;
        if (!(rw(ls, c) & 0x8000)) {
            ww(ls, c, (uint16_t)(rw(ls, c) | 0x8000));
            ww(ls, (uint16_t)(c + 6), (uint16_t)((rw(ls, (uint16_t)(c + 6)) & 0x3f) | 0x40));
        }
        count = (rw(ls, obj) & 0x8000) ? (int16_t)((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x3ff) : 1;
        ww(ls, (uint16_t)(c + 6), (uint16_t)((rw(ls, (uint16_t)(c + 6)) & 0x3f)
                                             | ((((rw(ls, (uint16_t)(c + 6)) >> 6) & 0x3ff) + count) & 0x3ff) << 6));
        ls[(uint16_t)(c + 4)] = (uint8_t)((ls[(uint16_t)(c + 4)] & 0xc0)
                                          | (((ls[(uint16_t)(c + 4)] & 0x3f) + (ls[(uint16_t)(obj + 4)] & 0x3f)) >> 1 & 0x3f));
        pool_from_ds(m, &pool);
        uw_obj_free(&pool, obj);
        pool_to_ds(m, &pool);
        break;
    }
    if (!c) {
        pool_from_ds(m, &pool);
        uw_object_list_append(&pool, (uint16_t)(dest + 6), obj);
        pool_to_ds(m, &pool);
        if (into >= 0)
            ww(ds, (uint16_t)(INVENTORY_SLOTS + into * 2), (uint16_t)((rw(ds, (uint16_t)(INVENTORY_SLOTS + into * 2)) & 0x3f)
                                                                     | (obj_index_of(m, obj) & 0x3ff) << 6));
    }
    if (top && obj_index_of(m, dest) == ((rw(top, 8) >> 6) & 0x3ff)) {
        container_page_fill(m);
        inventory_panel_redraw(m, 0xc, 0x13);
    } else if (inventory_draw_weight_left(m, 1)) {
        ds[FONT_LOADED] = 1;                /* font_open(font5x6p.sys) */
    }
    if ((rw(ls, obj) & 0x1ff) >= 0x94 && (rw(ls, obj) & 0x1ff) < 0x98)
        ww(ls, obj, (uint16_t)((rw(ls, obj) & 0xfff0) | (((rw(ls, obj) & 0xf) - 4) & 0xf)));
    return 1;
}

/* container_slot_swap(obj, slot), from the instructions: the
 * held thing and a page slot's exchanged. The link that names the slot's
 * thing is found along the open container's contents (the top node's +8;
 * not there, 0). The slot's thing goes onto the cursor
 * (inventory_pick_up_from_slot); the held one, if inventory_place_object
 * takes it, goes in at that link and into the slot -- refused, it is held
 * again (cursor_shape_pop(3), its image pushed) and the other goes back
 * instead. Every open container's running weight gains the difference, the
 * carried weight the thing put in, then player_state_recalc,
 * container_page_fill and the slot's element redrawn. 1 when swapped. */
static int container_slot_swap(uw_motion *m, uint16_t obj, int16_t slot) {
    uint8_t *ds = m->ds, *ls = m->lseg, *top;
    uint16_t target = inventory_slot_object(m, slot), link, p, wd;
    uw_objpool pool;
    int ok = 1;
    int8_t e = (int8_t)ds[(uint16_t)(INVENTORY_CLICK_ORDER + slot)];
    top = far_bytes(m, rw(ds, CONTAINER_STACK_TOP), rw(ds, (uint16_t)(CONTAINER_STACK_TOP + 2)), 0xc);
    if (!top) {
        UW_NOT_CARRIED(m->not_carried);
        return 0;
    }
    link = (uint16_t)(obj_at(m, (uint16_t)((rw(top, 8) >> 6) & 0x3ff)) + 6);
    for (;;) {
        p = deref_link(m, link);
        if (p == target) break;
        if (!p) return 0;
        link = (uint16_t)(p + 4);
    }
    inventory_pick_up_from_slot(m, slot, 0);
    if (!inventory_place_object(m, obj, slot)) {
        ww(ds, 0x5b06, obj);
        ww(ds, 0x5b08, rw(ds, (uint16_t)(MOBILE_BASE + 2)));
        cursor_shape_pop(m, 3);
        cursor_shape_push(m, (uint16_t)(rw(ls, obj) & 0x1ff));
        ok = 0;
        obj = target;
    }
    pool_from_ds(m, &pool);
    uw_object_list_insert(&pool, link, obj);
    pool_to_ds(m, &pool);
    ww(ds, (uint16_t)(INVENTORY_SLOTS + slot * 2), (uint16_t)((rw(ds, (uint16_t)(INVENTORY_SLOTS + slot * 2)) & 0x3f)
                                                            | (obj_index_of(m, obj) & 0x3ff) << 6));
    wd = (uint16_t)(object_weight(m, obj) - object_weight(m, target));
    container_stack_less_weight(m, (uint16_t)-wd);
    ww(ds, CARRIED_WEIGHT, (uint16_t)(rw(ds, CARRIED_WEIGHT) + object_weight(m, obj)));
    player_state_recalc(m);
    container_page_fill(m);
    inventory_panel_redraw(m, e, e);
    return ok;
}

/* inventory_slot_combine(held, slot), from the instructions: a
 * held thing dropped onto a filled slot. A container there takes it
 * (container_insert_object) and player_state_recalc runs. Two
 * that stack, and a slot inventory_place_object accepts, merge: the held
 * one's count (1 for a single thing) onto the slot's, which becomes a
 * quantity of 1 first if it was not one, their weight -- properties +1's
 * high twelve bits a thing -- onto the carried weight (and every open
 * container's running weight, for a page slot), player_state_recalc, the qualities averaged, the
 * held one freed, and 1. Two that a recipe combines make its product
 * (object_create of the recipe's third word; none, 0): a held ingredient
 * the recipe uses up is removed and the product goes onto the cursor in its
 * place (action_state 1, the cursor popped and the product's image pushed);
 * a slot ingredient used up has the product placed in the slot unless the
 * cursor took it, then is taken out whole (inventory_remove_all) and
 * removed. Else
 * they swap below slot 0x13: the slot's object taken out
 * (inventory_take_object with no count), the held one placed -- or, refused,
 * the other put back -- and whatever is held then the cursor's shape
 * (cursor_shape_pop(0), then its item id pushed); an open container's slots
 * swap through container_slot_swap. Either way the slot's element redrawn,
 * and 0. */
static int inventory_slot_combine(uw_motion *m, uint16_t held, int16_t slot) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t o = inventory_slot_object(m, slot);
    int ok = 0;
    int16_t recipe;
    if (((rw(ls, o) & 0x1c0) >> 6) == 2 && !(rw(ls, o) & 0x30)) {
        ok = container_insert_object(m, held, slot);
        player_state_recalc(m);
        return ok;
    }
    if (objects_can_stack(m, held, o)) {
        uw_objpool pool;
        int16_t count, w;
        if (inventory_place_object(m, held, slot) != 1) return 0;
        count = (rw(ls, held) & 0x8000) ? (int16_t)((rw(ls, (uint16_t)(held + 6)) >> 6) & 0x3ff) : 1;
        if (!(rw(ls, o) & 0x8000)) {
            ww(ls, o, (uint16_t)(rw(ls, o) | 0x8000));
            ww(ls, (uint16_t)(o + 6), (uint16_t)((rw(ls, (uint16_t)(o + 6)) & 0x3f) | 0x40));
        }
        w = (int16_t)((int16_t)((rw(ds, (uint16_t)(0x5b6f + (rw(ls, held) & 0x1ff) * 11)) >> 4) & 0xfff) * count);
        if (slot > 0x13) container_stack_less_weight(m, (uint16_t)-w);
        ww(ds, CARRIED_WEIGHT, (uint16_t)(rw(ds, CARRIED_WEIGHT) + w));
        player_state_recalc(m);
        ww(ls, (uint16_t)(o + 6), (uint16_t)((rw(ls, (uint16_t)(o + 6)) & 0x3f)
                                             | ((((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff) + count) & 0x3ff) << 6));
        ls[(uint16_t)(o + 4)] = (uint8_t)((ls[(uint16_t)(o + 4)] & 0xc0)
                                          | (((ls[(uint16_t)(o + 4)] & 0x3f) + (ls[(uint16_t)(held + 4)] & 0x3f)) >> 1 & 0x3f));
        pool_from_ds(m, &pool);
        uw_obj_free(&pool, held);
        pool_to_ds(m, &pool);
        ok = 1;
    } else if ((recipe = combine_find_recipe(m, held, o)) >= 0) {
        /* combine_spawn_product: object_create of the recipe's
         * third word */
        uint16_t product = create_object(m, rw(ds, (uint16_t)(0x48ca + recipe * 6)), 0);
        int gone;
        if (!product) return 0;
        gone = combine_ingredient_consumed(m, held, recipe);
        if (gone) {
            object_remove(m, 0, held, 1);
            ww(ds, 0x5b06, product);
            ww(ds, 0x5b08, rw(ds, (uint16_t)(MOBILE_BASE + 2)));
            ww(ds, ACTION_STATE_WORD, 1);
            cursor_shape_pop(m, 3);
            cursor_shape_push(m, (uint16_t)(rw(ls, product) & 0x1ff));
        }
        if (combine_ingredient_consumed(m, o, recipe)) {
            if (!gone) inventory_place_in_slot(m, product, slot);
            inventory_remove_quantity(m, o, -1);         /* inventory_remove_all */
            object_remove(m, 0, o, 1);
        }
    } else if (slot > 0x12) {
        container_slot_swap(m, held, slot);
    } else {
        uint16_t out = inventory_unlink_object(m, -1, -1, -1, slot, 0);
        player_state_recalc(m);
        if (!inventory_place_in_slot(m, held, slot)) {
            inventory_place_in_slot(m, out, slot);
        } else {
            ww(ds, 0x5b06, out);
            ww(ds, 0x5b08, out ? rw(ds, (uint16_t)(MOBILE_BASE + 2)) : 0);
        }
        if (rw(ds, 0x5b06) || rw(ds, 0x5b08)) {
            cursor_shape_pop(m, 0);
            cursor_shape_push(m, (uint16_t)(rw(ls, rw(ds, 0x5b06)) & 0x1ff));
        }
    }
    inventory_slot_click(m, (int8_t)ds[(uint16_t)(0x186c + slot)]);
    return ok;
}

/* inventory_drop_on_slot(slot): the held thing onto a filled
 * slot through inventory_slot_combine, into an empty one through
 * inventory_place_in_slot, and the hand emptied when either says so. */
void inventory_drop_on_slot(uw_motion *m, int16_t slot) {
    uint8_t *ds = m->ds;
    int ok;
    if ((rw(ds, (uint16_t)(INVENTORY_SLOTS + slot * 2)) >> 6) & 0x3ff)
        ok = inventory_slot_combine(m, rw(ds, 0x5b06), slot);
    else
        ok = inventory_place_in_slot(m, rw(ds, 0x5b06), slot);
    if (ok) {
        ww(ds, 0x5b06, 0);
        ww(ds, 0x5b08, 0);
    }
}

/* cursor_wait_for_drag(pump) -> 1 when the cursor moved more
 * than six from where the button went down, 0 when it simply came up: the
 * port makes the first sample's writes, which with no button down ends it. */
int cursor_wait_for_drag(uw_motion *m, int pump) {
    (void)pump;
    if (!mouse_sample_buttons(m)) return 0;
    UW_NOT_CARRIED(m->not_carried);
    return 0;
}

/* inventory_click_take(2): the object in the slot the event's
 * position names (inventory_panel_hit_test through inventory_search_order;
 * inventory_slot_object_2, a byte-for-byte copy of
 * inventory_slot_object), or 0 past 0x13. Every caller passes 2, so its
 * other arm -- inventory_take_all_from_slot, the slot emptied
 * and redrawn -- is never taken; and inventory_find_free_slot,
 * the first empty pack slot from 5, is referenced by nothing. */
uint16_t inventory_click_take(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t ev = rw(ds, 0x00e2);
    int16_t region = inventory_panel_hit_test(m, (int16_t)(rs(ds, ev) + 0xf0), (int16_t)(rs(ds, (uint16_t)(ev + 2)) + 0x52));
    if (region < 0 || region > 0x13) return 0;
    return inventory_slot_object(m, (int8_t)ds[(uint16_t)(INVENTORY_SEARCH_ORDER + region)]);
}

/* inventory_remove_quantity(obj, count), from the instructions:
 * 1 when the Avatar had it, else 0. In one of the 28 slots (the link's index
 * its own): inventory_take_object -- inventory_unlink_object with the count,
 * then player_state_recalc -- and below 0x13 the slot's element redrawn; from
 * 0x13 on the page refilled (container_page_fill), container_view_refresh,
 * and its weight, taken before the unlink, off every open container's.
 * Otherwise found in the Avatar's
 * contents by object_find_link, which leaves the chain's head in
 * object_find_list: a stack of more than `count` split, a copy
 * after it keeping the rest and it the count; it out of that chain, its
 * weight -- taken before the split -- off the carried weight, element 0x13
 * redrawn and player_state_recalc. */
int inventory_remove_quantity(uw_motion *m, uint16_t obj, int16_t count) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w = object_weight(m, obj), idx = obj_index_of(m, obj), q;
    uw_objpool pool;
    int16_t slot;
    for (slot = 0; slot < 0x1c; slot++)
        if (((rw(ds, (uint16_t)(INVENTORY_SLOTS + slot * 2)) >> 6) & 0x3ff) == idx) break;
    if (slot < 0x1c) {
        inventory_unlink_object(m, -1, -1, -1, slot, count);
        player_state_recalc(m);
        if (slot < 0x13) {
            inventory_slot_click(m, (int8_t)ds[(uint16_t)(INVENTORY_CLICK_ORDER + slot)]);
        } else {
            container_page_fill(m);
            container_view_refresh(m);
            container_stack_less_weight(m, w);
        }
        return 1;
    }
    if (!object_find_link(m, (uint16_t)(rw(ds, TRACKED_OBJECT) + 6), idx)) return 0;
    q = (uint16_t)((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x3ff);
    if (count > 0 && (rw(ls, obj) & 0x8000) && !(q & 0x200) && (int16_t)q > 1 && count < (int16_t)q) {
        uint16_t copy;
        pool_from_ds(m, &pool);
        copy = uw_obj_alloc(&pool, 0);
        pool_to_ds(m, &pool);
        if (!copy) {
            UW_NOT_CARRIED(m->not_carried);
            return 1;
        }
        memmove(ls + copy, ls + obj, 8);    /* struct_copy_far */
        ww(ls, (uint16_t)(copy + 6), (uint16_t)((rw(ls, (uint16_t)(copy + 6)) & 0x3f) | ((q - count) & 0x3ff) << 6));
        ww(ls, (uint16_t)(obj + 6), (uint16_t)((rw(ls, (uint16_t)(obj + 6)) & 0x3f) | (count & 0x3ff) << 6));
        pool_from_ds(m, &pool);
        uw_object_list_insert(&pool, (uint16_t)(obj + 4), copy);
        pool_to_ds(m, &pool);
    }
    pool_from_ds(m, &pool);
    uw_object_list_remove(&pool, rw(ds, 0x2736), obj);
    pool_to_ds(m, &pool);
    ww(ds, CARRIED_WEIGHT, (uint16_t)(rw(ds, CARRIED_WEIGHT) - w));
    inventory_slot_click(m, 0x13);
    player_state_recalc(m);
    return 1;
}

/* object_clear(obj, from_inventory, force): 1 when the object is gone.
 * From the inventory,
 * inventory_remove_quantity of one and object_remove with no tile. From the
 * world, the chain of the action's target tile: found there by
 * object_find_link, object_remove from the chain object_find_list names and
 * post_event(2); not there -- a thing on the cursor -- freed with whatever
 * follows it through object_chain_clear over a word on the stack. */
int object_clear(uw_motion *m, uint16_t obj, int from_inventory, int force) {
    if (!from_inventory) {
        uint8_t *ds = m->ds;
        uint16_t tile = tile_ptr(m, rw(ds, 0x269a), rw(ds, 0x269c));
        if (object_find_link(m, (uint16_t)(tile + 2), obj_index_of(m, obj))) {
            obj = object_remove(m, rw(ds, 0x2736), obj, force);
            ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));   /* post_event(2) */
            return obj == 0;
        }
        object_chain_clear_local(m, obj);
        return 1;
    }
    inventory_remove_quantity(m, obj, 1);
    return object_remove(m, 0, obj, force) == 0;
}

int uw_motion_inventory_remove_quantity(uw_motion *m, uint16_t obj, int16_t count) {
    return inventory_remove_quantity(m, obj, count);
}
