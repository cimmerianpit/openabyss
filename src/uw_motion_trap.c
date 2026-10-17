/* SPDX-License-Identifier: MIT */
/* doors, traps and triggers: door_open, door_close and door_toggle, the
 * level effects they leave, the unlock attempt, and the traps --
 * trap_dispatch, trigger_chain, find_landing_spot and the teleport trap.
 *
 * One of src/uw_motion*.c: see uw_motion.h and uw_motion_int.h. */
#include "uw_motion_int.h"
#include "uw_scroll.h"
#include "uw_strings.h"
#include <stdlib.h>

/* ---- Doors: door_open, door_close and door_toggle with what they reach.
 * A door does not swing on its own: door_start_moving turns it into the
 * moving door 0x1cf and the level effect list moves it, and at the end of the
 * swing level_effect_expire seats it as the open or closed door again
 * (level_effect_door_seat). */

enum { EFFECT_COUNT = 0x3656, EFFECT_LIST = 0x369c, ACTION_TARGET_X = 0x269a, ACTION_TARGET_Y = 0x269c };

/* object_find_matching: the first object along the chain from
 * *link whose class (word 0 bits 6..8), subclass (4..5) and type (0..3)
 * match, 0xffff matching any. With `recurse` a non-quantity's contents are
 * searched after it fails, and a hit there -- only there -- moves *link to
 * the link the hit hangs from. */
uint16_t object_find_matching(uw_motion *m, uint16_t *link, int recurse, uint16_t cls, uint16_t sub,
                                     uint16_t type) {
    uint8_t *ls = m->lseg;
    uint16_t at = *link, o;
    for (;;) {
        uint16_t w0;
        o = deref_link(m, at);
        if (!o) return 0;
        w0 = rw(ls, o);
        if ((cls == 0xffff || ((w0 & 0x1c0) >> 6) == cls) && (sub == 0xffff || ((w0 & 0x30) >> 4) == sub)
            && (type == 0xffff || (w0 & 0xf) == type))
            return o;
        if (recurse && !(w0 & 0x8000) && (rw(ls, (uint16_t)(o + 6)) >> 6)) {
            uint16_t inner = (uint16_t)(o + 6), f = object_find_matching(m, &inner, recurse, cls, sub, type);
            if (f) {
                *link = inner;
                return f;
            }
        }
        at = (uint16_t)(o + 4);
    }
}

/* object_find_in_tilemap(class, sub, type, &x, &y): the next
 * matching object on the level, 0 at the end -- a RESUMABLE scan. It starts
 * at the tile (*x, *y) it is handed, wrapping an x past 63 into the next
 * row, walks the tile words from there, and runs object_find_matching with
 * recursion over every tile that holds a chain; on a hit *x and *y are left
 * at that tile, so a caller steps *x and calls again. 0xffff in a field
 * matches any. The answer is "something was found". */
uint16_t object_find_in_tilemap(uw_motion *m, uint16_t cls, uint16_t sub, uint16_t type,
                                int16_t *x, int16_t *y) {
    uint8_t *ls = m->lseg;
    if (*x > 0x3f) { *x = 0; (*y)++; }
    for (; *y <= 0x3f; (*y)++, *x = 0) {
        for (; *x < 0x40; (*x)++) {
            uint16_t link = (uint16_t)(tile_ptr(m, (uint16_t)*x, (uint16_t)*y) + 2), o;
            if (!(rw(ls, link) >> 6)) continue;
            o = object_find_matching(m, &link, 1, cls, sub, type);
            if (o) return o;
        }
    }
    return 0;
}

/* player_near_tile(mode, x, y): 0 when the player's tile is
 * within 8 of (x, y) in BOTH axes, 1 otherwise -- and 1 outright when mode
 * is clear. Its callers read a 1 as "go ahead", so the name reads backwards:
 * with mode 0 everything goes ahead, and with mode set only what is far from
 * the player does. */
static int player_near_tile(uw_motion *m, int8_t mode, int16_t x, int16_t y) {
    uint8_t *ls = m->lseg;
    uint16_t pl = rw(m->ds, TRACKED_OBJECT);
    int16_t d;
    if (!mode) return 1;
    d = (int16_t)((rw(ls, (uint16_t)(pl + 0x16)) >> 10) - x);
    if (d < 0) d = (int16_t)-d;
    if (d < 8) {
        d = (int16_t)(((rw(ls, (uint16_t)(pl + 0x16)) & 0x3f0) >> 4) - y);
        if (d < 0) d = (int16_t)-d;
        if (d < 8) return 0;
    }
    return 1;
}

/* trigger_create_object_trap(mode):
 * every create-object trap on the level -- class 6 subclass 0 type 7, item
 * id 0x187, which STRINGS.PAK block 4 calls "a create object trap" --
 * whose word 0 bits 9..12 are clear, with its linked object taken; a MOBILE
 * one is marked for deletion (+0x0d bit 8) and, where player_near_tile
 * agrees, the trap fires. So the trap is not sprung by walking over it:
 * this sweeps the level and fires the ones whose target is a live creature,
 * which is how a level puts its creatures back. The game step runs it with
 * mode 1, so only traps more than eight tiles from the Avatar fire, and a
 * sleep runs it with 0, where every one does. */
void trigger_create_object_trap(uw_motion *m, int8_t mode, uint16_t bp) {
    uint8_t *ls = m->lseg;
    int16_t x = 0, y = 0;
    uint16_t o;
    while ((o = object_find_in_tilemap(m, 6, 0, 7, &x, &y)) != 0) {
        if (!(rw(ls, o) & 0x1e00)) {
            uint16_t link = (uint16_t)(o + 6), t = deref_link(m, link);
            if (t && obj_index_of(m, t) < 0x100) {
                ww(ls, (uint16_t)(t + 0xd), (uint16_t)((rw(ls, (uint16_t)(t + 0xd)) & 0xfeff) | 0x100));
                if (player_near_tile(m, mode, x, y))
                    trap_dispatch(m, o, (uint16_t)x, (uint16_t)y, bp);
            }
        }
        x++;
    }
}

/* find_and_close_doors(mode): every
 * class-5 object on the level through object_find_in_tilemap, and for an
 * OPEN door -- its id's low nibble 8 or above -- on a tile whose byte 1 has
 * bit 7 clear, three draws in ten (rand() * 10 / 0x8000 < 3)
 * records the tile at 0x269a/0x269c and closes the door, if
 * player_near_tile agrees. With mode 0, which is what sleeping passes, it
 * always agrees; with the flag at 0x0aaf set, eight level-effect ticks
 * follow so the doors that were closed finish swinging. The passage-of-time
 * sweep. */
void find_and_close_doors(uw_motion *m, int8_t mode) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    int16_t x = 0, y = 0, i;
    uint16_t o;
    while ((o = object_find_in_tilemap(m, 5, 0, 0xffff, &x, &y)) != 0) {
        uint16_t tp = tile_ptr(m, (uint16_t)x, (uint16_t)y);
        if (!(ls[(uint16_t)(tp + 1)] & 0x80) && (rw(ls, o) & 0xf) > 7
            && (int32_t)rt_rand(m) * 10 / 0x8000 < 3) {
            ww(ds, ACTION_TARGET_X, (uint16_t)x);
            ww(ds, ACTION_TARGET_Y, (uint16_t)y);
            if (player_near_tile(m, mode, x, y)) door_close(m, o);
        }
        x++;
    }
    if (mode == 0 && ds[EFFECTS_ON_MOVE])
        for (i = 0; i < 8; i++) level_effects_tick(m, 1, (uint16_t)(FRAME_BP - 0x80));
}

/* level_effect_find: the record naming the object, or -1; the
 * count is a signed byte. */
static int level_effect_find(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds;
    uint16_t idx = obj_index_of(m, obj);
    int i, n = (int8_t)ds[EFFECT_COUNT];
    for (i = 0; i < n; i++)
        if (((rw(ds, (uint16_t)(EFFECT_LIST + i * 6)) >> 6) & 0x3ff) == idx) break;
    return i == n ? -1 : i;
}

/* level_effect_get_timer: -2 for an object with no record. */
static int16_t level_effect_get_timer(uw_motion *m, uint16_t obj) {
    int i = level_effect_find(m, obj);
    return i < 0 ? -2 : rs(m->ds, (uint16_t)(EFFECT_LIST + 2 + i * 6));
}

/* level_effect_set_timer. */
static void level_effect_set_timer(uw_motion *m, uint16_t obj, int16_t v) {
    int i = level_effect_find(m, obj);
    if (i >= 0) ww(m->ds, (uint16_t)(EFFECT_LIST + 2 + i * 6), (uint16_t)v);
}

/* door_reverse_motion: the direction bit of word 0 bits 9..12
 * flipped (a stage without bit 3 gains 8, one with it keeps its low three)
 * and the countdown reflected about the swing's length -- 5, or 4 for a
 * portcullis, a closed one (class 5, low bits 6) or a moving one (class 7,
 * quality low bits 6). */
static int16_t door_reverse_stage(uw_motion *m, uint16_t obj) {
    uint8_t *ls = m->lseg;
    uint16_t w0 = rw(ls, obj), stage = (uint16_t)((w0 & 0x1e00) >> 9);
    int16_t limit = 5;
    if ((((w0 & 0x1c0) >> 6) == 5 && (w0 & 7) == 6) || (((w0 & 0x1c0) >> 6) == 7 && (ls[(uint16_t)(obj + 6)] & 7) == 6))
        limit = 4;
    stage = (uint16_t)((stage & 8) ? (stage & 7) : (((stage & 7) + 8) & 0xf));
    ww(ls, obj, (uint16_t)((w0 & 0xe1ff) | (stage << 9)));
    return limit;
}

static void door_reverse_motion(uw_motion *m, uint16_t obj) {
    int16_t limit = door_reverse_stage(m, obj), t = level_effect_get_timer(m, obj);
    if (t >= 0) level_effect_set_timer(m, obj, (int16_t)(limit - t));
}

/* The swing's sound at the door's fine place in the action target tile:
 * 0x14 for low bits 6, else 0xb -- read
 * after door_start_moving has made the door 0x1cf, whose low bits are 7,
 * so every door, the portcullis too, sounds 0xb. */
static void door_sound(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds;
    uint16_t w0 = rw(m->lseg, obj), w1 = rw(m->lseg, (uint16_t)(obj + 2));
    play_sound_effect_at_xy(m, (uint8_t)((w0 & 7) == 6 ? 0x14 : 0xb),
                            (int16_t)(rs(ds, ACTION_TARGET_TILE_X) * 8 + (w1 >> 13)),
                            (int16_t)(rs(ds, ACTION_TARGET_TILE_Y) * 8 + ((w1 & 0x1c00) >> 10)), 0);
}

/* door_start_moving: the old id's low six bits into the quality,
 * the id made 0x1cf, and an effect of 5 counts (4 when the low bits were 6)
 * at the action target tile. */
static void door_start_moving(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w0 = rw(ls, obj);
    int16_t timer = (int16_t)((w0 & 7) == 6 ? 4 : 5);
    ls[(uint16_t)(obj + 6)] = (uint8_t)((ls[(uint16_t)(obj + 6)] & 0xc0) | (w0 & 0x3f));
    ww(ls, obj, (uint16_t)((w0 & 0xfe00) | 0x1cf));
    level_effect_add(m, obj_index_of(m, obj), timer, 0, ds[ACTION_TARGET_X], ds[ACTION_TARGET_Y]);
}

static uint16_t trap_run(uw_motion *m, uint16_t who, uint16_t what, uint16_t trap, uint16_t x, uint16_t y, uint16_t bp);
static void object_free_with_owned(uw_motion *m, uint16_t link, uint16_t obj);

/* trigger_object_link(who, obj, kind, x, y) where the port does
 * not model the caller's stack: the target tile and a
 * frame of the port's under the event frame. */
void trigger_object_link_port(uw_motion *m, uint16_t who, uint16_t obj, int kind) {
    trigger_object_link_at(m, who, obj, kind, m->ds[ACTION_TARGET_TILE_X], m->ds[ACTION_TARGET_TILE_Y],
                           (uint16_t)(FRAME_BP - 0x80));
}

/* ... whole, with the instigator, the tile and this routine's frame: eight
 * bytes of locals under it, trigger_chain's seven words of arguments and
 * trap_run's eight. A caller whose own frame is not known passes one of
 * the port's (FRAME_BP - 0x80). ftrig2: a thing with a use trigger burned
 * by the flame wind, object_damage's kind 4 into trigger_chain, the text
 * trap's print and trap_consume_use. */
