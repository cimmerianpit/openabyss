/* SPDX-License-Identifier: MIT */
/* the view's actions: a step or turn from the keys, view_pick_object,
 * the look, use, get and talk actions on a thing in reach, the tools
 * used on the world (key, rock hammer, bones, pole, oil, spike,
 * picklock) and view_action_dispatch.
 *
 * One of src/uw_motion*.c: see uw_motion.h and uw_motion_int.h. */
#include "uw_motion_int.h"

/* player_step_or_turn(dir), from the instructions -- running,
 * as the movement arrows' handler calls it, with BP 0x9574: refused while
 * airborne (vertical_gravity) or at an acceleration step's speed.
 * A turn (dir other than 0, 2 and -2) snaps an unaligned heading to its
 * octant first -- rounding up for a right turn -- then turns 45 degrees and
 * writes the heading's octant into the tracked object's +2 bits 7..9 and its
 * top five bits into +0x18. A step is 0x80 along the heading (2: the same,
 * the fit's slope argument set), -2 0x40 backward; while levitating or
 * flying (motion_spell_bits 0x14, which enchantments set) the slope
 * argument is set too. The step is taken when item_fits_in_tile accepts it
 * and the support there is walkable ground, the player's own movement state
 * or, levitating or flying, the air (support 0x10: off a ledge):
 * the position written, the object relinked into a new tile and its tile
 * coordinates set, the fine position into +2, the z put on the floor unless
 * a slope-argument step leaves the Avatar eight or more above it -- then
 * falling (gravity -4) unless levitating or flying -- the movement state from the
 * support, the clock's top two bits into +0xb, and a spatial query at the
 * destination on the frame (BP - 0x2a) firing trigger_chain on every
 * overlapping object of item 0x1a0. */
static int player_step_or_turn(uw_motion *m, int16_t dir) {
    enum { BP = 0x9574 };
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t obj = rw(ds, TRACKED_OBJECT), q = (uint16_t)(BP - 0x2a), saved;
    int16_t step, angle, x, y, tile, i, start, count;
    uint8_t slope = 0, flying;
    if (rw(ds, VERTICAL_GRAVITY) || rs(ds, MOVEMENT_SPEED) >= rs(ds, SPEED_MAX_ACCEL)) return 0;
    if (dir == -2) {
        step = 0x40;
        angle = (int16_t)((uint16_t)(rw(ds, PLAYER_HEADING) + 0x8000) >> 8);
    } else {
        if (dir != 0) {
            if (dir != 2) {
                uint16_t h = rw(ds, PLAYER_HEADING);
                if (h & 0x1fff) {
                    dir = (int16_t)(dir > 0);
                    h &= 0xe000;
                }
                h = (uint16_t)(h + dir * 0x2000);
                ww(ds, PLAYER_HEADING, h);
                ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0xfc7f) | ((((int16_t)h >> 0xd) & 7) << 7)));
                ls[(uint16_t)(obj + 0x18)] = (uint8_t)((ls[(uint16_t)(obj + 0x18)] & 0xe0) | ((h >> 8) & 0x1f));
                return 1;
            }
            slope = 1;
        }
        step = 0x80;
        angle = (int16_t)((int16_t)rw(ds, PLAYER_HEADING) / 0x100);
    }
    flying = (ds[BLOCK_FLAGS] & 0x14) != 0;
    ww(ds, (uint16_t)(BP - 6), rw(ds, PLAYER_X));
    ww(ds, (uint16_t)(BP - 8), rw(ds, PLAYER_Y));
    angle_to_offset(m, (uint16_t)angle, step, (uint16_t)(BP - 6), (uint16_t)(BP - 8));
    x = rs(ds, (uint16_t)(BP - 6));
    y = rs(ds, (uint16_t)(BP - 8));
    if (!item_fits_in_tile(m, 0x7f, 1, (int16_t)(x / 0x20), (int16_t)(y / 0x20), (int16_t)(rw(ls, (uint16_t)(obj + 2)) & 0x7f),
                           slope | flying, 8, (uint16_t)(BP - 0x28 - 4 - 14 - 4 - 2)))
        return 0;
    if (!(slope || rw(ds, PLACE_SUPPORT) == 1 || rw(ds, PLACE_SUPPORT) == rw(ds, MOVEMENT_STATE)
          || (rw(ds, PLACE_SUPPORT) == 0x10 && flying)))
        return 0;
    ww(ds, PLAYER_X, (uint16_t)x);
    ww(ds, PLAYER_Y, (uint16_t)y);
    tile = (int16_t)((x >> 8) + (y >> 8) * 0x40);
    if (tile != rs(ds, TRACKED_TILE)) {
        if (rs(ds, TRACKED_TILE) != -1)
            uw_motion_object_list_remove(m, (uint16_t)(rw(ds, TILEMAP_PTR) + rs(ds, TRACKED_TILE) * 4 + 2), obj);
        ww(ds, TRACKED_TILE, (uint16_t)tile);
        uw_motion_object_list_insert(m, (uint16_t)(rw(ds, TILEMAP_PTR) + tile * 4 + 2), obj);
        ww(ls, (uint16_t)(obj + 0x16), (uint16_t)((rw(ls, (uint16_t)(obj + 0x16)) & 0x3ff) | ((uint16_t)(rs(ds, PLAYER_X) >> 8) << 10)));
        ww(ls, (uint16_t)(obj + 0x16), (uint16_t)((rw(ls, (uint16_t)(obj + 0x16)) & 0xfc0f) | (((rs(ds, PLAYER_Y) >> 8) & 0x3f) << 4)));
    }
    ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0x1fff) | ((uint16_t)(rs(ds, PLAYER_X) >> 5) << 0xd)));
    ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0xe3ff) | (((rs(ds, PLAYER_Y) >> 5) & 7) << 10)));
    if ((!slope && !flying) || (rs(ds, PLAYER_Z) >> 3) - 8 <= rs(ds, PLACE_FLOOR)) {
        ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0xff80) | (rw(ds, PLACE_FLOOR) & 0x7f)));
        ww(ds, PLAYER_Z, (uint16_t)(rw(ds, PLACE_FLOOR) << 3));
    } else if (!rw(ds, VERTICAL_GRAVITY) && !flying) {
        ww(ds, VERTICAL_GRAVITY, (uint16_t)-4);
    }
    set_movement_state(m, rw(ds, PLACE_SUPPORT), 0);
    ww(ls, (uint16_t)(obj + 0xb), (uint16_t)((rw(ls, (uint16_t)(obj + 0xb)) & 0xfff) | ((((uint16_t)m->clock & 0xff) >> 6) << 0xc)));
    saved = rw(ds, SQ_PTR);
    ww(ds, SQ_PTR, q);
    ww(ds, (uint16_t)(q + 0xa), 1);
    ds[(uint16_t)(q + 8)] = (uint8_t)(prop(m, 0x7f, 1) & 7);
    ds[(uint16_t)(q + 9)] = prop(m, 0x7f, 0);
    ww(ds, q, (uint16_t)(x / 0x20));
    ww(ds, (uint16_t)(q + 2), (uint16_t)(y / 0x20));
    ww(ds, (uint16_t)(q + 4), (uint16_t)(rw(ls, (uint16_t)(obj + 2)) & 0x7f));
    sq_gather(m, 0, 0);
    sq_sort(m);
    start = (int8_t)ds[(uint16_t)(q + 0x16)];
    count = ds[(uint16_t)(q + 0x15)];
    for (i = start; i < start + count; i++) {
        uint16_t o = obj_at(m, (uint16_t)((rec_link(m, i) >> 6) & 0x3ff));
        if (o && (rw(ls, o) & 0x1ff) == 0x1a0)
            trigger_chain(m, obj, 0, o, 0, (uint16_t)(BP - 0x28 - 4 - 14 - 4 - 2));
    }
    ww(ds, SQ_PTR, saved);
    return 1;
}

