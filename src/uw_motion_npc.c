/* SPDX-License-Identifier: MIT */
/* the creatures: creature_behaviour_dispatch and its goals, npc_goto,
 * the magic and missile attacks, and the round-robin
 * update_mobile_objects runs each frame (creature_tick,
 * mobile_object_update).
 *
 * One of src/uw_motion*.c: see uw_motion.h and uw_motion_int.h. */
#include "uw_motion_int.h"

/* ---- the behaviour: creature_behaviour_dispatch and its goals ---------- */

uint16_t npc(uw_motion *m) { return rw(m->ds, CURRENT_NPC); }
uint8_t nb(uw_motion *m, int off) { return m->lseg[(uint16_t)(npc(m) + off)]; }
void nbset(uw_motion *m, int off, uint8_t v) { m->lseg[(uint16_t)(npc(m) + off)] = v; }
uint16_t nw(uw_motion *m, int off) { return rw(m->lseg, (uint16_t)(npc(m) + off)); }
void nwset(uw_motion *m, int off, uint16_t v) { ww(m->lseg, (uint16_t)(npc(m) + off), v); }
uint8_t crit(uw_motion *m, int off) { return m->ds[(uint16_t)(rw(m->ds, AI_SELF_CRITTER) + off)]; }

/* The two-bit counter in +0x0b bits 12..13, stepped mod 4. */
static void counter_step4(uw_motion *m) {
    uint16_t w = nw(m, 0xb);
    nwset(m, 0xb, (uint16_t)((w & 0xfff) | ((((w >> 12) + 1) & 3) << 12)));
}

/* A heading byte written into the object: +9, the octant in word 1 bits
 * 7..9 and the fine part in +0x18 bits 0..4. */
static void set_heading(uw_motion *m, uint8_t h) {
    nbset(m, 9, h);
    nwset(m, 2, (uint16_t)((nw(m, 2) & 0xfc7f) | (((h >> 5) & 7) << 7)));
    nbset(m, 0x18, (uint8_t)((nb(m, 0x18) & 0xe0) | (h & 0x1f)));
}

/* creature_set_goal: the old goal nibble, when it is 4, first saved into +0x0d's
 * low nibble. */
void creature_set_goal(uw_motion *m, int goal, int gtarg) {
    if ((nb(m, 0xb) & 0xf) == 4)
        nwset(m, 0xd, (uint16_t)((nw(m, 0xd) & 0xfff0) | (nw(m, 0xb) & 0xf)));
    nwset(m, 0xb, (uint16_t)((nw(m, 0xb) & 0xfff0) | (goal & 0xf)));
    nwset(m, 0xb, (uint16_t)((nw(m, 0xb) & 0xf00f) | ((gtarg & 0xff) << 4)));
}

/* vector_to_heading: the octant of a byte vector. */
uint8_t vector_to_heading(int8_t dx, int8_t dy) {
    int8_t cl = (int8_t)(uint8_t)(dx << 1), bl = (int8_t)(uint8_t)(dy << 1);
    if (dy > cl) {
        if (dx > -bl) return (uint8_t)(dy > -cl ? 0 : 7);
        return (uint8_t)(dx > bl ? 5 : 6);
    }
    if (dx > -bl) return (uint8_t)(dx > bl ? 2 : 1);
    return (uint8_t)(dy > -cl ? 3 : 4);
}

/* creature_distance_to_target: the AI context's target fields
 * from gtarg; 0 when the target's +8 (hit points) is zero. */
static int distance_to_target(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t idx = (uint16_t)((nw(m, 0xb) & 0xff0) >> 4), t, w16, w2;
    if (idx == 0) {
        UW_NOT_CARRIED(m->not_carried);
        return 0;
    }
    t = obj_at(m, idx);
    ww(ds, AI_TARGET_PTR, t);
    ww(ds, (uint16_t)(AI_TARGET_PTR + 2), rw(ds, (uint16_t)((idx < 0x100 ? MOBILE_BASE : STATIC_BASE) + 2)));
    if (ls[(uint16_t)(t + 8)] == 0) return 0;
    w16 = rw(ls, (uint16_t)(t + 0x16));
    w2 = rw(ls, (uint16_t)(t + 2));
    ds[AI_TARGET_TILE_X] = (uint8_t)(w16 >> 10);
    ds[AI_TARGET_TILE_Y] = (uint8_t)((w16 & 0x3f0) >> 4);
    ds[AI_TARGET_Z] = (uint8_t)((w2 & 0x7f) >> 3);
    ww(ds, AI_TARGET_FINE_X, (uint16_t)((ds[AI_TARGET_TILE_X] << 3) + ((w2 & 0xe000) >> 13)));
    ww(ds, AI_TARGET_FINE_Y, (uint16_t)((ds[AI_TARGET_TILE_Y] << 3) + ((w2 & 0x1c00) >> 10)));
    ww(ds, AI_TARGET_DX, (uint16_t)(rw(ds, AI_TARGET_FINE_X) - rw(ds, AI_SELF_FINE_X)));
    ww(ds, AI_TARGET_DY, (uint16_t)(rw(ds, AI_TARGET_FINE_Y) - rw(ds, AI_SELF_FINE_Y)));
    {
        uint16_t ax = (uint16_t)(ds[AI_TARGET_TILE_X] - ds[AI_SELF_TILE_X]);
        uint16_t ay = (uint16_t)(ds[AI_TARGET_TILE_Y] - ds[AI_SELF_TILE_Y]);
        ww(ds, AI_TARGET_TILE_DIST2, (uint16_t)(ax * ax + ay * ay));
        ax = rw(ds, AI_TARGET_DX);
        ay = rw(ds, AI_TARGET_DY);
        ww(ds, AI_TARGET_FINE_DIST2, (uint16_t)(ax * ax + ay * ay));
        ww(ds, (uint16_t)(AI_TARGET_FINE_DIST2 + 2), 0);
    }
    return 1;
}

/* creature_turn_toward: the heading of (dx, dy) by gfx_atan2 over
 * the fine distance to the target -- dy's fraction the sine, dx's the cosine,
 * so 0 is +x and the heading is 0x40 less -- and a turn of exactly an eighth toward it;
 * 1 when already within an eighth. The distance is the root of
 * ai_target_dx/dy squared -- a 16-bit sum sign-extended -- and each axis is
 * put over it as a 1.15 fraction, +-1.0 written directly. */
static int creature_turn_toward(uw_motion *m, int8_t dx, int8_t dy) {
    uint8_t *ds = m->ds;
    uint8_t cur = (uint8_t)((((nw(m, 2) & 0x380) >> 7) << 5) + (nb(m, 0x18) & 0x1f)), want, h;
    int16_t sum = (int16_t)(rs(ds, AI_TARGET_DX) * rs(ds, AI_TARGET_DX) + rs(ds, AI_TARGET_DY) * rs(ds, AI_TARGET_DY));
    uint16_t dist = uw_isqrt32((uint32_t)(int32_t)sum);
    int16_t fx, fy;
    int turned = 0;
    if (dist == 0) return 1;
    if (dx == dist) fx = 0x7fff;
    else if (-dx == dist) fx = (int16_t)0x8000;
    else fx = (int16_t)((int32_t)dx * 0x8000 / (int32_t)dist);    /* rt_lshl 15, rt_ldiv */
    if (dy == dist) fy = 0x7fff;
    else if (-dy == dist) fy = (int16_t)0x8000;
    else fy = (int16_t)((int32_t)dy * 0x8000 / (int32_t)dist);
    want = (uint8_t)(0x40 - ((uint16_t)uw_atan2(fy, fx) >> 8));
    if ((uint8_t)(want - cur) < 0x20 || (uint8_t)(want - cur) > 0xe0) {
        h = want;
        turned = 1;
    } else {
        h = (uint8_t)(cur + ((uint8_t)(want - cur) < 0x80 ? 0x20 : 0xe0));
    }
    nwset(m, 2, (uint16_t)((nw(m, 2) & 0xfc7f) | (((h >> 5) & 7) << 7)));
    nbset(m, 0x18, (uint8_t)((nb(m, 0x18) & 0xe0) | (h & 0x1f)));
    return turned;
}

/* creature_turn_to_target: with `fine` set, creature_turn_toward
 * the target's fine offset; without, the octant of it and a step of the
 * three heading bits in word 1 toward that, 1 when they already agree. */
static int turn_to_target(uw_motion *m, int fine) {
    uint8_t *ds = m->ds;
    int8_t dx = (int8_t)(ds[AI_TARGET_FINE_X] - ds[AI_SELF_FINE_X]);
    int8_t dy = (int8_t)(ds[AI_TARGET_FINE_Y] - ds[AI_SELF_FINE_Y]);
    int8_t oct = (int8_t)vector_to_heading(dx, dy);
    int8_t cur = (int8_t)((nw(m, 2) & 0x380) >> 7);
    int16_t diff = (int16_t)((oct - cur + 8) % 8);
    if (fine) return creature_turn_toward(m, dx, dy);
    if (diff == 0) return 1;
    nwset(m, 2, (uint16_t)((nw(m, 2) & 0xfc7f)
                           | ((((nw(m, 2) & 0x380) >> 7) + (diff <= 4 ? 1 : -1)) & 7) << 7));
    return 0;
}

/* creature_pitch_to_target: the vertical aim for projectile_aim_z
 * -- the rise over the fine distance, times four, clamped to +-15; a lobbed
 * shot with a speed adds distance * 3 / speed. */
static int8_t pitch_to_target(uw_motion *m, uint16_t speed, int lob) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    int16_t rise, p;
    uint16_t dist;
    distance_to_target(m);
    rise = (int16_t)((rw(ls, (uint16_t)(rw(ds, AI_TARGET_PTR) + 2)) & 0x7f) - (nw(m, 2) & 0x7f));
    dist = uw_isqrt32((uint32_t)rw(ds, AI_TARGET_FINE_DIST2) | ((uint32_t)rw(ds, (uint16_t)(AI_TARGET_FINE_DIST2 + 2)) << 16));
    if (dist == 0) return (int8_t)(rise > 0 ? 15 : -15);
    p = (int16_t)((int16_t)(uint16_t)(rise << 2) / (int16_t)dist);
    if (p > 15) p = 15;
    if (p < -15) p = -15;
    if (lob && speed) p = (int16_t)(p + (uint16_t)(dist * 3) / speed);
    return (int8_t)p;
}

/* creature_vector_to_player: a heading steered away from the
 * player when the player is within `range` eighths... of a tile, squared
 * against the fine distance. */
static uint8_t vector_to_player(uw_motion *m, uint8_t h, int16_t range) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t pl = obj_at(m, 1);
    uint16_t w16 = rw(ls, (uint16_t)(pl + 0x16)), w2 = rw(ls, (uint16_t)(pl + 2));
    int16_t dx = (int16_t)((((w16 >> 10) << 3) + ((w2 & 0xe000) >> 13)) - rw(ds, AI_SELF_FINE_X));
    int16_t dy = (int16_t)(((((w16 & 0x3f0) >> 4) << 3) + ((w2 & 0x1c00) >> 10)) - rw(ds, AI_SELF_FINE_Y));
    uint16_t d2 = (uint16_t)(dx * dx + dy * dy);
    uint8_t away, diff;
    if (!((uint16_t)(range * range) > d2)) return h;
    away = (uint8_t)(((vector_to_heading((int8_t)dx, (int8_t)dy) + 4) % 8) << 5);
    diff = (uint8_t)((away + 0x100 - h) % 0x100);
    if (diff < 0x40 || diff > 0xc0) return h;
    if (diff < 0x60) return (uint8_t)((away + 0xe0) % 0x100);
    if (diff < 0x80) return (uint8_t)((h + 0x20) % 0x100);
    if (diff > 0xa0) return (uint8_t)((away + 0x20) % 0x100);
    return (uint8_t)((h + 0xe0) % 0x100);
}

/* creature_react_to_player: a creature standing still, or any
 * creature while player_record+0x5f bit 1 is set, that finds the player
 * within 0x90 fine units squared stops, idles and turns to face them. */
static void react_to_player(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t d2;
    uint8_t h;
    if ((nb(m, 0x13) & 0x7f) && !((ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x5f)] >> 1) & 1)) return;
    nwset(m, 0xb, (uint16_t)((nw(m, 0xb) & 0xf00f) | 0x10));
    distance_to_target(m);
    d2 = (uint16_t)(rs(ds, AI_TARGET_DX) * rs(ds, AI_TARGET_DX) + rs(ds, AI_TARGET_DY) * rs(ds, AI_TARGET_DY));
    if (d2 >= 0x90) return;
    h = vector_to_heading((int8_t)ds[AI_TARGET_DX], (int8_t)ds[AI_TARGET_DY]);
    nbset(m, 0x13, (uint8_t)(nb(m, 0x13) & 0x80));
    nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 0x20));
    nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 6));
    if (rt_rand(m) % 2) counter_step4(m);
    nwset(m, 2, (uint16_t)((nw(m, 2) & 0xfc7f) | ((h & 7) << 7)));
    nbset(m, 0x18, (uint8_t)(nb(m, 0x18) & 0xe0));
}