void trigger_object_link_at(uw_motion *m, uint16_t who, uint16_t obj, int kind, uint16_t x, uint16_t y, uint16_t bp) {
    uint8_t *ls = m->lseg;
    uint16_t link = (uint16_t)(obj + 6), f;
    if ((rw(ls, obj) & 0x8000) || !(rw(ls, (uint16_t)(obj + 6)) >> 6)) return;
    f = object_find_matching(m, &link, 0, 6, 0xffff, 0xffff);
    if (!f) return;
    if (((rw(ls, f) & 0x30) >> 4) >= 2) {
        trigger_chain(m, who, obj, f, (int16_t)kind, (uint16_t)(bp - 8 - 14 - 4 - 2));
    } else if (!(rw(ls, f) & 0x1e00) && kind == 4) {
        trap_run(m, who, obj, f, x, y, (uint16_t)(bp - 8 - 16 - 4 - 2));
        object_free_with_owned(m, link, f);
    }
}

/* door_open: a moving door on its way shut (quality nibble 8 and
 * up) loses 8 and reverses; a closed door (id nibble under 8) has quality
 * bit 0 cleared, its z raised by 0x18 unless the nibble is 6, the link's
 * triggers of kind 7 fired and starts moving; either sounds (door_sound).
 * Anything else is left. */
void door_open(uw_motion *m, uint16_t actor, uint16_t obj) {
    uint8_t *ls = m->lseg;
    uint16_t w0 = rw(ls, obj);
    if ((w0 & 0x1ff) == 0x1cf) {
        uint8_t q = (uint8_t)(ls[(uint16_t)(obj + 6)] & 0xf);
        if (q < 8) return;
        ls[(uint16_t)(obj + 6)] = (uint8_t)((ls[(uint16_t)(obj + 6)] & 0xc0) | ((q - 8) & 0x3f));
        door_reverse_motion(m, obj);
        door_sound(m, obj);
        return;
    }
    if ((w0 & 0xf) >= 8) return;
    ls[(uint16_t)(obj + 6)] = (uint8_t)((ls[(uint16_t)(obj + 6)] & 0xc0) | (ls[(uint16_t)(obj + 6)] & 0x3e));
    if ((w0 & 0xf) != 6) {
        uint16_t w1 = rw(ls, (uint16_t)(obj + 2));
        ww(ls, (uint16_t)(obj + 2), (uint16_t)((w1 & 0xff80) | (((w1 & 0x7f) + 0x18) & 0x7f)));
    }
    trigger_object_link_port(m, actor, obj, 7);   /* trigger_object_link(actor, door, 7, the target tile) */
    door_start_moving(m, obj);
    door_sound(m, obj);
}

/* door_close: a moving door on its way open (nibble under 8)
 * gains 8 and reverses; an open door (id nibble 8 and up) starts moving;
 * either sounds. */
void door_close(uw_motion *m, uint16_t obj) {
    uint8_t *ls = m->lseg;
    uint16_t w0 = rw(ls, obj);
    if ((w0 & 0x1ff) == 0x1cf) {
        uint8_t q = (uint8_t)(ls[(uint16_t)(obj + 6)] & 0xf);
        if (q >= 8) return;
        ls[(uint16_t)(obj + 6)] = (uint8_t)((ls[(uint16_t)(obj + 6)] & 0xc0) | ((q + 8) & 0x3f));
        door_reverse_motion(m, obj);
        door_sound(m, obj);
        return;
    }
    if ((w0 & 0xf) < 8) return;
    door_start_moving(m, obj);
    door_sound(m, obj);
}

/* door_closing_check(effect, step), from the instructions: a
 * door swinging shut, each step, asked whether it fits closed where it
 * stands -- item_fits_in_tile(0x140 + the quality's subclass and nibble,
 * the door, its fine place in the effect's tile (into motion_tile_x/y),
 * z less 0x18 unless the low bits are 6, slope 1, radius 8), the query on
 * its own frame 0x22 under this one's. Refused -- something in the
 * doorway -- the swing turns back: the stage's direction bit cleared and
 * its low three made (low three - step - 1) & 7, and with a countdown the
 * record's becomes the budget (5; 4 for a portcullis door, class 5 low
 * bits 6, or a moving one whose quality's are) less it, plus one, which
 * the tick leaves alone (level_effect_moved). */
void level_effect_door_move(uw_motion *m, uw_effects *e, int index, int step, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t p = uw_effect_object(e, index), w0, w1, w3, stage;
    int16_t z, budget = 5;
    if (!p) return;
    w0 = rw(ls, p);
    w1 = rw(ls, (uint16_t)(p + 2));
    w3 = rw(ls, (uint16_t)(p + 6));
    z = (int16_t)(w1 & 0x7f);
    if ((w3 & 7) != 6) z = (int16_t)(z - 0x18);
    ww(ds, MOTION_TILE_X, e->rec[index].tile_x);
    ww(ds, MOTION_TILE_Y, e->rec[index].tile_y);
    if (item_fits_in_tile(m, (uint16_t)((w3 & 0x30) + (w3 & 0xf) + 0x140), obj_index_of(m, p),
                          (int16_t)(e->rec[index].tile_x * 8 + (w1 >> 13)),
                          (int16_t)(e->rec[index].tile_y * 8 + ((w1 & 0x1c00) >> 10)), z, 1, 8,
                          (uint16_t)(bp - 0xa - 4 - 14 - 4 - 2)))
        return;
    if ((((w0 & 0x1c0) == 0x140) && ((w0 & 7) == 6)) || (((w0 & 0x1c0) == 0x1c0) && ((w3 & 7) == 6)))
        budget = 4;
    stage = (uint16_t)((w0 >> 9) & 7);
    stage = (uint16_t)((stage - step - 1) & 0xf);
    ww(ls, p, (uint16_t)((w0 & 0xe1ff) | (stage << 9)));
    if (e->rec[index].timer >= 0) {
        e->rec[index].timer = (int16_t)(budget - e->rec[index].timer + 1);
        e->moved = 1;
    }
}

/* level_effect_expire's kind 0xf, from the instructions: the
 * moving door at the end of its swing, with `bp` the expiry's frame. Its
 * quality holds the old id's low six bits (door_start_moving): bits 4..5
 * the subclass, 0..3 the nibble. Swinging open (the stage's bit 3 clear)
 * the nibble gains 8 -- the open door. Swinging shut it loses 8, the z
 * comes down 0x18 unless the low bits are 6 (the portcullis), and the
 * closed door must fit where it stands: item_fits_in_tile(0x140 + subclass
 * * 0x10 + nibble, the door, its fine place in the effect's tile -- which
 * goes into motion_tile_x/y -- z, slope 1, radius 8), its query on its own
 * frame 0x26 under the expiry's (0xe of locals, SI, DI, seven words of
 * arguments). Refused, the quality takes the closed nibble and the door
 * turns back as door_reverse_motion turns it -- the countdown the RECORD's
 * here, which is the list the tick is walking -- and 0: the record stays.
 * Accepted, sound 0xc at the door. Either way seated then: the z written,
 * the id 0x140 + subclass * 0x10 + nibble, the quality cleared, the
 * stage's bit 3 flipped (clear: low three + 8; set: low three); 1. */
int level_effect_door_seat(uw_motion *m, uw_effects *e, int index, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t p = uw_effect_object(e, index), w0, w1, stage, id;
    uint8_t q;
    int16_t z, sub, nib;
    if (!p) return 1;
    w0 = rw(ls, p);
    w1 = rw(ls, (uint16_t)(p + 2));
    q = ls[(uint16_t)(p + 6)];
    sub = (int16_t)((q & 0x3f) >> 4);
    nib = (int16_t)(q & 0xf);
    z = (int16_t)(w1 & 0x7f);
    if (!(((w0 & 0x1e00) >> 9) & 8)) {
        nib |= 8;
    } else {
        int16_t fx, fy;
        if (nib >= 8) nib = (int16_t)(nib - 8);
        if ((nib & 7) != 6) z = (int16_t)(z - 0x18);
        ww(ds, MOTION_TILE_X, e->rec[index].tile_x);
        ww(ds, MOTION_TILE_Y, e->rec[index].tile_y);
        fx = (int16_t)(e->rec[index].tile_x * 8 + (w1 >> 13));
        fy = (int16_t)(e->rec[index].tile_y * 8 + ((w1 & 0x1c00) >> 10));
        if (!item_fits_in_tile(m, (uint16_t)(0x140 + sub * 0x10 + nib), obj_index_of(m, p), fx, fy, z, 1, 8,
                               (uint16_t)(bp - 0xe - 4 - 14 - 4 - 2))) {
            int16_t limit;
            ls[(uint16_t)(p + 6)] = (uint8_t)((q & 0xc0) | (nib & 0x3f));
            limit = door_reverse_stage(m, p);
            if (e->rec[index].timer >= 0) e->rec[index].timer = (int16_t)(limit - e->rec[index].timer);
            return 0;
        }
        play_sound_effect_at_xy(m, 0xc, fx, fy, 0);
    }
    ww(ls, (uint16_t)(p + 2), (uint16_t)((w1 & 0xff80) | (z & 0x7f)));
    id = (uint16_t)(0x140 | (sub << 4) | (nib & 0xf));
    w0 = (uint16_t)((rw(ls, p) & 0xfe00) | id);
    ls[(uint16_t)(p + 6)] = (uint8_t)(ls[(uint16_t)(p + 6)] & 0xc0);
    stage = (uint16_t)((w0 & 0x1e00) >> 9);
    stage = (uint16_t)((stage & 8) ? (stage & 7) : ((stage & 7) + 8));
    ww(ls, p, (uint16_t)((w0 & 0xe1ff) | (stage << 9)));
    return 1;
}

uint16_t uw_motion_find_matching(uw_motion *m, uint16_t *link, int recurse, uint16_t cls, uint16_t sub, uint16_t type) {
    return object_find_matching(m, link, recurse, cls, sub, type);
}

void uw_motion_door_open(uw_motion *m, uint16_t actor, uint16_t obj) { door_open(m, actor, obj); }
void uw_motion_door_close(uw_motion *m, uint16_t obj) { door_close(m, obj); }

/* door_toggle(actor, door): a closed door (id nibble under 8)
 * opened by the actor, else closed. */
void uw_motion_door_toggle(uw_motion *m, uint16_t actor, uint16_t obj) {
    if ((rw(m->lseg, obj) & 0xf) < 8) door_open(m, actor, obj);
    else door_close(m, obj);
}

/* door_unlock_attempt(actor, door, skill), from the instructions:
 * the lock (item 0x10f) among the things chained from a door's +6, against a
 * key's lock number (skill above 0) or a skill negated (below 0). 1 when
 * there is no lock (or the door is a quantity). A lock not locked (word 0
 * bit 9 clear) is 4 -- but a key on a closed door that it fits locks it, 2,
 * and one that does not is 0; a key on an open door, or on a container
 * 0x80..0x8b with bit 0 set, is 4 too. A locked lock with a skill: quality
 * (+2's low seven bits) 0xf, or 0xe against a skill under 0x1f, refuses,
 * otherwise check_skill_roll(skill, quality * 3) must come out 1 or 2; with
 * a key, the key's number must be the lock's +6 link bits 0..8. Unlocked:
 * trigger_object_link(actor, door, 6) and the lock taken off the chain and
 * freed, or, with its bit 10 set, left with bit 9 clear; 3. The actor is only
 * passed on to the link. */
int door_unlock_attempt(uw_motion *m, uint16_t actor, uint16_t door, int16_t skill) {
    uint8_t *ls = m->lseg;
    uint16_t w0 = rw(ls, door), link = (uint16_t)(door + 6), lock, key;
    if ((w0 & 0x8000) || !((rw(ls, link) >> 6) & 0x3ff)) return 1;
    lock = object_find_matching(m, &link, 0, 4, 0, 0xf);
    if (!lock) return 1;
    key = (uint16_t)((rw(ls, (uint16_t)(lock + 6)) >> 6) & 0x1ff);
    if (!(rw(ls, lock) & 0x200)) {
        if (skill <= 0) return 4;
        if (((w0 & 0x1f0) >> 4) == 0x14 && (w0 & 0xf) >= 8) return 4;
        if (((w0 & 0x1f0) >> 4) == 8 && (w0 & 0xf) < 0xc && (w0 & 1)) return 4;
        if (key != (uint16_t)skill) return 0;
        ww(ls, lock, (uint16_t)(rw(ls, lock) | 0x200));
        return 2;
    }
    if (skill < 0) {
        uint16_t q = (uint16_t)(rw(ls, (uint16_t)(lock + 2)) & 0x7f);
        if ((q == 0xe && -skill < 0x1f) || q == 0xf) return 0;
        if (check_skill_roll(m, -skill, (int)(q * 3)) <= 0) return 0;
    } else if (skill == 0 || !key || key != (uint16_t)skill) {
        return 0;
    }
    trigger_object_link_port(m, actor, door, 6);   /* trigger_object_link(actor, door, 6, the target tile) */
    if (!(rw(ls, lock) & 0x400)) {
        uw_objpool pool;
        pool_from_ds(m, &pool);
        uw_object_list_remove(&pool, link, lock);
        uw_obj_free(&pool, lock);
        pool_to_ds(m, &pool);
    } else {
        ww(ls, lock, (uint16_t)(rw(ls, lock) & 0xfdff));
    }
    return 3;
}