/* key_step_or_turn(dir), the movement arrows' handler (and the
 * capital movement letters'): a step or turn taken costs a quarter of a
 * second -- the frame stamp (last_move_key_clock) set to the
 * clock, the turn phase up 4, the speed zeroed, the level's effects ticked
 * once when they tick on moves, 0x40 on the record's play clock, and
 * player_frame_update(0x40, the turn phase's substeps, still) with the phase
 * reset or, turns halved, kept to its low bit -- and event 10 posted. Then it
 * spins until 0x18 ticks have passed since it began (time the pass spends,
 * no state) and clears a pending mouse code. */
void key_step_or_turn(uw_motion *m, int16_t dir) {
    uint8_t *ds = m->ds;
    if (player_step_or_turn(m, dir)) {
        uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
        uint32_t t;
        uint8_t sub;
        ww(ds, CLOCK_STAMP, (uint16_t)m->clock);
        ww(ds, (uint16_t)(CLOCK_STAMP + 2), (uint16_t)(m->clock >> 16));
        ds[TURN_PHASE] = (uint8_t)(ds[TURN_PHASE] + 4);
        ww(ds, MOVEMENT_SPEED, 0);
        if (ds[EFFECTS_ON_MOVE]) level_effects_tick(m, 1, (uint16_t)(FRAME_BP - 0x80));
        t = (uint32_t)rw(ds, (uint16_t)(rec + 0xce)) | ((uint32_t)rw(ds, (uint16_t)(rec + 0xd0)) << 16);
        t += 0x40;
        ww(ds, (uint16_t)(rec + 0xce), (uint16_t)t);
        ww(ds, (uint16_t)(rec + 0xd0), (uint16_t)(t >> 16));
        sub = ds[TURN_PHASE];
        if (!ds[TURN_HALVED]) {
            ds[TURN_PHASE] = 0;
        } else {
            sub = (uint8_t)(sub >> 1);
            ds[TURN_PHASE] &= 1;
        }
        uw_motion_player_frame(m, 0x40, sub, 1);
        ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 10));
    }
    ww(ds, 0x0115, 0xffff);                 /* mouse_clear_pending */
}

/* view_pick_object: view_refresh_pick_map, then the byte under
 * the event's position -- below the counter, an object: its index from
 * drawlist_object_index, its tile from drawlist_object_x into pick_tile,
 * pick_tile_chain two on, and cursor_pick_valid set for a static thing whose
 * properties +7 bit 5 say it can be taken; 0xc0..0xfa, a surface into
 * view_pick_surface as the code less 0xbf. The object, or 0. */
static uint16_t view_pick_object(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t ev = rw(ds, 0x00e2), si = 0, at, o;
    const uint8_t *map;
    uint8_t b;
    if (!m->pick_map || !(map = m->pick_map(m->pick_user, ds))) {
        UW_NOT_CARRIED(m->not_carried);
        return 0;
    }
    at = (uint16_t)((uint16_t)(rs(ds, (uint16_t)(ev + 2)) * (int16_t)(rw(ds, VIEW_WIDTH_WORD) + 2))
                    + rw(ds, ev) + 2);
    b = map[at];
    ww(ds, VIEW_PICK_SURFACE, 0);
    if (b >= 1 && (int16_t)b < rs(ds, DRAWLIST_OBJ_COUNT)) {
        si = rw(ds, (uint16_t)(DRAWLIST_OBJ_INDEX + b * 2));
        ww(ds, PICK_TILE, (uint16_t)(rw(ds, TILEMAP_ORIGIN) + rw(ds, (uint16_t)(DRAWLIST_OBJ_X + b * 2)) * 4));
        ww(ds, (uint16_t)(PICK_TILE + 2), rw(ds, (uint16_t)(TILEMAP_ORIGIN + 2)));
    } else if (b >= 0xc0 && b <= 0xfa) {
        ww(ds, VIEW_PICK_SURFACE, (uint16_t)(b - 0xbf));
    }
    if (!si) return 0;
    o = obj_at(m, si);
    ww(ds, PICK_TILE_CHAIN, (uint16_t)(rw(ds, PICK_TILE) + 2));
    ww(ds, (uint16_t)(PICK_TILE_CHAIN + 2), rw(ds, (uint16_t)(PICK_TILE + 2)));
    ds[CURSOR_PICK_VALID] = (uint8_t)(((prop(m, obj_id(m, o), 7) >> 5) & 1) && o >= rw(ds, STATIC_BASE));
    (void)ls;
    return o;
}