static void goal_stand_still(uw_motion *m);
static void npc_goto(uw_motion *m, uint8_t x, uint8_t y, uint8_t z);
static void npc_try_open_door(uw_motion *m, uint16_t door);

/* creature_goal_wander, goal 2 and the peaceful creature near
 * home: the walk/idle toggle, a turn when blocked, the wobble, a step away
 * from the player. */
static void goal_wander(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t tp;
    uint8_t p = (uint8_t)(crit(m, 0x1f) & 0xf), h, r;
    if (nb(m, 0x15) & 0x80) {
        ww(ds, NPC_PATH_SLOT_MASK, (uint16_t)(rw(ds, NPC_PATH_SLOT_MASK) | (1 << (nb(m, 0x16) & 0xf))));
        nbset(m, 0x15, (uint8_t)(nb(m, 0x15) & 0x7f));
    }
    tp = tile_ptr(m, ds[AI_SELF_TILE_X], ds[AI_SELF_TILE_Y]);
    if (!ds[AI_MAY_MOVE]) {
        nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 1));
        return;
    }
    if ((nw(m, 0xd) >> 14) == 0 && rt_rand(m) % 2 != 0) {
        goal_stand_still(m);
        return;
    }
    if (crit(m, 0xa) & 0x80) {              /* a flier's height */
        uint8_t v;
        if (ds[AI_SELF_Z] > 0xe)
            v = (uint8_t)(rt_rand(m) % 3 + 0xe);
        else if (ds[AI_SELF_Z] < ((m->lseg[tp] >> 4) & 0xf) + 2)
            v = (uint8_t)(rt_rand(m) % 3 + 0x10);
        else
            v = (uint8_t)(rt_rand(m) % 5 + 0xe);
        nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 7) | ((v & 0x1f) << 3)));
    }
    if ((nb(m, 0x15) & 0x3f) == 0x20) {
        r = (uint8_t)(rt_rand(m) % 16);
        if (p > r && (nw(m, 0xb) >> 12) == 3)
            nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 0x2c));
    } else {
        r = (uint8_t)(rt_rand(m) % 16);
        if (p < r && (nw(m, 0xb) >> 12) == 3)
            nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 0x20));
        else
            nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 0x2c));
    }
    if ((nb(m, 0x15) & 0x3f) == 0x2c) {
        if (ds[AI_MOTION_BLOCKED] && !ds[AI_HEADING_CHANGED]) {
            int16_t d = (int16_t)(((rt_rand(m) % 2) * 2 - 1) << 6);
            h = (uint8_t)((nb(m, 9) + d + 0x100) % 0x100);
            set_heading(m, h);
            nbset(m, 0x13, (uint8_t)(nb(m, 0x13) & 0x80));
            return;
        }
        r = (uint8_t)(rt_rand(m) % 64);
        if (r < p + 8)
            h = (uint8_t)((nb(m, 9) + rt_rand(m) % 64 + 0xe0) % 0x100);
        else
            h = nb(m, 9);
        if (!ds[AI_HEADING_CHANGED]) h = vector_to_player(m, h, 10);
        set_heading(m, h);
    } else {
        r = (uint8_t)(rt_rand(m) % 128);
        if (p > r) set_heading(m, (uint8_t)((nb(m, 9) + rt_rand(m) % 64 + 0xe0) % 0x100));
    }
    if ((nb(m, 0x15) & 0x3f) == 0x20) {
        nbset(m, 0x15, (uint8_t)(nb(m, 0x15) | 0x40));
        nbset(m, 0x13, (uint8_t)(nb(m, 0x13) & 0x80));
        nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 6));
        if (rt_rand(m) % 2 != 0) counter_step4(m);
    } else {
        nbset(m, 0x15, (uint8_t)(nb(m, 0x15) & 0xbf));
        nbset(m, 0x13, (uint8_t)((nb(m, 0x13) & 0x80) | (crit(m, 0xb) & 0x7f)));
        nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 4));
        counter_step4(m);
    }
    react_to_player(m);
}

/* creature_goal_keep_near_home, goal 8. */
static void goal_keep_near_home(uw_motion *m) {
    uint8_t *ds = m->ds;
    int8_t dx, dy;
    int16_t rr;
    if (!ds[AI_MAY_MOVE]) return;
    if ((nw(m, 0xd) >> 14) == 0 && (nb(m, 0xb) & 0xf) != 4) {
        creature_set_goal(m, 4, 1);
        return;
    }
    dx = (int8_t)(ds[AI_SELF_HOME_X] - ds[AI_SELF_TILE_X]);
    dy = (int8_t)(ds[AI_SELF_HOME_Y] - ds[AI_SELF_TILE_Y]);
    rr = (int16_t)(((crit(m, 0x1c) >> 4) & 0xf) * ((crit(m, 0x1c) >> 4) & 0xf));
    if ((int16_t)(dx * dx + dy * dy) > rr)
        npc_goto(m, ds[AI_SELF_HOME_X], ds[AI_SELF_HOME_Y],
                 (uint8_t)((m->lseg[tile_ptr(m, ds[AI_SELF_HOME_X], ds[AI_SELF_HOME_Y])] >> 4) & 0xf));
    else
        goal_wander(m);
}

static void npc_set_destination(uw_motion *m, uint8_t x, uint8_t y, uint8_t z);

/* creature_search_for_target, from the instructions: 0 when the
 * target is known -- within a quarter of hearing, or within sight, facing it
 * (0, 1 or 7 octants off) and with a clear line from eye to eye, which sets
 * +0x19 bit 0 -- 2 when within four times hearing, else 1 with bit 0
 * cleared. Hearing and sight are the target's stealth nibbles (critter row
 * +0x1d) times this critter's awareness nibbles (+0x1e) over 16, squared,
 * against the squared tile distance. The target's tile goes to *x, *y. */
static int search_for_target(uw_motion *m, uint8_t *x, uint8_t *y) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t t = rw(ds, AI_TARGET_PTR), trow = (uint16_t)(0x4a52 + (rw(ls, t) & 0x3f) * 0x30);
    int8_t dx = (int8_t)(ds[AI_TARGET_TILE_X] - ds[AI_SELF_TILE_X]);
    int8_t dy = (int8_t)(ds[AI_TARGET_TILE_Y] - ds[AI_SELF_TILE_Y]);
    int16_t si = (int16_t)(dx * dx + dy * dy), h, hear, sight;
    *x = ds[AI_TARGET_TILE_X];
    *y = ds[AI_TARGET_TILE_Y];
    h = (int16_t)(((ds[(uint16_t)(trow + 0x1d)] & 0xf) * (crit(m, 0x1e) & 0xf)) / 16);
    hear = (int16_t)(h * h);
    if (hear / 4 > si) return 0;
    h = (int16_t)((((ds[(uint16_t)(trow + 0x1d)] >> 4) & 0xf) * ((crit(m, 0x1e) >> 4) & 0xf)) / 16);
    sight = (int16_t)(h * h);
    if (si <= sight) {
        int8_t d = (int8_t)(((int8_t)vector_to_heading(dx, dy) - (int8_t)((nw(m, 2) & 0x380) >> 7) + 8) % 8);
        uint16_t self = npc(m);
        if ((d == 0 || d == 1 || d == 7)
            && test_between_points(m, rs(ds, AI_SELF_FINE_X), rs(ds, AI_SELF_FINE_Y),
                                   (int16_t)((rw(ls, (uint16_t)(self + 2)) & 0x7f) + prop(m, obj_id(m, self), 0)),
                                   rs(ds, AI_TARGET_FINE_X), rs(ds, AI_TARGET_FINE_Y),
                                   (int16_t)((rw(ls, (uint16_t)(t + 2)) & 0x7f) + prop(m, obj_id(m, t), 0)))) {
            nbset(m, 0x19, (uint8_t)(nb(m, 0x19) | 1));
            return 0;
        }
    }
    if ((int16_t)(hear << 2) > si) return 2;
    nbset(m, 0x19, (uint8_t)(nb(m, 0x19) & 0xfe));
    return 1;
}

/* creature_goal_stand_still, goals 0, 4 and 7: a hostile
 * creature's look-out for the player, then by the goal nibble. */
static void goal_stand_still(uw_motion *m) {
    uint8_t *ds = m->ds;
    int16_t c;
    if (!ds[AI_MAY_MOVE]) return;
    if ((nw(m, 0xd) >> 14) == 0) {
        nwset(m, 0xb, (uint16_t)((nw(m, 0xb) & 0xf00f) | 0x10));
        distance_to_target(m);
        if (nb(m, 0x19) & 1) {
            creature_set_goal(m, 5, 1);
            return;
        }
        c = (int16_t)((crit(m, 0x1f) >> 4) & 0xf);
        if (nb(m, 0x19) & 2) {
            if ((int16_t)(rt_rand(m) % 16) > c)
                nbset(m, 0x19, (uint8_t)(nb(m, 0x19) & 0xfd));
            else
                turn_to_target(m, 0);
        }
        if ((int16_t)(rt_rand(m) % 16) < c) {
            uint8_t x, y;
            int r = search_for_target(m, &x, &y);
            if (r == 0) {                   /* known: go for it */
                nbset(m, 0x19, (uint8_t)(nb(m, 0x19) | 1));
                npc_set_destination(m, x, y, ds[AI_TARGET_Z]);
                creature_set_goal(m, 5, 1);
                return;
            }
            if (r == 2) {                   /* heard: half the time, go and look */
                nbset(m, 0x19, (uint8_t)((nb(m, 0x19) & 0xfd) | 2));
                if (rt_rand(m) % 2 == 0) {
                    npc_goto(m, x, y, ds[AI_TARGET_Z]);
                    return;
                }
            }
        }
    }
    switch (nb(m, 0xb) & 0xf) {
    case 0: case 7:
        nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 6));
        nbset(m, 0x13, (uint8_t)(nb(m, 0x13) & 0x80));
        nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 0x20));
        if (rt_rand(m) % 2 != 0) counter_step4(m);
        break;
    case 2:
        goal_wander(m);
        break;
    default:
        goal_keep_near_home(m);
        break;
    }
}

/* npc_set_destination: x into +0x0f bits 0..5, y into 6..11,
 * z into +0x0d bits 4..7; a change sets +0x18 bit 5 (re-path) and clears
 * bit 6 (given up). */
static void npc_set_destination(uw_motion *m, uint8_t x, uint8_t y, uint8_t z) {
    if ((nw(m, 0xf) & 0x3f) == x && ((nw(m, 0xf) & 0xfc0) >> 6) == y
        && ((nw(m, 0xd) & 0xf0) >> 4) == z)
        return;
    nwset(m, 0xf, (uint16_t)((nw(m, 0xf) & 0xffc0) | (x & 0x3f)));
    nwset(m, 0xf, (uint16_t)((nw(m, 0xf) & 0xf03f) | ((y & 0x3f) << 6)));
    nwset(m, 0xd, (uint16_t)((nw(m, 0xd) & 0xff0f) | ((z & 0xf) << 4)));
    nbset(m, 0x18, (uint8_t)(nb(m, 0x18) | 0x20));
    nbset(m, 0x18, (uint8_t)(nb(m, 0x18) & 0xbf));
}

/* creature_reset_goal: back to the stashed goal with gtarg 1,
 * or wander with gtarg 0. */
static void creature_reset_goal(uw_motion *m) {
    if ((nw(m, 0xd) & 0xf) == 0) {
        nwset(m, 0xb, (uint16_t)((nw(m, 0xb) & 0xfff0) | 2));
        nwset(m, 0xb, (uint16_t)(nw(m, 0xb) & 0xf00f));
    } else {
        nwset(m, 0xb, (uint16_t)((nw(m, 0xb) & 0xfff0) | (nw(m, 0xd) & 0xf)));
        nwset(m, 0xb, (uint16_t)((nw(m, 0xb) & 0xf00f) | 0x10));
        nwset(m, 0xd, (uint16_t)(nw(m, 0xd) & 0xfff0));
    }
}

static int8_t absz(int v) { return (int8_t)(v < 0 ? -v : v); }

