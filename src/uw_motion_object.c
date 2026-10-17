/* SPDX-License-Identifier: MIT */
/* objects in the level: the pools as the data segment holds them,
 * removal and culling, a thing put down, a
 * dead creature's remains and loot, projectile_motion_apply, an
 * object's weight, and the object primitives exported to the
 * conversation builtins (src/uw_convbi.c).
 *
 * One of src/uw_motion*.c: see uw_motion.h and uw_motion_int.h. */
#include "uw_motion_int.h"
#include "uw_scroll.h"

/* The pools as the data segment holds their pointers. */
static int pool_reclaim(void *user, uw_objpool *p, int margin, int max);

void pool_from_ds(uw_motion *m, uw_objpool *pool) {
    memset(pool, 0, sizeof *pool);
    pool->seg = m->lseg;
    pool->reclaim = pool_reclaim;
    pool->reclaim_user = m;
    pool->mobile_sp = rw(m->ds, 0x2756);
    pool->static_sp = rw(m->ds, 0x274a);
    pool->active_end = rw(m->ds, 0x2732);
}
void pool_to_ds(uw_motion *m, uw_objpool *pool) {
    ww(m->ds, 0x2756, pool->mobile_sp);
    ww(m->ds, 0x274a, pool->static_sp);
    ww(m->ds, 0x2732, pool->active_end);
    m->not_carried += pool->not_carried;
    pool->not_carried = 0;
}

enum { OBJECT_CULL_LIMIT = 0x2740 };

/* object_worth_keeping: immune (word 0 bit 13), or obj_properties
 * +9 bits 2..5 plus half a quantity past one above the cull limit. */