/* tile_highest_bridge: the greatest z of a bridge (item 0x164)
 * in the tile's chain, or -1 for none. */
static int16_t tile_highest_bridge(uw_motion *m, uint16_t tile) {
    uint8_t *ls = m->lseg;
    uint16_t o = deref_link(m, (uint16_t)(tile + 2));
    int16_t z = -1;
    while (o) {
        if ((rw(ls, o) & 0x1ff) == 0x164 && z < (int16_t)(rw(ls, (uint16_t)(o + 2)) & 0x7f))
            z = (int16_t)(rw(ls, (uint16_t)(o + 2)) & 0x7f);
        o = deref_link(m, (uint16_t)(o + 4));
    }
    return z;
}

/* action_path_clear(dist2, obj): 1 when something between the
 * Avatar and the action target's tile blocks -- nothing with no distance or
 * on its own tile. The object's z outside the band from the Avatar's less
 * (scale + 1) * 8, floored at 0, to (scale + 1) * 0x20 above that blocks;
 * then each tile stepped towards the target, both axes at once: with no
 * bridge between the band's floor and the object's z, a floor below the
 * band's blocks for a distance of 0x90, and short of the target a terrain
 * kind neither 0 nor the Avatar's tile's for one under 0x91. */
static int action_path_clear(uw_motion *m, int16_t dist2, uint16_t obj) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t av = rw(ds, TRACKED_OBJECT), t, kind;
    int16_t x = (int16_t)(rw(ls, (uint16_t)(av + 0x16)) >> 10), y = (int16_t)((rw(ls, (uint16_t)(av + 0x16)) & 0x3f0) >> 4);
    int16_t tx = rs(ds, ACTION_TARGET_TILE_X), ty = rs(ds, ACTION_TARGET_TILE_Y), sx, sy, floor, oz, bridge;
    int16_t scale = (int16_t)((int8_t)ds[ACTION_REACH_SCALE] + 1);
    int on;
    if (!dist2) return 0;
    t = tile_ptr(m, (uint16_t)x, (uint16_t)y);
    kind = rw(ds, (uint16_t)(TERRAIN_KINDS + ((ls[(uint16_t)(t + 1)] & 0x3c) >> 2) * 2));
    if (tx == x && ty == y) return 0;
    sx = (int16_t)(tx < x ? -1 : x < tx ? 1 : 0);
    sy = (int16_t)(ty < y ? -1 : y < ty ? 1 : 0);
    oz = (int16_t)(rw(ls, (uint16_t)(av + 2)) & 0x7f);
    floor = (int16_t)(scale * 8 < oz ? oz - scale * 8 : 0);
    oz = (int16_t)(rw(ls, (uint16_t)(obj + 2)) & 0x7f);
    if (oz < floor || floor + scale * 0x20 < oz) return 1;
    for (;;) {
        x = (int16_t)(x + sx);
        if ((sx < 0 && x < tx) || (sx > 0 && tx < x)) x = tx;
        y = (int16_t)(y + sy);
        if ((sy < 0 && y < ty) || (sy > 0 && ty < y)) y = ty;
        t = tile_ptr(m, (uint16_t)x, (uint16_t)y);
        bridge = tile_highest_bridge(m, t);
        on = bridge >= 0 && bridge <= oz && floor <= bridge;
        if (!on && (uint16_t)(ls[t] >> 4) < (uint16_t)(floor >> 3) && dist2 == 0x90) return 1;
        if (x == tx && y == ty) return 0;
        if (dist2 < 0x91 && !on) {
            uint16_t k = rw(ds, (uint16_t)(TERRAIN_KINDS + ((ls[(uint16_t)(t + 1)] & 0x3c) >> 2) * 2));
            if (k && k != kind) return 1;
        }
    }
}

/* action_use: the release wait first; after it, in reach
 * (action_reach_dist2) with a clear path (action_path_clear) the object's
 * use (object_use_dispatch), else -- but for items 0x16e and 0x16f -- "You
 * are unable to use that from here." `after` is the part after the wait,
 * which the pair that holds the wait's end runs. */
void action_use(uw_motion *m, int after) {
    uint8_t *ds = m->ds;
    uint16_t obj = rw(ds, CURSOR_PICK_OBJECT);
    if (!after) {
        input_wait_button_release(m, 1);
        m->view_wait = 0x0e49;
        return;
    }
    if (action_in_reach(m, rs(ds, 0x0286), obj, rw(ds, PICK_TILE))
        && !action_path_clear(m, rs(ds, 0x0286), obj)) {
        /* object_use_dispatch's frame under action_use's 0x9572 (chain1's
         * 014: the wait's frames above it): no locals, five words pushed */
        m->use_bp = (uint16_t)(0x9572 - 10 - 4 - 2);
        object_use_dispatch(m, rw(ds, TRACKED_OBJECT), obj, 0);
        m->use_bp = 0;
        return;
    }
    if ((rw(m->lseg, obj) & 0x1fe) != 0x16e) print_message(m, 0xb9);
}

/* object_tree_contains_id(obj, id): the object, or anything in a
 * non-quantity's contents at any depth, of that item id. */
int object_tree_contains_id(uw_motion *m, uint16_t obj, uint16_t id) {
    uint16_t link = (uint16_t)(obj + 6);
    if ((rw(m->lseg, obj) & 0x1ff) == id) return 1;
    if (rw(m->lseg, obj) & 0x8000) return 0;
    return object_find_matching(m, &link, 1, (uint16_t)(id >> 6), (uint16_t)((id & 0x30) >> 4), (uint16_t)(id & 0xf)) != 0;
}