/* creature_attack_search_target: the pursuit step. */
static void attack_search_target(uw_motion *m, uint8_t x, uint8_t y, uint8_t range) {
    uint8_t *ds = m->ds;
    if (!((nw(m, 0xf) & 0x3f) == x && ((nw(m, 0xf) & 0xfc0) >> 6) == y) && rt_rand(m) % 8 == 0) {
        /* one time in eight, look again: lost (1) gives up the chase, heard
         * (2) gives it up half the time with +0x19 bit 1 set, and seen (0)
         * or the other half heads for where the target is */
        int r = search_for_target(m, &x, &y);
        if (r == 1 || (r == 2 && rt_rand(m) % 2 == 0)) {
            nbset(m, 0x19, (uint8_t)(nb(m, 0x19) & 0xfe));
            nbset(m, 0x19, (uint8_t)((nb(m, 0x19) & 0xfd) | (r == 2 ? 2 : 0)));
            creature_reset_goal(m);
            return;
        }
        if (r == 0 || r == 2)
            npc_set_destination(m, x, y, ds[AI_TARGET_Z]);
    }
    if (x == ds[AI_SELF_TILE_X] && y == ds[AI_SELF_TILE_Y]
        && absz((int8_t)ds[AI_TARGET_Z] - (int8_t)ds[AI_SELF_Z]) < 4)
        return;
    if (!(range > 1 && (uint16_t)(range * range) < rw(ds, AI_TARGET_TILE_DIST2))) {
        int32_t r64 = (int16_t)(uint16_t)((range * range) << 6);
        uint32_t fd2 = (uint32_t)rw(ds, AI_TARGET_FINE_DIST2) | ((uint32_t)rw(ds, AI_TARGET_FINE_DIST2 + 2) << 16);
        if (!((uint32_t)r64 < fd2)) {
            if (range > 1) return;
            if (absz((int8_t)ds[AI_TARGET_Z] - (int8_t)ds[AI_SELF_Z]) < 4) return;
        }
    }
    npc_goto(m, x, y, ds[AI_TARGET_Z]);
    if (nb(m, 0x18) & 0x40) {               /* the path gave up: the chase with it */
        creature_reset_goal(m);
        nbset(m, 0x19, (uint8_t)(nb(m, 0x19) & 0xfd));
    }
}

/* creature_choose_manoeuvre: face the target, then by distance
 * retreat, sidestep, close in, feint or stand; a flier's attitude; and one
 * time in four within 0x64 the blow, chosen over the critter's three
 * attack chances. */
static void choose_manoeuvre(uw_motion *m, uint16_t di) {
    uint8_t *ds = m->ds;
    uint8_t h = vector_to_heading((int8_t)ds[AI_TARGET_DX], (int8_t)ds[AI_TARGET_DY]);
    nwset(m, 2, (uint16_t)((nw(m, 2) & 0xfc7f) | ((h & 7) << 7)));
    nbset(m, 0x18, (uint8_t)(nb(m, 0x18) & 0xe0));
    nbset(m, 9, (uint8_t)(h << 5));
    nbset(m, 0x15, (uint8_t)(nb(m, 0x15) & 0xbf));
    if (di < 0x31) {
        if (rt_rand(m) % 4 == 0) {          /* sidestep */
            int16_t d = (int16_t)(((rt_rand(m) % 2) * 2 - 1) * 2);
            h = (uint8_t)((h + d + 8) % 8);
            nbset(m, 0x15, (uint8_t)(nb(m, 0x15) & 0xc0));
            nbset(m, 9, (uint8_t)(h << 5));
            nbset(m, 0x13, (uint8_t)((nb(m, 0x13) & 0x80) | ((crit(m, 0xb) * 2 / 3) & 0x7f)));
        } else {                            /* back off */
            h = (uint8_t)((h + 4) % 8);
            nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 7));
            nbset(m, 9, (uint8_t)(h << 5));
            nbset(m, 0x13, (uint8_t)((nb(m, 0x13) & 0x80) | 2));
        }
    } else if (di > 0x51) {
        nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 0x2c));
        nbset(m, 9, (uint8_t)(h << 5));
        nbset(m, 0x13, (uint8_t)((nb(m, 0x13) & 0x80) | 2));
    } else if ((int16_t)(rt_rand(m) % 64) < crit(m, 6)) {
        h = (uint8_t)(rt_rand(m) % 8);
        nbset(m, 0x15, (uint8_t)(nb(m, 0x15) & 0xc0));
        nbset(m, 9, (uint8_t)(h << 5));
        nbset(m, 0x13, (uint8_t)((nb(m, 0x13) & 0x80) | 1));
    } else {
        nbset(m, 0x15, (uint8_t)(nb(m, 0x15) & 0xc0));
        nbset(m, 9, (uint8_t)(h << 5));
        nbset(m, 0x13, (uint8_t)(nb(m, 0x13) & 0x80));
    }
    if (crit(m, 0xa) & 0x80) {
        uint16_t t = rw(ds, AI_TARGET_PTR);
        int8_t dz = (int8_t)(uint8_t)(((m->lseg[(uint16_t)(t + 2)] & 0x7f) + 0xe) - (nb(m, 2) & 0x7f));
        uint8_t v;
        if (dz > 1) v = 0x90;
        else if (dz < -1) v = 0x70;
        else v = (uint8_t)(((rt_rand(m) % 3 + 0xf) & 0x1f) << 3);
        nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 7) | v));
    }
    if (di <= 0x64) {
        if (rt_rand(m) % 4 == 0) {
            int16_t si = (int16_t)(rt_rand(m) % 100);
            int k = 0;
            while (!(crit(m, 0x15 + k * 3) > si) && k < 2) {
                si = (int16_t)(si - crit(m, 0x15 + k * 3));
                k++;
            }
            nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | ((k + 1) & 0x3f)));
            nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 4));
            nwset(m, 0xb, (uint16_t)(nw(m, 0xb) & 0xfff));
            return;
        }
        if ((nw(m, 0xf) >> 12) < 0xf)
            nwset(m, 0xf, (uint16_t)((nw(m, 0xf) & 0xfff) | ((((nw(m, 0xf) >> 12) + 1) & 0xf) << 12)));
    }
    nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 4));
    counter_step4(m);
}

/* npc_goto, from the instructions. The paths through the pool
 * -- path_step, turn_towards_path, the slot, pathfind_between_tiles and
 * path_pack -- and the door and flier helpers are counted. */
static void npc_goto(uw_motion *m, uint8_t x, uint8_t y, uint8_t z) {
    uint8_t *ds = m->ds;
    int gave = 0, dive = 0;
    int8_t dx, dy;
    uint8_t h;
    npc_set_destination(m, x, y, z);
    if ((nb(m, 0x18) & 0x20) && (nb(m, 0x15) & 0x80)) release_path_slot(m);
    dx = (int8_t)(uint8_t)(x - ds[AI_SELF_TILE_X]);
    dy = (int8_t)(uint8_t)(y - ds[AI_SELF_TILE_Y]);
    if (dx == 0 && dy == 0) {
        if (nb(m, 0x15) & 0x80) release_path_slot(m);
        if ((nb(m, 0xb) & 0xf) == 1) {
            creature_set_goal(m, 8, 0);
        } else if (ds[AI_MAY_MOVE]) {
            nbset(m, 0x13, (uint8_t)(nb(m, 0x13) & 0x80));
            nbset(m, 0x15, (uint8_t)(nb(m, 0x15) | 0x40));
            nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 0x20));
            return;
        }
    }
    if (!ds[AI_MAY_MOVE]) {
        uint16_t rec = (uint16_t)(PATH_POOL + (nw(m, 0x16) & 0xf) * PATH_RECORD);
        nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 1));
        if (!(nb(m, 0x15) & 0x80)) return;
        if (!m->ext) { UW_NOT_CARRIED(m->not_carried); return; }
        if (m->ext[rec] == (nw(m, 0x16) >> 10) && m->ext[(uint16_t)(rec + 1)] == ((nw(m, 0x16) & 0x3f0) >> 4))
            path_step(m, rec);
        return;
    }
    if (ds[AI_MOTION_BLOCKED] && !ds[AI_HEADING_CHANGED] && !(nb(m, 0x18) & 0x40)) {
        if (ds[AI_BLOCKED_ANY]) {
            if (ds[AI_WALKER_BLOCKED]) {
                nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 0x20));
                if (rt_rand(m) % 4 != 0 && (nw(m, 0xd) >> 14) == 0)
                    npc_try_open_door(m, rw(ds, AI_MOTION_BLOCKER));
                else
                    nbset(m, 0x18, (uint8_t)(nb(m, 0x18) | 0x40));
            } else {
                uint16_t b = rw(ds, AI_MOTION_BLOCKER), bid = obj_id(m, b);
                if (((bid & 0x1c0) >> 6) == 1 && bid != 0x7f && (nb(m, 0xb) & 0xf) == 5
                    && (m->lseg[(uint16_t)(b + 0xb)] & 0xf) == 5) {
                    /* two attackers in each other's way: no give-up */
                } else if ((bid >> 4) == 0x14 && (bid & 0xf) >= 8 && (crit(m, 0xa) & 0x80)) {
                    nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 7) | 0x70));
                    ds[AI_MOTION_BLOCKED] = 0;
                    ds[AI_GOTO_ACTIVE] = 1;
                    dive = 1;
                } else {
                    nbset(m, 0x18, (uint8_t)(nb(m, 0x18) | 0x40));
                }
            }
        }
        if (ds[AI_MOTION_BLOCKED]) {
            if (nb(m, 0x15) & 0x80) release_path_slot(m);
            nbset(m, 0x18, (uint8_t)(nb(m, 0x18) & 0x7f));
            gave = 1;
        }
    }
    if (nb(m, 0x15) & 0x80) {
        if (!m->ext) UW_NOT_CARRIED(m->not_carried);
        else if (!turn_towards_path(m, (uint16_t)(PATH_POOL + (nw(m, 0x16) & 0xf) * PATH_RECORD)))
            release_path_slot(m);
    } else if (!(nb(m, 0x18) & 0x20) && (nb(m, 0x18) & 0x80)) {
        h = vector_to_heading(dx, dy);
        nbset(m, 9, (uint8_t)(h << 5));
        nwset(m, 2, (uint16_t)((nw(m, 2) & 0xfc7f) | ((h & 7) << 7)));
        nbset(m, 0x18, (uint8_t)(nb(m, 0x18) & 0xe0));
        if (crit(m, 0xa) & 0x80)
            npc_pose_from_height(m, x, y);
    } else if (!(nb(m, 0x18) & 0x20) && (nb(m, 0x18) & 0x40)) {
        if (rt_rand(m) % 8 == 0) nbset(m, 0x18, (uint8_t)(nb(m, 0x18) & 0xbf));
        goal_wander(m);
        return;
    } else if (!gave && m->ext && tile_line_walk(m, ds[AI_SELF_TILE_X], ds[AI_SELF_TILE_Y], x, y) == 1) {
        nbset(m, 0x18, (uint8_t)(nb(m, 0x18) | 0x80));
        h = vector_to_heading(dx, dy);
        nbset(m, 9, (uint8_t)(h << 5));
        nwset(m, 2, (uint16_t)((nw(m, 2) & 0xfc7f) | ((h & 7) << 7)));
        nbset(m, 0x18, (uint8_t)(nb(m, 0x18) & 0xe0));
        nbset(m, 0x18, (uint8_t)(nb(m, 0x18) & 0xbf));
        if (nb(m, 0x15) & 0x80) release_path_slot(m);
    } else {
        uint8_t slot = 0;
        if (!m->ext || !m->grid) {
            UW_NOT_CARRIED(m->not_carried);
            return;
        }
        if (path_slot_alloc(m, &slot)
            && pathfind_between_tiles(m, ds[AI_SELF_TILE_X], ds[AI_SELF_TILE_Y],
                                      (uint8_t)((nw(m, 2) & 0x7f) >> 3), x, y, z, creature_acceptable_danger(m))) {
            uint16_t rec = (uint16_t)(PATH_POOL + slot * PATH_RECORD);
            ww(ds, NPC_PATH_SLOT_MASK, (uint16_t)(rw(ds, NPC_PATH_SLOT_MASK) & ~(1 << slot)));
            path_pack(m, rec);
            nbset(m, 0x18, (uint8_t)(nb(m, 0x18) & 0xbf));
            nbset(m, 0x15, (uint8_t)(nb(m, 0x15) | 0x80));
            nwset(m, 0x16, (uint16_t)((nw(m, 0x16) & 0xfff0) | (slot & 0xf)));
            turn_towards_path(m, rec);
        } else {
            nbset(m, 0x18, (uint8_t)(nb(m, 0x18) | 0x40));
            nbset(m, 0x18, (uint8_t)(nb(m, 0x18) & 0x7f));
            goal_wander(m);
            return;
        }
    }
    if (ds[AI_FACED_PATH_STEP]) return;
    nbset(m, 0x15, (uint8_t)(nb(m, 0x15) & 0xbf));
    nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 0x2c));
    if (dive)
        nbset(m, 0x13, (uint8_t)(nb(m, 0x13) & 0x80));
    else
        nbset(m, 0x13, (uint8_t)((nb(m, 0x13) & 0x80)
                                 | (crit(m, (nb(m, 0xb) & 0xf) == 5 ? 0xc : 0xb) & 0x7f)));
    counter_step4(m);
    nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 4));
}