static int object_worth_keeping(uw_motion *m, uint16_t obj) {
    uint8_t *ls = m->lseg;
    uint16_t w0 = rw(ls, obj), q = (uint16_t)((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x3ff);
    int16_t si = 0;
    if (w0 & 0x2000) return 1;
    if ((w0 & 0x8000) && !(q & 0x200)) si = (int16_t)(q - 1);
    return (int16_t)(((prop(m, obj_id(m, obj), 9) >> 2) & 0xf) + si / 2) > rs(m->ds, OBJECT_CULL_LIMIT);
}

/* run_code_on_object_chain(first, callback), from the
 * instructions, with its one callback: along the chain from `first`, each
 * thing tested and then, a non-quantity with contents, its contents walked
 * the same way; 1 at the first thing worth keeping, 0 at the chain's end. */
static int chain_worth_keeping(uw_motion *m, uint16_t o) {
    uint8_t *ls = m->lseg;
    int guard = 0;
    while (o && guard++ < 0x400) {
        if (object_worth_keeping(m, o)) return 1;
        if (!(rw(ls, o) & 0x8000) && ((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff)
            && chain_worth_keeping(m, deref_link(m, (uint16_t)(o + 6))))
            return 1;
        o = deref_link(m, (uint16_t)(o + 4));
    }
    return 0;
}

/* object_cull_test: 1 when the thing may be discarded -- a limit
 * of the importance plus rand() * 3 / 0x8000 when there is one, nothing worth
 * keeping in it or in its contents (run_code_on_object_chain with
 * object_worth_keeping), and a d10 under the limit. */
int object_cull_test(uw_motion *m, int16_t importance, uint16_t obj) {
    uint8_t *ls = m->lseg;
    if (!obj) return 0;
    if (importance) importance = (int16_t)(importance + (int16_t)(((int32_t)rt_rand(m) * 3) / 0x8000));
    ww(m->ds, OBJECT_CULL_LIMIT, (uint16_t)importance);
    if (object_worth_keeping(m, obj)) return 0;
    if (!(rw(ls, obj) & 0x8000) && ((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x3ff)
        && chain_worth_keeping(m, deref_link(m, (uint16_t)(obj + 6))))
        return 0;
    return (int16_t)(((int32_t)rt_rand(m) * 10) / 0x8000) < rs(m->ds, OBJECT_CULL_LIMIT);
}

/* obj_reclaim_distant(margin, max): what obj_alloc does with an
 * empty free stack. Every tile of the level whose distance from the
 * player's tile -- the two axes added, not squared -- is more than
 * 10 - margin has its chain walked, and every object object_cull_test will
 * discard goes, contents and all, until `max` have gone. So the game
 * throws distant things away under memory pressure, which is a gameplay
 * behaviour and not only an allocator's: a sleep runs it with (1, 0x14),
 * and an exhausted pool with (3, 5) for a mobile or (3, 10) for a static.
 *
 * The chain is walked by index and the next index is taken BEFORE the test,
 * as the original takes the object's +4 before it culls, so a removal does
 * not lose the rest of the tile. */
void obj_reclaim_distant(uw_motion *m, int16_t margin, int16_t max) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t pl = rw(ds, TRACKED_OBJECT), w16 = rw(ls, (uint16_t)(pl + 0x16));
    int px = w16 >> 10, py = (w16 & 0x3f0) >> 4, x, y, freed = 0;
    for (y = 0; y < 0x40; y++) {
        int dy = py - y;
        if (dy < 0) dy = -dy;
        for (x = 0; x < 0x40; x++) {
            uint16_t link, idx;
            int dx = px - x;
            if (dx < 0) dx = -dx;
            if (dx + dy <= 10 - margin) continue;
            link = (uint16_t)(tile_ptr(m, (uint16_t)x, (uint16_t)y) + 2);
            idx = (uint16_t)((rw(ls, link) >> 6) & 0x3ff);
            while (idx) {
                uint16_t o = obj_at(m, idx);
                uint16_t next = (uint16_t)((rw(ls, (uint16_t)(o + 4)) >> 6) & 0x3ff);
                if (object_cull_test(m, margin, o)) {
                    object_chain_remove(m, link, o);
                    if (++freed >= max) return;
                }
                idx = next;
            }
        }
    }
}

/* The reclaim as the object pool reaches it: the pool's live depths put
 * back where obj_reclaim_distant's callees read them, the sweep, and the
 * depths it leaves read into the pool again. pool_from_ds installs it, so
 * every allocation made through the motion port can free and retry the way
 * obj_alloc does; a pool built without one (the two that are built by hand
 * for a single list operation) keeps counting instead. */
static int pool_reclaim(void *user, uw_objpool *p, int margin, int max) {
    uw_motion *m = (uw_motion *)user;
    ww(m->ds, 0x2756, p->mobile_sp);
    ww(m->ds, 0x274a, p->static_sp);
    ww(m->ds, 0x2732, p->active_end);
    m->not_carried += p->not_carried;
    p->not_carried = 0;
    obj_reclaim_distant(m, (int16_t)margin, (int16_t)max);
    p->mobile_sp = rw(m->ds, 0x2756);
    p->static_sp = rw(m->ds, 0x274a);
    p->active_end = rw(m->ds, 0x2732);
    return 1;
}

/* level_effect_remove(index), from the instructions: the first
 * record of level_effect_list (six bytes each) whose word 0 bits
 * 6..15 are the index, below the signed byte level_effect_count; found, the
 * count goes down one and, unless that leaves none or the record was the
 * last, the last is copied over it (struct_copy_far). No expiry runs. */
static void level_effect_remove(uw_motion *m, uint16_t index) {
    uint8_t *ds = m->ds;
    int16_t i = 0, n;
    while (i < (int8_t)ds[0x3656] && ((rw(ds, (uint16_t)(0x369c + i * 6)) >> 6) & 0x3ff) != index) i++;
    if ((int8_t)ds[0x3656] <= i) return;
    ds[0x3656] = (uint8_t)(ds[0x3656] - 1);
    n = (int8_t)ds[0x3656];
    if (n <= 0 || n == i) return;
    memcpy(ds + 0x369c + i * 6, ds + 0x369c + n * 6, 6);
}

/* Two neighbours the port leaves out, because nothing in the image reaches
 * them -- no near or far call, no stored pointer: level_effect_expire_for_object,
 * the effect of an object index let run to its last frame; and
 * spawn_effect_object, a class-7 object made off a source with
 * nine arguments. (object_talk_to, which sat with them, is the 0x32 hack's
 * callback, a PUSH-immediate far pointer: src/uw_motion_trap.c.)
 *
 * level_effect_reassign(new, old), from the instructions: the
 * first effect record whose object is `old` pointed at `new` -- word 0's
 * bits 6..15 -- its countdown and tile left alone. */
static void level_effect_reassign(uw_motion *m, uint16_t new_obj, uint16_t old_obj) {
    uint8_t *ds = m->ds;
    uint16_t n = obj_index_of(m, new_obj), o = obj_index_of(m, old_obj);
    int16_t i;
    for (i = 0; i < (int8_t)ds[0x3656]; i++) {
        uint16_t at = (uint16_t)(0x369c + i * 6);
        if (((rw(ds, at) >> 6) & 0x3ff) == o) {
            ww(ds, at, (uint16_t)((rw(ds, at) & 0x3f) | ((n & 0x3ff) << 6)));
            return;
        }
    }
}

/* object_remove: unless forced, only junk; a class-7 effect off
 * the effect list (level_effect_remove); out of the tile's chain and freed, a container's contents
 * with it -- with no tile, object_chain_clear over a word on the stack that
 * holds the object's index. 0 when removed. */
uint16_t object_remove(uw_motion *m, uint16_t link, uint16_t obj, int force) {
    uint8_t *ls = m->lseg;
    uw_objpool pool;
    if (!force && !object_cull_test(m, 10, obj)) return obj;
    if (((rw(ls, obj) & 0x1c0) >> 6) == 7)
        level_effect_remove(m, obj_index_of(m, obj));
    if (!link) {
        object_chain_clear_local(m, obj);
        return 0;
    }
    /* object_chain_remove */
    if (!(rw(ls, obj) & 0x8000) && ((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x3ff))
        object_chain_clear(m, (uint16_t)(obj + 6));
    pool_from_ds(m, &pool);
    uw_object_list_remove(&pool, link, obj);
    uw_obj_free(&pool, obj);
    pool_to_ds(m, &pool);
    return 0;
}

/* object_hits_floor, from the instructions: what a moving thing
 * becomes when it comes to rest at (motion_tile_x, _y). Its break chance is
 * obj_properties +7 bits 1..4; a break chance of 1..8 breaks on rand() & 7
 * under it when object_cull_test(10) agrees, and level 9 breaks everything.
 * Whole, a static copy takes its place -- quality from its hit points, a lit
 * light source unlit, its heading kept but for classes 5 and 6 -- and the
 * mobile is removed. The copy, or 0. */
uint16_t object_hits_floor(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    int keep = 1;
    uint16_t di = (uint16_t)((prop(m, obj_id(m, obj), 7) >> 1) & 0xf);
    uint16_t state = (uint16_t)((ls[(uint16_t)(obj + 0xa)] & 0x70) >> 4);
    int16_t x = rs(ds, MOTION_TILE_X), y = rs(ds, MOTION_TILE_Y);
    uint16_t link, nobj = 0;
    if (state == 1) {
        di = 8;
        spawn_class7(m, obj, 6, 3, 0, 0, (uint8_t)x, (uint8_t)y);     /* the splash */
    } else if (state == 2 && di == 10 && rw(ds, CURRENT_LEVEL_WORD) == 8
               && abs(x - 0x20) + abs(y - 0x20) < 6 && ls[(uint16_t)(rw(ds, TRACKED_OBJECT) + 8)]) {
        /* the endgame ritual: a talisman (break class 10) thrown into the
         * middle of level 8 by a living Avatar. Before Garamon's bones are
         * buried (record +0x62 bit 2) it only marks a dream (+0x6e bit 3);
         * after, the ritual counter (+0x6d) goes down -- at 0 "A rending
         * sound fills the air." (0x116) and the ending (event 0x400); else
         * the talisman becomes a column of flame, item 0x1c2, rising (+4..11)
         * with a copy set down about the spot (spawn_animo_copies, the
         * height's rand first, then y's, then x's) for each step from 8
         * down to the counter -- and it is kept by no one either way. */
        uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
        if (!(ds[(uint16_t)(rec + 0x62)] & 4)) {
            ww(ds, (uint16_t)(rec + 0x6e), (uint16_t)(rw(ds, (uint16_t)(rec + 0x6e)) | 8));
        } else {
            di = 8;
            keep = 0;
            ds[(uint16_t)(rec + 0x6d)] = (uint8_t)(ds[(uint16_t)(rec + 0x6d)] - 1);
            if (!ds[(uint16_t)(rec + 0x6d)]) {
                print_message(m, 0x116);
                ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 0x400));   /* post_event(0x400) */
            } else {
                int16_t step;
                ww(ls, obj, (uint16_t)((rw(ls, obj) & 0xfe00) | 0x1c2));
                for (step = 8; ds[(uint16_t)(rec + 0x6d)] <= step; step--) {
                    int16_t dy, dx;
                    uint16_t w2 = rw(ls, (uint16_t)(obj + 2));
                    ww(ls, (uint16_t)(obj + 2), (uint16_t)((w2 & 0xff80) | (((w2 & 0x7f) + (rt_rand(m) & 7) + 4) & 0x7f)));
                    dy = (int16_t)((int16_t)rt_rand(m) % 3);
                    dx = (int16_t)((int16_t)rt_rand(m) % 3);
                    spawn_animo_copies(m, obj, (uint8_t)(x + dx - 1), (uint8_t)(y + dy - 1));
                }
            }
        }
    }
    if (di != 0 && di <= 8 && (uint16_t)(rt_rand(m) & 7) < di && object_cull_test(m, 10, obj))
        keep = 0;
    if (rw(ds, CURRENT_LEVEL_WORD) == 9) keep = 0;
    link = (uint16_t)(tile_ptr(m, (uint16_t)x, (uint16_t)y) + 2);
    if (keep) {
        uw_objpool pool;
        pool_from_ds(m, &pool);
        nobj = uw_obj_alloc(&pool, 0);
        pool_to_ds(m, &pool);
        if (nobj) {
            uint16_t w0, cls;
            memmove(ls + nobj, ls + obj, 8);        /* struct_copy_far */
            ww(ls, (uint16_t)(obj + 6), (uint16_t)(rw(ls, (uint16_t)(obj + 6)) & 0x3f));
            w0 = rw(ls, nobj);
            cls = (uint16_t)((w0 & 0x1c0) >> 6);
            if (cls == 7)
                level_effect_reassign(m, nobj, obj);
            else if (((w0 & 0x1f0) >> 4) == 9 && (w0 & 0xf) >= 4 && (w0 & 0xf) <= 6)
                ww(ls, nobj, (uint16_t)((w0 & 0xfff0) | (((w0 & 0xf) - 4) & 0xf)));
            ls[(uint16_t)(nobj + 4)] = (uint8_t)((ls[(uint16_t)(nobj + 4)] & 0xc0) | (ls[(uint16_t)(obj + 8)] & 0x3f));
            if (cls != 5 && cls != 6 && (prop(m, obj_id(m, nobj), 9) & 3) != 2)
                ww(ls, (uint16_t)(nobj + 2), (uint16_t)((rw(ls, (uint16_t)(nobj + 2)) & 0xfc7f)
                                                        | ((ls[(uint16_t)(obj + 0x1a)] & 7) << 7)));
        }
    }
    {
        /* a projectile (break class 9) detonates: its attacker the byte at
         * +0x12, none for a creature */
        uint8_t attacker = (uint8_t)(((rw(ls, obj) & 0x1c0) >> 6) == 1 ? 0 : ls[(uint16_t)(obj + 0x12)]);
        object_remove(m, link, obj, 1);
        if (nobj) {
            uw_objpool pool;
            pool_from_ds(m, &pool);
            uw_object_list_insert(&pool, link, nobj);
            pool_to_ds(m, &pool);
        }
        if (di == 9) {
            if (!nobj) UW_NOT_CARRIED(m->not_carried);
            else if (!projectile_detonate(m, nobj, (uint8_t)x, (uint8_t)y, attacker, (uint16_t)(FRAME_BP - 0x80))
                     && !object_remove(m, link, nobj, 0))
                nobj = 0;
        }
    }
    return nobj;
}

/* ---- putting a thing down -- */

/* obj_deref_link of a link word in the level segment. */
uint16_t deref_link(uw_motion *m, uint16_t at) {
    return obj_at(m, (uint16_t)((rw(m->lseg, at) >> 6) & 0x3ff));
}

/* object_chain_clear: the chain from `link` freed, depth first --
 * the rest of the chain through +4, then a container's contents through +6,
 * then the object out of the chain and back to its pool. A trap or trigger
 * (class 6) goes through trap_after_firing instead, and the rest of its
 * chain with it only as far as that takes it. */
void object_chain_clear(uw_motion *m, uint16_t link) {
    uint8_t *ls = m->lseg;
    uw_objpool pool;
    uint16_t o = deref_link(m, link);
    if (!o) return;
    if (((rw(ls, o) & 0x1c0) >> 6) == 6) {
        trap_after_firing(m, link, o);
        return;
    }
    if ((rw(ls, (uint16_t)(o + 4)) >> 6) & 0x3ff) object_chain_clear(m, (uint16_t)(o + 4));
    if (!(rw(ls, o) & 0x8000) && ((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff))
        object_chain_clear(m, (uint16_t)(o + 6));
    pool_from_ds(m, &pool);
    uw_object_list_remove(&pool, link, o);
    uw_obj_free(&pool, o);
    pool_to_ds(m, &pool);
}

/* object_chain_clear over a link word outside the level segment holding
 * `o`'s index -- object_remove's local: the same walk, the rest of `o`'s
 * chain included, and object_list_remove's unlink of `o` clearing its next
 * link while the local takes it. A trap or trigger's trap_after_firing on
 * that local is not carried. */
void object_chain_clear_local(uw_motion *m, uint16_t o) {
    uint8_t *ls = m->lseg;
    uw_objpool pool;
    if (!o) return;
    if (((rw(ls, o) & 0x1c0) >> 6) == 6) {
        UW_NOT_CARRIED(m->not_carried);
        return;
    }
    if ((rw(ls, (uint16_t)(o + 4)) >> 6) & 0x3ff) object_chain_clear(m, (uint16_t)(o + 4));
    if (!(rw(ls, o) & 0x8000) && ((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff))
        object_chain_clear(m, (uint16_t)(o + 6));
    ww(ls, (uint16_t)(o + 4), (uint16_t)(rw(ls, (uint16_t)(o + 4)) & 0x3f));
    pool_from_ds(m, &pool);
    uw_obj_free(&pool, o);
    pool_to_ds(m, &pool);
}

/* object_make_mobile: a static thing at (motion_tile_x, _y)
 * promoted -- a mobile copy readied by motion_state_init, its hit points
 * from the quality, its heading kept but for class 5 and props +9 kind 2,
 * the static freed and the copy put in its place. The copy, or 0. */
static uint16_t object_make_mobile(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uw_objpool pool;
    uint16_t link = (uint16_t)(tile_ptr(m, rw(ds, MOTION_TILE_X), rw(ds, MOTION_TILE_Y)) + 2), n;
    pool_from_ds(m, &pool);
    n = uw_obj_alloc(&pool, 1);
    pool_to_ds(m, &pool);
    if (!n) return 0;
    memmove(ls + n, ls + obj, 8);
    motion_state_init(m, n, rw(ds, MOTION_TILE_X), rw(ds, MOTION_TILE_Y));
    ls[(uint16_t)(n + 8)] = (uint8_t)(ls[(uint16_t)(obj + 4)] & 0x3f);
    if (((rw(ls, obj) & 0x1c0) >> 6) != 5 && (prop(m, obj_id(m, obj), 9) & 3) != 2)
        ls[(uint16_t)(n + 0x1a)] = (uint8_t)((rw(ls, (uint16_t)(obj + 2)) & 0x380) >> 7);
    if (((rw(ls, n) & 0x1c0) >> 6) == 7)
        level_effect_reassign(m, n, obj);
    pool_from_ds(m, &pool);
    uw_object_list_remove(&pool, link, obj);
    uw_obj_free(&pool, obj);
    uw_object_list_insert(&pool, link, n);
    pool_to_ds(m, &pool);
    return n;
}

/* placed_object_collision, from the instructions: a thing just
 * put at tile (tx, ty) resolved against where it lies -- its bounding
 * cylinder through the terrain, gather and sort (a query local at bp - 0x1e,
 * which spatial_query_ptr keeps); resting on what it can stand on, removed
 * from a wall, lava (terrain 5) or water that burns it (6, unless its
 * toughness is 3), and with nothing under it made mobile to fall, scattered
 * when it landed on something standable and tossed when `toss`. The thing,
 * perhaps a new mobile one, or 0 when removed. The incorporeal (props +3
 * bit 3) are left alone. */
uint16_t placed_object_collision(uw_motion *m, uint16_t obj, uint16_t tx, uint16_t ty, int toss,
                                        uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t q = (uint16_t)(bp - 0x1e), id = obj_id(m, obj);
    int remove = 0, retry = 0, on_floor;
    /* cleared once: the retry comes back in past the clears, so a standable hit the first pass found stands in the second */
    ds[PLACE_STANDABLE] = 0;
    ds[PLACE_ON_OPEN] = 0;
    for (;;) {
        ww(ds, SQ_PTR, q);
        ww(ds, (uint16_t)(q + 0xa), obj_index_of(m, obj));
        if ((prop(m, id, 3) >> 3) & 1) return obj;
        ds[(uint16_t)(q + 8)] = (uint8_t)(prop(m, id, 1) & 7);
        ds[(uint16_t)(q + 9)] = prop(m, id, 0);
        ww(ds, (uint16_t)(q + 4), (uint16_t)(rw(ls, (uint16_t)(obj + 2)) & 0x7f));
        ww(ds, q, (uint16_t)((tx << 3) + ((rw(ls, (uint16_t)(obj + 2)) & 0xe000) >> 13)));
        ww(ds, (uint16_t)(q + 2), (uint16_t)((ty << 3) + ((rw(ls, (uint16_t)(obj + 2)) & 0x1c00) >> 10)));
        sq_terrain(m, ds[(uint16_t)(q + 8)]);
        on_floor = (int16_t)(ds[(uint16_t)(q + 0x10)] + ds[(uint16_t)(q + 8)]) >= rs(ds, (uint16_t)(q + 4)) && !retry;
        sq_gather(m, on_floor, 1);
        sq_sort(m);
        ds[MR_HIT_SLOT] = 0xff;
        if (!ds[(uint16_t)(q + 0x15)] && (int8_t)ds[(uint16_t)(q + 0x16)] > 0
            && ds[(uint16_t)(q + 0x16)] <= ds[(uint16_t)(q + 0x14)]) {
            for (;;) {
                int8_t k;
                uint16_t o;
                ds[(uint16_t)(q + 0x16)]--;
                k = (int8_t)ds[(uint16_t)(q + 0x16)];
                if (k < 0) break;
                if (rec_top(m, k) != rw(ds, (uint16_t)(q + 4))) break;
                ds[MR_HIT_SLOT] = (uint8_t)k;
                o = obj_at(m, (uint16_t)((rec_link(m, k) >> 6) & 0x3ff));
                ww(ds, MR_HIT_ITEM, obj_id(m, o));
                if (((prop(m, obj_id(m, o), 3) >> 1) & 1) == 1) {
                    if (ds[(uint16_t)(RESULTS + k * 6 + 2)] & 0x10) {
                        ds[PLACE_ON_OPEN] = 1;
                        break;
                    }
                    ds[PLACE_STANDABLE] = 1;
                }
            }
        }
        if (((rw(ds, (uint16_t)(q + 0xc)) | rw(ds, (uint16_t)(q + 0xe))) & 0x300) || ds[(uint16_t)(q + 0x15)]) {
            remove = 1;
        } else if ((rw(ds, (uint16_t)(q + 0xc)) & 7) == 5) {
            remove = 1;
        } else if ((rw(ds, (uint16_t)(q + 0xc)) & 7) == 6) {
            remove = ((prop(m, id, 6) >> 2) & 3) == 3 ? 0 : compute_damage(m, obj, 1, 8) != 0;
        } else if (!(rw(ds, (uint16_t)(q + 0xc)) & 8) && !ds[PLACE_ON_OPEN]) {
            uint16_t sx, sy;
            if (on_floor) {
                retry = 1;
                continue;
            }
            sx = rw(ds, MOTION_TILE_X);
            sy = rw(ds, MOTION_TILE_Y);
            ww(ds, MOTION_TILE_X, tx);
            ww(ds, MOTION_TILE_Y, ty);
            obj = object_make_mobile(m, obj);
            ww(ds, MOTION_TILE_X, sx);
            ww(ds, MOTION_TILE_Y, sy);
            if (ds[PLACE_STANDABLE]) {
                ls[(uint16_t)(obj + 0x13)] = (uint8_t)((ls[(uint16_t)(obj + 0x13)] & 0x80) | 3);
                ls[(uint16_t)(obj + 9)] = (uint8_t)(ls[(uint16_t)(obj + 9)] + (uint8_t)((rt_rand(m) % 9) << 4) + 0xc0);
            }
            if (toss) {
                ls[(uint16_t)(obj + 0x13)] = (uint8_t)((ls[(uint16_t)(obj + 0x13)] & 0x80) | (((rt_rand(m) & 3) + 1) & 0x7f));
                ls[(uint16_t)(obj + 0x14)] = (uint8_t)((ls[(uint16_t)(obj + 0x14)] & 7) | ((((rt_rand(m) & 3) + 0xe) & 0x1f) << 3));
            }
        }
        break;
    }
    if (remove)
        return object_remove(m, (uint16_t)(tile_ptr(m, tx, ty) + 2), obj, 0);
    return obj;
}

/* item_fits_in_tile, from the instructions: may item `id`
 * (object `index`) stand at fine (x, y, z)? Its own query local at bp - 0x1e
 * for the duration, spatial_query_ptr restored after. Refused past the
 * ceiling, into a wall, onto a result it may not stand on, or -- unless
 * `slope` -- over a slope's low side. Leaves the floor under it and its
 * support behind. */
int item_fits_in_tile(uw_motion *m, uint16_t id, uint16_t index, int16_t x, int16_t y, int16_t z,
                             int slope, uint8_t radius, uint16_t bp) {
    uint8_t *ds = m->ds;
    uint16_t saved = rw(ds, SQ_PTR), q = (uint16_t)(bp - 0x1e);
    int fits = 0, di = -1;
    int16_t r2;
    ww(ds, SQ_PTR, q);
    ww(ds, (uint16_t)(q + 0xa), index);
    ds[(uint16_t)(q + 8)] = (uint8_t)(prop(m, id, 1) & 7);
    ds[(uint16_t)(q + 9)] = prop(m, id, 0);
    ww(ds, q, (uint16_t)x);
    ww(ds, (uint16_t)(q + 2), (uint16_t)y);
    ww(ds, (uint16_t)(q + 4), (uint16_t)z);
    if (ds[(uint16_t)(q + 9)] != 0x80 && (int16_t)(ds[(uint16_t)(q + 9)] + z) > 0x7f) goto done;
    sq_terrain(m, radius);
    if ((rw(ds, (uint16_t)(q + 0xc)) | rw(ds, (uint16_t)(q + 0xe))) & 0x300) goto done;
    ww(ds, PLACE_FLOOR, (int16_t)(z + radius) >= ds[(uint16_t)(q + 0x11)] ? ds[(uint16_t)(q + 0x11)] : ds[(uint16_t)(q + 0x10)]);
    r2 = ds[(uint16_t)(q + 8)];
    if (radius > r2) r2 = radius;
    if (rs(ds, (uint16_t)(q + 4)) > (int16_t)(ds[(uint16_t)(q + 0x10)] + r2))
        ww(ds, PLACE_SUPPORT, 0x10);
    else
        ww(ds, PLACE_SUPPORT, (uint16_t)(1 << (rw(ds, (uint16_t)(q + 0xc)) & 3)));
    sq_gather(m, rw(ds, PLACE_SUPPORT) != 0x10 && (int16_t)index >= 0x100, 1);
    if (ds[(uint16_t)(q + 0x14)]) {
        int si;
        sq_sort(m);
        if (ds[(uint16_t)(q + 0x15)]) goto done;
        if (ds[(uint16_t)(q + 0x14)]) {
            for (si = 0; si < (int8_t)ds[(uint16_t)(q + 0x16)]; si++)
                if (rec_top(m, si) > rw(ds, PLACE_FLOOR)) {
                    di = si;
                    ww(ds, PLACE_FLOOR, rec_top(m, si));
                }
        }
        if (di > -1) {
            uint16_t o = obj_at(m, (uint16_t)((rec_link(m, di) >> 6) & 0x3ff));
            if (!((prop(m, obj_id(m, o), 3) >> 1) & 1)) goto done;
            ww(ds, PLACE_SUPPORT, 1);
        }
    }
    if (!slope && ((rw(ds, (uint16_t)(q + 0xc)) | rw(ds, (uint16_t)(q + 0xe))) & 0x800)
        && (int16_t)(rs(ds, (uint16_t)(q + 4)) - radius) > rs(ds, PLACE_FLOOR))
        goto done;
    fits = 1;
done:
    ww(ds, SQ_PTR, saved);
    return fits;
}

/* object_place_near: up to 24 tries at a point within `spread`
 * of (fx, fy) -- the exact point first only while object_place_scatter is
 * set -- that item_fits_in_tile allows; there the thing is appended to its
 * tile's chain and, for a static one, placed_object_collision runs. */
int object_place_near(uw_motion *m, uint16_t obj, int16_t fx, int16_t fy, int16_t z, int16_t spread,
                             uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    int16_t width = (int16_t)(spread * 2 + 1), x, y;
    uint8_t t;
    for (t = 0; t < 0x18; t++) {
        if (t == 0 && ds[PLACE_SCATTER]) {
            x = fx;
            y = fy;
        } else {
            x = (int16_t)(fx - spread + (int16_t)rt_rand(m) % width);
            y = (int16_t)(fy - spread + (int16_t)rt_rand(m) % width);
        }
        if (!item_fits_in_tile(m, obj_id(m, obj), obj_index_of(m, obj), x, y, z, 1, 0,
                               (uint16_t)(bp - 0xe - 14 - 4 - 2)))
            continue;
        {
            uw_objpool pool;
            uint16_t tp = tile_ptr(m, (uint16_t)(x >> 3), (uint16_t)(y >> 3));
            ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0x1fff) | ((x & 7) << 13)));
            ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0xe3ff) | ((y & 7) << 10)));
            ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0xff80) | (z & 0x7f)));
            pool_from_ds(m, &pool);
            uw_object_list_append(&pool, (uint16_t)(tp + 2), obj);
            pool_to_ds(m, &pool);
            if (obj < rw(ds, STATIC_BASE)) {
                ww(ls, (uint16_t)(obj + 0x16), (uint16_t)((rw(ls, (uint16_t)(obj + 0x16)) & 0x3ff) | (((x >> 3) & 0x3f) << 10)));
                ww(ls, (uint16_t)(obj + 0x16), (uint16_t)((rw(ls, (uint16_t)(obj + 0x16)) & 0xfc0f) | (((y >> 3) & 0x3f) << 4)));
            } else {
                placed_object_collision(m, obj, (uint16_t)(x >> 3), (uint16_t)(y >> 3), 1,
                                        (uint16_t)(bp - 0xe - 10 - 4 - 2));
            }
        }
        return 1;
    }
    return 0;
}