/* use_door_furniture_or_switch(actor, obj), object_use_dispatch's
 * class 5, from the instructions -- by subclass. 0, doors: a closed one goes
 * through door_unlock_attempt with no skill, and opens (door_open) unless
 * that refused -- when the player hears "The <door> is locked." -- or the
 * actor is not the player and the door's +6 bit 0 is set; an open one
 * closes. 1: a shrine (nibble 7) chants, a barrel or chest (0xb, 0xd) not a
 * quantity opens as a container; nothing else. 2: a lever or switch (1, 2)
 * steps word 0 bits 9..12 to (bits + 1) & 7 and posts event 2; the rest is
 * looked at. 3: a button's sound at the action target, its id's nibble
 * bit 3 toggled, event 2. */
void use_door_furniture_or_switch(uw_motion *m, uint16_t actor, uint16_t obj) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w0 = rw(ls, obj), nib = (uint16_t)(w0 & 0xf);
    switch ((w0 & 0x30) >> 4) {
    case 0:
        if (nib >= 8) {
            door_close(m, obj);
        } else if (!door_unlock_attempt(m, actor, obj, 0)) {
            if ((rw(ls, actor) & 0x1ff) == 0x7f) object_locked_message(m, obj);
        } else if (actor == rw(ds, TRACKED_OBJECT) || !(ls[(uint16_t)(obj + 6)] & 1)) {
            door_open(m, actor, obj);
        }
        break;
    case 1:
        if (nib == 7) chant_mantra(m);
        else if ((nib == 0xb || nib == 0xd) && !(w0 & 0x8000)) open_container(m, actor, obj, 0);
        break;
    case 2:
        if (nib == 1 || nib == 2) {
            ww(ls, obj, (uint16_t)((w0 & 0xe1ff) | (((((w0 & 0x1e00) >> 9) + 1) & 7) << 9)));
            ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));      /* post_event(2) */
        } else {
            look_at_scenery(m, obj, -1);
        }
        break;
    default:
        /* the click, in the middle of the target tile */
        play_sound_effect_at_xy(m, 0x13, (int16_t)(rs(ds, ACTION_TARGET_TILE_X) * 8 + 3), (int16_t)(rs(ds, ACTION_TARGET_TILE_Y) * 8 + 3), 0);
        ww(ls, obj, (uint16_t)((w0 & 0xfff0) | ((nib + 8) & 0xf)));
        ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));          /* post_event(2) */
        break;
    }
}

/* ---- traps and triggers ------------------------------------------------- */

/* object_find_link(&link, flags, index): the object of that
 * index along the chain from `link`, a non-quantity's contents searched
 * before the walk moves on; object_find_list keeps the link heading the
 * chain it was found in. 0 for none. */
uint16_t object_find_link(uw_motion *m, uint16_t link, uint16_t index) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t at = link, o;
    if (!((rw(ls, link) >> 6) & 0x3ff)) return 0;
    ww(ds, OBJECT_FIND_LIST, link);
    ww(ds, (uint16_t)(OBJECT_FIND_LIST + 2), rw(ds, (uint16_t)(TILEMAP_PTR + 2)));
    for (;;) {
        o = deref_link(m, at);
        if (obj_index_of(m, o) == index) break;
        if (!(rw(ls, o) & 0x8000) && ((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff)) {
            uint16_t f = object_find_link(m, (uint16_t)(o + 6), index);
            if (f) return f;
        }
        if (!((rw(ls, (uint16_t)(o + 4)) >> 6) & 0x3ff)) return 0;
        at = (uint16_t)(o + 4);
    }
    ww(ds, OBJECT_FIND_LIST, link);
    ww(ds, (uint16_t)(OBJECT_FIND_LIST + 2), rw(ds, (uint16_t)(TILEMAP_PTR + 2)));
    return o;
}

/* object_chain_remove(link, obj): a non-quantity's contents
 * cleared, the thing out of the chain when a link is given, and freed. */
void object_chain_remove(uw_motion *m, uint16_t link, uint16_t obj) {
    uint8_t *ls = m->lseg;
    uw_objpool pool;
    if (!(rw(ls, obj) & 0x8000) && ((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x3ff))
        object_chain_clear(m, (uint16_t)(obj + 6));
    pool_from_ds(m, &pool);
    if (link) uw_object_list_remove(&pool, link, obj);
    uw_obj_free(&pool, obj);
    pool_to_ds(m, &pool);
}

/* object_free_owned_in_chain: along the chain from `link`, and
 * into contents, every trigger (items 0x1a0..0x1af) whose +6 names the
 * owner is taken out of the chain, freed, its +6 cleared and counted off. */
static void object_free_owned_in_chain(uw_motion *m, uint16_t link) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t o;
    for (o = deref_link(m, link); o; o = deref_link(m, (uint16_t)(o + 4))) {
        if ((rw(ls, o) & 0x1f0) == 0x1a0 && ((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff) == rw(ds, OWNED_INDEX)) {
            uw_objpool pool;
            pool_from_ds(m, &pool);
            uw_object_list_remove(&pool, link, o);
            uw_obj_free(&pool, o);
            pool_to_ds(m, &pool);
            ww(ls, (uint16_t)(o + 6), (uint16_t)(rw(ls, (uint16_t)(o + 6)) & 0x3f));
            ww(ds, OWNED_COUNT, (uint16_t)(rw(ds, OWNED_COUNT) - 1));
        }
        if (!(rw(ls, o) & 0x8000) && ((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff))
            object_free_owned_in_chain(m, (uint16_t)(o + 6));
    }
}

/* object_free_with_owned(link, obj): the triggers it owns --
 * as many as its word 0 bits 9..12 say, swept for over the tile array --
 * then the thing itself, found from `link` and removed. */
static void object_free_with_owned(uw_motion *m, uint16_t link, uint16_t obj) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t found;
    ww(ds, OWNED_COUNT, (uint16_t)((rw(ls, obj) & 0x1e00) >> 9));
    if (rw(ds, OWNED_COUNT)) {
        uint16_t t = rw(ds, TILEMAP_PTR), i;
        ww(ds, OWNED_INDEX, obj_index_of(m, obj));
        for (i = 0; rs(ds, OWNED_COUNT) > 0 && i < 0x1000; i++, t = (uint16_t)(t + 4))
            if ((rw(ls, (uint16_t)(t + 2)) >> 6) & 0x3ff) object_free_owned_in_chain(m, (uint16_t)(t + 2));
    }
    found = object_find_link(m, link, obj_index_of(m, obj));
    if (found) object_chain_remove(m, rw(ds, OBJECT_FIND_LIST), found);
}

/* trap_consume_use(link, trigger): one use off the trap the
 * trigger links (its word 0 bits 9..12) and the trigger freed out of the
 * chain -- or, at the last use, the trap freed with what it owns from the
 * tile the trigger names. */
static void trap_consume_use(uw_motion *m, uint16_t link, uint16_t trig) {
    uint8_t *ls = m->lseg;
    uint16_t trap = deref_link(m, (uint16_t)(trig + 6)), uses = (uint16_t)((rw(ls, trap) & 0x1e00) >> 9);
    uw_objpool pool;
    if (uses == 1) {
        object_free_with_owned(m, (uint16_t)(tile_ptr(m, ls[(uint16_t)(trig + 4)] & 0x3f, ls[(uint16_t)(trig + 6)] & 0x3f) + 2),
                               trap);
        return;
    }
    ww(ls, trap, (uint16_t)((rw(ls, trap) & 0xe1ff) | (((uses - 1) & 0xf) << 9)));
    pool_from_ds(m, &pool);
    uw_object_list_remove(&pool, link, trig);
    uw_obj_free(&pool, trig);
    pool_to_ds(m, &pool);
}

/* trap_after_firing(link, obj): how object_chain_clear takes a
 * class-6 thing out of a chain -- a trap freed with what it owns, a trigger
 * through trap_consume_use. */
void trap_after_firing(uw_motion *m, uint16_t link, uint16_t obj) {
    if (((rw(m->lseg, obj) & 0x30) >> 4) < 2) object_free_with_owned(m, link, obj);
    else trap_consume_use(m, link, obj);
}

/* area_contains_match(obj): whether a creature within four
 * tiles of it, not it and not the player, has +0x0d bit 8 --
 * effect_area_apply over area_match_callback. `bp` is this routine's frame. */
static int area_contains_match(uw_motion *m, uint16_t obj, uint16_t bp) {
    uint8_t *ds = m->ds;
    ds[AREA_MATCH_FLAG] = 0;
    store_obj_far(m, AREA_MATCH_EXCEPT, obj);
    effect_area_apply(m, obj, 1, AREA_MATCH_FAR, 0, 0, 4, (uint16_t)(bp - 16 - 4 - 2));
    return ds[AREA_MATCH_FLAG];
}

uint16_t trap_dispatch(uw_motion *m, uint16_t trap, uint16_t x, uint16_t y, uint16_t bp);

/* projectile_fire_from_trap(trap, x, y): the missile whose item
 * id the trap's quality and owner pack (quality << 5 | owner), at speed
 * 0x14 with both aims 2, launched from the trap toward the tile (x, y).
 * `bp` is this routine's frame. */
static void projectile_fire_from_trap(uw_motion *m, uint16_t trap, uint16_t x, uint16_t y, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    ww(ds, PROJ_ITEM, (uint16_t)(((ls[(uint16_t)(trap + 4)] & 0x3f) << 5) | (ls[(uint16_t)(trap + 6)] & 0x3f)));
    ww(ds, PROJ_SPEED, 0x14);
    ww(ds, PROJ_AIM_X, 2);
    ww(ds, PROJ_AIM_Z, 2);
    ww(ds, PROJ_TARGET_X, x);
    ww(ds, PROJ_TARGET_Y, y);
    store_obj_far(m, PROJ_FIRER, trap);
    ww(ds, PROJ_HEADING, 0);
    launch_projectile(m, (uint16_t)(bp - 4 - 2));
}

/* trap_change_tile(x, y, wall, floor, height,
 * type, dx, dy, mode): the tiles x..x+dx by y..y+dy, inclusive. A height of
 * 13 or less is written (mode 1 or 3 makes it the old height + 2 - mode,
 * and then only 0..13); what stood on a floor that rose below it is lifted
 * to it, what stood exactly on a floor that fell drops with it -- a mobile
 * thing that is not a creature with its +0x0f, the player's height
 * on the way up and player_set_movement_state(0x10, 1) on the
 * way down; traps and triggers stay. Then a floor texture under 11, a wall
 * under 0x30 and a type under 10 are written, and event 6 is posted. */
static uint16_t trap_change_tile(uw_motion *m, int16_t x, int16_t y, int16_t wall, int16_t floor_tex, int16_t height,
                                 int16_t type, int16_t dx, int16_t dy, int16_t mode) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    int16_t tx, ty;
    for (tx = x; tx <= x + dx; tx++) {
        for (ty = y; ty <= y + dy; ty++) {
            uint16_t t = tile_ptr(m, (uint16_t)tx, (uint16_t)ty), old = (uint16_t)(ls[t] >> 4), now, link;
            if (mode == 1 || mode == 3) {
                height = (int16_t)(old + 2 - mode);
                if (height >= 0 && height <= 13) ls[t] = (uint8_t)((ls[t] & 0xf) | ((height & 0xf) << 4));
            } else if (height <= 13) {
                ls[t] = (uint8_t)((ls[t] & 0xf) | ((height & 0xf) << 4));
            }
            now = (uint16_t)(ls[t] >> 4);
            if (now != old) {
                for (link = (uint16_t)(t + 2); (rw(ls, link) >> 6) & 0x3ff; ) {
                    uint16_t o = deref_link(m, link), w2 = rw(ls, (uint16_t)(o + 2)), cls = (uint16_t)((rw(ls, o) & 0x1c0) >> 6);
                    if (cls != 6 && (now > old ? (w2 & 0x7f) < (uint16_t)(height << 3) : (w2 & 0x7f) == (old << 3))) {
                        ww(ls, (uint16_t)(o + 2), (uint16_t)((w2 & 0xff80) | ((height << 3) & 0x7f)));
                        if (o < rw(ds, STATIC_BASE) && cls != 1)
                            ww(ls, (uint16_t)(o + 0xf), (uint16_t)(height << 6));
                        else if (o == rw(ds, TRACKED_OBJECT)) {
                            if (now > old) ww(ds, PLAYER_Z, (uint16_t)(height << 6));
                            else set_movement_state(m, 0x10, 1);
                        }
                    }
                    link = (uint16_t)(o + 4);
                }
            }
            if (floor_tex < 0xb) ls[(uint16_t)(t + 1)] = (uint8_t)((ls[(uint16_t)(t + 1)] & 0xc3) | ((floor_tex & 0xf) << 2));
            if (wall < 0x30) ls[(uint16_t)(t + 2)] = (uint8_t)((ls[(uint16_t)(t + 2)] & 0xc0) | (wall & 0x3f));
            if (type < 10) ls[t] = (uint8_t)((ls[t] & 0xf0) | (type & 0xf));
        }
    }
    ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 6));      /* post_event(6) */
    return 2;
}