/* tile_no_magic_bit: tile word bit 14. */
int tile_no_magic(uw_motion *m, uint16_t x, uint16_t y) {
    return (m->lseg[(uint16_t)(tile_ptr(m, x, y) + 1)] >> 6) & 1;
}

/* Tyball's wait: on level 7 (0-based) the creature of race 0x13 casts only
 * once player_record+0x60 bit 5 is set. */
static int magic_withheld(uw_motion *m) {
    uint8_t *ds = m->ds;
    return rw(ds, CURRENT_LEVEL_WORD) == 7 && !((ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x60)] >> 5) & 1)
           && crit(m, 9) == 0x13;
}

/* creature_try_magic_attack: with a third spell (+0x2c not 0xff)
 * and rand() % 256 under half of +0x2d, off a no-magic tile, stop and start
 * casting it -- action 0xd, slot 3, the counter cleared. */
static int try_magic_attack(uw_motion *m) {
    uint8_t *ds = m->ds;
    if (crit(m, 0x2c) == 0xff) return 0;
    if ((int16_t)((int16_t)rt_rand(m) % 0x100) >= ((crit(m, 0x2d) >> 1) & 0x7f)) return 0;
    if (tile_no_magic(m, ds[AI_SELF_TILE_X], ds[AI_SELF_TILE_Y]) || magic_withheld(m)) return 0;
    nbset(m, 0x13, (uint8_t)(nb(m, 0x13) & 0x80));
    nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 0xd));
    nbset(m, 0x19, (uint8_t)((nb(m, 0x19) & 0xf3) | 0xc));
    nwset(m, 0xb, (uint16_t)(nw(m, 0xb) & 0xfff));
    return 1;
}

/* creature_start_magic_attack: within eight tiles, off a
 * no-magic tile, with a clear line from eye to eye and facing the target
 * (creature_turn_to_target(1)), 1; and then with rand() % 128 under half of
 * +0x2d, the cast -- action 0xd, slot 1 eleven times in sixteen, else 2. */
static int start_magic_attack(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t t = rw(ds, AI_TARGET_PTR), self = npc(m);
    if (tile_no_magic(m, ds[AI_SELF_TILE_X], ds[AI_SELF_TILE_Y]) || magic_withheld(m)) return 0;
    if (rw(ds, AI_TARGET_TILE_DIST2) >= 0x40) return 0;
    if (tile_no_magic(m, ds[AI_SELF_TILE_X], ds[AI_SELF_TILE_Y])) return 0;
    if (!test_between_points(m, rs(ds, AI_SELF_FINE_X), rs(ds, AI_SELF_FINE_Y),
                             (int16_t)((rw(ls, (uint16_t)(self + 2)) & 0x7f) + prop(m, obj_id(m, self), 0)),
                             rs(ds, AI_TARGET_FINE_X), rs(ds, AI_TARGET_FINE_Y),
                             (int16_t)((rw(ls, (uint16_t)(t + 2)) & 0x7f) + prop(m, obj_id(m, t), 0))))
        return 0;
    if (!turn_to_target(m, 1)) return 0;
    if ((int16_t)((int16_t)rt_rand(m) % 0x80) < ((crit(m, 0x2d) >> 1) & 0x7f)) {
        nbset(m, 0x13, (uint8_t)(nb(m, 0x13) & 0x80));
        nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 0xd));
        nbset(m, 0x19, (uint8_t)((nb(m, 0x19) & 0xf3) | (((int16_t)rt_rand(m) % 0x10 < 0xb ? 1 : 2) << 2)));
        nwset(m, 0xb, (uint16_t)(nw(m, 0xb) & 0xfff));
    }
    return 1;
}

/* creature_start_missile_attack, once creature_begin_melee:
 * within four tiles (squared tile distance under 0x10), a clear line from
 * eye to eye and facing the target, 1; and then with rand() % 192 no more
 * than the critter's dexterity (+6), action 5 -- which creature_ai_update
 * fires with projectile_fire_from_object at counter 4 -- speed 0 and the
 * counter cleared. */
static int start_missile_attack(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t t = rw(ds, AI_TARGET_PTR), self = npc(m);
    if (rw(ds, AI_TARGET_TILE_DIST2) >= 0x10) return 0;
    if (!test_between_points(m, rs(ds, AI_SELF_FINE_X), rs(ds, AI_SELF_FINE_Y),
                             (int16_t)((rw(ls, (uint16_t)(self + 2)) & 0x7f) + prop(m, obj_id(m, self), 0)),
                             rs(ds, AI_TARGET_FINE_X), rs(ds, AI_TARGET_FINE_Y),
                             (int16_t)((rw(ls, (uint16_t)(t + 2)) & 0x7f) + prop(m, obj_id(m, t), 0))))
        return 0;
    if (!turn_to_target(m, 1)) return 0;
    if ((int16_t)((int16_t)rt_rand(m) % 0xc0) <= crit(m, 6)) {
        nbset(m, 0x13, (uint8_t)(nb(m, 0x13) & 0x80));
        nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 5));
        nwset(m, 0xb, (uint16_t)(nw(m, 0xb) & 0xfff));
    }
    return 1;
}

/* The heading written in its three places, the octant `o` in word 1. */
static void face_octant(uw_motion *m, uint8_t o) {
    nbset(m, 9, (uint8_t)(o << 5));
    nwset(m, 2, (uint16_t)((nw(m, 2) & 0xfc7f) | ((o & 7) << 7)));
    nbset(m, 0x18, (uint8_t)(nb(m, 0x18) & 0xe0));
}

/* creature_goal_flee, goal 6, from the instructions. A flier
 * rolls its climb. Cornered -- within two tiles and level -- it stands its
 * ground (bit 4, goal 9) by the courage roll or when blocked, else backs off
 * (action 7) at half its walk speed: +9 turned away, word 1 NOT turned. Farther
 * and blocked, it stands within three tiles or turns a random quarter; not
 * blocked, a caster may cast over its shoulder, the heading wobbles by the
 * restlessness nibble, and it steers away from the player. Then it walks. */
static void goal_flee(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint8_t away, h;
    int8_t dz;
    uint16_t d2 = rw(ds, AI_TARGET_TILE_DIST2);
    if (!ds[AI_MAY_MOVE]) return;
    away = vector_to_heading((int8_t)ds[AI_TARGET_DX], (int8_t)ds[AI_TARGET_DY]);
    dz = (int8_t)((ls[(uint16_t)(rw(ds, AI_TARGET_PTR) + 2)] & 0x7f) - (nb(m, 2) & 0x7f));
    if (crit(m, 0xa) & 0x80) {
        uint8_t v = (uint8_t)((nw(m, 2) & 0x7f) > 0x6e ? rt_rand(m) % 5 + 0xd : rt_rand(m) % 5 + 0xf);
        nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 7) | ((v & 0x1f) << 3)));
    }
    if (d2 <= 3 && abs(dz) < 0x10) {
        if ((int16_t)(rt_rand(m) % 0x100) < ((crit(m, 0x1c) & 0xf) >> 3)
            || (ds[AI_MOTION_BLOCKED] && !ds[AI_HEADING_CHANGED])) {
            nbset(m, 0x19, (uint8_t)(nb(m, 0x19) | 0x10));
            creature_set_goal(m, 9, (uint8_t)((nw(m, 0xb) & 0xff0) >> 4));
            return;
        }
        nbset(m, 9, (uint8_t)(((away + 4) % 8) << 5));
        nwset(m, 2, (uint16_t)((nw(m, 2) & 0xfc7f) | ((away & 7) << 7)));
        nbset(m, 0x18, (uint8_t)(nb(m, 0x18) & 0xe0));
        nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 7));
        counter_step4(m);
        nbset(m, 0x13, (uint8_t)((nb(m, 0x13) & 0x80) | (((crit(m, 0xb) + 1) / 2) & 0x7f)));
        return;
    }
    if (ds[AI_MOTION_BLOCKED] && !ds[AI_HEADING_CHANGED]) {
        if (d2 < 9) {
            if ((nb(m, 0xb) & 0xf) == 9) {
                nbset(m, 0x13, (uint8_t)(nb(m, 0x13) & 0x80));
                face_octant(m, away);
                nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 4));
                nbset(m, 0x15, (uint8_t)(nb(m, 0x15) & 0xc0));
                counter_step4(m);
                return;
            }
            nbset(m, 0x19, (uint8_t)(nb(m, 0x19) | 0x10));
            creature_set_goal(m, 9, (uint8_t)((nw(m, 0xb) & 0xff0) >> 4));
            return;
        }
        {
            int16_t turn = (int16_t)((((int16_t)rt_rand(m) % 2) * 2 - 1) * 2);
            uint8_t q = (uint8_t)((((nb(m, 9) >> 5) + turn + 8) % 8) << 5);
            h = (uint8_t)(q + (uint8_t)((int16_t)rt_rand(m) % 0x20));
        }
    } else {
        uint8_t r;
        if (try_magic_attack(m)) return;
        r = (uint8_t)((int16_t)rt_rand(m) % 0x40);
        if ((int16_t)r < (int16_t)((crit(m, 0x1f) & 0xf) + 8))
            h = (uint8_t)((nb(m, 9) + (int16_t)rt_rand(m) % 0x40 + 0xe0) % 0x100);
        else
            h = nb(m, 9);
        if (!ds[AI_HEADING_CHANGED]) h = vector_to_player(m, h, 0x18);
    }
    nbset(m, 9, h);
    nwset(m, 2, (uint16_t)((nw(m, 2) & 0xfc7f) | (((h >> 5) & 7) << 7)));
    nbset(m, 0x18, (uint8_t)((nb(m, 0x18) & 0xe0) | (h & 0x1f)));
    nbset(m, 0x13, (uint8_t)((nb(m, 0x13) & 0x80) | ((d2 < 0x40 ? crit(m, 0xc) : crit(m, 0xb)) & 0x7f)));
    nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 0x2c));
    counter_step4(m);
    nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 4));
}

static int start_missile_attack(uw_motion *m);
static void choose_manoeuvre(uw_motion *m, uint16_t si);

/* creature_goal_stand_ground, goal 9: in reach the manoeuvre;
 * within two tiles face the target and stand; farther, magic or a missile,
 * and a creature with neither flees. */
static void goal_stand_ground(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t si = (uint16_t)(rs(ds, AI_TARGET_DX) * rs(ds, AI_TARGET_DX) + rs(ds, AI_TARGET_DY) * rs(ds, AI_TARGET_DY));
    if (!ds[AI_MAY_MOVE]) return;
    if (si < 0x90 || (ds[AI_TARGET_TILE_X] == ds[AI_SELF_TILE_X] && ds[AI_SELF_TILE_Y] == ds[AI_TARGET_TILE_Y])) {
        choose_manoeuvre(m, si);
        return;
    }
    if (rw(ds, AI_TARGET_TILE_DIST2) > 4) {
        if (try_magic_attack(m)) return;
        if ((crit(m, 0x2d) >> 1) & 0x7f) start_magic_attack(m);
        else if ((((crit(m, 0x20) >> 1) & 0x7f) >> 4) == 1) start_missile_attack(m);
        else goal_flee(m);
        return;
    }
    nbset(m, 0x13, (uint8_t)(nb(m, 0x13) & 0x80));
    face_octant(m, vector_to_heading((int8_t)ds[AI_TARGET_DX], (int8_t)ds[AI_TARGET_DY]));
    nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 4));
    nbset(m, 0x15, (uint8_t)(nb(m, 0x15) & 0xc0));
    counter_step4(m);
}

/* npc_try_open_door, from the instructions: a creature with the
 * door skill (critter +0x2e) uses the door (object_use_dispatch at the
 * door's tile); a closed door (0x140..0x147) is then half the time forced
 * with that skill, else a quarter of the time bashed for rand() % the
 * critter's first attack damage (+0x14), type 4. */
static void npc_try_open_door(uw_motion *m, uint16_t door) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w0 = rw(ls, door);
    if ((w0 & 7) == 7) return;
    if (crit(m, 0x2e)) {
        ww(ds, 0x269a, ds[AI_DOOR_TILE_X]);
        ww(ds, 0x269c, ds[AI_DOOR_TILE_Y]);
        object_use_dispatch(m, npc(m), door, 0);
    }
    w0 = rw(ls, door);
    if (((w0 & 0x1f0) >> 4) != 0x14 || (w0 & 0xf) >= 8) return;
    if (crit(m, 0x2e) && rt_rand(m) % 2 != 0) {
        door_unlock_attempt(m, npc(m), door, (int16_t)-crit(m, 0x2e));     /* forcing the lock */
        return;
    }
    if (rt_rand(m) % 4 == 0) {
        uint8_t dmg = (uint8_t)((int16_t)rt_rand(m) % crit(m, 0x14));
        apply_damage(m, door, npc(m), ds[AI_DOOR_TILE_X], ds[AI_DOOR_TILE_Y], dmg, 4);
    }
}