/* object_move_to_coords: object_place_near, and failing it --
 * unless `keep` -- junk (object_cull_test(10)) is dropped; otherwise the fine
 * position is written and the thing goes at the head of its tile's chain. */
int object_move_to_coords(uw_motion *m, int16_t fx, int16_t fy, int16_t z, uint16_t obj, int16_t spread,
                                 int keep, uint16_t bp) {
    uint8_t *ls = m->lseg;
    uw_objpool pool;
    if (object_place_near(m, obj, fx, fy, z, spread, (uint16_t)(bp - 4 - 12 - 4 - 2))) return 1;
    if (!keep && object_cull_test(m, 10, obj)) {
        /* object_chain_remove with no tile */
        if (!(rw(ls, obj) & 0x8000) && ((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x3ff))
            object_chain_clear(m, (uint16_t)(obj + 6));
        pool_from_ds(m, &pool);
        uw_obj_free(&pool, obj);
        pool_to_ds(m, &pool);
        return 0;
    }
    ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0x1fff) | ((fx & 7) << 13)));
    ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0xe3ff) | ((fy & 7) << 10)));
    pool_from_ds(m, &pool);
    uw_object_list_insert(&pool, (uint16_t)(tile_ptr(m, (uint16_t)(fx >> 3), (uint16_t)(fy >> 3)) + 2), obj);
    pool_to_ds(m, &pool);
    return 1;
}