/* action_take_from_container(obj), with the pick valid: the
 * thing's pick-up triggers (trigger_object_link, kind 2), the thing out of
 * the picked tile's chain, event 2 posted and the pick spent. */
static void action_take_from_container(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds;
    uw_objpool pool;
    if (!ds[CURSOR_PICK_VALID]) return;
    trigger_object_link_port(m, rw(ds, TRACKED_OBJECT), obj, 2);   /* trigger_object_link(player, obj, 2, the target tile) */
    memset(&pool, 0, sizeof pool);
    pool.seg = m->lseg;
    uw_object_list_remove(&pool, rw(ds, PICK_TILE_CHAIN), obj);
    ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));      /* post_event(2) */
    ds[CURSOR_PICK_VALID] = 0;
}

/* inventory_begin_drag(obj): the object on the cursor
 * (held_object_ptr) and its item id pushed as the cursor's shape; with a
 * button still down, the release wait and the drop where it comes up (not
 * carried). */
static void inventory_begin_drag(uw_motion *m, uint16_t obj, uint16_t seg) {
    uint8_t *ds = m->ds;
    ww(ds, 0x5b06, obj);
    ww(ds, 0x5b08, seg);
    cursor_shape_push(m, (uint16_t)(rw(m->lseg, obj) & 0x1ff));
    if (mouse_sample_buttons(m)) {
        /* the release wait, whose passes pair as any; where the button comes
         * up, inventory_panel_hit_test and the drop -- paperdoll_click's rest
         * with the thing held (uw_motion_inventory_release), which the shell
         * runs at the wait's end */
        m->drag_param = 4;
        m->drag_param_set = 1;
        input_wait_button_release(m, 1);
    }
}

/* action_pickup, action_handlers[3]: in reach (action_reach_dist2)
 * with a clear path and the pick valid, a stack of more than one asks how
 * many (inventory_split_stack, not carried); too heavy, "That is too heavy
 * for you to pick up."; else a moonstone within clears the record's +0x5e
 * low nibble, report_crime, action_take_from_container, action_state 1 and
 * inventory_begin_drag. Otherwise: a pick not valid during a look's drag is
 * action_talk's or action_use's (not carried), item 0x1ca in reach is used,
 * anything else 0x60; a valid one out of reach 0x5d, blocked 0x5e; and the
 * release wait. */
/* inventory_split_stack's prompt: "Move how many? " on the
 * scroll and the field the shell types into; the action goes on from
 * uw_motion_stack_answer. */
void stack_ask(uw_motion *m, uint16_t obj, int path, int16_t slot) {
    uw_motion_print_ds_string(m, 0x18c1);
    m->stack_ask = obj;
    m->stack_ask_path = (uint8_t)path;
    m->stack_ask_slot = slot;
}

/* action_pickup's take, past the stack's question. */
void action_pickup_take(uw_motion *m, uint16_t obj, uint16_t seg) {
    uint8_t *ds = m->ds;
    {
        if (!(uw_motion_inventory_can_carry(m, obj) & 0xff)) {
            print_message(m, 0x5f);
            return;
        }
        if (object_tree_contains_id(m, obj, 0x126)) {
            uint16_t rec = (uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x5e);
            ds[rec] &= 0xf0;
        }
        report_crime(m, obj, 0, 0x9500);   /* the theft: the witnesses over the 15 x 15 (src/uw_motion_trap.c); the frame is the handler's */
        action_take_from_container(m, obj);
        ww(ds, ACTION_STATE_WORD, 1);
        inventory_begin_drag(m, obj, seg);
    }
}

void action_pickup(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t obj = rw(ds, CURSOR_PICK_OBJECT), seg = rw(ds, (uint16_t)(CURSOR_PICK_OBJECT + 2));
    int reach = action_in_reach(m, rs(ds, 0x0286), obj, rw(ds, PICK_TILE));
    int blocked = action_path_clear(m, rs(ds, 0x0286), obj);
    if (ds[CURSOR_PICK_VALID] && reach && !blocked) {
        uint16_t q = (uint16_t)(rw(ls, (uint16_t)(obj + 6)) >> 6);
        if ((rw(ls, obj) & 0x8000) && !(q & 0x200) && q != 1) {
            stack_ask(m, obj, 1, 0);
            return;
        }
        action_pickup_take(m, obj, seg);
        return;
    }
    if (!ds[CURSOR_PICK_VALID]) {
        if (ds[LOOK_IN_PROGRESS]) {
            /* a look's drag on what cannot be taken: a creature (mobile, class
             * 1) is talked to (action_talk: the release wait, then the
             * conversation), anything else used (action_use) */
            if (obj < rw(ds, STATIC_BASE) && (rw(ls, obj) & 0x1c0) == 0x40) {
                input_wait_button_release(m, 1);
                m->talk_object = obj;
            } else {
                action_use(m, 0);
            }
            return;
        }
        if ((rw(ls, obj) & 0x1ff) == 0x1ca) {
            if (reach && !blocked) object_use_dispatch(m, rw(ds, TRACKED_OBJECT), obj, 0);
        } else {
            print_message(m, 0x60);
        }
    } else {
        print_message(m, (uint16_t)(reach + 0x5d));
    }
    input_wait_button_release(m, 1);
}

/* use_key(obj, hit), from the instructions, the handler a key's
 * use leaves pending: with a hit, the cursor's shape popped, the hand
 * emptied, action_state 0, and message 2 + door_unlock_attempt with the
 * key's lock number (pending_action_object's +6 low six bits): "The key does
 * not fit.", "There is no lock on that.", "The key locks the lock.", "The
 * key unlocks the lock." or "That is already open.". */
static void use_key(uw_motion *m, uint16_t obj, int hit) {
    uint8_t *ds = m->ds;
    if (!hit) return;
    cursor_shape_pop(m, 3);
    ww(ds, 0x5b06, 0);
    ww(ds, 0x5b08, 0);
    ww(ds, ACTION_STATE_WORD, 0);
    print_message(m, (uint16_t)(door_unlock_attempt(m, rw(ds, TRACKED_OBJECT), obj, (int16_t)(m->lseg[(uint16_t)(rw(ds, 0x2688) + 6)] & 0x3f)) + 2));
}