/* creature_goal_attack, goal 5. */
static void goal_attack(uw_motion *m) {
    uint8_t *ds = m->ds;
    int started = 0;
    uint8_t range = 4;
    uint16_t si, di;
    int8_t hx, hy;
    if (!ds[AI_MAY_MOVE]) return;
    if (rw(ds, CURRENT_LEVEL_WORD) == 7 && !((ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x60)] >> 5) & 1)
        && crit(m, 9) == 0x13)
        range = 1;
    si = (uint16_t)(rs(ds, AI_TARGET_DX) * rs(ds, AI_TARGET_DX) + rs(ds, AI_TARGET_DY) * rs(ds, AI_TARGET_DY));
    hx = (int8_t)(ds[AI_SELF_HOME_X] - ds[AI_SELF_TILE_X]);
    hy = (int8_t)(ds[AI_SELF_HOME_Y] - ds[AI_SELF_TILE_Y]);
    di = (uint16_t)(hx * hx + hy * hy);
    if (((nw(m, 0xb) & 0xff0) >> 4) == 1)
        nwset(m, 0xd, (uint16_t)(nw(m, 0xd) & 0x3fff));
    if ((si < 100 || (ds[AI_SELF_TILE_X] == ds[AI_TARGET_TILE_X] && ds[AI_SELF_TILE_Y] == ds[AI_TARGET_TILE_Y]))
        && (absz((int8_t)ds[AI_TARGET_Z] - (int8_t)ds[AI_SELF_Z]) < 4 || (crit(m, 0xa) & 0x80))) {
        choose_manoeuvre(m, si);
    } else if ((crit(m, 0x2d) >> 1) & 0x7f) {
        /* A cast begun by the roll leaves `started` clear. */
        if (!try_magic_attack(m) && (crit(m, 0x2d) & 1))
            started = start_magic_attack(m);
    } else if ((((crit(m, 0x20) >> 1) & 0x7f) >> 4) == 1) {
        started = start_missile_attack(m);
    }
    if (started) {
        /* In position: unless casting, shooting or striking, stand. */
        uint8_t action = (uint8_t)(nb(m, 0x15) & 0x3f);
        if (action == 5 || action == 0xd || action == 1) return;
        nbset(m, 0x15, (uint8_t)(nb(m, 0x15) & 0xc0));
        nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 4));
        counter_step4(m);
        nbset(m, 0x13, (uint8_t)(nb(m, 0x13) & 0x80));
        return;
    }
    if (si > 0x100 && (nw(m, 0xd) & 0xf) == 4 && !(nb(m, 0x19) & 0x20)
        && (uint16_t)((((crit(m, 0x1c) >> 4) & 0xf) * ((crit(m, 0x1c) >> 4) & 0xf)) << 2) < di) {
        nbset(m, 0x19, (uint8_t)(nb(m, 0x19) & 0xfe));
        nbset(m, 0x19, (uint8_t)(nb(m, 0x19) & 0xfd));
        creature_set_goal(m, 4, 0);
        return;
    }
    attack_search_target(m, ds[AI_TARGET_TILE_X], ds[AI_TARGET_TILE_Y],
                         (crit(m, 0x2d) & 1) ? range : 1);
}

/* creature_turn_body: the body heading moves at most 0x20 from
 * where the tick began; the travel heading follows it. */
static void turn_body(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint8_t travel = nb(m, 9), body, d;
    body = (uint8_t)((((nw(m, 2) & 0x380) >> 7) << 5) + (nb(m, 0x18) & 0x1f));
    d = (uint8_t)((body + 0x100 - ds[AI_SELF_HEADING32]) % 0x100);
    if (d >= 0x20 && d <= 0xe0)
        body = (uint8_t)(((d < 0x80 ? 0x20 : 0xe0) + ds[AI_SELF_HEADING32]) % 0x100);
    nwset(m, 2, (uint16_t)((nw(m, 2) & 0xfc7f) | (((body >> 5) & 7) << 7)));
    nbset(m, 0x18, (uint8_t)((nb(m, 0x18) & 0xe0) | (body & 0x1f)));
    if (!ds[AI_HEADING_CHANGED]) {
        if (ds[AI_SELF_SPEED] <= 1) return;
        if ((int16_t)(nb(m, 0x13) & 0x7f) <= 1) return;
        d = (uint8_t)((travel + 0x100 - ds[AI_SELF_HEADING]) % 0x100);
        if (d < 0x20 || d > 0xe0) { nbset(m, 9, travel); return; }
        if (d < 0x40) { nbset(m, 9, (uint8_t)((ds[AI_SELF_HEADING] + 0x20) % 0x100)); return; }
        if (d > 0xc0) { nbset(m, 9, (uint8_t)((ds[AI_SELF_HEADING] + 0xe0) % 0x100)); return; }
        nbset(m, 0x13, (uint8_t)(nb(m, 0x13) & 0x80));
    }
    nbset(m, 9, ds[AI_SELF_HEADING]);
}

/* A goal whose target may have gone: without the dispatcher's target flag, a
 * failed distance_to_target resets the goal (creature_reset_goal). */
static int target_or_reset(uw_motion *m, int have_target) {
    if (have_target || distance_to_target(m)) return 1;
    creature_reset_goal(m);
    return 0;
}

/* creature_behaviour_dispatch, from the instructions: footsteps,
 * the witness test, the pending target at +0x12, then the goal switch and
 * creature_turn_body. */
/* creature_goal_travel, goal 3, to the goal target: arrived
 * within a squared tile distance of 2, face it (word 1 only), action 1, stop;
 * within 64 walk there with npc_goto; farther, TELEPORT -- to the point four
 * tile units short of the target along the line back to the creature, out
 * of one tile's chain and into the other's, in the middle of the tile at
 * its floor. */
static void goal_travel(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t d2 = rw(ds, AI_TARGET_TILE_DIST2);
    if (d2 <= 2) {
        if (!ds[AI_MAY_MOVE]) return;
        nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 1));
        counter_step4(m);
        nbset(m, 0x13, (uint8_t)(nb(m, 0x13) & 0x80));
        nwset(m, 2, (uint16_t)((nw(m, 2) & 0xfc7f)
                               | ((vector_to_heading((int8_t)ds[AI_TARGET_DX], (int8_t)ds[AI_TARGET_DY]) & 7) << 7)));
        nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 4));
        return;
    }
    if (d2 > 0x40) {
        uw_objpool pool;
        int16_t si = (int16_t)uw_isqrt32(d2);
        int8_t nx = (int8_t)(ds[AI_TARGET_TILE_X]
                             + (int8_t)((int16_t)(((int8_t)ds[AI_SELF_TILE_X] - (int8_t)ds[AI_TARGET_TILE_X]) << 2) / si));
        int8_t ny = (int8_t)(ds[AI_TARGET_TILE_Y]
                             + (int8_t)((int16_t)(((int8_t)ds[AI_SELF_TILE_Y] - (int8_t)ds[AI_TARGET_TILE_Y]) << 2) / si));
        uint16_t from = tile_ptr(m, ds[AI_SELF_TILE_X], ds[AI_SELF_TILE_Y]);
        uint16_t to = tile_ptr(m, (uint16_t)(int16_t)nx, (uint16_t)(int16_t)ny);
        pool_from_ds(m, &pool);
        uw_object_list_remove(&pool, (uint16_t)(from + 2), npc(m));
        uw_object_list_insert(&pool, (uint16_t)(to + 2), npc(m));
        pool_to_ds(m, &pool);
        nwset(m, 0x16, (uint16_t)((nw(m, 0x16) & 0x3ff) | ((nx & 0x3f) << 10)));
        nwset(m, 0x16, (uint16_t)((nw(m, 0x16) & 0xfc0f) | ((ny & 0x3f) << 4)));
        nwset(m, 2, (uint16_t)((nw(m, 2) & 0x1fff) | 0x8000));
        nwset(m, 2, (uint16_t)((nw(m, 2) & 0xe3ff) | 0x1000));
        nwset(m, 2, (uint16_t)((nw(m, 2) & 0xff80) | ((((ls[to] >> 4) & 0xf) << 3) & 0x7f)));
        return;
    }
    if (ds[AI_MAY_MOVE]) npc_goto(m, ds[AI_TARGET_TILE_X], ds[AI_TARGET_TILE_Y], ds[AI_TARGET_Z]);
}

/* creature_goal_talk, goal 10: the Avatar as target, the
 * look-out's search; lost or farther than 20 fine units squared under 0x190,
 * it stands; otherwise it stands FACING the Avatar, and within 0x90 with the
 * Avatar facing it (3..5 octants between their headings) it opens the
 * conversation itself. */
static void goal_talk(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t si;
    uint8_t x, y, h;
    int r;
    if (!ds[AI_MAY_MOVE]) return;
    nwset(m, 0xb, (uint16_t)((nw(m, 0xb) & 0xf00f) | 0x10));
    distance_to_target(m);
    si = (uint16_t)(rs(ds, AI_TARGET_DX) * rs(ds, AI_TARGET_DX) + rs(ds, AI_TARGET_DY) * rs(ds, AI_TARGET_DY));
    r = search_for_target(m, &x, &y);
    if (r == 1 || si >= 0x190) {
        nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 6));
        nbset(m, 0x13, (uint8_t)(nb(m, 0x13) & 0x80));
        nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 0x20));
        if (rt_rand(m) % 2 != 0) counter_step4(m);
        return;
    }
    h = vector_to_heading((int8_t)ds[AI_TARGET_DX], (int8_t)ds[AI_TARGET_DY]);
    nbset(m, 0x13, (uint8_t)(nb(m, 0x13) & 0x80));
    nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 0x20));
    nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 6));
    if (rt_rand(m) % 2 != 0) counter_step4(m);
    face_octant(m, h);
    if (si < 0x90) {
        int8_t d = (int8_t)(((uint8_t)((rw(ls, (uint16_t)(rw(ds, TRACKED_OBJECT) + 2)) & 0x380) >> 7) + 8 - h) & 7);
        if (d >= 3 && d <= 5) {
            ds[0x0726] = 0;                 /* cursor_bounds_reset_countdown */
            cursor_reset_bounds(m);
            /* conv_begin_with_object(npc): the creature hails the Avatar
             * from inside the frame, and the conversation is the host's
             * (the shell's src/uw_talk.c), which begins it at the top of
             * the next pass. */
            m->talk_object = npc(m);
        }
    }
}

/* creature_goal_hover, goal 12: a hostile creature not
 * watching (goal 4) watches instead; off its home tile it walks there; on it,
 * it stands (mode 6, idle, the counter stepped half the time). */
static void goal_go_home(uw_motion *m) {
    uint8_t *ds = m->ds;
    int8_t dx, dy;
    if (!ds[AI_MAY_MOVE]) return;
    dx = (int8_t)(ds[AI_SELF_HOME_X] - ds[AI_SELF_TILE_X]);
    dy = (int8_t)(ds[AI_SELF_HOME_Y] - ds[AI_SELF_TILE_Y]);
    if ((nw(m, 0xd) >> 14) == 0 && (nb(m, 0xb) & 0xf) != 4) {
        creature_set_goal(m, 4, 1);
        return;
    }
    if (dx || dy) {
        npc_goto(m, ds[AI_SELF_HOME_X], ds[AI_SELF_HOME_Y],
                 (uint8_t)((m->lseg[tile_ptr(m, ds[AI_SELF_HOME_X], ds[AI_SELF_HOME_Y])] >> 4) & 0xf));
        return;
    }
    nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 6));
    nbset(m, 0x13, (uint8_t)(nb(m, 0x13) & 0x80));
    nbset(m, 0x15, (uint8_t)((nb(m, 0x15) & 0xc0) | 0x20));
    if (rt_rand(m) % 2 != 0) counter_step4(m);
}

/* creature_is_wounded_enough, from the instructions: a band --
 * no above three quarters of `max` or below an eighth; yes when the damage
 * taken this round is over half of `max`; otherwise no when 15 - courage is
 * under hp * 16 / max + rand() % 4. */
static int wounded_enough(uw_motion *m, uint8_t max, uint8_t hp, uint8_t courage, uint8_t taken) {
    int16_t v;
    if ((int16_t)((max * 3) >> 2) < hp) return 0;
    if ((int16_t)(max >> 3) > hp) return 0;
    if ((int16_t)(max >> 1) < taken) return 1;
    if (max == 0) return 0;
    v = (int16_t)rt_rand(m) % 4;
    v = (int16_t)((int16_t)(hp << 4) / (int16_t)max + v);
    return (uint16_t)(0xf - courage) >= (uint16_t)v;
}