/* trap_fire(x, y, trap, who, class, arg): the
 * effect target made (x, y), then effect_dispatch_2(class, arg, trap, who)
 * -- or, for a negative class, effect_dispatch(arg, trap, who), which a
 * trap's six-bit quality never is but an enchantment's -1 is. `who` is
 * the other the effect may act on (a class 4 heal's). `bp` is this
 * routine's frame. */
uint16_t trap_fire(uw_motion *m, uint8_t x, uint8_t y, uint16_t trap, uint16_t who, int16_t cls, uint8_t arg, uint16_t bp) {
    uint8_t *ds = m->ds;
    ds[EFFECT_TARGET_X] = x;
    ds[EFFECT_TARGET_Y] = y;
    if (cls < 0) effect_dispatch(m, arg, trap, who, (uint16_t)(bp - 10 - 4 - 2));
    else effect_dispatch_2(m, (uint8_t)cls, arg, trap, who, (uint16_t)(bp - 12 - 4 - 2));
    return 2;
}

/* trap_damage(index, amount): a positive amount
 * dealt as damage type 4 at the thing's own tile, 0x10 when that kills or
 * breaks it; a negative one at the player poisons -- to the amount, when
 * that is more than the poison already there and compute_damage(1, 0x10)
 * lets it through -- and at anything else is dealt as its magnitude. */
static uint16_t trap_damage(uw_motion *m, uint16_t index, int16_t amount) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t obj = obj_at(m, index);
    if (amount < 0) {
        if (obj == rw(ds, TRACKED_OBJECT)) {
            uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
            if ((uint16_t)-amount > (uint16_t)((ds[(uint16_t)(rec + 0x5f)] >> 2) & 0xf)
                && compute_damage(m, obj, 1, 0x10))
                ds[(uint16_t)(rec + 0x5f)] = (uint8_t)((ds[(uint16_t)(rec + 0x5f)] & 0xc3) | ((-amount & 0xf) << 2));
        } else {
            amount = (int16_t)-amount;
        }
    }
    if (amount > 0) {
        uint16_t w16 = rw(ls, (uint16_t)(obj + 0x16));
        if (apply_damage(m, obj, 0, (int16_t)(w16 >> 10), (int16_t)((w16 & 0x3f0) >> 4), (uint8_t)amount, 4))
            return 0x10;
    }
    return 2;
}

/* A bit of find_landing_spot's visited map: `shl ax,cl` on a 16-bit word,
 * the count masked to five bits as the 386 does. */
static uint16_t landing_bit(uint8_t cl) {
    cl &= 0x1f;
    return (uint16_t)(cl < 16 ? 1u << cl : 0);
}

/* One neighbour for find_landing_spot's next round: unvisited, room in the
 * queue, inside the window -- the visited word read first, at whatever
 * column the tile gives, the window checked after. */
static void landing_try(uw_motion *m, uint16_t map, uint16_t next, uint8_t *n, int16_t nx, int16_t ny,
                        int8_t x0, int8_t y0, int8_t x1, int8_t y1) {
    uint8_t *ds = m->ds;
    uint16_t at = (uint16_t)(map + (uint16_t)((nx - x0) * 2)), bit = landing_bit((uint8_t)(ny - y0));
    if ((rw(ds, at) & bit) || *n >= 0x14) return;
    if (nx < x0 || nx > x1 || ny < y0 || ny > y1) return;
    ds[(uint16_t)(next + *n * 2)] = (uint8_t)nx;
    ds[(uint16_t)(next + *n * 2 + 1)] = (uint8_t)ny;
    (*n)++;
    ww(ds, at, (uint16_t)(rw(ds, at) | bit));
}

/* find_landing_spot(obj, x, y, &out_x, &out_y, relax): a breadth-first search
 * out from (x, y) over the window of ten tiles from four below each
 * coordinate (clamped to 1..0x3a, and its far side to 0x3e), two queues of
 * ten tiles and a visited word per column in the frame. Each tile in turn --
 * with `relax` first culling what object_remove will take of its heavy or
 * mobile things -- is tried with item_fits_in_tile(obj, the tile's centre,
 * its floor, raised 4 where the type's flags have bit 5, radius 8): the first
 * that fits is the answer, 1. Else its neighbours go into the next round by
 * the tile's shape -- a diagonal's two open sides, and west, south, east and
 * north for open floor and the slopes -- and 0 when a round finds none. The
 * queues and the map are the frame's (`bp`): memset clears 9 bytes of the 20
 * the map spans, and the rest is what the stack held. */
int find_landing_spot(uw_motion *m, uint16_t obj, int16_t x, int16_t y, uint16_t out_x, uint16_t out_y,
                             uint8_t relax, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t cur = (uint16_t)(bp - 0x40), next = (uint16_t)(bp - 0x68), map = (uint16_t)(bp - 0x7a), t;
    uint8_t ncur = 1, nnext = 0, i;
    int8_t x0, y0, x1, y1;
    memset(ds + cur, 0, 0x14);
    memset(ds + next, 0, 0x14);
    memset(ds + map, 0, 9);
    x0 = (int8_t)((int16_t)(x - 4) < 1 ? 1 : (uint8_t)(x - 4));
    if (x0 >= 0x3a) x0 = 0x3a;
    y0 = (int8_t)((int16_t)(y - 4) < 1 ? 1 : (uint8_t)(y - 4));
    if (y0 >= 0x3a) y0 = 0x3a;
    x1 = (int8_t)(x0 + 9);
    if (x1 >= 0x3e) x1 = 0x3e;
    y1 = (int8_t)(y0 + 9);
    if (y1 >= 0x3e) y1 = 0x3e;
    ds[cur] = (uint8_t)x;
    ds[(uint16_t)(cur + 1)] = (uint8_t)y;
    t = (uint16_t)(map + (uint16_t)((x - x0) * 2));
    ww(ds, t, (uint16_t)(rw(ds, t) | (1u << ((uint8_t)(y - y0) & 0x1f))));
    while (ncur) {
        for (i = 0; i < ncur; i++) {
            int16_t tx = (int8_t)ds[(uint16_t)(cur + i * 2)], ty = (int8_t)ds[(uint16_t)(cur + i * 2 + 1)];
            uint16_t tile = tile_ptr(m, (uint16_t)tx, (uint16_t)ty), z;
            if (relax) {
                uint16_t link;
                for (link = (uint16_t)(tile + 2); (rw(ls, link) >> 6) & 0x3ff; ) {
                    uint16_t o = deref_link(m, link);
                    if (prop(m, obj_id(m, o), 0) || o < rw(ds, STATIC_BASE))
                        object_remove(m, (uint16_t)(tile + 2), o, 0);
                    link = (uint16_t)(o + 4);
                }
            }
            z = (uint16_t)((((ls[tile] >> 4) & 0xf) << 3) + ((ds[(uint16_t)(TILE_TYPE_FLAGS + (ls[tile] & 0xf))] & 0x20) ? 4 : 0));
            if (item_fits_in_tile(m, obj_id(m, obj), obj_index_of(m, obj), (int16_t)(tx * 8 + 3), (int16_t)(ty * 8 + 3),
                                  (int16_t)z, 0, 8, (uint16_t)(bp - 0x7a - 4 - 14 - 4 - 2))) {
                ww(ds, out_x, (uint16_t)tx);
                ww(ds, out_y, (uint16_t)ty);
                return 1;
            }
            switch (ls[tile] & 0xf) {
            case 0:
                break;
            case 2:
                landing_try(m, map, next, &nnext, (int16_t)(tx + 1), ty, x0, y0, x1, y1);
                landing_try(m, map, next, &nnext, tx, (int16_t)(ty - 1), x0, y0, x1, y1);
                break;
            case 3:
                landing_try(m, map, next, &nnext, (int16_t)(tx - 1), ty, x0, y0, x1, y1);
                landing_try(m, map, next, &nnext, tx, (int16_t)(ty - 1), x0, y0, x1, y1);
                break;
            case 4:
                landing_try(m, map, next, &nnext, (int16_t)(tx + 1), ty, x0, y0, x1, y1);
                landing_try(m, map, next, &nnext, tx, (int16_t)(ty + 1), x0, y0, x1, y1);
                break;
            case 5:
                landing_try(m, map, next, &nnext, (int16_t)(tx - 1), ty, x0, y0, x1, y1);
                landing_try(m, map, next, &nnext, tx, (int16_t)(ty + 1), x0, y0, x1, y1);
                break;
            default:
                landing_try(m, map, next, &nnext, (int16_t)(tx - 1), ty, x0, y0, x1, y1);
                landing_try(m, map, next, &nnext, tx, (int16_t)(ty - 1), x0, y0, x1, y1);
                landing_try(m, map, next, &nnext, (int16_t)(tx + 1), ty, x0, y0, x1, y1);
                landing_try(m, map, next, &nnext, tx, (int16_t)(ty + 1), x0, y0, x1, y1);
                break;
            }
        }
        t = cur;
        cur = next;
        next = t;
        ncur = nnext;
        nnext = 0;
    }
    return 0;
}

/* trap_teleport(who, x, y, level): 2 unless the
 * level is this one or the player is going; to this level (or level 0) at a
 * real tile the destination is find_landing_spot's, 2 when it finds none;
 * for the player the pending teleport is set and event
 * 0x20 posted. 0x10 when it goes -- a creature included, which it does not
 * move. `bp` is this routine's frame. */
uint16_t trap_teleport(uw_motion *m, uint16_t who, int16_t x, int16_t y, int16_t level, uint16_t bp) {
    uint8_t *ds = m->ds;
    uint16_t player = rw(ds, TRACKED_OBJECT);
    if (level != rs(ds, CURRENT_LEVEL_WORD) && who != player) return 2;
    if ((level == 0 || level == rs(ds, CURRENT_LEVEL_WORD)) && x != 0x3f && y != 0x3f) {
        level = rs(ds, CURRENT_LEVEL_WORD);
        if (!find_landing_spot(m, who, x, y, (uint16_t)(bp - 2), (uint16_t)(bp - 4), 0,
                               (uint16_t)(bp - 6 - 4 - 14 - 4 - 2)))
            return 2;
        x = rs(ds, (uint16_t)(bp - 2));
        y = rs(ds, (uint16_t)(bp - 4));
    }
    if (who == player) {
        ww(ds, TELEPORT_DEST_LEVEL, (uint16_t)level);
        ww(ds, TELEPORT_DEST_X, (uint16_t)x);
        ww(ds, TELEPORT_DEST_Y, (uint16_t)y);
        ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 0x20));   /* post_event(0x20) */
    }
    return 0x10;
}

/* trap_run(who, what, trap, x, y): the chain's instigator and
 * instrument kept unless an outer run holds them, trap_dispatch, and the
 * instigator let go. */
static uint16_t trap_run(uw_motion *m, uint16_t who, uint16_t what, uint16_t trap, uint16_t x, uint16_t y,
                         uint16_t bp) {
    uint8_t *ds = m->ds;
    uint16_t r;
    if (!(rw(ds, TRAP_WHO) | rw(ds, (uint16_t)(TRAP_WHO + 2)))) {
        store_obj_far(m, TRAP_WHO, who);
        store_obj_far(m, TRAP_WHAT, what);
    }
    r = trap_dispatch(m, trap, x, y, (uint16_t)(bp - 2 - 8 - 4 - 2));
    ww(ds, TRAP_WHO, 0);
    ww(ds, (uint16_t)(TRAP_WHO + 2), 0);
    return r;
}

/* trigger_chain(who, what, trigger, event): 2 unless a trigger
 * (subclass 2) fires. For an event (not -1): a switch that is on (items
 * 0x170..0x17f, type over 7) passes it to the next trigger along; the
 * trigger's type must answer the event (trigger_props); the player needs
 * its bit 11 and, for a look (5) with a difficulty, a Search roll; a
 * creature its bit 12 and a trigger not of major class 5; anything else is
 * refused by bit 12 without bit 11, unless the trigger is of class 5.
 * Then trap_run on the trap it links, at its (x, y), and unless bit 10 says
 * it stays, the trap freed with what it owns -- and 0x20. */