/* use_rock_hammer(obj, hit, in_inventory), the rock hammer's
 * pending handler, from the instructions: the cursor's shape popped, the hand
 * emptied and action_state 0; then only for a thing in the world the Avatar
 * does not carry. A boulder (0x153..0x156) breaks -- "The rock breaks into
 * smaller pieces." (0x87) -- into rand() * 2 / 0x8000 + (0x155 - id) + 1
 * pieces: each object_create'd, a copy of the boulder's record, its id the
 * boulder's plus rand() * 2 / 0x8000 + 1 -- past 0x156 small stones (0x10), a
 * quantity of rand() % 6 + 3 -- and put near the boulder's fine position in
 * the action's target tile (object_move_to_coords, spread 6); the boulder
 * then removed from its tile, forced, and event 2 posted. Anything else is
 * "It seems to have no effect." (0x84). Its frame: view_action_dispatch's at
 * 0x9582 and one word, the handler's four arguments and far call, ours with
 * ten bytes and two registers. */
static void use_rock_hammer(uw_motion *m, uint16_t obj, int hit, int in_inventory) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t bp = (uint16_t)(0x9582 - 2 - 8 - 4 - 2), id, tile;
    int16_t n;
    cursor_shape_pop(m, 3);
    ww(ds, 0x5b06, 0);
    ww(ds, 0x5b08, 0);
    ww(ds, ACTION_STATE_WORD, 0);
    if (!hit || in_inventory) return;
    if (object_find_link(m, (uint16_t)(rw(ds, TRACKED_OBJECT) + 6), obj_index_of(m, obj))) return;
    id = (uint16_t)(rw(ls, obj) & 0x1ff);
    if (id < 0x153 || id > 0x156) {
        print_message(m, 0x84);
        return;
    }
    print_message(m, 0x87);
    tile = tile_ptr(m, rw(ds, 0x269a), rw(ds, 0x269c));
    n = (int16_t)(((int32_t)(int16_t)rt_rand(m) * 2) / 0x8000 + (0x155 - id) + 1);
    for (; n > 0; n--) {
        uint16_t piece = create_object(m, 1, 0), w2;
        int16_t size;
        if (!piece) break;
        memmove(ls + piece, ls + obj, 8);   /* struct_copy_far */
        size = (int16_t)(id + ((int32_t)(int16_t)rt_rand(m) * 2) / 0x8000 + 1);
        if (size > 0x156) size = 0x10;
        ww(ls, piece, (uint16_t)((rw(ls, piece) & 0xfe00) | (size & 0x1ff)));
        if (size == 0x10) {
            ww(ls, piece, (uint16_t)(rw(ls, piece) | 0x8000));
            ww(ls, (uint16_t)(piece + 6), (uint16_t)((rw(ls, (uint16_t)(piece + 6)) & 0x3f)
                                                    | ((((int16_t)rt_rand(m) % 6 + 3) & 0x3ff) << 6)));
        }
        w2 = rw(ls, (uint16_t)(obj + 2));
        object_move_to_coords(m, (int16_t)((rw(ds, 0x269a) << 3) + (w2 >> 13)),
                              (int16_t)((rw(ds, 0x269c) << 3) + ((w2 & 0x1c00) >> 10)),
                              (int16_t)(w2 & 0x7f), piece, 6, 0,
                              (uint16_t)(bp - 0xa - 4 - 14 - 4 - 2));
    }
    object_remove(m, (uint16_t)(tile + 2), obj, 1);
    ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));      /* post_event(2) */
}

/* use_bones(obj, hit), the skull's and bones' pending handler:
 * the cursor's shape popped, the hand emptied and
 * action_state 0. On a gravestone (0x165) ordinary bones are "You
 * thoughtfully give the bones a final resting place." (0x86) and used up
 * (object_clear of pending_action_object, forced); the bones whose +6 low
 * bits are 0x3e are Garamon's -- on a grave not his (not a quantity with the
 * special value 0x221) "The bones do not seem at rest in the grave, and you
 * take them back." (0x103); on his, the record's +0x62 bits 2 and 3 set, the
 * grave's word 3 made 0x8880 over its low six bits (a quantity, 0x22 --
 * Garamon's grave filled), a conversation with a creature made on the stack
 * -- item 0x7e, goal 7, attitude 3, whoami 0x1b, Garamon's ghost -- and
 * trigger_chain(player, 0, the first object at (0x36, 0x34), 0) when there
 * is one; then the bones are used up (object_clear, forced). The port
 * makes the stack creature a throwaway object (create_object, freed when
 * the conversation ends: talk_object_temp) and, since the conversation is
 * the host's after the pass, runs the trigger before it rather than after
 * -- and the trigger's frame is the port's. Anything else is "It seems to
 * have no effect." (0x84). */
static void use_bones(uw_motion *m, uint16_t obj, int hit) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    cursor_shape_pop(m, 3);
    ww(ds, 0x5b06, 0);
    ww(ds, 0x5b08, 0);
    ww(ds, ACTION_STATE_WORD, 0);
    if (!obj || (rw(ls, obj) & 0x1ff) != 0x165) {
        print_message(m, 0x84);
        return;
    }
    if ((ls[(uint16_t)(rw(ds, 0x2688) + 6)] & 0x3f) == 0x3e) {
        uint16_t w3 = (uint16_t)(rw(ls, (uint16_t)(obj + 6)) >> 6);
        if (!(rw(ls, obj) & 0x8000) || !(w3 & 0x200) || (w3 & 0x1ff) != 0x21) {
            print_message(m, 0x103);
            return;
        }
        {
            uint16_t rec = rw(ds, PLAYER_RECORD_PTR), ghost, t;
            ds[(uint16_t)(rec + 0x62)] |= 0xc;
            ww(ls, (uint16_t)(obj + 6), (uint16_t)((rw(ls, (uint16_t)(obj + 6)) & 0x3f) | 0x8880));
            ghost = create_object(m, 0x7e, 1);
            if (ghost) {
                ls[(uint16_t)(ghost + 0x1a)] = 0x1b;
                ww(ls, (uint16_t)(ghost + 0xb), (uint16_t)((rw(ls, (uint16_t)(ghost + 0xb)) & 0xfff0) | 7));
                ww(ls, (uint16_t)(ghost + 0xd), (uint16_t)((rw(ls, (uint16_t)(ghost + 0xd)) & 0x3fff) | 0xc000));
                m->talk_object = ghost;
                m->talk_object_temp = 1;
            } else {
                UW_NOT_CARRIED(m->not_carried);
            }
            t = deref_link(m, (uint16_t)(tile_ptr(m, 0x36, 0x34) + 2));
            if (t) trigger_chain(m, rw(ds, TRACKED_OBJECT), 0, t, 0, (uint16_t)(FRAME_BP - 0x80 - 0x24 - 4 - 14 - 4 - 2));
            object_clear(m, rw(ds, 0x2688), hit, 1);
        }
        return;
    }
    print_message(m, 0x86);
    object_clear(m, rw(ds, 0x2688), hit, 1);
}