static void behaviour_dispatch(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    int have_target = 0;
    ds[AI_FACED_PATH_STEP] = 0;
    nbset(m, 0x18, (uint8_t)(nb(m, 0x18) & 0xdf));
    nbset(m, 0x15, (uint8_t)(nb(m, 0x15) & 0xbf));
    if ((nb(m, 0xb) & 0xf) == 0xb) goto sw;
    if ((nb(m, 0x15) & 0x3f) == 0x2c && ((nw(m, 0xb) >> 12) & 1)) {
        /* A walking creature's footsteps, by critter+0x10's low nibble: a biped effect 1 or 2 by its stride,
         * then 0x17, 5, 0x0e and 0x0d, where the creature stands */
        static const uint8_t steps[5] = { 0, 0x17, 5, 0x0e, 0x0d };
        uint16_t k = (uint16_t)((crit(m, 0x10) & 0xf) - 1);
        uint16_t c = (uint16_t)(nw(m, 0xb) >> 12);
        if (k <= 4 && (k != 0 || c == 1 || c == 3))
            play_sound_effect_at_xy(m, k ? steps[k] : (c == 1 ? 1 : 2), rs(ds, AI_SELF_FINE_X), rs(ds, AI_SELF_FINE_Y), 0);
    }
    if ((crit(m, 0xa) >> 1) & 1) goto sw;
    /* The witness test: a creature of the assaulted one's race (or one that
     * saw it, +0x19 bit 6) near the assault within 0x200 ticks joins in. */
    if (((nb(m, 0x19) & 0x40) == 0 && ds[ASSAULT_VICTIM_INDEX] != ds[AI_SELF_INDEX]
         && crit(m, 9) == ds[ASSAULT_VICTIM_RACE] && !(nb(m, 0xa) & 0x80))
        || (nb(m, 0x19) & 0x40)) {
        uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
        uint32_t until = ((uint32_t)rw(ds, ASSAULT_TIME) | ((uint32_t)rw(ds, ASSAULT_TIME + 2) << 16)) + 0x200;
        uint32_t now = (uint32_t)rw(ds, (uint16_t)(rec + 0xce)) | ((uint32_t)rw(ds, (uint16_t)(rec + 0xd0)) << 16);
        if (until > now) {
            int16_t d = (int16_t)(abs((int16_t)(ds[AI_SELF_TILE_X] - ds[ASSAULT_TILE_X]))
                                  + abs((int16_t)(ds[AI_SELF_TILE_Y] - ds[ASSAULT_TILE_Y])));
            if (d < (crit(m, 0x1e) & 0xf)) {
                nwset(m, 0xd, (uint16_t)(nw(m, 0xd) & 0x3fff));
                nbset(m, 0x19, (uint8_t)(nb(m, 0x19) | 1));
                if ((nb(m, 0xb) & 0xf) != 9 && (nb(m, 0xb) & 0xf) != 6) {
                    creature_set_goal(m, 5, (nb(m, 0x19) & 0x40) ? ds[ASSAULT_VICTIM_INDEX] : 1);
                    npc_set_destination(m, ds[ASSAULT_TILE_X], ds[ASSAULT_TILE_Y], ds[ASSAULT_Z]);
                }
            }
        }
    }
    if (nb(m, 0x12) == 0) goto sw;
    if (!((nb(m, 0x12) == 1 && !(nb(m, 0x19) & 0x40)) || (nb(m, 0x19) & 0x40)
          || (ls[(uint16_t)(obj_at(m, nb(m, 0x12)) + 0x19)] & 0x40)))
        goto sw;
    if (nb(m, 0x12) != ((nw(m, 0xb) & 0xff0) >> 4))
        nwset(m, 0xb, (uint16_t)((nw(m, 0xb) & 0xf00f) | (nb(m, 0x12) << 4)));
    if (!distance_to_target(m)) goto sw;
    have_target = 1;
    if (nb(m, 0x12) == 1) {
        uint16_t pl = rw(ds, TRACKED_OBJECT);
        nwset(m, 0xd, (uint16_t)(nw(m, 0xd) & 0x3fff));
        npc_set_destination(m, (uint8_t)(rw(ls, (uint16_t)(pl + 0x16)) >> 10),
                            (uint8_t)((rw(ls, (uint16_t)(pl + 0x16)) & 0x3f0) >> 4),
                            (uint8_t)((rw(ls, (uint16_t)(pl + 2)) & 0x7f) >> 3));
        nbset(m, 0x19, (uint8_t)(nb(m, 0x19) | 1));
    }
    {
        /* The choice of attack (5), flight (6) or standing ground (9): far
         * and with no magic to use, attack and latch bit 5; latched, attack;
         * wounded enough and not yet latched to stand (bit 4), flee;
         * latched to stand, stand; else attack. */
        uint8_t goal = 5;
        if (rw(ds, AI_TARGET_TILE_DIST2) > 2
            && (!(crit(m, 0x2d) & 1) || tile_no_magic(m, ds[AI_SELF_TILE_X], ds[AI_SELF_TILE_Y]))) {
            nbset(m, 0x19, (uint8_t)(nb(m, 0x19) | 0x20));
        } else if (!(nb(m, 0x19) & 0x20)) {
            if (!(nb(m, 0x19) & 0x10) && wounded_enough(m, crit(m, 4), nb(m, 8), (uint8_t)(crit(m, 0x1c) & 0xf), nb(m, 0x11)))
                goal = 6;
            else if (nb(m, 0x19) & 0x10)
                goal = 9;
        }
        creature_set_goal(m, goal, nb(m, 0x12));
    }
    nbset(m, 0x12, 0);
    nbset(m, 0x11, 0);
sw:
    switch (nb(m, 0xb) & 0xf) {
    case 8:
        goal_keep_near_home(m);
        break;
    case 2:
        goal_wander(m);
        break;
    case 5:
        if (target_or_reset(m, have_target)) goal_attack(m);
        break;
    case 3:
        if (target_or_reset(m, have_target)) goal_travel(m);
        break;
    case 6:
        if (target_or_reset(m, have_target)) goal_flee(m);
        break;
    case 9:
        if (target_or_reset(m, have_target)) goal_stand_ground(m);
        break;
    case 11:                                /* a random drift; nothing sets goal 11 */
        nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 4));
        nbset(m, 0x13, (uint8_t)((nb(m, 0x13) & 0x80) | ((rt_rand(m) % 2) & 0x7f)));
        nbset(m, 9, (uint8_t)(rt_rand(m) % 0x100));
        nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 7) | (((rt_rand(m) % 3 + 0xf) & 0x1f) << 3)));
        counter_step4(m);
        nbset(m, 0x15, (uint8_t)(nb(m, 0x15) | 0x40));
        break;
    case 0: case 4: case 7:
        goal_stand_still(m);
        break;
    case 1:                                 /* straight home, whether or not it may move */
        npc_goto(m, ds[AI_SELF_HOME_X], ds[AI_SELF_HOME_Y],
                 (uint8_t)((ls[tile_ptr(m, ds[AI_SELF_HOME_X], ds[AI_SELF_HOME_Y])] >> 4) & 0xf));
        break;
    case 10:
        goal_talk(m);
        break;
    case 12:
        goal_go_home(m);
        break;
    default:
        nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 7));
        break;
    }
    turn_body(m);
}

/* creature_ai_update past the distance cull, from the
 * instructions: the motion, the context, then by goal and action. 1 when the
 * tick ran, -1 for a death this port does not carry. */