uint16_t trigger_chain(uw_motion *m, uint16_t who, uint16_t what, uint16_t trig, int16_t kind,
                              uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w0 = rw(ls, trig), trap, x, y, r;
    if (((w0 & 0x30) >> 4) != 2) return 2;
    if (kind >= 0) {
        if (what && ((rw(ls, what) & 0x1f0) >> 4) == 0x17 && (rw(ls, what) & 0xf) > 7
            && ((rw(ls, (uint16_t)(trig + 4)) >> 6) & 0x3ff))
            return trigger_chain(m, who, what, deref_link(m, (uint16_t)(trig + 4)), kind,
                                 (uint16_t)(bp - 0xa - 4 - 14 - 4 - 2));
        if (ds[(uint16_t)(TRIGGER_PROPS + (w0 & 0xf))] != kind) return 2;
        if (!who) {
            /* no instigator (the conversation's gronk_door passes a null
             * one): the original reads the interrupt table's first word as
             * the object; the port takes it for neither the Avatar nor a
             * creature */
            UW_NOT_CARRIED(m->not_carried);
            if ((w0 & 0x1000) && ((w0 & 0x1c0) >> 6) != 5 && !(w0 & 0x800)) return 2;
        } else if ((rw(ls, who) & 0x1ff) == 0x7f) {
            uint16_t difficulty = (uint16_t)(rw(ls, (uint16_t)(trig + 2)) & 0x7f);
            if (!(w0 & 0x800)) return 2;
            if (kind == 5 && difficulty
                && check_skill_roll(m, ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x2c)], difficulty) <= 0)
                return 2;
        } else if (((rw(ls, who) & 0x1c0) >> 6) == 1) {
            /* a creature: the trigger's bit 12, on a trigger not of major
             * class 5 */
            if (!(w0 & 0x1000) || ((w0 & 0x1c0) >> 6) == 5) return 2;
        } else if ((w0 & 0x1000) && ((w0 & 0x1c0) >> 6) != 5 && !(w0 & 0x800)) {
            return 2;
        }
    }
    trap = deref_link(m, (uint16_t)(trig + 6));
    x = (uint16_t)(ls[(uint16_t)(trig + 4)] & 0x3f);
    y = (uint16_t)(ls[(uint16_t)(trig + 6)] & 0x3f);
    if (!trap) return 2;
    r = trap_run(m, who, what, trap, x, y, (uint16_t)(bp - 0xa - 4 - 16 - 4 - 2));
    if (!(rw(ls, trig) & 0x400) && ((rw(ls, (uint16_t)(trig + 6)) >> 6) & 0x3ff)) {
        object_free_with_owned(m, (uint16_t)(tile_ptr(m, x, y) + 2), trap);
        r |= 0x20;
    }
    return r;
}

/* trap_dispatch(trap, x, y), by the trap's type -- its item id's
 * low six bits through a 17-entry table. Ported: 0, damage,
 * 1, teleport, 2, arrow, 5, change terrain, 6, spell
 * (as far as effect_dispatch_2 is ported), 7, create object, 11, delete
 * object, 12, inventory, 13, set variable, 14, check variable, 8, the door
 * trap, 16, the text string, and 4 and 15 (the pit and the combination),
 * which are the tail alone; the others are counted. A case that leaves its
 * flag set runs the tail: a trap or trigger linked from the trap's +6 goes
 * next, its result ORed in. An id past the table (a trigger) is the tail. */
/* direction_between and report_direction_to. */
static int direction_between(int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
    return delta_to_direction((int8_t)(x2 - x1), (int8_t)(y2 - y1));
}

void report_direction_to(uw_motion *m, const char *name, int16_t fx, int16_t fy, int16_t flevel,
                         int16_t tx, int16_t ty, int16_t tlevel, int16_t near) {
    int printed = 0;
    if (!m->scroll) { UW_NOT_CARRIED(m->not_carried); return; }
    uw_scroll_print(m->scroll, name);
    if (near < 0) {
        print_message(m, (uint16_t)(0x23 - near));
        printed = 1;
    } else if (abs(fx - tx) + abs(fy - ty) > near) {
        print_message(m, (uint16_t)(0x24 + direction_between(fx, fy, tx, ty)));
        printed = 1;
    }
    if (flevel == tlevel || flevel == 0) {
        if (!printed && flevel) uw_scroll_print(m->scroll, "very near");
    } else {
        if (printed) uw_scroll_print(m->scroll, " and ");
        print_message(m, (uint16_t)(flevel + 0x33 - tlevel));
    }
    uw_scroll_print(m->scroll, ".\n");
}

/* ==== the do trap's cases ==== */

enum { CRITTER_PROPS_ROW = 0x4a52 };       /* critter_properties: 0x30 bytes a creature type */

void run_function_on_whoami_list(uw_motion *m, uint8_t whoami, int all, int16_t arg,
                                 int (*fn)(uw_motion *, uint16_t, int16_t)) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t p;
    for (p = rw(ds, ACTIVE_LIST); p < rw(ds, ACTIVE_END); p++) {
        uint16_t obj = obj_at(m, ls[p]);
        if (!obj || ls[(uint16_t)(obj + 0x1a)] != whoami) continue;
        if (fn(m, obj, arg)) p--;
        if (!all) return;
    }
}

/* trap_cutscene_3: level_effects_tick(4); cutscene_play(3). */
static void trap_cutscene_3(uw_motion *m) {
    level_effects_tick(m, 4, (uint16_t)(FRAME_BP - 0x80));
    uw_motion_cutscene_request(m, 3);
}

/* object_talk_to(obj), the 0x32 hack's callback: a creature at goal 7 set
 * to goal 1, the cursor's bounds put back and conv_begin_with_object --
 * the conversation, which the shell opens at the top of the next pass.
 * 0: the creature stays on the list. */
static int object_talk_to_cb(uw_motion *m, uint16_t obj, int16_t arg) {
    uint8_t *ls = m->lseg;
    (void)arg;
    if ((rw(ls, (uint16_t)(obj + 0xb)) & 0xf) == 7)
        ww(ls, (uint16_t)(obj + 0xb), (uint16_t)((rw(ls, (uint16_t)(obj + 0xb)) & 0xfff0) | 1));
    cursor_reset_bounds(m);
    m->talk_object = obj;
    return 0;
}

/* trap_bullfrog(owner), from the instructions: the level 4
 * terrain puzzle at tiles 48..55 square, its state the record's +0x88 (the
 * cursor's x), +0x89 (y) and +0x8a (the moves left). Off level 4, message
 * 0xbf. Owner 0 and 1 raise and lower: with under two moves left message
 * 0xc0 and the count set to 1; else a move spent, the 3 x 3 around the
 * cursor (clamped at the edges to two wide) by one and the centre by one
 * more, through trap_change_tile's mode owner * 2 + 1. Owner 2 steps the
 * cursor's y and 3 its x, each wrapping at 8. Owner 4 resets: 0x3f moves,
 * the whole grid to height 4, message 0xc1. */
void trap_bullfrog(uw_motion *m, uint8_t owner) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    int16_t x, y, x0, y0, dx = 2, dy = 2;
    if (rw(ds, CURRENT_LEVEL_WORD) != 4) { print_message(m, 0xbf); return; }
    switch (owner) {
    case 0: case 1:
        x = (int16_t)(ds[(uint16_t)(rec + 0x88)] + 0x30);
        y = (int16_t)(ds[(uint16_t)(rec + 0x89)] + 0x30);
        if (ds[(uint16_t)(rec + 0x8a)] <= 1) {
            print_message(m, 0xc0);
            ds[(uint16_t)(rec + 0x8a)] = 1;
            return;
        }
        ds[(uint16_t)(rec + 0x8a)]--;
        x0 = (int16_t)(x - 1);
        y0 = (int16_t)(y - 1);
        if (ds[(uint16_t)(rec + 0x88)] == 0) x0 = x;
        if (ds[(uint16_t)(rec + 0x88)] == 0 || ds[(uint16_t)(rec + 0x88)] == 7) dx = 1;
        if (ds[(uint16_t)(rec + 0x89)] == 0) y0 = y;
        if (ds[(uint16_t)(rec + 0x89)] == 0 || ds[(uint16_t)(rec + 0x89)] == 7) dy = 1;
        trap_change_tile(m, x0, y0, 0x3f, 0xf, 0xf, 0xf, dx, dy, (int16_t)(owner * 2 + 1));
        trap_change_tile(m, x, y, 0x3f, 0xf, 0xf, 0xf, 0, 0, (int16_t)(owner * 2 + 1));
        break;
    case 2: ds[(uint16_t)(rec + 0x89)] = (uint8_t)((ds[(uint16_t)(rec + 0x89)] + 1) & 7); break;
    case 3: ds[(uint16_t)(rec + 0x88)] = (uint8_t)((ds[(uint16_t)(rec + 0x88)] + 1) & 7); break;
    case 4:
        ds[(uint16_t)(rec + 0x8a)] = 0x3f;
        trap_change_tile(m, 0x30, 0x30, 0x3f, 0xf, 4, 0xf, 7, 7, 0);
        print_message(m, 0xc1);
        break;
    default: break;
    }
}

/* trap_emerald(_, _, x, y): the four-gem puzzle. An emerald
 * (class 2, subclass 2, type 7) looked for in each of the four tiles four
 * away diagonally; all four there, a Vas stone (0xfd) made at (x, y + 1),
 * z 0x40, centred, inserted and settled, and the four gems removed. */
static void trap_emerald(uw_motion *m, uint16_t x, uint16_t y, uint16_t bp) {
    uint8_t *ls = m->lseg;
    uint16_t links[4], gems[4];
    int n = 0, dx, dy, k;
    for (dx = -4; dx <= 4; dx += 8)
        for (dy = -4; dy <= 4; dy += 8) {
            uint16_t link = (uint16_t)(tile_ptr(m, (uint16_t)(x + dx), (uint16_t)(y + dy)) + 2), o;
            o = object_find_matching(m, &link, 0, 2, 2, 7);
            if (o && n < 4) { links[n] = link; gems[n] = o; n++; }
        }
    if (n == 4) {
        uw_objpool pool;
        uint16_t o = create_object(m, 0xfd, 0);
        if (!o) { UW_NOT_CARRIED(m->not_carried); return; }
        ww(ls, (uint16_t)(o + 2), (uint16_t)((rw(ls, (uint16_t)(o + 2)) & 0xff80) | 0x40));
        ww(ls, (uint16_t)(o + 2), (uint16_t)((rw(ls, (uint16_t)(o + 2)) & 0x1fff) | 0x6000));
        ww(ls, (uint16_t)(o + 2), (uint16_t)((rw(ls, (uint16_t)(o + 2)) & 0xe3ff) | 0xc00));
        pool_from_ds(m, &pool);
        uw_object_list_insert(&pool, (uint16_t)(tile_ptr(m, x, (uint16_t)(y + 1)) + 2), o);
        pool_to_ds(m, &pool);
        placed_object_collision(m, o, x, (uint16_t)(y + 1), 1, bp);
        for (k = 0; k < 4; k++) object_remove(m, links[k], gems[k], 1);
    }
}

/* trap_exploding_book: a book (class 4, subclass 1, type 4) in
 * the Avatar's own tile explodes -- the message, quest flag 8, three dice
 * of non-lethal damage, the book taken from the pack and removed, the
 * panel's container button and the recalc. */
void trap_exploding_book(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t pl = rw(ds, TRACKED_OBJECT), rec = rw(ds, PLAYER_RECORD_PTR), pos = rw(ls, (uint16_t)(pl + 0x16));
    uint16_t link = (uint16_t)(tile_ptr(m, (uint16_t)(pos >> 10), (uint16_t)((pos & 0x3f0) >> 4)) + 2), book;
    book = object_find_matching(m, &link, 1, 4, 1, 4);
    if (!book) return;
    if (m->scroll) uw_scroll_print(m->scroll, "The book explodes in your face!\n");
    else UW_NOT_CARRIED(m->not_carried);
    ww(ds, (uint16_t)(rec + 0x65), (uint16_t)(rw(ds, (uint16_t)(rec + 0x65)) | 0x100));
    effect_nonlethal_damage(m, pl, 3);
    inventory_remove_quantity(m, book, 1);
    object_remove(m, 0, book, 1);
    inventory_panel_container_button(m);
    player_state_recalc(m);
}

/* door_lock_remove(door, all): unless
 * the door is a quantity or links nothing, the locks in its chain -- class
 * 4 subclass 0 type 0xf, found with object_find_matching recursing -- taken
 * off the list and freed; the first only unless `all`, whose answer is
 * whether any went. */
int door_lock_remove(uw_motion *m, uint16_t door, int all) {
    uint8_t *ls = m->lseg;
    uint16_t link = (uint16_t)(door + 6), o;
    int any = 0;
    if ((rw(ls, door) & 0x8000) || !(rw(ls, link) >> 6)) return 0;
    while ((o = object_find_matching(m, &link, 1, 4, 0, 0xf)) != 0) {
        uw_objpool pool;
        pool_from_ds(m, &pool);
        uw_object_list_remove(&pool, link, o);
        uw_obj_free(&pool, o);
        pool_to_ds(m, &pool);
        if (!all) return 0;
        any = 1;
    }
    return any;
}