/* ---- a dead creature's removal --------- */

enum { LOOT_ROW = 0x736c };      /* the critter_properties row spawn_npc_loot works from */

/* The loot spawners, from the overlay body's
 * instructions: each object_create'd thing goes into the creature's own
 * contents (+6) before the contents spill. */
static void loot_into(uw_motion *m, uint16_t npc, uint16_t o) {
    uw_objpool pool;
    if (!o) {
        UW_NOT_CARRIED(m->not_carried);
        return;
    }
    pool_from_ds(m, &pool);
    uw_object_list_insert(&pool, (uint16_t)(npc + 6), o);
    pool_to_ds(m, &pool);
}

uint16_t create_object(uw_motion *m, uint16_t id, int mobile) {
    uw_objpool pool;
    uint16_t o;
    pool_from_ds(m, &pool);
    o = uw_object_create(&pool, id, mobile, m->ds + OBJ_PROPERTIES);
    pool_to_ds(m, &pool);
    return o;
}

/* A loot thing's quality: half the time the level times four plus
 * rand() % (level * 4), else rand() % 64. */
static uint8_t loot_quality(uw_motion *m) {
    int16_t lvl4 = (int16_t)(rw(m->ds, CURRENT_LEVEL_WORD) << 2);
    if ((int16_t)rt_rand(m) % 2 == 0) {
        int16_t r = (int16_t)rt_rand(m);
        if (!lvl4) { UW_NOT_CARRIED(m->not_carried); return 0; }
        return (uint8_t)(lvl4 + r % lvl4);
    }
    return (uint8_t)((int16_t)rt_rand(m) % 0x40);
}