static int creature_tick(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t pr = (uint16_t)(OBJ_PROPERTIES + (rw(ls, obj) & 0x1ff) * 11);
    uint16_t row = rw(ds, AI_SELF_CRITTER), block, filt;
    uint16_t bp = (uint16_t)(FRAME_BP - 0x12 - 8 - 12);     /* creature_ai_update's */
    uint8_t action;
    if (ds[(uint16_t)(row + 0xa)] & 0x80) {
        block = BLOCK_FLIER; filt = FILTER_FLIER;
    } else if (ds[(uint16_t)(row + 0xa)] & 0x40) {
        block = BLOCK_SWIMMER; filt = FILTER_SWIMMER;
    } else {
        block = BLOCK_WALKER; filt = FILTER_WALKER;
    }
    ww(ds, AI_MOTION_BLOCK, block);
    ww(ds, AI_FILTER_DESC, filt);
    if (ds[(uint16_t)(pr + 8)] & 8) {
        ww(ds, (uint16_t)(filt + 2), (uint16_t)(rw(ds, (uint16_t)(filt + 2)) & 0xffdf));
        ww(ds, (uint16_t)(filt + 6), (uint16_t)(rw(ds, (uint16_t)(filt + 6)) & 0xffdf));
        ww(ds, filt, (uint16_t)(rw(ds, filt) | 0x20));
    }
    ds[AI_MOTION_BLOCKED] = 0;
    ds[AI_MAY_MOVE] = 1;
    ds[AI_FLIER_STEP] = 0;
    ds[AI_HEADING_CHANGED] = 0;
    ds[AI_BLOCKED_ANY] = 0;
    ds[AI_WALKER_BLOCKED] = 0;
    ds[AI_GOTO_ACTIVE] = 0;
    action = (uint8_t)(ls[(uint16_t)(obj + 0x15)] & 0x3f);
    if (action != 0x2c && action != 0x20 && (ls[(uint16_t)(obj + 0x15)] & 0x80)) {
        ww(ds, NPC_PATH_SLOT_MASK, (uint16_t)(rw(ds, NPC_PATH_SLOT_MASK) | (1 << (ls[(uint16_t)(obj + 0x16)] & 0xf))));
        ls[(uint16_t)(obj + 0x15)] = (uint8_t)(ls[(uint16_t)(obj + 0x15)] & 0x7f);
    }
    if (!((ls[(uint16_t)(obj + 0x15)] & 0x40) && !(ls[(uint16_t)(obj + 0x13)] & 0x7f)
          && ((ls[(uint16_t)(obj + 0x14)] & 0xf8) >> 3) == 0x10)) {
        uint8_t heading;
        motion_params_init(m, obj, block);
        heading = ls[(uint16_t)(obj + 9)];
        ww(ds, AI_TERRAIN_FLAGS, object_terrain_test(m, obj, (uint16_t)(bp - 0x18)));
        /* motion_top_level */
        ww(ds, (uint16_t)(block + 0x12), (uint16_t)((ls[(uint16_t)(obj + 0x14)] & 7) << 4));
        motion_run(m, block, filt, (uint16_t)(bp - 0x18 - 2 - 4 - 4 - 2));
        ww(ds, MOTION_TILE_X, (uint16_t)(rw(ls, (uint16_t)(obj + 0x16)) >> 10));
        ww(ds, MOTION_TILE_Y, (uint16_t)((rw(ls, (uint16_t)(obj + 0x16)) & 0x3f0) >> 4));
        projectile_motion_apply(m, obj, block, (uint16_t)(bp - 0x14 - 6 - 4 - 2));
        if (ls[(uint16_t)(obj + 9)] != heading) ds[AI_HEADING_CHANGED] = 1;
    }
    if (ds[(uint16_t)(pr + 8)] & 8) {
        ww(ds, (uint16_t)(filt + 2), (uint16_t)(rw(ds, (uint16_t)(filt + 2)) | 0x20));
        ww(ds, (uint16_t)(filt + 6), (uint16_t)(rw(ds, (uint16_t)(filt + 6)) | 0x20));
        ww(ds, filt, (uint16_t)(rw(ds, filt) & 0xffdf));
    }
    {
        uint16_t w16 = rw(ls, (uint16_t)(obj + 0x16)), w2 = rw(ls, (uint16_t)(obj + 2));
        ds[AI_SELF_TILE_X] = (uint8_t)(w16 >> 10);
        ds[AI_SELF_TILE_Y] = (uint8_t)((w16 & 0x3f0) >> 4);
        ds[AI_SELF_Z] = (uint8_t)((w2 & 0x7f) >> 3);
        ww(ds, AI_SELF_FINE_X, (uint16_t)((ds[AI_SELF_TILE_X] << 3) + ((w2 & 0xe000) >> 13)));
        ww(ds, AI_SELF_FINE_Y, (uint16_t)((ds[AI_SELF_TILE_Y] << 3) + ((w2 & 0x1c00) >> 10)));
        ds[AI_SELF_HOME_X] = (uint8_t)(ls[(uint16_t)(obj + 4)] & 0x3f);
        ds[AI_SELF_HOME_Y] = (uint8_t)(ls[(uint16_t)(obj + 6)] & 0x3f);
        ds[AI_SELF_HEADING] = ls[(uint16_t)(obj + 9)];
        ds[AI_SELF_HEADING32] = (uint8_t)((((w2 & 0x380) >> 7) << 5) + (ls[(uint16_t)(obj + 0x18)] & 0x1f));
        ds[AI_SELF_SPEED] = (uint8_t)(ls[(uint16_t)(obj + 0x13)] & 0x7f);
        ds[AI_SELF_HEIGHT] = ds[pr];
    }
    {
        uint16_t goal = (uint16_t)(ls[(uint16_t)(obj + 0xb)] & 0xf);
        uint16_t w0b = rw(ls, (uint16_t)(obj + 0xb));
        uint16_t step = (uint16_t)((w0b & 0xfff) | ((((w0b >> 12) + 1) & 0xf) << 12));
        action = (uint8_t)(ls[(uint16_t)(obj + 0x15)] & 0x3f);
        if (goal == 0xb || goal == 3) {
            behaviour_dispatch(m);
        } else if (action == 0xc) {         /* dying */
            if ((w0b >> 12) == 3) {
                /* The body goes: use_special_npc(1), out of its tile, its
                 * loot into its contents, its remains and blood, its
                 * contents spilled, and its record freed. */
                uw_objpool pool;
                uint16_t w16 = rw(ls, (uint16_t)(obj + 0x16));
                use_special_npc(m, obj, 1);
                ww(ds, MOTION_TILE_X, (uint16_t)(w16 >> 10));
                ww(ds, MOTION_TILE_Y, (uint16_t)((w16 & 0x3f0) >> 4));
                pool_from_ds(m, &pool);
                uw_object_list_remove(&pool, (uint16_t)(tile_ptr(m, rw(ds, MOTION_TILE_X), rw(ds, MOTION_TILE_Y)) + 2), obj);
                pool_to_ds(m, &pool);
                spawn_npc_loot(m, obj);
                drop_npc_remains(m, obj, (uint8_t)((ds[(uint16_t)(row + 8)] >> 5) & 7),
                                 (uint8_t)((ds[(uint16_t)(row + 0xa)] >> 2) & 7), (uint16_t)(bp - 0xe - 8 - 4 - 2));
                /* spill_critter_inventory: spill_inventory(obj, its race, critter_properties +9) */
                spill_inventory(m, obj, ds[(uint16_t)(CRITTER_BASE + (rw(ls, obj) & 0x3f) * 0x30 + 9)],
                                (uint16_t)(bp - 0xe - 4 - 4 - 2 - 6 - 4 - 2));
                pool_from_ds(m, &pool);
                uw_obj_free(&pool, obj);
                pool_to_ds(m, &pool);
                return 0;
            }
            ww(ls, (uint16_t)(obj + 0xb), step);
        } else if (action >= 1 && action <= 3) {    /* melee */
            if ((w0b >> 12) == 0 && ((w0b & 0xff0) >> 4) == 1) {
                /* The combat music: get_music_file_no is the
                 * playing track, set_theme_music the wanted one. */
                if (ds[MUSIC_TRACK_PLAYING] < 5 || ds[MUSIC_TRACK_PLAYING] > 7)
                    ds[MUSIC_TRACK_WANTED] = 6;
                ww(ds, COMBAT_MUSIC_TIME, (uint16_t)m->clock);
                ww(ds, (uint16_t)(COMBAT_MUSIC_TIME + 2), (uint16_t)(m->clock >> 16));
            }
            if ((w0b >> 12) == 4) {
                uint8_t scale = m->ext ? m->ext[(uint16_t)(0x3c0 + (rw(ls, (uint16_t)(obj + 0xf)) >> 12) * 2)] : 0;
                int16_t swing = (int16_t)(rt_rand(m) % 9);
                if (!m->ext) UW_NOT_CARRIED(m->not_carried);
                execute_attack(m, obj, swing, scale, (int16_t)(action - 1), crit(m, 0xf),
                               (uint16_t)(FRAME_BP - 0x12 - 8 - 12));
                ls[(uint16_t)(obj + 0x15)] = (uint8_t)(ls[(uint16_t)(obj + 0x15)] & 0xc0);
                ww(ls, (uint16_t)(obj + 0xb), (uint16_t)(rw(ls, (uint16_t)(obj + 0xb)) & 0xfff));
                ww(ls, (uint16_t)(obj + 0xf), (uint16_t)(rw(ls, (uint16_t)(obj + 0xf)) & 0xfff));
            } else {
                ww(ls, (uint16_t)(obj + 0xb), step);
            }
        } else if (action == 0xd && (ls[(uint16_t)(obj + 0x19)] & 0xc)) {
            if ((w0b >> 12) == 4) {
                ww(ds, PROJ_AIM_Z, (uint16_t)(int16_t)pitch_to_target(m, 0x1e, 0));
                effect_dispatch(m, ds[(uint16_t)(row + 0x29 + ((ls[(uint16_t)(obj + 0x19)] & 0xc) >> 2))], obj, 0,
                                (uint16_t)(bp - 0xe - 10 - 4 - 2));
                ls[(uint16_t)(obj + 0x15)] = (uint8_t)(ls[(uint16_t)(obj + 0x15)] & 0xc0);
                ww(ls, (uint16_t)(obj + 0xb), (uint16_t)(rw(ls, (uint16_t)(obj + 0xb)) & 0xfff));
                ls[(uint16_t)(obj + 0x19)] = (uint8_t)(ls[(uint16_t)(obj + 0x19)] & 0xf3);
            } else {
                ww(ls, (uint16_t)(obj + 0xb), step);
            }
        } else if (action == 5) {           /* the missile */
            if ((w0b >> 12) == 4) {
                /* projectile_fire_from_object: the kind from the
                 * critter's missile nibble, its speed, straight ahead. */
                uint16_t kind = (uint16_t)(((crit(m, 0x20) >> 1) & 0x7f) & 0xf);
                uint8_t speed = ds[(uint16_t)(MISSILE_PROPS + kind * 3)];
                uint16_t w16 = rw(ls, (uint16_t)(obj + 0x16));
                ww(ds, PROJ_AIM_Z, (uint16_t)(int16_t)pitch_to_target(m, speed, 1));
                ww(ds, PROJ_ITEM, (uint16_t)(kind + 0x10));
                ww(ds, PROJ_SPEED, speed);
                ww(ds, PROJ_TARGET_X, (uint16_t)(w16 >> 10));
                ww(ds, PROJ_TARGET_Y, (uint16_t)((w16 & 0x3f0) >> 4));
                ww(ds, PROJ_HEADING, 1);
                ww(ds, PROJ_FIRER, obj);
                ww(ds, (uint16_t)(PROJ_FIRER + 2), rw(ds, (uint16_t)(MOBILE_BASE + 2)));
                ww(ds, PROJ_AIM_X, 0);
                launch_projectile(m, (uint16_t)(bp - 0xe - 8 - 4 - 2 - 4 - 2));
                ls[(uint16_t)(obj + 0x15)] = (uint8_t)(ls[(uint16_t)(obj + 0x15)] & 0xc0);
                ww(ls, (uint16_t)(obj + 0xb), (uint16_t)(rw(ls, (uint16_t)(obj + 0xb)) & 0xfff));
            } else {
                ww(ls, (uint16_t)(obj + 0xb), step);
            }
        } else {
            behaviour_dispatch(m);
        }
    }
    {
        uint8_t b = ls[(uint16_t)(obj + 0xa)];
        ls[(uint16_t)(obj + 0xa)] = (uint8_t)((b & 0xf0)
                                              | (((b & 0xf) + (ls[(uint16_t)(obj + 0x14)] & 7)) % 16));
    }
    return 1;
}

/* ---- mobile_object_update, from the instructions ------------- */

enum { BLOCK_OBJECT = 0x27f8, FILTER_OBJECT = 0x2874 };

/* A thing in flight or sliding: its motion through the object block and
 * descriptor, then projectile_motion_apply, and the schedule nibble stepped
 * by the motion mode. A spent object (+8 zero) that its type lets be culled
 * goes to object_remove from its tile, unforced: gone, 0 (the list closed
 * over it); kept, its +8 made 1 and on it goes. */
static int mobile_object_update(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t id = obj_id(m, obj);
    int r;
    if (ls[(uint16_t)(obj + 8)] == 0 && ((prop(m, id, 6) >> 2) & 3) < 3) {
        uint16_t w16 = rw(ls, (uint16_t)(obj + 0x16));
        if (!object_remove(m, (uint16_t)(tile_ptr(m, (uint16_t)(w16 >> 10), (uint16_t)((w16 & 0x3f0) >> 4)) + 2), obj, 0))
            return 0;
        ls[(uint16_t)(obj + 8)] = 1;
    }
    ww(ds, FILTER_OBJECT, (uint16_t)(((prop(m, id, 3) >> 3) & 1) ? 0x1000 : 0));
    motion_params_init(m, obj, BLOCK_OBJECT);
    ww(ds, (uint16_t)(BLOCK_OBJECT + 0x12), (uint16_t)((ls[(uint16_t)(obj + 0x14)] & 7) << 4));
    motion_run(m, BLOCK_OBJECT, FILTER_OBJECT, (uint16_t)(FRAME_BP - 0x26 - 2 - 4 - 4 - 2 - 2 - 4 - 4 - 2));
    ww(ds, MOTION_TILE_X, (uint16_t)(rw(ls, (uint16_t)(obj + 0x16)) >> 10));
    ww(ds, MOTION_TILE_Y, (uint16_t)((rw(ls, (uint16_t)(obj + 0x16)) & 0x3f0) >> 4));
    r = projectile_motion_apply(m, obj, BLOCK_OBJECT, (uint16_t)(FRAME_BP - 0x26 - 2 - 6 - 4 - 2));
    if (r > 0) {
        uint8_t b = ls[(uint16_t)(obj + 0xa)];
        ls[(uint16_t)(obj + 0xa)] = (uint8_t)((b & 0xf0) | (((b & 0xf) + (ls[(uint16_t)(obj + 0x14)] & 7)) & 0xf));
    }
    return r;
}

/* ---- update_mobile_objects, from the instructions ------------ */

/* mobile_update_due: is the object's schedule nibble inside the
 * window the phase moved through?  Its second argument, the motion mode, is
 * pushed and never read. */
static int mobile_update_due(const uint8_t *ds, int sched) {
    int phase = (int8_t)ds[MOBILE_PHASE], prev = (int8_t)ds[MOBILE_PHASE_PREV];
    if (phase > sched && sched + 4 >= phase) return 1;
    if (phase + 16 <= sched) return 0;
    return prev <= sched && prev > phase;
}

/* ---- the sleeper's callers (25ab, 8aa0) -------------------------------- */

/* creature_ai_load_context(obj):
 * everything the per-creature AI reads, cached into the scratch block --
 * the object, its index, its critter_properties row, tile x/y from +0x16,
 * z and the sub-tile position from +2, the home tile from +4 and +6, the
 * heading byte at +9 and the thirty-second heading from +2 and +0x18, the
 * speed at +0x13, and the obj_properties byte for the item id. Then one of
 * three motion descriptor pairs by bits 6..7 of critter_properties +0x0a:
 * a flier's (block 0x27d0, filter 0x2868), a swimmer's (0x2820, 0x2880) or
 * a walker's (0x27a8, 0x285c) -- the 6cef labels 0x03a0/0x0438,
 * 0x03f0/0x0450 and 0x0378/0x042c in this segment's spelling. */