/* use_special_tmap_object(obj, hit), the Key of Infinity's
 * pending handler (item 0xe7, use_object_dispatch_misc's prompt):
 * the cursor's shape popped, the hand emptied and
 * action_state 0; on a special tmap object (0x16e) whose texture (word 3's
 * low six bits) reads 0xb in the low byte of its wall_terrain_kinds word
 * the key is used up (object_clear of pending_action_object,
 * forced) and the object's trigger fires with kind 7
 * (trigger_object_link(player, obj, 7, the target tile)); anything else is
 * "It seems to have no effect." (0x84). The trigger's frame is the port's. */
static void use_special_tmap_object(uw_motion *m, uint16_t obj, int hit) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    cursor_shape_pop(m, 3);
    ww(ds, 0x5b06, 0);
    ww(ds, 0x5b08, 0);
    ww(ds, ACTION_STATE_WORD, 0);
    if (obj && (rw(ls, obj) & 0x1ff) == 0x16e
        && (rw(ds, (uint16_t)(0x720c + 2 * (ls[(uint16_t)(obj + 6)] & 0x3f))) & 0xff) == 0xb) {
        object_clear(m, rw(ds, 0x2688), hit, 1);
        trigger_object_link_at(m, rw(ds, TRACKED_OBJECT), obj, 7, ds[ACTION_TARGET_TILE_X], ds[ACTION_TARGET_TILE_Y],
                               (uint16_t)(FRAME_BP - 0x80));
    } else {
        print_message(m, 0x84);
    }
}

/* use_pole(obj), the pole's pending handler:
 * action_reach_scale back to 0 and player_state_recalc; a
 * switch (0x170..0x17f) is "Using the pole you trigger the switch." (0x9d)
 * and used by the Avatar (object_use_dispatch, not from the inventory);
 * anything else "The pole cannot be used on that." (0x9e). */
static void use_pole(uw_motion *m, uint16_t obj) {
    m->ds[0x1b00] = 0;
    player_state_recalc(m);
    if (obj && (rw(m->lseg, obj) & 0x1f0) == 0x170) {
        print_message(m, 0x9d);
        object_use_dispatch(m, rw(m->ds, TRACKED_OBJECT), obj, 0);
    } else {
        print_message(m, 0x9e);
    }
}

/* use_oil(obj, hit, in_inventory), the oil flask's pending
 * handler: the cursor's shape popped, the hand
 * emptied and action_state 0; then only for a thing in the inventory. Wood
 * (0xcc, 0xcd) becomes a torch (0x91): "Dousing a cloth with oil..." (0xb5),
 * the flask used up (object_clear of pending_action_object, forced) and the
 * thing's slot redrawn. A lantern or torch not full (0x90, 0x91, quality
 * under 0x3f) is filled by 0x20, to 0x3f at most, with message 0xb3 (0xb7
 * for the torch) and the flask used up; full, 0xb4 (0xb8). A lit one (0x94,
 * 0x95) is 0xb2 (0xb6); anything else "You cannot use oil on that." (0xb1). */
static void use_oil(uw_motion *m, uint16_t obj, int hit, int in_inventory) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t id = (uint16_t)(rw(ls, obj) & 0x1ff);
    uint16_t base = (id == 0x90 || id == 0x94) ? 0 : 4;
    cursor_shape_pop(m, 3);
    ww(ds, 0x5b06, 0);
    ww(ds, 0x5b08, 0);
    ww(ds, ACTION_STATE_WORD, 0);
    if (!hit || !in_inventory) return;
    if (id == 0xcc || id == 0xcd) {
        print_message(m, 0xb5);
        object_clear(m, rw(ds, 0x2688), hit, 1);
        ww(ls, obj, (uint16_t)((rw(ls, obj) & 0xfe00) | 0x91));
        inventory_click_slot_index(m, inventory_find_object(m, obj));
        return;
    }
    if (id == 0x90 || id == 0x91) {
        uint8_t q = (uint8_t)(ls[(uint16_t)(obj + 4)] & 0x3f);
        if (q != 0x3f) {
            ls[(uint16_t)(obj + 4)] = (uint8_t)((ls[(uint16_t)(obj + 4)] & 0xc0) | (q < 0x20 ? (q + 0x20) & 0x3f : 0x3f));
            print_message(m, (uint16_t)(base + 0xb3));
            object_clear(m, rw(ds, 0x2688), hit, 1);
            return;
        }
        print_message(m, (uint16_t)(base + 0xb4));
    } else if (id == 0x94 || id == 0x95) {
        print_message(m, (uint16_t)(base + 0xb2));
    } else {
        print_message(m, 0xb1);
    }
}

/* door_spike(obj), the spike's pending handler:
 * a closed door (0x140..0x147) is spiked -- "The door is now
 * spiked closed." (0x81), its +6 low six bits all set, and the spike used up
 * (object_clear of pending_action_object, from the inventory, forced) --
 * anything else "You can only spike closed doors." (0x80). Then the cursor's
 * shape popped, the hand emptied and action_state 0. */