/* disarm_trap(obj, skill): the chain
 * search_for_trap makes -- the object's first class-6 thing, a trigger
 * (subclass 2 on) standing for what it links -- and a trap of kind 0..2
 * tried by check_skill_roll(skill, 8). Above 0: "The ", the
 * trap's name, " on the ", the object's, " was successfully
 * dearmed.\n" -- names by format_object_name, "UNNAMED"
 * without -- and the object's contents cleared (object_chain_clear). 0:
 * "Unable to defuse trap.\n". Below: "Your bumbling attempts have
 * set off the ", the trap's name and ".\n", and the trap set off --
 * a bare trap through trap_run(player, obj, trap, the target tile) and freed
 * with what it owns, one behind a trigger by trigger_chain(player, obj, the
 * trigger, -1). The roll; 0 with nothing to try. The frames are the
 * port's. */
int disarm_trap(uw_motion *m, uint16_t obj, uint8_t skill) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t link, trig, trap;
    char name[0x40], text[0x40];
    int r;
    if (!obj || (rw(ls, obj) & 0x8000) || !((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x3ff)) return 0;
    link = (uint16_t)(obj + 6);
    trig = object_find_matching(m, &link, 0, 6, 0xffff, 0xffff);
    if (!trig) return 0;
    if (((rw(ls, trig) & 0x30) >> 4) < 2) {
        trap = trig;
        trig = 0;
    } else {
        trap = deref_link(m, (uint16_t)(trig + 6));
        if (!trap) { UW_NOT_CARRIED(m->not_carried); return 0; }
    }
    if ((rw(ls, trap) & 0x3f) >= 3) return 0;
    r = check_skill_roll(m, skill, 8);
    if (r > 0) {
        scroll_print(m, ds_text(m, 0x1cf0, text, sizeof text));
        if (!format_object_name(m, name, sizeof name, rw(ls, trap), ls[(uint16_t)(trap + 0x1a)], 0, 0))
            ds_text(m, 0x1ce8, name, sizeof name);
        scroll_print(m, name);
        scroll_print(m, ds_text(m, 0x1cf5, text, sizeof text));
        if (!format_object_name(m, name, sizeof name, rw(ls, obj), ls[(uint16_t)(obj + 0x1a)], 0, 0))
            ds_text(m, 0x1ce8, name, sizeof name);
        scroll_print(m, name);
        scroll_print(m, ds_text(m, 0x1cfe, text, sizeof text));
        object_chain_clear(m, (uint16_t)(obj + 6));
    } else if (r == 0) {
        scroll_print(m, ds_text(m, 0x1d43, text, sizeof text));
    } else {
        scroll_print(m, ds_text(m, 0x1d1a, text, sizeof text));
        if (!format_object_name(m, name, sizeof name, rw(ls, trap), ls[(uint16_t)(trap + 0x1a)], 0, 0))
            ds_text(m, 0x1ce8, name, sizeof name);
        scroll_print(m, name);
        scroll_print(m, ds_text(m, 0x1c96, text, sizeof text));
        if (!trig) {
            trap_run(m, rw(ds, TRACKED_OBJECT), obj, trap, rw(ds, ACTION_TARGET_TILE_X), rw(ds, ACTION_TARGET_TILE_Y),
                     (uint16_t)(FRAME_BP - 0x80));
            object_free_with_owned(m, link, trap);
        } else {
            trigger_chain(m, rw(ds, TRACKED_OBJECT), obj, trig, -1, (uint16_t)(FRAME_BP - 0x80));
        }
    }
    return r;
}

/* trap_create(x, y, kind): two static
 * objects, the first freed when the second will not come. The first a move
 * trigger (item 0x1a0) with word 0's bits 12..15 0xf but bit 11 and 10
 * clear -- a quantity whose value is the second object's index -- at the
 * tile's floor and the sub-tile (3, 3), its quality x and owner y (the
 * tile it fires for); the second a trap of the kind (0x180 + kind), bits 13
 * to 15 set, bits 9..12 1, at the same place, quality 0x3f. Both into the
 * tile's chain; the trigger's index, 0 when there was no room. */
uint16_t trap_create(uw_motion *m, int16_t x, int16_t y, uint8_t kind) {
    uint8_t *ls = m->lseg;
    uw_objpool pool;
    uint16_t a, b, tile, z;
    pool_from_ds(m, &pool);
    a = uw_obj_alloc(&pool, 0);
    if (!a) { pool_to_ds(m, &pool); return 0; }
    b = uw_obj_alloc(&pool, 0);
    if (!b) { uw_obj_free(&pool, a); pool_to_ds(m, &pool); return 0; }
    tile = tile_ptr(m, (uint16_t)x, (uint16_t)y);
    if (!tile) { UW_NOT_CARRIED(m->not_carried); pool_to_ds(m, &pool); return 0; }
    z = (uint16_t)((ls[tile] >> 4) << 3);
    ww(ls, a, (uint16_t)((rw(ls, a) & 0xf3ff & 0xfe00) | 0xf000 | 0x1a0));
    ww(ls, (uint16_t)(a + 2), (uint16_t)((rw(ls, (uint16_t)(a + 2)) & 0x0380 & 0xfc7f) | 0x6000 | 0x0c00 | (z & 0x7f)));
    ww(ls, (uint16_t)(a + 4), (uint16_t)(x & 0x3f));
    ww(ls, (uint16_t)(a + 6), (uint16_t)((uw_obj_index(b) << 6) | (y & 0x3f)));
    uw_object_list_insert(&pool, (uint16_t)(tile + 2), a);
    /* word 0's every field written -- the last, and
     * 0xe1ff, takes bits 9..12 to 1, so nothing of what the slot held
     * before stays; word 3 keeps its low six bits */
    ww(ls, b, (uint16_t)(0xe000 | 0x0200 | 0x180 | (kind & 0xf)));
    ww(ls, (uint16_t)(b + 2), (uint16_t)((rw(ls, (uint16_t)(b + 2)) & 0x0380) | 0x6000 | 0x0c00 | (z & 0x7f)));
    ww(ls, (uint16_t)(b + 4), 0x3f);
    ww(ls, (uint16_t)(b + 6), (uint16_t)(rw(ls, (uint16_t)(b + 6)) & 0x3f));
    uw_object_list_insert(&pool, (uint16_t)(tile + 2), b);
    pool_to_ds(m, &pool);
    return uw_obj_index(a);
}

/* conv_begin_with_door: a throwaway creature, item 0x40 with
 * whoami 0x19 (STRINGS.PAK's "Door"), attitude 3 and goal 10, talked to
 * and freed again -- the talk is the host's, and the free follows it. */
static void conv_begin_with_door(uw_motion *m) {
    uint8_t *ls = m->lseg;
    uint16_t o = create_object(m, 0x40, 1);
    if (!o) { UW_NOT_CARRIED(m->not_carried); return; }
    ls[(uint16_t)(o + 0x1a)] = 0x19;
    ww(ls, (uint16_t)(o + 0xd), (uint16_t)((rw(ls, (uint16_t)(o + 0xd)) & 0x3fff) | 0xc000));
    ww(ls, (uint16_t)(o + 0xb), (uint16_t)((rw(ls, (uint16_t)(o + 0xb)) & 0xfff0) | 10));
    m->talk_object = o;
    m->talk_object_temp = 1;
}

/* trap_platform(delta, obj, x, y): z = the trap's own height
 * plus delta * 8; quality 3 raises the tile's floor to it (under 0x68),
 * anything else moves the linked object to it. */
static void trap_platform(uw_motion *m, int16_t delta, uint16_t obj, uint16_t x, uint16_t y) {
    uint8_t *ls = m->lseg;
    int16_t z = (int16_t)((rw(ls, (uint16_t)(obj + 2)) & 0x7f) + delta * 8);
    if ((ls[(uint16_t)(obj + 4)] & 0x3f) == 3) {
        if (z < 0x68) trap_change_tile(m, (int16_t)x, (int16_t)y, 0xff, 0xff, (int16_t)(z >> 3), 0xff, 0, 0, 0);
    } else {
        uint16_t t = obj_at(m, (uint16_t)((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x1ff));
        if (t) ww(ls, (uint16_t)(t + 2), (uint16_t)((rw(ls, (uint16_t)(t + 2)) & 0xff80) | (z & 0x7f)));
        else UW_NOT_CARRIED(m->not_carried);
    }
}

/* crystal_ball(obj, x, y), the hack trap of quality 2 -- the scrying
 * orbs of levels 2 and 4: the free camera at the tile plus the object's
 * sub-tile offsets, its height and heading from word 1; shade level 6; the
 * automap's switch held off around cameras_fade -- the view faded over to
 * the free camera, held while a button is down, faded back to the Avatar's,
 * which the host draws (view_hook); the recalc. */
static void crystal_ball(uw_motion *m, uint16_t obj, uint16_t x, uint16_t y) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w2 = rw(ls, (uint16_t)(obj + 2));
    uint8_t saved;
    ww(ds, 0x3592, (uint16_t)(x * 0x100 + (w2 >> 13) * 0x20));
    ww(ds, 0x3594, (uint16_t)(y * 0x100 + ((w2 & 0x1c00) >> 10) * 0x20));
    ww(ds, 0x3596, (uint16_t)((w2 & 0x7f) << 3));
    ww(ds, 0x358c, (uint16_t)(((w2 & 0x380) >> 7) << 13));
    ww(ds, 0x358e, 0);
    ww(ds, 0x3590, 0);
    shade_set_level(m, 6);
    saved = ds[0x0546];
    if (saved) ds[0x0546] = 0;
    if (m->view_hook) m->view_hook(m->view_user, m, UW_VIEW_CAMERAS_FADE);
    else UW_NOT_CARRIED(m->not_carried);
    if (saved) ds[0x0546] = 1;
    player_state_recalc(m);
}

/* npc_witness_crime(x, y, obj): the per-witness callback over
 * report_crime's square. The creature reacts when its race is the crime
 * kind's low five bits, the WITNESS's own byte +0x0a bit 7 (the object's,
 * not its critter row) is
 * clear unless the kind has bit 5, the kind is not 0x20 unless that bit is
 * set, the kind is not
 * 0x0d unless the record's +0x69 is under 3, and the criminal is within
 * the notice radius (props +0x1e's high nibble, in tiles) and in sight:
 * then its attitude drops one, floored at 0, and its name, with its
 * article ("a goblin"), is printed with
 * string 0xe1 + the new attitude ("is angered by", "is annoyed by",
 * "notes your action"). */
int npc_witness_crime(uw_motion *m, int16_t x, int16_t y, uint16_t obj) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t row = (uint16_t)(CRITTER_PROPS_ROW + (rw(ls, obj) & 0x3f) * 0x30), rec = rw(ds, PLAYER_RECORD_PTR);
    uint8_t kind = ds[0x1235], flier = (uint8_t)(ls[(uint16_t)(obj + 0xa)] >> 7);
    uint16_t crim = rw(ds, 0x1231), cw2, w2 = rw(ls, (uint16_t)(obj + 2));
    int16_t cx, cy, cz, px, py, pz, dx, dy, radius;
    if (ds[(uint16_t)(row + 9)] != (kind & 0x1f)) return 0;
    if (flier && !(kind & 0x20)) return 0;
    if (kind == 0x20 && !flier) return 0;
    if (kind == 0x0d && ds[(uint16_t)(rec + 0x69)] >= 3) return 0;
    cx = (int16_t)(x * 8 + (w2 >> 13));
    cy = (int16_t)(y * 8 + ((w2 & 0x1c00) >> 10));
    cz = (int16_t)((w2 & 0x7f) + prop(m, (uint16_t)(rw(ls, obj) & 0x1ff), 0));
    cw2 = rw(ls, (uint16_t)(crim + 2));
    px = (int16_t)(rs(ds, 0x269a) * 8 + (cw2 >> 13));
    py = (int16_t)(rs(ds, 0x269c) * 8 + ((cw2 & 0x1c00) >> 10));
    pz = (int16_t)((cw2 & 0x7f) + prop(m, (uint16_t)(rw(ls, crim) & 0x1ff), 0) + 0xc);
    dx = (int16_t)((cx - px) / 8);
    dy = (int16_t)((cy - py) / 8);
    radius = (int16_t)(ds[(uint16_t)(row + 0x1e)] >> 4);
    if (dx * dx + dy * dy > radius * radius) return 0;
    if (!test_between_points(m, cx, cy, cz, px, py, pz)) return 0;
    {
        int16_t att = (int16_t)((rw(ls, (uint16_t)(obj + 0xd)) >> 14) - 1);
        char buf[0x50], part[0x40];
        if (att < 0) att = 0;
        ww(ls, (uint16_t)(obj + 0xd), (uint16_t)((rw(ls, (uint16_t)(obj + 0xd)) & 0x3fff) | (uint16_t)(att << 14)));
        buf[0] = 0;
        format_object_name(m, buf, sizeof buf, rw(ls, obj), ls[(uint16_t)(obj + 0x1a)], 1, 0);
        if (m->strings && uw_strings_by_id(m->strings, (uint16_t)(0x200 | (0xe1 + att)), part, (int)sizeof part) >= 0)
            strncat(buf, part, sizeof buf - strlen(buf) - 1);
        else UW_NOT_CARRIED(m->not_carried);
        if (m->scroll) uw_scroll_print(m->scroll, buf);
        else UW_NOT_CARRIED(m->not_carried);
    }
    return 1;
}