void creature_ai_load_context(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w0 = rw(ls, obj), w2 = rw(ls, (uint16_t)(obj + 2)), w16 = rw(ls, (uint16_t)(obj + 0x16));
    uint16_t row = (uint16_t)(CRITTER_BASE + (w0 & 0x3f) * 0x30);
    uint8_t move;
    ww(ds, CURRENT_NPC, obj);
    ww(ds, (uint16_t)(CURRENT_NPC + 2), rw(ds, (uint16_t)(MOBILE_BASE + 2)));
    ds[AI_SELF_INDEX] = (uint8_t)obj_index_of(m, obj);
    ww(ds, AI_SELF_CRITTER, row);
    ds[AI_SELF_TILE_X] = (uint8_t)(w16 >> 10);
    ds[AI_SELF_TILE_Y] = (uint8_t)((w16 & 0x3f0) >> 4);
    ds[AI_SELF_Z] = (uint8_t)((w2 & 0x7f) >> 3);
    ww(ds, AI_SELF_FINE_X, (uint16_t)(ds[AI_SELF_TILE_X] * 8 + (w2 >> 13)));
    ww(ds, AI_SELF_FINE_Y, (uint16_t)(ds[AI_SELF_TILE_Y] * 8 + ((w2 & 0x1c00) >> 10)));
    ds[AI_SELF_HOME_X] = (uint8_t)(ls[(uint16_t)(obj + 4)] & 0x3f);
    ds[AI_SELF_HOME_Y] = (uint8_t)(ls[(uint16_t)(obj + 6)] & 0x3f);
    ds[AI_SELF_HEADING] = ls[(uint16_t)(obj + 9)];
    ds[AI_SELF_HEADING32] = (uint8_t)(((w2 & 0x380) >> 7) * 0x20 + (ls[(uint16_t)(obj + 0x18)] & 0x1f));
    ds[AI_SELF_SPEED] = (uint8_t)(ls[(uint16_t)(obj + 0x13)] & 0x7f);
    ds[AI_SELF_HEIGHT] = ds[(uint16_t)(OBJ_PROPERTIES + (w0 & 0x1ff) * 11)];
    move = ds[(uint16_t)(row + 0xa)];
    if (move & 0x80)      { ww(ds, AI_MOTION_BLOCK, 0x27d0); ww(ds, AI_FILTER_DESC, 0x2868); }
    else if (move & 0x40) { ww(ds, AI_MOTION_BLOCK, 0x2820); ww(ds, AI_FILTER_DESC, 0x2880); }
    else                  { ww(ds, AI_MOTION_BLOCK, 0x27a8); ww(ds, AI_FILTER_DESC, 0x285c); }
}

/* hostile_nearby_probe, the callback hostile_creature_nearby
 * runs through effect_area_apply: the flag at 0x1230 set when the object is
 * not the player, its goal is 4, 5 or 9 -- the hostile ones -- and +0x19
 * bit 0 is set, which is the bit npc_hunt_sleeper_probe puts on a creature
 * it has walked to the player. So it asks "is something actively coming for
 * me?", not "is something nearby". Always 0. */
int hostile_nearby_probe(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint8_t goal = (uint8_t)(rw(ls, (uint16_t)(obj + 0xb)) & 0xf);
    if (obj_index_of(m, obj) != 1 && (goal == 4 || goal == 5 || goal == 9)
        && (ls[(uint16_t)(obj + 0x19)] & 1))
        ds[HOSTILE_NEARBY] = 1;
    return 0;
}

/* hostile_creature_nearby: the probe over a radius of two about
 * the player, up to 0x7f objects. Its one caller is player_sleep, which on
 * a true answer prints "There are hostile creatures near!" and will not
 * make camp. */
int hostile_creature_nearby(uw_motion *m, uint16_t bp) {
    uint8_t *ds = m->ds;
    ds[HOSTILE_NEARBY] = 0;
    effect_area_apply(m, rw(ds, TRACKED_OBJECT), 0x7f, HOSTILE_NEARBY_FAR, 0, 0, 2, bp);
    return ds[HOSTILE_NEARBY];
}

/* npc_hunt_sleeper_probe, and the reason sleeping in Ultima
 * Underworld is dangerous. For a creature that is not the player and whose
 * attitude is 0 (hostile), half the time: the AI context loaded, a distance
 * test against three times the square of critter_properties +0x1c's high
 * nibble, then pathfind_between_tiles from the creature to the player. With
 * a path of more than one tile it WALKS THE PATH FIRING TRAPS -- every tile
 * scanned for a class 6 subclass 2 object whose link holds a class 6
 * subclass 0 type 9, and trap_dispatch called on it, so a creature crossing
 * your tripwires while you sleep sets them off -- and then moves to the
 * path's entry PATH_LEN - 2 (the index PATH_LEN * 4 - 8 of a buffer of
 * PATH_LEN + 1 entries, whose last is the player's tile): two tiles short
 * of the player, so it does not arrive on top of you,
 * takes +0x19 bit 0, is sent at the player's tile and sets the flag. 1 when
 * it moved, which stops the sweep. `bp` is this routine's frame: 0x1c of
 * locals and SI, DI under it, where item_fits_in_tile keeps its query. */
int npc_hunt_sleeper_probe(uw_motion *m, uint16_t obj, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg, *e = m->ext;
    uint16_t pl = rw(ds, TRACKED_OBJECT), w16, row, tp, dp;
    uint8_t px, py, dx, dy, sx = 0, sy = 0, z;
    int16_t ddx, ddy;
    uint8_t n;
    uw_objpool pool;
    if (obj_index_of(m, obj) == 1 || (rw(ls, (uint16_t)(obj + 0xd)) >> 14) != 0) return 0;
    if (rt_rand(m) % 2 == 0) return 0;
    creature_ai_load_context(m, obj);
    w16 = rw(ls, (uint16_t)(pl + 0x16));
    px = (uint8_t)(w16 >> 10);
    py = (uint8_t)((w16 & 0x3f0) >> 4);
    row = rw(ds, AI_SELF_CRITTER);
    ddx = (int16_t)(ds[AI_SELF_TILE_X] - px);
    ddy = (int16_t)(ds[AI_SELF_TILE_Y] - py);
    if ((int16_t)(ddx * ddx + ddy * ddy) > (int16_t)((ds[(uint16_t)(row + 0x1c)] >> 4)
                                                     * (ds[(uint16_t)(row + 0x1c)] >> 4) * 3))
        return 0;
    if (!pathfind_between_tiles(m, ds[AI_SELF_TILE_X], ds[AI_SELF_TILE_Y],
                                (uint8_t)((rw(ls, (uint16_t)(obj + 2)) & 0x7f) >> 3),
                                px, py, (uint8_t)((rw(ls, (uint16_t)(pl + 2)) & 0x7f) >> 3), 0))
        return 0;
    if (ds[PATH_LEN] <= 1) return 0;
    for (n = 0; n < ds[PATH_LEN]; n++) {
        uint16_t link = (uint16_t)(tile_ptr(m, e[(uint16_t)(n << 2)], e[(uint16_t)((n << 2) + 1)]) + 2), o;
        while ((o = deref_link(m, link)) != 0) {
            if ((rw(ls, o) & 0x1f0) == 0x1a0 && (rw(ls, (uint16_t)(o + 6)) >> 6)) {
                uint16_t inner = (uint16_t)(o + 6), t = deref_link(m, inner);
                if (t && (rw(ls, t) & 0x1ff) == 0x189)
                    trap_dispatch(m, t, e[(uint16_t)(n << 2)], e[(uint16_t)((n << 2) + 1)],
                                  (uint16_t)(bp - 0x1c - 4 - 8 - 4 - 2));
            }
            link = (uint16_t)(o + 4);
        }
    }
    dx = e[(uint16_t)(ds[PATH_LEN] * 4 - 8)];
    dy = e[(uint16_t)(ds[PATH_LEN] * 4 - 7)];
    tp = tile_ptr(m, dx, dy);
    if (!tile_standing_spot((uint8_t)(ls[tp] & 0xf), &sx, &sy)) return 0;
    /* the height the path arrived at: byte +2 of the pathfinder's node for
     * the landing tile, shifted up three -- not the tile's floor, which a flier or a slope's
     * traversal need not stand at */
    z = (uint8_t)(m->grid[(uint16_t)(dx * 0x140 + dy * 5 + 2)] << 3);
    if (!item_fits_in_tile(m, (uint16_t)(rw(ls, obj) & 0x1ff), obj_index_of(m, obj),
                           (int16_t)(dx * 8 + sx), (int16_t)(dy * 8 + sy), z,
                           (ds[(uint16_t)(row + 0xa)] >> 7) & 1, 8, (uint16_t)(bp - 0x1c - 4 - 14 - 4 - 2)))
        return 0;
    dp = tile_ptr(m, ds[AI_SELF_TILE_X], ds[AI_SELF_TILE_Y]);
    memset(&pool, 0, sizeof pool);
    pool.seg = ls;
    uw_object_list_remove(&pool, (uint16_t)(dp + 2), obj);
    uw_object_list_insert(&pool, (uint16_t)(tp + 2), obj);
    ww(ls, (uint16_t)(obj + 0x16), (uint16_t)((rw(ls, (uint16_t)(obj + 0x16)) & 0x3ff) | ((dx & 0x3f) << 10)));
    ww(ls, (uint16_t)(obj + 0x16), (uint16_t)((rw(ls, (uint16_t)(obj + 0x16)) & 0xfc0f) | ((dy & 0x3f) << 4)));
    ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0x1fff) | ((sx & 7) << 13)));
    ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0xe3ff) | ((sy & 7) << 10)));
    ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0xff80) | (z & 0x7f)));
    ls[(uint16_t)(obj + 0x19)] |= 1;
    npc_set_destination(m, px, py, (uint8_t)((rw(ls, (uint16_t)(pl + 2)) & 0x7f) >> 3));
    ds[HUNT_SLEEPER_CAME] = 1;
    return 1;
}

/* npc_hunt_sleeper: the probe over a radius of eight about the
 * player, stopping at the first that moved. player_sleep calls it after the
 * rest has elapsed, and a true answer means something came -- which skips
 * npc_settle_level, so an interrupted sleep heals and repositions nothing. */
int npc_hunt_sleeper(uw_motion *m, uint16_t bp) {
    uint8_t *ds = m->ds;
    ds[HUNT_SLEEPER_CAME] = 0;
    effect_area_apply(m, rw(ds, TRACKED_OBJECT), 1, HUNT_SLEEPER_FAR, 0, 0, 8, bp);
    return ds[HUNT_SLEEPER_CAME];
}

/* creature_ai_update as far as its distance cull: 1 when the
 * creature was handled, 0 when it was freed, -1 when the tick needs the full
 * AI, which is not ported. */
static int creature_ai_update(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t player = (uint16_t)(rw(ds, MOBILE_BASE) + 27);   /* obj_ptr_from_index(1) */
    int tx, ty, px, py, dv, dp;
    ds[AI_SELF_INDEX] = (uint8_t)((uint16_t)(obj - rw(ds, MOBILE_BASE)) / 27);
    ww(ds, AI_SELF_CRITTER, (uint16_t)((rw(ls, obj) & 0x3f) * 0x30 + 0x4a52));
    ds[AI_SELF_TILE_X] = (uint8_t)(rw(ls, (uint16_t)(obj + 0x16)) >> 10);
    ds[AI_SELF_TILE_Y] = (uint8_t)((rw(ls, (uint16_t)(obj + 0x16)) & 0x3f0) >> 4);
    tx = (int8_t)ds[AI_SELF_TILE_X];
    ty = (int8_t)ds[AI_SELF_TILE_Y];
    px = (int8_t)(rw(ls, (uint16_t)(player + 0x16)) >> 10);
    py = (int8_t)((rw(ls, (uint16_t)(player + 0x16)) & 0x3f0) >> 4);
    dv = (int16_t)(uint16_t)((tx - rs(ds, VIEW_TILE_X)) * (tx - rs(ds, VIEW_TILE_X))
                             + (ty - rs(ds, VIEW_TILE_Y)) * (ty - rs(ds, VIEW_TILE_Y)));
    dp = (int16_t)(uint16_t)((tx - px) * (tx - px) + (ty - py) * (ty - py));
    if (dv > 100 && dp > 100 && (ls[(uint16_t)(obj + 0xb)] & 0xf) != 3) {
        uint8_t b = ls[(uint16_t)(obj + 0xa)];
        ls[(uint16_t)(obj + 0xa)] = (uint8_t)((b & 0xf0) | (((b & 0xf) + 8) % 16));
        return 1;
    }
    return creature_tick(m, obj);
}

void update_mobile_objects(uw_motion *m, uint8_t substeps) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t p;
    ds[MOBILE_PHASE] = (uint8_t)((ds[MOBILE_PHASE_PREV] + substeps) & 0xf);
    ww(ds, CURRENT_NPC, 0);
    ww(ds, CURRENT_NPC + 2, 0);
    for (p = rw(ds, ACTIVE_LIST); p < rw(ds, ACTIVE_END); p++) {
        uint16_t obj = (uint16_t)(rw(ds, MOBILE_BASE) + ls[p] * 27);
        ww(ds, CURRENT_NPC, obj);
        ww(ds, CURRENT_NPC + 2, rw(ds, MOBILE_BASE + 2));
        while (mobile_update_due(ds, ls[(uint16_t)(obj + 0xa)] & 0xf)) {
            int r;
            if (((rw(ls, obj) & 0x1c0) >> 6) == 1) {
                r = creature_ai_update(m, obj);
            } else {
                r = mobile_object_update(m, obj);
            }
            if (r < 0) break;               /* not carried: the slot is left as it is */
            if (r == 0) { p--; break; }     /* freed: the list closed up over the slot */
        }
    }
    if (rw(ds, CURRENT_NPC) || rw(ds, CURRENT_NPC + 2))
        ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));   /* post_event(2) */
    ds[MOBILE_PHASE_PREV] = ds[MOBILE_PHASE];
}