static void door_spike(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t id = obj ? (uint16_t)(rw(ls, obj) & 0x1ff) : 0;
    if (id < 0x140 || id > 0x147) {
        print_message(m, 0x80);
    } else {
        print_message(m, 0x81);
        ls[(uint16_t)(obj + 6)] = (uint8_t)(ls[(uint16_t)(obj + 6)] | 0x3f);
        object_clear(m, rw(ds, 0x2688), 1, 1);
    }
    cursor_shape_pop(m, 3);
    ww(ds, 0x5b06, 0);
    ww(ds, 0x5b08, 0);
    ww(ds, ACTION_STATE_WORD, 0);
}

/* use_picklock(obj, hit), from the instructions, the lockpick's
 * pending handler: with a hit, the cursor's shape popped, the hand emptied,
 * action_state 0, and door_unlock_attempt with the Picklock skill (record
 * +0x31) negated: 0 "Your lockpicking attempt failed." (0x78), 1 "There is
 * no lock on that." (3), 4 "That is not locked." (0x7a), otherwise
 * play_sound_effect(0x13, 0x40, 0) and "You succeed in picking the lock."
 * (0x79). */
static void use_picklock(uw_motion *m, uint16_t obj, int hit) {
    uint8_t *ds = m->ds;
    int r;
    if (!hit) return;
    cursor_shape_pop(m, 3);
    ww(ds, 0x5b06, 0);
    ww(ds, 0x5b08, 0);
    ww(ds, ACTION_STATE_WORD, 0);
    r = door_unlock_attempt(m, rw(ds, TRACKED_OBJECT), obj, (int16_t)-(int16_t)ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x31)]);
    if (r == 0) print_message(m, 0x78);
    else if (r == 1) print_message(m, 3);
    else if (r == 4) print_message(m, 0x7a);
    else {
        play_sound_effect(m, 0x13, 0x40, 0);
        print_message(m, 0x79);
    }
}

/* The call through pending_action_handler, by its runtime
 * address, with (the pick, 1): use_key and use_picklock; the spike's and
 * the other prompts' handlers are not carried. */
static void use_oil(uw_motion *m, uint16_t obj, int hit, int in_inventory);
static void use_rock_hammer(uw_motion *m, uint16_t obj, int hit, int in_inventory);
static void use_pole(uw_motion *m, uint16_t obj);
static void use_bones(uw_motion *m, uint16_t obj, int hit);

/* door_and_trap_spells(target), the pending handler class 11's
 * cases 2..5 arm, by the pending argument:
 * 3 search_for_trap(target, 0x2d) and on any answer but 0 disarm_trap(target,
 * 0x2d) -- no question asked; 4
 * look_at(target, 3) and, but for a door or a creature (class 5 or 1) or a
 * thing whose obj_properties +9 low bits are 2, word 1 bits 7..9 set --
 * identified; 5 door_unlock_attempt(the Avatar, target, -0x2d), message
 * 0x10e on its 3, else "The spell has no discernable effect." (0x10f); 2
 * nothing. Then the targeting cursor popped, action_state 0 and the release
 * wait. */
static void door_and_trap_spells(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    int16_t which = rs(ds, 0x2690);
    if (which == 3) {
        if (search_for_trap(m, obj, 0x2d)) disarm_trap(m, obj, 0x2d);
    } else if (which == 4) {
        uint16_t w0 = rw(ls, obj);
        look_at(m, obj, 3);
        if ((w0 & 0x1c0) != 0x140 && (w0 & 0x1c0) != 0x40 && (prop(m, (uint16_t)(w0 & 0x1ff), 9) & 3) != 2)
            ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0xfc7f) | 0x380));
    } else if (which == 5) {
        print_message(m, door_unlock_attempt(m, rw(ds, TRACKED_OBJECT), obj, -0x2d) == 3 ? 0x10e : 0x10f);
    }
    cursor_shape_pop(m, 3);
    ww(ds, ACTION_STATE_WORD, 0);
    input_wait_button_release(m, 1);
}

void pending_action_run(uw_motion *m, uint16_t obj, int in_inventory) {
    if (rw(m->ds, 0x2686) == 0x3b62 && rw(m->ds, 0x2684) == 0x051d) use_key(m, obj, 1);
    else if (rw(m->ds, 0x2686) == 0x3b62 && rw(m->ds, 0x2684) == 0x0498) use_picklock(m, obj, 1);
    else if (rw(m->ds, 0x2686) == 0x3b62 && rw(m->ds, 0x2684) == 0x0662) door_spike(m, obj);
    else if (rw(m->ds, 0x2686) == 0x3b62 && rw(m->ds, 0x2684) == 0x1580) use_oil(m, obj, 1, in_inventory);
    else if (rw(m->ds, 0x2686) == 0x3b62 && rw(m->ds, 0x2684) == 0x139e) use_rock_hammer(m, obj, 1, in_inventory);
    else if (rw(m->ds, 0x2686) == 0x3b62 && rw(m->ds, 0x2684) == 0x0882) use_pole(m, obj);
    else if (rw(m->ds, 0x2686) == 0x3b62 && rw(m->ds, 0x2684) == 0x0719) use_bones(m, obj, 1);
    else if (rw(m->ds, 0x2686) == 0x3b62 && rw(m->ds, 0x2684) == 0x0a12) use_tyballs_orb(m, obj, 1);
    else if (rw(m->ds, 0x2686) == 0x3b62 && rw(m->ds, 0x2684) == 0x08cf) use_anvil(m, obj, in_inventory);
    else if (rw(m->ds, 0x2686) == 0x3b62 && rw(m->ds, 0x2684) == 0x0aff) use_special_tmap_object(m, obj, 1);
    else if (rw(m->ds, 0x2686) == 0x393e && rw(m->ds, 0x2684) == 0x1560) door_and_trap_spells(m, obj);
    else UW_NOT_CARRIED(m->not_carried);
}