/* report_crime(obj, kind): a kind of 0 asks the object -- an
 * owned item type (props +7 bit 7) reports its owner field's low six bits,
 * anything else nothing; with a kind, the object stashed as the crime's
 * and the witnesses run over the 15 x 15 square seven tiles back from the
 * action position, then the owner cleared when under 0x1c. */
void report_crime(uw_motion *m, uint16_t obj, uint8_t kind, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    ds[0x1235] = 0;
    if (kind == 0) {
        if (!(prop(m, (uint16_t)(rw(ls, obj) & 0x1ff), 7) & 0x80)) return;
        kind = (uint8_t)(ls[(uint16_t)(obj + 6)] & 0x3f);
    }
    ds[0x1235] = kind;
    if (!kind) return;
    ww(ds, 0x1231, obj);
    ww(ds, 0x1233, rw(ds, (uint16_t)(MOBILE_BASE + 2)));
    run_code_on_objects_in_area(m, 0x14, 0, NPC_WITNESS_CRIME_FAR, 0, (int8_t)(ds[0x269a] - 7), (int8_t)(ds[0x269c] - 7),
                                0xf, 0xf, bp);
    if ((ls[(uint16_t)(obj + 6)] & 0x1f) < 0x1c) ls[(uint16_t)(obj + 6)] &= 0xc0;
}

/* obj_remove_from_world(obj): object_chain_remove from its tile's chain
 * -- its contents freed, it unlinked and freed, off the active list --
 * and 1. */
static int obj_remove_from_world_cb(uw_motion *m, uint16_t obj, int16_t arg) {
    uint8_t *ls = m->lseg;
    uint16_t pos = rw(ls, (uint16_t)(obj + 0x16));
    (void)arg;
    object_chain_remove(m, (uint16_t)(tile_ptr(m, (uint16_t)(pos >> 10), (uint16_t)((pos & 0x3f0) >> 4)) + 2), obj);
    return 1;
}

/* trap_tyball_death: cutscene 2, dream mask bit 2, the first
 * creature of each of nine whoamis (taken in
 * reverse) out of the world, and every move trigger (0x1a0) in tile
 * (0x17, 0x38) removed and freed. */
void trap_tyball_death(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), link, o;
    int i;
    uw_objpool pool;
    uw_motion_cutscene_request(m, 2);
    ww(ds, (uint16_t)(rec + 0x6e), (uint16_t)(rw(ds, (uint16_t)(rec + 0x6e)) | 4));
    for (i = 9; i > 0; i--) run_function_on_whoami_list(m, ds[(uint16_t)(0x12c0 + i)], 0, 0, obj_remove_from_world_cb);
    link = (uint16_t)(tile_ptr(m, 0x17, 0x38) + 2);
    for (o = deref_link(m, link); o; ) {
        uint16_t next = deref_link(m, (uint16_t)(o + 4));
        if ((rw(ls, o) & 0x1ff) == 0x1a0) {
            pool_from_ds(m, &pool);
            uw_object_list_remove(&pool, link, o);
            uw_obj_free(&pool, o);
            pool_to_ds(m, &pool);
        }
        o = next;
    }
}

/* the orb's callback: the creature's hit points over
 * the argument plus one, and bit 9 of its +0xd. */
static int tyball_orb_cb(uw_motion *m, uint16_t obj, int16_t arg) {
    uint8_t *ls = m->lseg;
    ls[(uint16_t)(obj + 8)] = (uint8_t)(ls[(uint16_t)(obj + 8)] / (arg ? arg : 1) + 1);
    ww(ls, (uint16_t)(obj + 0xd), (uint16_t)(rw(ls, (uint16_t)(obj + 0xd)) | 0x200));
    return 0;
}

/* use_tyballs_orb(obj, applied): the orb (0x117) destroyed --
 * message 0x85, the pending object cleared when applied, a class 7 spark
 * of kind 4 at the target tile, the orb out of that tile, the target
 * forgotten, the record's +0x60 bit 5 and the mana and its maximum back
 * from +0xb0, and Tyball halved -- else message 0x84 when applied; and
 * what the cursor held let go of. */
void use_tyballs_orb(uw_motion *m, uint16_t obj, int applied) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    if ((rw(ls, obj) & 0x1ff) == 0x117) {
        int16_t x = rs(ds, 0x269a), y = rs(ds, 0x269c);
        print_message(m, 0x85);
        if (applied) object_clear(m, rw(ds, 0x2688), applied, 1);
        spawn_class7(m, obj, 4, 5, 0, 0, (uint8_t)x, (uint8_t)y);
        object_remove(m, (uint16_t)(tile_ptr(m, (uint16_t)x, (uint16_t)y) + 2), obj, 1);
        ww(ds, 0x269a, 0xffff);
        ds[(uint16_t)(rec + 0x60)] |= 0x20;
        ds[(uint16_t)(rec + 0x38)] = ds[(uint16_t)(rec + 0xb0)];
        ds[(uint16_t)(rec + 0x37)] = ds[(uint16_t)(rec + 0xb0)];
        run_function_on_whoami_list(m, 0xe7, 0, 2, tyball_orb_cb);
    } else if (applied) print_message(m, 0x84);
    if (rw(ds, CURSOR_OBJECT) || rw(ds, (uint16_t)(CURSOR_OBJECT + 2))) {
        cursor_shape_pop(m, 3);
        ww(ds, CURSOR_OBJECT, 0);
        ww(ds, (uint16_t)(CURSOR_OBJECT + 2), 0);
        ww(ds, ACTION_STATE_WORD, 0);
    }
}

enum { TRAP_PENDING_CODE = 0x1c8f };
/* trap_do(trap, x, y): trap type 3's own
 * dispatch, on the trap's quality. 0x3f is the ending -- trap_pending_code
 * set to the owner plus one and event 0x400 posted, which runs
 * game_ending_sequence. The rest: 2
 * crystal_ball, 3 and 4 trap_platform with the context object's
 * word 0 bits 9..12, 5 report_crime_by_player with the trap's owner, 0x18
 * trap_bullfrog with the owner, 0x28 trap_emerald, 0x29
 * trap_exploding_book, 0x2a conv_begin_with_door, 0x32
 * run_function_on_whoami_list over whoami 216 -- object_talk_to for the
 * first guard -- 0x39 trap_cutscene_3, and 0x3c..0x3e player_apply_impact
 * when the context object is the player. Any other quality
 * does nothing. 2. */
static uint16_t trap_do(uw_motion *m, uint16_t trap, uint16_t x, uint16_t y, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint8_t q = (uint8_t)(ls[(uint16_t)(trap + 4)] & 0x3f);
    switch (q) {
    case 0x3f:
        ds[TRAP_PENDING_CODE] = (uint8_t)((ls[(uint16_t)(trap + 6)] & 0x3f) + 1);
        ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 0x400));     /* post_event(0x400) */
        break;
    case 0x3c: case 0x3d: case 0x3e:
        if (rw(ds, TRAP_WHO) == rw(ds, TRACKED_OBJECT) && rw(ds, (uint16_t)(TRAP_WHO + 2)) == rw(ds, (uint16_t)(TRACKED_OBJECT + 2)))
            player_apply_impact(m, (uint16_t)(q - 0x3b), (int16_t)(ls[(uint16_t)(trap + 6)] & 0x3f));
        break;
    case 2:
        crystal_ball(m, trap, x, y);
        break;
    case 3: case 4:
        trap_platform(m, (int16_t)((rw(ls, rw(ds, TRAP_WHAT)) & 0x1e00) >> 9), trap, x, y);
        break;
    case 5:
        report_crime(m, rw(ds, TRACKED_OBJECT), (uint8_t)(ls[(uint16_t)(trap + 6)] & 0x3f), (uint16_t)(bp - 8));
        break;
    case 0x18:
        trap_bullfrog(m, (uint8_t)(ls[(uint16_t)(trap + 6)] & 0x3f));
        break;
    case 0x28:
        trap_emerald(m, x, y, (uint16_t)(bp - 8));
        break;
    case 0x29:
        trap_exploding_book(m);
        break;
    case 0x2a:
        conv_begin_with_door(m);
        break;
    case 0x32:
        run_function_on_whoami_list(m, 216, 0, 0, object_talk_to_cb);
        break;
    case 0x39:
        trap_cutscene_3(m);
        break;
    default:
        break;
    }
    return 2;
}