static void spawn_valuable_loot(uw_motion *m, uint16_t npc) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t row = rw(ds, LOOT_ROW), o;
    uint8_t lo = (uint8_t)(ds[(uint16_t)(row + 0x26)] & 0xf), hi = (uint8_t)(ds[(uint16_t)(row + 0x26)] >> 4);
    int16_t lvl3 = (int16_t)(rw(ds, CURRENT_LEVEL_WORD) * 3), r, q;
    int8_t t, v, count;
    if ((int16_t)rt_rand(m) % 0x10 >= hi) return;
    r = (int16_t)((int16_t)rt_rand(m) % (int16_t)(0x28 - lvl3));
    t = (int8_t)((uint8_t)r - (uint8_t)(0x21 - (uint8_t)lvl3));
    if (t < 0) t = 0;
    v = (int8_t)ds[(uint16_t)(0x6252 + t * 11)];
    if (v == 0) v = 1;
    if (v >= 12) v = (int8_t)((uint8_t)(v << 3) + 0xbc);
    else if (v >= 8) v = (int8_t)((uint8_t)(v << 2) + 0xec);
    else if (v >= 4) v = (int8_t)((uint8_t)(v << 1) + 0xfc);
    q = (int16_t)(lo << 2);
    if (q < v) {
        if ((int16_t)rt_rand(m) % v >= q) return;
        count = 1;
    } else {
        int8_t k = (int8_t)((uint8_t)(q / v) << 1);
        count = (int8_t)(roll_dice(m, 4, k) >> 2);
    }
    if (count < 1) return;
    o = create_object(m, (uint16_t)(0xa0 + t), 0);
    if (o) ww(ls, (uint16_t)(o + 6), (uint16_t)((rw(ls, (uint16_t)(o + 6)) & 0x3f) | ((count & 0x3ff) << 6)));
    loot_into(m, npc, o);
}

static void spawn_food_loot(uw_motion *m, uint16_t npc) {
    uint8_t *ds = m->ds;
    uint16_t row = rw(ds, LOOT_ROW);
    if ((int16_t)rt_rand(m) % 0x10 >= (ds[(uint16_t)(row + 0x27)] & 0xf)) return;
    loot_into(m, npc, create_object(m, (uint16_t)(0xb0 + (ds[(uint16_t)(row + 0x27)] >> 4)), 0));
}

static void spawn_arms_loot(uw_motion *m, uint16_t npc) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint8_t i;
    for (i = 0; i < 2; i++) {
        uint16_t row = rw(ds, LOOT_ROW), o;
        uint8_t b = ds[(uint16_t)(row + 0x20 + i)], q;
        if (!(b & 1)) continue;
        b = (uint8_t)((b >> 1) & 0x7f);
        o = create_object(m, (uint16_t)((((b >> 4) & 3) << 4) + (b & 0xf)), 0);
        q = loot_quality(m);
        if (!o) { loot_into(m, npc, o); continue; }
        ls[(uint16_t)(o + 4)] = (uint8_t)((ls[(uint16_t)(o + 4)] & 0xc0) | (q & 0x3f));
        if (((rw(ls, o) & 0x30) >> 4) == 1 && ds[(uint16_t)(0x5942 + 2 + (rw(ls, o) & 0xf) * 3)] == 0xc0)
            ww(ls, (uint16_t)(o + 6), (uint16_t)((rw(ls, (uint16_t)(o + 6)) & 0x3f)
                                                  | ((((int16_t)rt_rand(m) % 8 + 4) & 0x3ff) << 6)));
        loot_into(m, npc, o);
    }
}

static void spawn_other_loot(uw_motion *m, uint16_t npc) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint8_t i;
    for (i = 0; i < 2; i++) {
        uint16_t row = rw(ds, LOOT_ROW), w = rw(ds, (uint16_t)(row + 0x22 + i * 2)), o;
        uint8_t q;
        if ((uint16_t)((int16_t)rt_rand(m) % 0x10) >= (w & 0xf)) continue;
        o = create_object(m, (uint16_t)((w >> 4) & 0xfff), 0);
        q = loot_quality(m);
        if (o) ls[(uint16_t)(o + 4)] = (uint8_t)((ls[(uint16_t)(o + 4)] & 0xc0) | (q & 0x3f));
        loot_into(m, npc, o);
    }
}

/* spawn_npc_loot: once (+0x0d bit 12), from the critter row. */
void spawn_npc_loot(uw_motion *m, uint16_t npc) {
    uint8_t *ls = m->lseg;
    uint16_t w0 = rw(ls, npc);
    if (rw(ls, (uint16_t)(npc + 0xd)) & 0x1000) return;
    ww(m->ds, LOOT_ROW, (uint16_t)((((w0 & 0x30) >> 4 << 4) + (w0 & 0xf)) * 0x30 + 0x4a52));   /* critter_properties */
    spawn_valuable_loot(m, npc);
    spawn_food_loot(m, npc);
    spawn_arms_loot(m, npc);
    spawn_other_loot(m, npc);
    ww(ls, (uint16_t)(npc + 0xd), (uint16_t)((rw(ls, (uint16_t)(npc + 0xd)) & 0xefff) | 0x1000));
}

/* drop_npc_remains, from the instructions: the remains (item
 * 0xd8 + kind) where the creature lies, resolved by placed_object_collision
 * and tossed; and seven times in sixteen the fluid (0xc0 + kind), its owner
 * the creature's race nibble, set down near its own position. `bp` is its
 * frame. */
void drop_npc_remains(uw_motion *m, uint16_t npc, uint8_t remains, uint8_t fluid, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w16 = rw(ls, (uint16_t)(npc + 0x16)), w2 = rw(ls, (uint16_t)(npc + 2));
    uint16_t tp = tile_ptr(m, (uint16_t)(w16 >> 10), (uint16_t)((w16 & 0x3f0) >> 4)), o;
    if (remains && (o = create_object(m, (uint16_t)(0xd8 + remains), 0)) != 0) {
        uw_objpool pool;
        ww(ls, (uint16_t)(o + 2), (uint16_t)((rw(ls, (uint16_t)(o + 2)) & 0x1fff) | (w2 & 0xe000)));
        ww(ls, (uint16_t)(o + 2), (uint16_t)((rw(ls, (uint16_t)(o + 2)) & 0xe3ff) | (w2 & 0x1c00)));
        ww(ls, (uint16_t)(o + 2), (uint16_t)((rw(ls, (uint16_t)(o + 2)) & 0xff80) | (w2 & 0x7f)));
        ls[(uint16_t)(o + 4)] = (uint8_t)((ls[(uint16_t)(o + 4)] & 0xc0) | 0x28);
        pool_from_ds(m, &pool);
        uw_object_list_insert(&pool, (uint16_t)(tp + 2), o);
        pool_to_ds(m, &pool);
        placed_object_collision(m, o, rw(ds, MOTION_TILE_X), rw(ds, MOTION_TILE_Y), 1,
                                (uint16_t)(bp - 8 - 10 - 4 - 2));
    }
    if (fluid && (int16_t)rt_rand(m) % 0x10 < 7 && (o = create_object(m, (uint16_t)(0xc0 + fluid), 0)) != 0) {
        uint16_t sw16 = rw(ls, (uint16_t)(npc + 0x16)), sw2 = rw(ls, (uint16_t)(npc + 2));
        uint16_t bpa = (uint16_t)(bp - 8 - 12 - 4 - 2);    /* object_place_at_own_coords */
        ls[(uint16_t)(o + 6)] = (uint8_t)((ls[(uint16_t)(o + 6)] & 0xc0) | (rw(ls, npc) & 0x3f));
        object_move_to_coords(m, (int16_t)(((sw16 >> 10) << 3) + ((sw2 & 0xe000) >> 13)),
                              (int16_t)((((sw16 & 0x3f0) >> 4) << 3) + ((sw2 & 0x1c00) >> 10)),
                              (int16_t)(sw2 & 0x7f), o, 4, 0, (uint16_t)(bpa - 14 - 4 - 2));
    }
}

/* spill_inventory, from the overlay body: a thing's contents
 * (+6) unlinked and each set down near it -- a mobile's own tile, anything
 * else at the spot last recorded -- with object_move_to_coords, spread 6; while
 * spilling, an ownable (props +7 bit 7) thing's own +6 owner bits take
 * `owner`. 1 when there was something. The overlay's frames are not
 * modelled: a query local under it lands where the port puts it. */