/* view_action_dispatch, the view's hotspot: a held left button
 * walks (movement_input_update); a click (flags bit 1) with
 * nothing in hand clears cursor_pick_object and, in the mode action_mode
 * less one (2, look, with none selected), fight goes to its handler (the
 * pair's CLIK) and the rest pick: nothing picked is look_at_texture's and a
 * release wait; a picked object action_handlers' -- use, look and pickup
 * (talk not carried). With something held (action_state 1) the click throws
 * it into the view, and waits for the release. With a thing being used
 * (action_state 2) the pick, in reach (action_in_reach) and with
 * a clear path (action_path_clear), goes to the pending handler, else
 * message 0x5e; then anything still held is dropped from the cursor (its
 * shape popped, action_state 0) and the release wait. */
void view_action_dispatch(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t ev = rw(ds, 0x00e2), o;
    uint8_t mode;
    if (rw(ds, (uint16_t)(ev + 6)) & 1) movement_input_update(m);
    if (rw(ds, (uint16_t)(ev + 8)) != 1) {
        /* in game mode 0x10 (a cutscene's) the right button
         * alone picks (view_pick_object(2)) and what it finds is used
         * (action_use); the map's and mode 8's clicks nothing */
        if (rw(ds, (uint16_t)(ev + 8)) == 0x10 && rw(ds, (uint16_t)(ev + 6)) == 2) {
            o = view_pick_object(m);
            ww(ds, CURSOR_PICK_OBJECT, o);
            ww(ds, (uint16_t)(CURSOR_PICK_OBJECT + 2), o ? rw(ds, (uint16_t)(MOBILE_BASE + 2)) : 0);
            if (o) action_use(m, 0);
        }
        return;
    }
    ww(ds, CURSOR_PICK_OBJECT, 0);
    ww(ds, (uint16_t)(CURSOR_PICK_OBJECT + 2), 0);
    if (!(rw(ds, (uint16_t)(ev + 6)) & 2) || rw(ds, ACTION_STATE_WORD) > 3) return;
    if (rw(ds, ACTION_STATE_WORD) == 1) {
        /* something held: into the view it goes (element 0x17),
         * and the release wait */
        inventory_panel_activate_view(m);
        input_wait_button_release(m, 1);
        return;
    }
    if (rw(ds, ACTION_STATE_WORD) == 2) {
        o = view_pick_object(m);
        ww(ds, CURSOR_PICK_OBJECT, o);
        ww(ds, (uint16_t)(CURSOR_PICK_OBJECT + 2), o ? rw(ds, (uint16_t)(MOBILE_BASE + 2)) : 0);
        if (o) {
            if (action_in_reach(m, rs(ds, 0x0286), o, rw(ds, PICK_TILE)) && !action_path_clear(m, rs(ds, 0x0286), o))
                pending_action_run(m, o, 0);
            else
                print_message(m, 0x5e);
        }
        if (rw(ds, 0x5b06) || rw(ds, 0x5b08)) {
            cursor_shape_pop(m, 3);
            ww(ds, 0x5b06, 0);
            ww(ds, 0x5b08, 0);
            ww(ds, ACTION_STATE_WORD, 0);
        }
        input_wait_button_release(m, 1);
        return;
    }
    if (rw(ds, ACTION_STATE_WORD) == 3) {
        /* effect_cast_at_click, from the instructions: the
         * pending caster's effect_cast_projectile with the pending argument,
         * action_state 0, the targeting cursor popped and the release wait;
         * its frame under view_action_dispatch's */
        effect_cast_projectile(m, rw(ds, 0x2688), (int8_t)ds[0x2690], (uint16_t)(0x9582 - 2 - 4 - 2 - 6 - 4 - 2));
        ww(ds, ACTION_STATE_WORD, 0);
        cursor_shape_pop(m, 3);
        input_wait_button_release(m, 1);
        return;
    }
    if (rw(ds, ACTION_STATE_WORD) != 0) return;     /* past the table's four: nothing */
    mode = rw(ds, 0x268c) ? (uint8_t)(rw(ds, 0x268c) - 1) : 2;
    /* action_handlers[1] is action_combat, which the HOST runs: the pass
     * reports reaching it through uw_motion_action_combat and the shell
     * calls uw_motion_action_combat_click after the delivery, so that the
     * click that reached it is the one that starts a swing */
    if (mode == 1) return;
    if (rw(ds, (uint16_t)(ev + 6)) & 1) return;
    o = view_pick_object(m);
    ww(ds, CURSOR_PICK_OBJECT, o);
    ww(ds, (uint16_t)(CURSOR_PICK_OBJECT + 2), o ? rw(ds, (uint16_t)(MOBILE_BASE + 2)) : 0);
    if (!o) {
        look_at_texture(m, mode, rs(ds, VIEW_PICK_SURFACE));
        input_wait_button_release(m, 1);
        return;
    }
    if (mode == 2) action_look(m);           /* action_handlers[2] */
    else if (mode == 0) action_use(m, 0);
    else if (mode == 3) action_pickup(m);   /* action_handlers[3] */
    else {
        /* action_talk: the release wait, then
         * conv_begin_with_object on the pick -- the conversation, which the
         * shell opens at the wait's end (src/uw_talk.c) */
        input_wait_button_release(m, 1);
        m->talk_object = o;
    }
}

/* action_combat, action_handlers[1]: the click in the 3-D view
 * in fight mode divided into a 3 x 3 grid of the viewport -- the column is
 * the event's x times three over the view's pixel width plus two, the row
 * its y (up from the view's foot) over the height plus two -- and
 * combat_swing of the cell, 1..9. */
void uw_motion_action_combat_click(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t ev = rw(ds, 0x00e2);
    int16_t col = (int16_t)(rs(ds, ev) * 3 / (rs(ds, VIEW_WIDTH_WORD) + 2));
    int16_t row = (int16_t)(rs(ds, (uint16_t)(ev + 2)) * 3 / (rs(ds, 0x7280) + 2));
    combat_swing(m, (int16_t)(col + row * 3 + 1));
}