uint16_t trap_dispatch(uw_motion *m, uint16_t trap, uint16_t x, uint16_t y, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t result = 2, type = (uint16_t)(rw(ls, trap) & 0x3f);
    int flag = 1;
    if (type == 0) {
        /* damage: the quality, negated by an owner (poison), to the
         * instigator -- after a draw, rand() * 10 / 0x8000 under 7, whose
         * answer trap_damage is passed and never reads. */
        int16_t amount = (int16_t)((ls[(uint16_t)(trap + 4)] & 0x3f) * ((ls[(uint16_t)(trap + 6)] & 0x3f) ? -1 : 1));
        rt_rand(m);
        result = trap_damage(m, obj_index_of(m, rw(ds, TRAP_WHO)), amount);
    } else if (type == 1) {
        /* teleport: to (quality, owner) on the level in the trap's z */
        result = trap_teleport(m, rw(ds, TRAP_WHO), (int16_t)(ls[(uint16_t)(trap + 4)] & 0x3f),
                               (int16_t)(ls[(uint16_t)(trap + 6)] & 0x3f), (int16_t)(rw(ls, (uint16_t)(trap + 2)) & 0x7f),
                               (uint16_t)(bp - 0x32 - 4 - 10 - 4 - 2));
    } else if (type == 2) {
        /* arrow: the missile the trap names, at the tile */
        projectile_fire_from_trap(m, trap, x, y, (uint16_t)(bp - 0x32 - 4 - 8 - 4 - 2));
    } else if (type == 3) {
        result = trap_do(m, trap, x, y, (uint16_t)(bp - 0x32 - 4 - 6 - 4 - 2));
    } else if (type == 5) {
        /* change terrain: at (x, y), the rectangle the trap's fine position
         * spans; wall from the owner, floor from the quality >> 1, height
         * from the z >> 3, type from heading * 2 + the quality's low bit
         * (15 meaning 10, leave it) */
        uint16_t w2 = rw(ls, (uint16_t)(trap + 2)), q = (uint16_t)(ls[(uint16_t)(trap + 4)] & 0x3f);
        int16_t ttype = (int16_t)((((w2 & 0x380) >> 7) << 1) + (q & 1));
        if (ttype == 0xf) ttype = 10;
        result = trap_change_tile(m, (int16_t)x, (int16_t)y, (int16_t)(ls[(uint16_t)(trap + 6)] & 0x3f), (int16_t)(q >> 1),
                                  (int16_t)((w2 & 0x7f) >> 3), ttype, (int16_t)((w2 & 0xe000) >> 13),
                                  (int16_t)((w2 & 0x1c00) >> 10), 0);
    } else if (type == 6) {
        /* spell: the quality a class, the owner its argument */
        result = trap_fire(m, (uint8_t)x, (uint8_t)y, trap, rw(ds, TRAP_WHO), (int16_t)(ls[(uint16_t)(trap + 4)] & 0x3f),
                           (uint8_t)(ls[(uint16_t)(trap + 6)] & 0x3f), (uint16_t)(bp - 0x32 - 4 - 16 - 4 - 2));
    } else if (type == 7) {
        /* create object: the template the trap links copied and scattered
         * about the tile, with the first thing it holds -- unless
         * rand() * 63 / 0x8000 falls under the trap's quality, or it is a
         * creature and another stands within four tiles. */
        uw_objpool pool;
        uint16_t q = (uint16_t)(ls[(uint16_t)(trap + 4)] & 0x3f), tmpl, n;
        int32_t roll = (int32_t)rt_rand(m) * 0x3f / 0x8000;
        if ((uint16_t)roll < q) return 2;
        flag = 0;
        if (!(rw(ls, trap) & 0x8000) && (tmpl = deref_link(m, (uint16_t)(trap + 6))) != 0
            && !(((rw(ls, tmpl) & 0x1c0) >> 6) == 1
                 && area_contains_match(m, tmpl, (uint16_t)(bp - 0x32 - 4 - 4 - 4 - 2)))) {
            pool_from_ds(m, &pool);
            n = uw_obj_alloc(&pool, tmpl < rw(ds, STATIC_BASE));
            pool_to_ds(m, &pool);
            if (n) {
                uint16_t w2;
                uint8_t placed;
                memmove(ls + n, ls + tmpl, tmpl < rw(ds, STATIC_BASE) ? 0x1b : 8);    /* struct_copy_far */
                w2 = rw(ls, (uint16_t)(n + 2));
                ds[PLACE_SCATTER] = 1;
                placed = (uint8_t)object_move_to_coords(m, (int16_t)(x * 8 + (w2 >> 13)),
                                                        (int16_t)(y * 8 + ((w2 & 0x1c00) >> 10)),
                                                        (int16_t)(w2 & 0x7f), n, 4, 0,
                                                        (uint16_t)(bp - 0x32 - 4 - 14 - 4 - 2));
                ds[PLACE_SCATTER] = 0;
                if (placed && !(rw(ls, n) & 0x8000) && ((rw(ls, (uint16_t)(n + 6)) >> 6) & 0x3ff)) {
                    uint16_t c;
                    pool_from_ds(m, &pool);
                    c = uw_obj_alloc(&pool, 0);
                    pool_to_ds(m, &pool);
                    if (c) {
                        memmove(ls + c, ls + obj_at(m, (uint16_t)((rw(ls, (uint16_t)(n + 6)) >> 6) & 0x3ff)), 8);
                        ww(ls, (uint16_t)(n + 6), (uint16_t)((rw(ls, (uint16_t)(n + 6)) & 0x3f)
                                                             | ((obj_index_of(m, c) & 0x3ff) << 6)));
                        while ((rw(ls, (uint16_t)(c + 4)) >> 6) & 0x3ff)
                            ww(ls, (uint16_t)(c + 4), (uint16_t)(rw(ls, (uint16_t)(c + 4)) & 0x3f));
                        if (!(rw(ls, c) & 0x8000) && ((rw(ls, (uint16_t)(c + 6)) >> 6) & 0x3ff))
                            ww(ls, (uint16_t)(c + 6), (uint16_t)(rw(ls, (uint16_t)(c + 6)) & 0x3f));
                    }
                }
                if (placed && ((rw(ls, n) & 0x1c0) >> 6) == 7)
                    level_effect_add(m, obj_index_of(m, n), -1, 0, (uint8_t)x, (uint8_t)y);   /* a made effect's record */
            }
            result = 2;
        }
    } else if (type == 8) {
        /* the door trap: the door on the tile (a closed one, class 5
         * subclass 0) loses its spike (0x10f) and gains a copy of what the
         * trap holds, then opens (quality 1), closes (2) or toggles (3); a
         * door already moving (0x1cf) is sent the way the quality asks. */
        uint16_t link = (uint16_t)(tile_ptr(m, x, y) + 2), q = (uint16_t)(ls[(uint16_t)(trap + 4)] & 0x3f), door;
        flag = 0;
        door = object_find_matching(m, &link, 0, 5, 0, 0xffff);
        ww(ds, ACTION_TARGET_X, x);
        ww(ds, ACTION_TARGET_Y, y);
        if (door) {
            uw_objpool pool;
            uint16_t spike;
            link = (uint16_t)(door + 6);
            spike = object_find_matching(m, &link, 0, 4, 0, 0xf);
            if (spike) {
                pool_from_ds(m, &pool);
                uw_object_list_remove(&pool, link, spike);
                uw_obj_free(&pool, spike);
                pool_to_ds(m, &pool);
            }
            if (!(rw(ls, trap) & 0x8000) && ((rw(ls, (uint16_t)(trap + 6)) >> 6) & 0x3ff)) {
                uint16_t src = deref_link(m, (uint16_t)(trap + 6)), c;
                pool_from_ds(m, &pool);
                c = uw_obj_alloc(&pool, 0);
                pool_to_ds(m, &pool);
                if (c) {
                    memmove(ls + c, ls + src, 8);    /* struct_copy_far */
                    pool_from_ds(m, &pool);
                    uw_object_list_insert(&pool, link, c);
                    pool_to_ds(m, &pool);
                }
            }
            if (q == 1) door_open(m, rw(ds, TRAP_WHO), door);                     /* door_open(who, door) */
            else if (q == 2) door_close(m, door);
            else if (q == 3) uw_motion_door_toggle(m, rw(ds, TRAP_WHO), door);    /* door_toggle(who, door) */
        } else if ((door = object_find_matching(m, &link, 0, 7, 0xffff, 0xf)) != 0) {
            if ((ls[(uint16_t)(door + 6)] & 0xf) < 8) {
                if (q == 2 || q == 3) door_close(m, door);
            } else if (q == 1 || q == 3) {
                door_open(m, rw(ds, TRAP_WHO), door);
            }
        }
    } else if (type == 11) {
        /* delete object: the thing the trap links, out of the tile (quality,
         * owner) with its contents, and event 2 */
        object_chain_remove(m, (uint16_t)(tile_ptr(m, ls[(uint16_t)(trap + 4)] & 0x3f, ls[(uint16_t)(trap + 6)] & 0x3f) + 2),
                            deref_link(m, (uint16_t)(trap + 6)));
        ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));      /* post_event(2) */
        flag = 0;
    } else if (type == 12) {
        /* inventory: the item (quality << 5 | owner) carried anywhere, as
         * many as the trap's z when it is a counted stack, runs the tail;
         * anything short of that returns 2 */
        uint16_t item = (uint16_t)(((ls[(uint16_t)(trap + 4)] & 0x3f) << 5) | (ls[(uint16_t)(trap + 6)] & 0x3f));
        uint16_t need = (uint16_t)(rw(ls, (uint16_t)(trap + 2)) & 0x7f), found;
        int16_t slot;
        found = inventory_find_by_kind(m, (int16_t)(item >> 6), (int16_t)((item & 0x30) >> 4), (int16_t)(item & 0xf), 4,
                                       &slot);
        if (!found) return 2;
        if (need && (rw(ls, found) & 0x8000)) {
            uint16_t q = (uint16_t)((rw(ls, (uint16_t)(found + 6)) >> 6) & 0x3ff);
            if (!(q & 0x200) && need > q) return 2;
        }
    } else if (type == 13) {
        /* set variable: the value (quality << 5 | owner) << 3 | the fine y,
         * the variable the trap's z, the operation its heading -- variable
         * 0 is a bit of the player record's 32-bit mask at +0x65 (1 clears
         * it, 5 flips it, anything else sets it; rt_lshl's shift), any
         * other the six-bit game variable at +0x70 + z: add, subtract, set,
         * and, or, xor, shift left (x86's shift count, five bits). */
        uint16_t w2 = rw(ls, (uint16_t)(trap + 2)), var = (uint16_t)(w2 & 0x7f), op = (uint16_t)((w2 & 0x380) >> 7);
        uint16_t v = (uint16_t)(((((ls[(uint16_t)(trap + 4)] & 0x3f) << 5) | (ls[(uint16_t)(trap + 6)] & 0x3f)) << 3)
                                | ((w2 & 0x1c00) >> 10));
        uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
        if (var == 0) {
            uint8_t cl = (uint8_t)v;
            uint32_t bit = cl < 16 ? (uint32_t)1 << cl : (uint32_t)(uint16_t)(1u << ((cl - 16) & 0x1f)) << 16;
            uint32_t mask = rd32(ds, (uint16_t)(rec + 0x65));
            if (op == 1) mask &= ~bit;
            else if (op == 5) mask ^= bit;
            else mask |= bit;
            wr32(ds, (uint16_t)(rec + 0x65), mask);
        } else {
            uint16_t at = (uint16_t)(rec + 0x70 + var);
            uint8_t g = ds[at];
            switch (op) {
            case 0: g = (uint8_t)(g + v); break;
            case 1: g = (uint8_t)(g - v); break;
            case 2: g = (uint8_t)v; break;
            case 3: g = (uint8_t)(g & v); break;
            case 4: g = (uint8_t)(g | v); break;
            case 5: g = (uint8_t)(g ^ v); break;
            case 6: g = (uint8_t)((unsigned)g << (v & 0x1f)); break;
            default: break;
            }
            ds[at] = (uint8_t)(g & 0x3f);
        }
    } else if (type == 14) {
        /* check variable: the game variables z .. z + heading, summed when
         * the fine x is set and otherwise read as octal digits (their low
         * three bits), against (quality << 5 | owner) << 3 | the fine y.
         * Equal runs the tail; unequal runs, through trigger_chain(-1), the
         * thing after the one the trap links -- or returns 2. */
        uint16_t w2 = rw(ls, (uint16_t)(trap + 2)), rec = rw(ds, PLAYER_RECORD_PTR), got = 0, want, next;
        int16_t var = (int16_t)(w2 & 0x7f), last = (int16_t)(var + ((w2 & 0x380) >> 7));
        want = (uint16_t)(((((ls[(uint16_t)(trap + 4)] & 0x3f) << 5) | (ls[(uint16_t)(trap + 6)] & 0x3f)) << 3)
                          | ((w2 & 0x1c00) >> 10));
        for (; var <= last; var++) {
            uint8_t g = ds[(uint16_t)(rec + 0x70 + var)];
            if (w2 >> 13) got = (uint16_t)(got + g);
            else got = (uint16_t)((got << 3) | (g & 7));
        }
        if (got != want) {
            if (!((rw(ls, (uint16_t)(trap + 6)) >> 6) & 0x3ff)) return result;
            next = deref_link(m, (uint16_t)(trap + 6));
            if (!((rw(ls, (uint16_t)(next + 4)) >> 6) & 0x3ff)) return 2;
            return trigger_chain(m, rw(ds, TRAP_WHO), rw(ds, TRAP_WHAT), deref_link(m, (uint16_t)(next + 4)), -1,
                                 (uint16_t)(bp - 0x32 - 4 - 14 - 4 - 2));
        }
    } else if (type == 9 || type == 10) {
        /* ward and tell: with something
         * having set it off, and the quality 0x3f or the thing's type
         * nibble, an amount of rand * the Avatar's +0x2a over 0x8000 plus 3
         * is worked out, "Your Rune of Warding has been set off " reported
         * with the direction from the Avatar's tile to the thing's (no
         * levels, near 0), and dealt to the thing by trap_damage. The
         * tail's flag is cleared on every path out, set off or not
         * -- a ward never runs what it links */
        uint16_t who = rw(ds, TRAP_WHO);
        flag = 0;
        if (who) {
            uint8_t q = (uint8_t)(ls[(uint16_t)(trap + 4)] & 0x3f);
            if (q == 0x3f || q == (rw(ls, who) & 0xf)) {
                uint16_t rec = rw(ds, PLAYER_RECORD_PTR), pl = rw(ds, TRACKED_OBJECT);
                uint16_t wp = rw(ls, (uint16_t)(who + 0x16)), pp = rw(ls, (uint16_t)(pl + 0x16));
                int16_t amount = (int16_t)(((int32_t)rt_rand(m) * ds[(uint16_t)(rec + 0x2a)]) / 0x8000 + 3);
                char name[0x60];
                if (!m->strings || uw_strings_by_id(m->strings, 0x2f5, name, (int)sizeof name) < 0) { name[0] = 0; UW_NOT_CARRIED(m->not_carried); }
                report_direction_to(m, name, (int16_t)(pp >> 10), (int16_t)((pp & 0x3f0) >> 4), 0,
                                    (int16_t)(wp >> 10), (int16_t)((wp & 0x3f0) >> 4), 0, 0);
                result = trap_damage(m, obj_index_of(m, who), amount);
            }
        }
    } else if (type == 16) {
        /* the text string: block 9's string (quality << 5 | owner) printed
         * -- after debug_printf("Look, it's a text trap\n"), which is empty */
        print_string(m, (uint16_t)(0x1200 | ((ls[(uint16_t)(trap + 4)] & 0x3f) << 5) | (ls[(uint16_t)(trap + 6)] & 0x3f)));
    }                                       /* 4, the pit, and 15, the combination: the tail alone */
    if (flag && ((rw(ls, (uint16_t)(trap + 6)) >> 6) & 0x3ff)) {
        uint16_t next = deref_link(m, (uint16_t)(trap + 6));
        if (((rw(ls, next) & 0x1c0) >> 6) == 6) {
            if (((rw(ls, next) & 0x30) >> 4) < 2)
                result |= trap_dispatch(m, next, x, y, (uint16_t)(bp - 0x32 - 4 - 8 - 4 - 2));
            else
                result |= trigger_chain(m, rw(ds, TRAP_WHO), rw(ds, TRAP_WHAT), next, -1,
                                        (uint16_t)(bp - 0x32 - 4 - 14 - 4 - 2));
        }
    }
    return result;
}