int spill_inventory(uw_motion *m, uint16_t obj, uint16_t owner, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t it, tx, ty, w2;
    if (!((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x3ff)) return 0;
    it = deref_link(m, (uint16_t)(obj + 6));
    ww(ls, (uint16_t)(obj + 6), (uint16_t)(rw(ls, (uint16_t)(obj + 6)) & 0x3f));
    if (obj < rw(ds, STATIC_BASE)) {
        tx = (uint16_t)(rw(ls, (uint16_t)(obj + 0x16)) >> 10);
        ty = (uint16_t)((rw(ls, (uint16_t)(obj + 0x16)) & 0x3f0) >> 4);
    } else {
        tx = rw(ds, 0x269a);
        ty = rw(ds, 0x269c);
    }
    w2 = rw(ls, (uint16_t)(obj + 2));
    while (it) {
        uint16_t next = deref_link(m, (uint16_t)(it + 4));
        if (owner && (prop(m, obj_id(m, obj), 7) & 0x80))
            ls[(uint16_t)(obj + 6)] = (uint8_t)((ls[(uint16_t)(obj + 6)] & 0xc0) | (owner & 0x3f));
        object_move_to_coords(m, (int16_t)((tx << 3) + ((w2 & 0xe000) >> 13)), (int16_t)((ty << 3) + ((w2 & 0x1c00) >> 10)),
                              (int16_t)(w2 & 0x7f), it, 6, 0, (uint16_t)(bp - 0xe - 14 - 4 - 2));
        it = next;
    }
    return 1;
}

/* projectile_motion_apply: the motion block written back into its
 * object -- tile lists, position, owner, fall damage, the settle of a thrown
 * thing, speed and state. 1 for a mobile object. `bp` is its frame. */
int projectile_motion_apply(uw_motion *m, uint16_t obj, uint16_t si, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    int16_t bx = rs(ds, si), by = rs(ds, (uint16_t)(si + 2));
    uw_objpool pool;
    memset(&pool, 0, sizeof pool);
    pool.seg = ls;
    if ((bx >> 8) != rs(ds, MOTION_TILE_X) || (by >> 8) != rs(ds, MOTION_TILE_Y)) {
        uw_object_list_remove(&pool, (uint16_t)(tile_ptr(m, rw(ds, MOTION_TILE_X), rw(ds, MOTION_TILE_Y)) + 2), obj);
        ww(ds, MOTION_TILE_X, (uint16_t)(bx >> 8));
        ww(ds, MOTION_TILE_Y, (uint16_t)(by >> 8));
        uw_object_list_insert(&pool, (uint16_t)(tile_ptr(m, rw(ds, MOTION_TILE_X), rw(ds, MOTION_TILE_Y)) + 2), obj);
    }
    ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0xff80)
                                            | ((rs(ds, (uint16_t)(si + 4)) >> 3) & 0x7f)));
    ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0x1fff) | (((bx >> 5) & 7) << 13)));
    ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0xe3ff) | (((by >> 5) & 7) << 10)));
    if (obj < rw(ds, STATIC_BASE)) {
        ls[(uint16_t)(obj + 8)] = ds[(uint16_t)(si + 0x1b)];
    } else {
        ls[(uint16_t)(obj + 4)] = (uint8_t)((ls[(uint16_t)(obj + 4)] & 0xc0) | (ds[(uint16_t)(si + 0x1b)] & 0x3f));
    }
    if (rw(ds, (uint16_t)(si + 0x26)) > 0x100) {
        if (((rw(ls, obj) & 0x1c0) >> 6) != 1)         /* effect 15 at the thing, louder by its weight */
            play_sound_effect_at_object(m, 0xf, obj, (int8_t)(uint8_t)((int16_t)(uint16_t)(
                ((rw(ds, (uint16_t)(OBJ_PROPERTIES + obj_id(m, obj) * 11 + 1)) >> 4) & 0xfff) - 600) / 0x32));
        apply_damage(m, obj, 0, rs(ds, MOTION_TILE_X), rs(ds, MOTION_TILE_Y), (uint8_t)(rw(ds, (uint16_t)(si + 0x26)) >> 8), 0);
    }
    if (obj < rw(ds, STATIC_BASE)) {
        ls[(uint16_t)(obj + 8)] = ds[(uint16_t)(si + 0x1b)];
    } else {
        ls[(uint16_t)(obj + 4)] = (uint8_t)((ls[(uint16_t)(obj + 4)] & 0xc0) | (ds[(uint16_t)(si + 0x1b)] & 0x3f));
    }
    if (ds[(uint16_t)(si + 0x25)] & 4) {
        if (rt_rand(m) % 5 == 0)            /* lava */
            apply_damage(m, obj, 0, rs(ds, MOTION_TILE_X), rs(ds, MOTION_TILE_Y), 1, 8);
    }
    if (((rw(ls, obj) & 0x1c0) >> 6) != 1) {
        int moving = (rw(ds, (uint16_t)(si + 0x14)) | rw(ds, (uint16_t)(si + 0xa)) | rw(ds, (uint16_t)(si + 0x10))) != 0;
        if (obj > rw(ds, STATIC_BASE)) {
            if (moving) {
                obj = object_make_mobile(m, obj);
                if (!obj) {
                    UW_NOT_CARRIED(m->not_carried);
                    return -1;
                }
            }
        } else if (!moving) {
            ls[(uint16_t)(obj + 0xa)] = (uint8_t)((ls[(uint16_t)(obj + 0xa)] & 0x8f)
                                                  | ((ds[(uint16_t)(MOTION_STATE_TABLE + ds[(uint16_t)(si + 0x25)])] & 7) << 4));
            obj = object_hits_floor(m, obj);
            if (!obj) return 0;
            obj = placed_object_collision(m, obj, rw(ds, MOTION_TILE_X), rw(ds, MOTION_TILE_Y), 0,
                                          (uint16_t)(bp - 14 - 10 - 4 - 2));
            if (!obj) return 0;
            if (obj < rw(ds, STATIC_BASE)) {
                /* motion_bounce_randomise */
                if (ds[PLACE_STANDABLE]) {
                    ww(ds, (uint16_t)(si + 0x1e), (uint16_t)(rw(ds, (uint16_t)(si + 0x1e)) + (rt_rand(m) & 0x3fff) + 0xe000));
                    ww(ds, (uint16_t)(si + 0x14), 0xbc);
                } else {
                    ww(ds, (uint16_t)(si + 0x14), (uint16_t)(((rt_rand(m) + 1) & 3) * 0x2f));
                    ww(ds, (uint16_t)(si + 0x10), 0xfffc);
                }
            }
        }
    }
    if (obj >= rw(ds, STATIC_BASE)) {
        if (((rw(ls, obj) & 0x1c0) >> 6) == 5)
            ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0xfc7f)
                                                    | (((rs(ds, (uint16_t)(si + 0x1e)) >> 13) & 7) << 7)));
        return 0;
    }
    {
        int16_t v;
        ls[(uint16_t)(obj + 9)] = (uint8_t)(rs(ds, (uint16_t)(si + 0x1e)) >> 8);
        ww(ls, (uint16_t)(obj + 0x16), (uint16_t)((rw(ls, (uint16_t)(obj + 0x16)) & 0x3ff)
                                                   | ((rw(ds, MOTION_TILE_X) & 0x3f) << 10)));
        ww(ls, (uint16_t)(obj + 0x16), (uint16_t)((rw(ls, (uint16_t)(obj + 0x16)) & 0xfc0f)
                                                   | ((rw(ds, MOTION_TILE_Y) & 0x3f) << 4)));
        ls[(uint16_t)(obj + 0x13)] = (uint8_t)((ls[(uint16_t)(obj + 0x13)] & 0x7f)
                                               | (rs(ds, (uint16_t)(si + 0x10)) == -4 ? 0x80 : 0));
        v = (int16_t)(rs(ds, (uint16_t)(si + 0xa)) / 0x40 + 0x10);
        if (v < 0) v = 0;
        else if (v > 0x1f) v = 0x1f;
        ls[(uint16_t)(obj + 0x14)] = (uint8_t)((ls[(uint16_t)(obj + 0x14)] & 7) | ((v & 0x1f) << 3));
        ls[(uint16_t)(obj + 0x13)] = (uint8_t)((ls[(uint16_t)(obj + 0x13)] & 0x80)
                                               | ((rs(ds, (uint16_t)(si + 0x14)) / 0x2f) & 0x7f));
        ls[(uint16_t)(obj + 0xa)] = (uint8_t)((ls[(uint16_t)(obj + 0xa)] & 0x8f)
                                              | ((ds[(uint16_t)(MOTION_STATE_TABLE + ds[(uint16_t)(si + 0x25)])] & 7) << 4));
        if (((rw(ls, obj) & 0x1c0) >> 6) != 1) {
            /* A thing that is not a creature keeps its fine position whole. */
            ww(ls, (uint16_t)(obj + 0xb), rw(ds, si));
            ww(ls, (uint16_t)(obj + 0xd), rw(ds, (uint16_t)(si + 2)));
            ww(ls, (uint16_t)(obj + 0xf), rw(ds, (uint16_t)(si + 4)));
        }
        return 1;
    }
}

uint16_t uw_motion_tile_ptr(uw_motion *m, uint16_t x, uint16_t y) {
    return tile_ptr(m, x, y);
}

uint16_t uw_motion_obj_at(uw_motion *m, uint16_t index) {
    return obj_at(m, index);
}

uint16_t uw_motion_deref_link(uw_motion *m, uint16_t link) {
    return deref_link(m, link);
}

uint16_t uw_motion_object_remove(uw_motion *m, uint16_t link, uint16_t obj, int force) {
    return object_remove(m, link, obj, force);
}

void uw_motion_creature_set_goal_for(uw_motion *m, uint16_t obj, int goal, int gtarg) {
    uint16_t saved = rw(m->ds, CURRENT_NPC);
    ww(m->ds, CURRENT_NPC, obj);
    creature_set_goal(m, goal, gtarg);
    ww(m->ds, CURRENT_NPC, saved);
}

void uw_motion_view_explored(uw_motion *m, long newly_seen) {
    uint8_t *ds = m->ds;
    uint8_t lvl = ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x3d)];
    int16_t level = rs(ds, CURRENT_LEVEL_WORD), delta;
    if (lvl == 0 || lvl >= 0x10 || level == 9) return;
    delta = (int16_t)((int16_t)((int16_t)newly_seen * level) / 10);
    if (delta) player_gain_experience(m, delta);
}

void uw_motion_player_gain_experience(uw_motion *m, int16_t delta) {
    player_gain_experience(m, delta);
}

/* object_chain_weight: each object along the chain from `link`
 * adds its unit weight (obj_properties +1 >> 4) times its quantity -- 1 for
 * a thing that is not a stack or whose count has bit 9 -- then the rest of
 * the chain, then, for a non-quantity, its contents. Sixteen bits. */
void object_chain_weight(uw_motion *m, uint16_t link, uint16_t *acc) {
    uint8_t *ls = m->lseg;
    uint16_t o = deref_link(m, link), w0, q, n;
    if (!o) return;
    w0 = rw(ls, o);
    q = (uint16_t)((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff);
    n = (uint16_t)((w0 & 0x8000) && !(q & 0x200) ? q : 1);
    *acc = (uint16_t)(*acc + ((rw(m->ds, (uint16_t)(0x5b6f + (w0 & 0x1ff) * 0xb)) >> 4) & 0xfff) * n);
    object_chain_weight(m, (uint16_t)(o + 4), acc);
    if (!(w0 & 0x8000)) object_chain_weight(m, (uint16_t)(o + 6), acc);
}

/* object_weight, in tenths of a stone: a stack whose count lacks
 * bit 9 is count times the unit; anything else the unit, and a non-quantity
 * with a +6 link adds its contents' chain. */
uint16_t object_weight(uw_motion *m, uint16_t obj) {
    uint8_t *ls = m->lseg;
    uint16_t w0 = rw(ls, obj), q = (uint16_t)((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x3ff);
    uint16_t unit = (uint16_t)((rw(m->ds, (uint16_t)(0x5b6f + (w0 & 0x1ff) * 0xb)) >> 4) & 0xfff), acc;
    if ((w0 & 0x8000) && !(q & 0x200)) return (uint16_t)(q * unit);
    acc = unit;
    if (!(w0 & 0x8000) && q) object_chain_weight(m, (uint16_t)(obj + 6), &acc);
    return acc;
}

void uw_motion_object_list_remove(uw_motion *m, uint16_t link, uint16_t obj) {
    uw_objpool pool;
    pool_from_ds(m, &pool);
    uw_object_list_remove(&pool, link, obj);
    pool_to_ds(m, &pool);
}

void uw_motion_object_list_insert(uw_motion *m, uint16_t link, uint16_t obj) {
    uw_objpool pool;
    pool_from_ds(m, &pool);
    uw_object_list_insert(&pool, link, obj);
    pool_to_ds(m, &pool);
}

/* obj_pool_init: every tile's object
 * link cleared, the two pools' bases, floors and tops over the tile map's
 * segment (mobiles at +0x4000, statics at +0x5b00; the free stacks at
 * +0x7300..0x74fa and +0x74fc..0x7afa, both full, their pointers at the
 * top), the stacks filled with the indices 2..0x3ff in one run -- the
 * mobile stack 2..0xff, the static 0x100..0x3ff -- and, with an Avatar,
 * its next and contents links cleared and inventory_reset; no level
 * effects, and the active roster empty at the static top + 2. */
void obj_pool_init(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t tm = rw(ds, 0x19b4), i, at, pl = rw(ds, TRACKED_OBJECT);
    for (i = 0; i < 0x1000; i++) {
        at = (uint16_t)(tm + i * 4 + 2);
        ww(ls, at, (uint16_t)(rw(ls, at) & 0x3f));
    }
    ww(ds, MOBILE_BASE, (uint16_t)(tm + 0x4000));
    ww(ds, STATIC_BASE, (uint16_t)(tm + 0x5b00));
    ww(ds, 0x2752, (uint16_t)(tm + 0x7300));     /* mobile_freelist_floor */
    ww(ds, 0x274e, (uint16_t)(tm + 0x74fa));     /* mobile_freelist_top */
    ww(ds, 0x2756, (uint16_t)(tm + 0x74fa));     /* mobile_freelist_sp */
    ww(ds, 0x2746, (uint16_t)(tm + 0x74fc));     /* static_freelist_floor */
    ww(ds, 0x2742, (uint16_t)(tm + 0x7afa));     /* static_freelist_top */
    ww(ds, 0x274a, (uint16_t)(tm + 0x7afa));     /* static_freelist_sp */
    for (i = 2, at = (uint16_t)(tm + 0x7300); i < 0x400; i++, at = (uint16_t)(at + 2)) ww(ls, at, i);
    if (pl || rw(ds, (uint16_t)(TRACKED_OBJECT + 2))) {
        ww(ls, (uint16_t)(pl + 4), (uint16_t)(rw(ls, (uint16_t)(pl + 4)) & 0x3f));
        ww(ls, (uint16_t)(pl + 6), (uint16_t)(rw(ls, (uint16_t)(pl + 6)) & 0x3f));
        inventory_reset(m);
    }
    ds[0x3656] = 0;                              /* level_effect_count */
    ww(ds, ACTIVE_LIST, (uint16_t)(tm + 0x7afc));
    ww(ds, ACTIVE_END, (uint16_t)(tm + 0x7afc));
}

/* game_world_reset: obj_pool_init, post_event(2), and the
 * Avatar's tile -1 -- it stands in no chain now. */
void game_world_reset(uw_motion *m) {
    obj_pool_init(m);
    ww(m->ds, PENDING_EVENTS, (uint16_t)(rw(m->ds, PENDING_EVENTS) | 2));
    ww(m->ds, TRACKED_TILE, 0xffff);
}

uint16_t uw_motion_object_create(uw_motion *m, uint16_t id, int mobile) {
    return create_object(m, id, mobile);
}

void uw_motion_obj_free(uw_motion *m, uint16_t obj) {
    uw_objpool pool;
    pool_from_ds(m, &pool);
    uw_obj_free(&pool, obj);
    pool_to_ds(m, &pool);
}

uint16_t uw_motion_obj_index(uw_motion *m, uint16_t obj) {
    return obj_index_of(m, obj);
}

uint16_t uw_motion_inventory_can_carry(uw_motion *m, uint16_t obj) {
    uint16_t sum = (uint16_t)(object_weight(m, obj) + rw(m->ds, 0x72d2));
    return (uint16_t)((sum & 0xff00) | (sum > rw(m->ds, 0x72d4) ? 0 : 1));
}

void uw_motion_spawn_npc_loot(uw_motion *m, uint16_t npc) {
    spawn_npc_loot(m, npc);
}

int uw_motion_item_enchantment(uw_motion *m, uint16_t obj, int16_t *effect, int16_t *magnitude, int *special) {
    return item_enchantment(m, obj, effect, magnitude, special);
}

int uw_motion_object_move_to_coords(uw_motion *m, int16_t fx, int16_t fy, int16_t z, uint16_t obj, int16_t spread,
                                    int keep, uint16_t bp) {
    return object_move_to_coords(m, fx, fy, z, obj, spread, keep, bp);
}

int uw_motion_item_fits_in_tile(uw_motion *m, uint16_t id, uint16_t index, int16_t x, int16_t y, int16_t z, int slope,
                                uint8_t radius, uint16_t bp) {
    return item_fits_in_tile(m, id, index, x, y, z, slope, radius, bp);
}

void uw_motion_object_list_append(uw_motion *m, uint16_t link, uint16_t obj) {
    uw_objpool pool;
    pool_from_ds(m, &pool);
    uw_object_list_append(&pool, link, obj);
    pool_to_ds(m, &pool);
}

uint16_t uw_motion_placed_object_collision(uw_motion *m, uint16_t obj, uint16_t tx, uint16_t ty, int toss,
                                           uint16_t bp) {
    return placed_object_collision(m, obj, tx, ty, toss, bp);
}

int uw_motion_object_place_at_own_coords(uw_motion *m, uint16_t owner, uint16_t obj, int16_t spread, int keep,
                                         uint16_t bp) {
    uint16_t w16 = rw(m->lseg, (uint16_t)(owner + 0x16)), w2 = rw(m->lseg, (uint16_t)(owner + 2));
    return object_move_to_coords(m, (int16_t)((w16 >> 10) * 8 + (w2 >> 13)),
                                 (int16_t)(((w16 & 0x3f0) >> 4) * 8 + ((w2 & 0x1c00) >> 10)),
                                 (int16_t)(w2 & 0x7f), obj, spread, keep, (uint16_t)(bp - 14 - 4 - 2));
}

/* ==== inventory_split_stack's answer, for the shell ==== */

void uw_motion_stack_answer(uw_motion *m, int n, int all, int cancel) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t obj = m->stack_ask, rest = obj, count;
    int path = m->stack_ask_path;
    int16_t slot = m->stack_ask_slot;
    m->stack_ask = 0;
    m->stack_ask_path = 0;
    if (!obj) return;
    count = (uint16_t)((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x3ff);
    if (cancel) return;
    if (all) n = count;
    if (n > count) n = count;
    uw_motion_print_ds_string(m, 0x18d1);               /* "\n" */
    if (n <= 0) return;
    if ((uint16_t)n != count) {
        /* obj_alloc(0), the record copied, the counts divided */
        rest = uw_motion_object_create(m, (uint16_t)(rw(ls, obj) & 0x1ff), 0);
        if (!rest) { UW_NOT_CARRIED(m->not_carried); return; }
        memcpy(ls + rest, ls + obj, 8);
        ww(ls, (uint16_t)(rest + 6), (uint16_t)((rw(ls, (uint16_t)(rest + 6)) & 0x3f) | (((count - n) & 0x3ff) << 6)));
        ww(ls, (uint16_t)(obj + 6), (uint16_t)((rw(ls, (uint16_t)(obj + 6)) & 0x3f) | ((n & 0x3ff) << 6)));
        /* the rest after the original in its chain (object_list_insert) */
        uw_motion_object_list_insert(m, (uint16_t)(obj + 4), rest);
    }
    if (path == 1) {
        /* action_pickup's own check comes first: a piece too heavy to carry
         * is merged back into the rest and the rest unlinked again */
        if (!(uw_motion_inventory_can_carry(m, obj) & 0xff)) {
            if (rest != obj) {
                uint16_t total = (uint16_t)(((rw(ls, (uint16_t)(obj + 6)) >> 6) + (rw(ls, (uint16_t)(rest + 6)) >> 6)) & 0x3ff);
                ww(ls, (uint16_t)(obj + 6), (uint16_t)((rw(ls, (uint16_t)(obj + 6)) & 0x3f) | (total << 6)));
                uw_motion_object_list_remove(m, (uint16_t)(obj + 4), rest);
            }
            print_message(m, 0x5f);
            return;
        }
        action_pickup_take(m, obj, rw(ds, (uint16_t)(MOBILE_BASE + 2)));
    } else if (path == 2) {
        inventory_drag_take(m, slot, obj, rest != obj, 1);
    } else if (path == 3) {
        barter_lift_take(m, obj, rest, 1);
    }
}

/* ==== the object list's own consistency check ==== */

/* objcheck_tile_chain and objcheck_chain: one chain
 * walked from its first object (the one a tile's link names, or the held
 * one), every object's index counted in the caller's 1,024-byte array,
 * and a container descended into -- the contents' verdict replacing the
 * chain's so far, as both originals store it. A second sighting of an index is a fault
 * -- a loop in a chain, or the same object linked from two places -- which
 * the original prints through the debug printer, giving up on that chain
 * after ten; the port keeps the count and the same limit. The two tallies
 * are what objcheck_run's totals are checked against. */
static int objcheck_walk(uw_motion *m, uint8_t *counts, uint16_t o, int *mobiles, int *statics,
                         int depth) {
    uint8_t *ls = m->lseg;
    int bad = 0, steps = 0;
    /* the original's flog_list/count_list have no depth limit -- a loop
     * through contents ends at counts > 10 -- so this bound is only the C
     * stack's, past any nesting 0x400 objects can make */
    if (depth > 0x400) return 1;
    while (o) {
        uint16_t i = obj_index_of(m, o);
        if (i >= 0x400) return 1;
        if (counts[i] < 0xff) counts[i]++;
        if (counts[i] > 1) {
            bad = 1;
            if (counts[i] > 10) return 1;
        }
        if (!(rw(ls, o) & 0x8000) && ((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff))
            bad = objcheck_walk(m, counts, deref_link(m, (uint16_t)(o + 6)), mobiles, statics, depth + 1);
        if (i < 0x100) (*mobiles)++;           /* obj_is_mobile: an index below 0x100 */
        else (*statics)++;
        if (++steps > 0x400) return 1;
        o = deref_link(m, (uint16_t)(o + 4));
    }
    return bad;
}

/* objcheck_run: a counter per object index -- both free lists
 * marked first, so an index already marked there is a free list with a
 * duplicate in it, then every tile's chain and what the cursor holds --
 * the held object, the chain after it and its contents. Each
 * index should end up counted exactly once, an object being either free or
 * placed and never both or neither, and the two tallies should come to the
 * pools' own sizes: 0xff mobiles and 0x300 statics. True when the level is
 * sound. */
int objcheck_run(uw_motion *m) {
    uint8_t *ds = m->ds, counts[0x400];
    uint16_t sp, tile = rw(ds, TILEMAP_PTR);
    int bad = 0, free_mobile, free_static = 0, mobiles = 0, statics = 0, x, y;
    memset(counts, 0, sizeof counts);
    free_mobile = rs(ds, TRACKED_TILE) < 0 ? 1 : 0;
    for (sp = rw(ds, 0x2756); sp >= UW_POOL_MOBILE_FLOOR; sp = (uint16_t)(sp - 2)) {
        uint16_t i = rw(m->lseg, sp);
        if (i >= 0x400) { bad = 1; break; }
        if (counts[i]) bad = 1;
        counts[i]++;
        free_mobile++;
    }
    for (sp = rw(ds, 0x274a); sp >= UW_POOL_STATIC_FLOOR; sp = (uint16_t)(sp - 2)) {
        uint16_t i = rw(m->lseg, sp);
        if (i >= 0x400) { bad = 1; break; }
        if (counts[i]) bad = 1;
        counts[i]++;
        free_static++;
    }
    for (y = 0; y < 0x40; y++) {
        for (x = 0; x < 0x40; x++, tile = (uint16_t)(tile + 4))
            if ((rw(m->lseg, (uint16_t)(tile + 2)) >> 6) & 0x3ff)
                bad |= objcheck_walk(m, counts, deref_link(m, (uint16_t)(tile + 2)), &mobiles, &statics, 0);
    }
    if ((rw(ds, ACTION_STATE_WORD) == 1 || rw(ds, ACTION_STATE_WORD) == 0)
        && (rw(ds, 0x5b06) || rw(ds, 0x5b08)))
        bad |= objcheck_walk(m, counts, rw(ds, 0x5b06), &mobiles, &statics, 0);
    if (free_mobile + mobiles != 0xff) bad = 1;
    if (free_static + statics != 0x300) bad = 1;
    return !bad;
}

/* objcheck_quiet: the check each tick asks for, which reports a failure
 * once and says no more until a run comes back clean --
 * report_internal_error's four prints to the message scroll, "Underworld
 * internal error\n ", "Problems in", " " and "object list". */
int uw_motion_objcheck(uw_motion *m) { return objcheck_run(m); }

void objcheck_quiet(uw_motion *m) {
    if (objcheck_run(m)) {
        m->ds[OBJCHECK_REPORTED] = 0;
        return;
    }
    if (!m->ds[OBJCHECK_REPORTED]) {
        if (m->scroll) {
            uw_scroll_print(m->scroll, "Underworld internal error\n ");
            uw_scroll_print(m->scroll, "Problems in");
            uw_scroll_print(m->scroll, " ");
            uw_scroll_print(m->scroll, "object list");
        } else UW_NOT_CARRIED(m->not_carried);
        m->ds[OBJCHECK_REPORTED] = 1;
    }
}
