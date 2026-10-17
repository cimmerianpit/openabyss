/* SPDX-License-Identifier: MIT */
/* the motion integrator, from the instructions --
 * motion_run and its substeps, the ground and airborne steps, sliding,
 * bouncing, the collision filters a creature's tick installs
 * (walker, flier, swimmer) and the collision response.
 *
 * One of src/uw_motion*.c: see uw_motion.h and uw_motion_int.h. */
#include "uw_motion_int.h"

/* ==== the motion integrator, from the instructions ===== */

static int32_t sar32(int32_t v, int n) { return v < 0 ? ~(~v >> n) : v >> n; }

/* motion_dead_compare, ten bytes that compare a data byte with
 * 9 and return, has no caller by any mechanism; the port has nothing for it.
 *
 * motion_calc_velocity */
static int8_t calc_velocity(uw_motion *m, int sync, int terrain) {
    int16_t sn, cs;
    int axis, minor;
    uw_sincos_lerp((uint16_t)cws(m, 0x1e), &sn, &cs);
    cww(m, 6, (uint16_t)sar32((int32_t)sn * (int32_t)cws(m, 0x14), 15));
    cww(m, 8, (uint16_t)sar32((int32_t)cs * (int32_t)cws(m, 0x14), 15));
    vset(m, 0, (uint16_t)(vel(m, 0) + (uint16_t)(cws(m, 0xc) * cws(m, 0x12))));
    vset(m, 1, (uint16_t)(vel(m, 1) + (uint16_t)(cws(m, 0xe) * cws(m, 0x12))));
    vset(m, 2, (uint16_t)(vel(m, 2) + (uint16_t)(cws(m, 0x10) * cws(m, 0x12))));
    if ((vel(m, 0) | vel(m, 1) | vel(m, 2)) == 0) return 0;
    if (sync) query_sync(m);
    {
        int16_t a0 = (int16_t)(vel(m, 0) < 0 ? -(uint16_t)vel(m, 0) : (uint16_t)vel(m, 0));
        int16_t a1 = (int16_t)(vel(m, 1) < 0 ? -(uint16_t)vel(m, 1) : (uint16_t)vel(m, 1));
        axis = a0 <= a1 ? 1 : 0;
    }
    minor = (axis + 1) % 2;
    gset(m, MR_MAJOR, (uint16_t)axis);
    gset(m, MR_MINOR, (uint16_t)minor);
    gset(m, (uint16_t)(MR_AXIS_STEP + axis * 2), vel(m, axis) > 0 ? 0x2000 : 0xe000);
    if (vel(m, axis) != 0) {
        int16_t t, at;
        int16_t other = (int16_t)(uint16_t)((gw(m, (uint16_t)(MR_AXIS_STEP + axis * 2)) / 0x100)
                                            * vel(m, minor));
        gset(m, (uint16_t)(MR_AXIS_STEP + minor * 2),
             (uint16_t)((uint16_t)(other / vel(m, axis)) << 8));
        t = (int16_t)(uint16_t)(vel(m, axis) * cws(m, 0x12));
        at = (int16_t)(t < 0 ? -(uint16_t)t : (uint16_t)t);
        gset(m, MR_STEP_LIMIT, (uint16_t)(at >> 13));
        gset(m, MR_STEP_REM, (uint16_t)(at & 0x1fff));
        {
            int16_t q = (int16_t)(0x2000 / vel(m, axis));
            gset(m, MR_TICKS_TILE, (uint16_t)(q < 0 ? -q : q));
        }
    } else {
        gset(m, (uint16_t)(MR_AXIS_STEP + minor * 2), 1);
        gset(m, (uint16_t)(MR_AXIS_STEP + axis * 2), 1);
        gset(m, MR_STEP_LIMIT, 0);
        gset(m, MR_STEP_REM, 0);
        gset(m, MR_TICKS_TILE, (uint16_t)cws(m, 0x12));
    }
    gset(m, MR_COUNTER, 0);
    if ((gw(m, 0x2772) == 1 || vel(m, 2) != 0) && terrain) {
        sq_terrain(m, cb(m, 0x24));
        sq_gather(m, 0, 0);
    }
    if (vel(m, 2) == 0) { gset(m, MR_Z_DIR, 0); return 1; }
    gset(m, MR_Z_DIR, vel(m, 2) > 0 ? 0x800 : 0xf800);
    pick_support(m, terrain);
    {
        int16_t zt = (int16_t)(uint16_t)((vel(m, 2) >> 1) * cws(m, 0x12));
        int16_t zp = (int16_t)(zt >> 5);
        zp = (int16_t)(zp < 0 ? -(uint16_t)zp : (uint16_t)zp);
        if (vel(m, axis) == 0) {
            gset(m, MR_Z_PER_TILE, (uint16_t)zp);
        } else {
            int32_t t1 = (int32_t)vel(m, 2) * (int32_t)(gw(m, (uint16_t)(MR_AXIS_STEP + axis * 2)) / 0x2000);
            int32_t t2 = (int32_t)vel(m, axis) * (int32_t)(gw(m, MR_Z_DIR) / 0x800);
            t1 = (int32_t)((uint32_t)t1 * 0x100u);
            t1 = t2 ? t1 / t2 : 0;
            if (t1 > 0x7fff || t1 < -0x8000) {
                gset(m, MR_STEP_REM, 0);
                gset(m, MR_Z_PER_TILE, (uint16_t)zp);
            } else {
                gset(m, MR_Z_PER_TILE, (uint16_t)t1);
            }
        }
    }
    return 1;
}

/* motion_recalc */
static void recalc(uw_motion *m, int terrain) {
    cww(m, 0x12, (uint16_t)(cws(m, 0x12) - (uint16_t)(gw(m, MR_TICKS_TILE) * gw(m, MR_COUNTER))));
    if (cws(m, 0x12) > 0 && calc_velocity(m, 0, terrain)) return;
    gset(m, MR_COUNTER, (uint16_t)(gw(m, MR_STEP_LIMIT) + 1));
}

/* motion_stop */
static void motion_stop(uw_motion *m) {
    cww(m, 8, 0); cww(m, 0xe, 0); cww(m, 6, 0); cww(m, 0xc, 0);
    cww(m, 0xa, 0); cww(m, 0x10, 0); cww(m, 0x14, 0);
    gset(m, MR_COUNTER, (uint16_t)(gw(m, MR_STEP_LIMIT) + 1));
}

/* motion_load_position */
static void load_position(uw_motion *m) {
    uint8_t *ds = m->ds;
    cww(m, 0, (uint16_t)((uint16_t)(qws(m, 0) << 5) + (gw(m, MR_FRAC_X) >> 8)));
    cww(m, 2, (uint16_t)((uint16_t)(qws(m, 2) << 5) + (gw(m, MR_FRAC_Y) >> 8)));
    cww(m, 4, (uint16_t)((uint16_t)(qws(m, 4) << 3) + (gw(m, MR_FRAC_Z) >> 8)));
    if (rw(ds, 0x2774) & 0x2000) {
        int16_t d = (int16_t)(qws(m, 4) - ds[0x2778]);
        if ((d < 0 ? -d : d) <= cb(m, 0x22) && gw(m, 0x2772) == 1 && cws(m, 0xa) == 0)
            cww(m, 4, tile_slope_height(m, cws(m, 0), cws(m, 2)));
    }
    cww(m, 0x1e, rw(ds, 0x276e));
}

/* motion_step_ground */
static int8_t step_ground(uw_motion *m, int8_t cx, int16_t si) {
    int axis = gw(m, MR_MAJOR), minor = gw(m, MR_MINOR);
    uint16_t fm = (uint16_t)(MR_FRAC_X + minor * 2), fa = (uint16_t)(MR_FRAC_X + axis * 2);
    if (gw(m, MR_STEP_LIMIT) + (si == -1 ? 1 : 0) > gw(m, MR_COUNTER)) {
        int16_t sm = gw(m, (uint16_t)(MR_AXIS_STEP + minor * 2));
        gset(m, fm, (uint16_t)(si == 1 ? gw(m, fm) + sm : gw(m, fm) - sm));
        if ((int16_t)(uint16_t)(gw(m, (uint16_t)(MR_AXIS_STEP + axis * 2)) * si) > 0)
            qww(m, axis * 2, (uint16_t)(qws(m, axis * 2) + 1));
        else
            qww(m, axis * 2, (uint16_t)(qws(m, axis * 2) - 1));
        if ((uint16_t)gw(m, fm) & 0xe000) {
            cx = 1;
            if (gw(m, fm) > 0) qww(m, minor * 2, (uint16_t)(qws(m, minor * 2) + 1));
            else qww(m, minor * 2, (uint16_t)(qws(m, minor * 2) - 1));
            gset(m, fm, (uint16_t)(gw(m, fm) & 0x1fff));
        }
        gset(m, MR_COUNTER, (uint16_t)(gw(m, MR_COUNTER) + si));
        return 1;
    }
    {
        int16_t part = (int16_t)(uint16_t)((gw(m, (uint16_t)(MR_AXIS_STEP + minor * 2)) >> 5)
                                           * (gw(m, MR_STEP_REM) >> 8));
        gset(m, fm, (uint16_t)(si == 1 ? gw(m, fm) + part : gw(m, fm) - part));
        if ((int16_t)(uint16_t)(gw(m, (uint16_t)(MR_AXIS_STEP + axis * 2)) * si) > 0)
            gset(m, fa, (uint16_t)(gw(m, fa) + gw(m, MR_STEP_REM)));
        else
            gset(m, fa, (uint16_t)(gw(m, fa) - gw(m, MR_STEP_REM)));
    }
    if ((uint16_t)gw(m, MR_FRAC_X) & 0xe000) {
        cx = 1;
        qww(m, 0, (uint16_t)(qws(m, 0) + (gw(m, MR_FRAC_X) > 0 ? 1 : -1)));
        gset(m, MR_FRAC_X, (uint16_t)(gw(m, MR_FRAC_X) & 0x1fff));
    }
    if ((uint16_t)gw(m, MR_FRAC_Y) & 0xe000) {
        cx = 1;
        qww(m, 2, (uint16_t)(qws(m, 2) + (gw(m, MR_FRAC_Y) > 0 ? 1 : -1)));
        gset(m, MR_FRAC_Y, (uint16_t)(gw(m, MR_FRAC_Y) & 0x1fff));
    }
    gset(m, MR_COUNTER, (uint16_t)(gw(m, MR_COUNTER) + si));
    return cx;
}

/* motion_turn_toward */
static int8_t turn_toward(uw_motion *m, uint16_t di) {
    uint8_t *ds = m->ds;
    int16_t si;
    if (cb(m, 0x17) & 0x40) return 0;
    if (cws(m, 0xa) != 0)
        cww(m, 0xa, (uint16_t)((cws(m, 0xa) / 16) * (cb(m, 0x16) + 1)));
    si = (int16_t)(uint16_t)(di - rw(ds, 0x276e));
    if (si > 0x4000 || si < -0x4000) {
        di = (uint16_t)(di + 0x8000);
        si = (int16_t)(uint16_t)((uint16_t)si + 0x8000);
    }
    if (cb(m, 0x17) & 0x80) {
        int16_t a = (int16_t)(si < 0 ? -(uint16_t)si : (uint16_t)si);
        if (a > 0x3000 && a < 0x5000) {
            cww(m, 0x26, (uint16_t)(cws(m, 0x26) + cws(m, 0x14)));
            return 0;
        }
        if (rw(ds, 0x276e) != di) {
            ww(ds, 0x276e, di);
        } else {
            uint8_t l4 = 1, l3 = (di & 0x4000) ? 1 : 0;
            if (rt_rand(m) % 2) { l3 = !l3; l4 = !l4; }
            gset(m, MR_FRAC_X, (uint16_t)(l3 * 0x1f00));
            gset(m, MR_FRAC_Y, (uint16_t)(l4 * 0x1f00));
        }
    } else {
        int16_t a = (int16_t)(si < 0 ? -(uint16_t)si : (uint16_t)si);
        if (a > 0x3000 && a < 0x5000)
            ww(ds, 0x276e, (uint16_t)(di + si));
        else
            ww(ds, 0x276e, (uint16_t)(di + (uint16_t)((si / 15) * cb(m, 0x16))));
    }
    if (!(cb(m, 0x17) & 0x80)) {
        cww(m, 0x26, (uint16_t)(cws(m, 0x26) + (int16_t)((int16_t)(uint16_t)(cws(m, 0x14) * (15 - cb(m, 0x16))) / 15)));
        cww(m, 0x14, (uint16_t)((int16_t)(uint16_t)(cws(m, 0x14) * cb(m, 0x16)) / 15));
    }
    cww(m, 0x1e, rw(ds, 0x276e));
    return 1;
}

/* motion_try_slide */
static void try_slide(uw_motion *m, int arg, uint16_t bp) {
    uint8_t *ds = m->ds;
    int si;
    if ((int8_t)ds[0x277f] > 0) {
        motion_substep(m, -1, (uint16_t)(bp - 2 - 2 - 4 - 2));
        gset(m, MR_COUNTER, (uint16_t)(gw(m, MR_STEP_LIMIT) + 1));
        return;
    }
    si = gw(m, MR_MAJOR) << 1;
    if (arg) {
        sq_corner_walk(m);
        if (ds[0x277a] != 9) si = ds[0x277a];
    }
    motion_substep(m, -1, (uint16_t)(bp - 2 - 2 - 4 - 2));
    if (turn_toward(m, rw(ds, (uint16_t)(MR_DIRS + si * 2)))) {
        recalc(m, 1);
        ds[0x277f] = 2;
        return;
    }
    gset(m, MR_COUNTER, (uint16_t)(gw(m, MR_STEP_LIMIT) + 1));
}

/* motion_bounce */
static void bounce(uw_motion *m) {
    cww(m, 0xa, 0xeb);
    cww(m, 0x10, 0xfffc);
    if (cws(m, 0x14) < 0xeb) cww(m, 0x14, 0xeb);
    m->ds[(uint16_t)(ctx(m) + 0x25)] = 0x10;
    cww(m, 0x1e, (uint16_t)(cws(m, 0x1e) - 0x3000));
    cww(m, 0x1e, (uint16_t)(cws(m, 0x1e) + (int16_t)(rt_rand(m) % 0x6000)));
}

/* motion_bounce_vertical */
static void bounce_vertical(uw_motion *m, uint16_t bp) {
    uint8_t *ds = m->ds;
    uint16_t mover = obj_at(m, (uint16_t)gw(m, 0x2772));
    uint16_t id = obj_id(m, mover);
    int16_t mass = (int16_t)((rw(ds, (uint16_t)(OBJ_PROPERTIES + id * 11 + 1)) >> 4) & 0xfff);
    uint16_t hit;
    int16_t loud = (int16_t)((int16_t)(uint16_t)(mass - 600) / 0x32);   /* the weight's share of the volume */
    if (gw(m, MR_Z_PER_TILE) > 4) {
        int16_t d = (int16_t)(qws(m, 4) - gw(m, MR_Z_BOUND));
        int16_t ad = (int16_t)(d < 0 ? -(uint16_t)d : (uint16_t)d);
        int16_t di = (int16_t)(uint16_t)(ad * gw(m, MR_TICKS_TILE));
        di = (int16_t)((int16_t)(uint16_t)(di << 4) / (int16_t)(gw(m, MR_Z_PER_TILE) / 4));
        cww(m, 0x12, (uint16_t)(cws(m, 0x12) - di));
    } else {
        cww(m, 0x12, 0);
    }
    qww(m, 4, (uint16_t)gw(m, MR_Z_BOUND));
    gset(m, MR_FRAC_Z, 0);
    if (ds[MR_HIT_SLOT] == 0xff && (rw(ds, 0x2774) & 1)
        && ds[0x2778] + ds[0x2770] >= qws(m, 4) && cws(m, 0xa) < 0) {
        motion_stop(m);
        ds[(uint16_t)(ctx(m) + 0x25)] = 2;
        /* the landing: effect 5 where the mover is, its fine x and y >> 5 */
        play_sound_effect_at_xy(m, 5, (int16_t)(cws(m, 0) >> 5), (int16_t)(cws(m, 2) >> 5), (int8_t)(uint8_t)loud);
        return;
    }
    /* the bounce: effect 15, louder by a tenth of the fall and the weight */
    play_sound_effect_at_xy(m, 0xf, (int16_t)(cws(m, 0) >> 5), (int16_t)(cws(m, 2) >> 5),
                            (int8_t)(uint8_t)((cws(m, 0xa) < 0 ? -cws(m, 0xa) : cws(m, 0xa)) / 10 + loud - 0x28));
    hit = collision_check(m, (int8_t)ds[MR_HIT_SLOT], (uint16_t)gw(m, 0x2772), (uint16_t)(bp - 0xa - 4 - 4 - 4 - 2));
    if (hit & 0x18) {
        if (hit & 0x10) motion_stop(m);
        else gset(m, MR_COUNTER, (uint16_t)(gw(m, MR_STEP_LIMIT) + 1));
        return;
    }
    if (hit & 4) {
        uint8_t rising = cws(m, 0xa) <= 0;
        cww(m, 0xa, (uint16_t)(-(cws(m, 0xa) / 15)));
        {
            int16_t t = (int16_t)(uint16_t)(cws(m, 0xa) * (15 - cb(m, 0x16)));
            cww(m, 0x26, (uint16_t)(t < 0 ? -(uint16_t)t : (uint16_t)t));
        }
        cww(m, 0xa, (uint16_t)(cws(m, 0xa) * cb(m, 0x16)));
        if (cb(m, 0x16))
            cww(m, 0x14, (uint16_t)(cws(m, 0x14) - (int16_t)((int16_t)(uint16_t)((15 - cb(m, 0x16)) * cws(m, 0x14)) / 30)));
        else
            cww(m, 0x14, 0);
        if (rising && cws(m, 0xa) < 0x8d) {
            cww(m, 0xa, 0);
            cww(m, 0x10, 0);
            if (ds[MR_HIT_SLOT] != 0xff) {
                uint16_t o = obj_at(m, (uint16_t)((rec_link(m, (int8_t)ds[MR_HIT_SLOT]) >> 6) & 0x3ff));
                if ((prop(m, obj_id(m, o), 3) >> 1) & 1) {
                    ds[(uint16_t)(ctx(m) + 0x25)] = 1;
                } else if (((rw(m->lseg, obj_at(m, (uint16_t)cws(m, 0x20))) & 0x1c0) >> 6) != 1) {
                    bounce(m);
                } else {
                    ds[(uint16_t)(ctx(m) + 0x25)] = 1;
                }
            } else if (ds[0x2778] + ds[0x2770] >= qws(m, 4)) {
                ds[(uint16_t)(ctx(m) + 0x25)] = (uint8_t)(1 << (ds[0x2774] & 3));
            } else if (((rw(m->lseg, obj_at(m, (uint16_t)cws(m, 0x20))) & 0x1c0) >> 6) != 1) {
                bounce(m);
            } else if (rw(ds, 0x2776) & 0x10) {
                ds[(uint16_t)(ctx(m) + 0x25)] = 2;
            } else if (rw(ds, 0x2776) & 0x20) {
                ds[(uint16_t)(ctx(m) + 0x25)] = 4;
            } else {
                ds[(uint16_t)(ctx(m) + 0x25)] = 1;
            }
        }
    } else if (ds[MR_HIT_SLOT] != 0xff) {
        /* The record hit is dropped: the last one copied over it. */
        ds[0x277c] = (uint8_t)(ds[0x277c] - 1);
        memmove(ds + RESULTS + (int8_t)ds[MR_HIT_SLOT] * 6, ds + RESULTS + ds[0x277c] * 6, 6);
    }
    recalc(m, 0);
}

/* motion_step_airborne */
static int8_t step_airborne(uw_motion *m, int8_t arg, int16_t di, uint16_t bp) {
    int32_t t;
    int16_t si;
    if (gw(m, MR_STEP_LIMIT) + (di == -1 ? 1 : 0) > gw(m, MR_COUNTER)) {
        int16_t z = (int16_t)(uint16_t)(gw(m, MR_Z_PER_TILE) << 5);
        t = (int16_t)(uint16_t)(di * gw(m, MR_Z_DIR)) > 0 ? (int32_t)z : (int32_t)(int16_t)(uint16_t)(-(uint16_t)z);
    } else if (gw(m, MR_STEP_REM) != 0) {
        int16_t k = (int16_t)(uint16_t)((int16_t)(uint16_t)(di * (gw(m, MR_Z_DIR) / 0x800)) * gw(m, MR_Z_PER_TILE));
        t = (int32_t)((uint32_t)(int32_t)gw(m, MR_STEP_REM) * (uint32_t)(int32_t)k);
        t = t / 0x100;
    } else {
        int16_t k = (int16_t)(uint16_t)((int16_t)(uint16_t)(di * (gw(m, MR_Z_DIR) / 0x800)) * gw(m, MR_Z_PER_TILE));
        t = (int32_t)((uint32_t)0x40 * (uint32_t)(int32_t)k);
    }
    t += gw(m, MR_FRAC_Z);
    if (t >= 0) si = (int16_t)(t / 0x800);
    else si = (int16_t)(-((-t) / 0x800 + 1));      /* labs, ldiv, inc, neg */
    gset(m, MR_FRAC_Z, (uint16_t)(t & 0x7ff));
    if (di == -1) {
        qww(m, 4, (uint16_t)(qws(m, 4) + si));
    } else if (si > 0) {
        m->ds[(uint16_t)(ctx(m) + 0x25)] = 0x10;
        if (qws(m, 4) + si > gw(m, MR_Z_BOUND)) { bounce_vertical(m, (uint16_t)(bp - 4 - 4 - 4 - 2)); return 0; }
        qww(m, 4, (uint16_t)(qws(m, 4) + si));
    } else if (si < 0) {
        m->ds[(uint16_t)(ctx(m) + 0x25)] = 0x10;
        if (qws(m, 4) + si < gw(m, MR_Z_BOUND)) { bounce_vertical(m, (uint16_t)(bp - 4 - 4 - 4 - 2)); return 0; }
        qww(m, 4, (uint16_t)(qws(m, 4) + si));
    }
    return step_ground(m, arg, di);
}

/* motion_collision_face */
uint8_t collision_face(uw_motion *m, uint16_t f) {
    if (f & 0x1000) return 0x10;
    if (f & 4) {
        if (gw(m, 0x2772) == 1 && (f & 3) == 1 && (f & 0xf8) != (f & 0x90)) return 0x20;
        return (uint8_t)(1 << (f & 3));
    }
    if (f & 0x88) return 1;
    if (f & 0x10) return 2;
    if (f & 0x20) return 4;
    return 8;
}

/* motion_collision_flags */
static uint16_t collision_flags(uw_motion *m, uint16_t bp) {
    uint8_t *ds = m->ds;
    uint16_t si = 0;
    uint8_t l3 = 0, l4, l5, l6 = 0;
    uint16_t filt = rw(ds, MR_FILTER);
    int di;
    l4 = (rw(ds, (uint16_t)(filt + 4)) & 0x80) ? 0 : 1;
    ds[MR_Z_SETTLED] = 0;
    sq_terrain(m, cb(m, 0x24));
    sq_gather(m, 0, 0);
    pick_support(m, 0);
    si = (uint16_t)(rw(ds, 0x2774) | rw(ds, 0x2776));
    l5 = (si & rw(ds, (uint16_t)(filt + 4))) ? 0 : 1;
    if (ds[0x277c] > 0) {
        for (di = (int8_t)ds[0x277e]; (int8_t)ds[0x277e] + ds[0x277d] > di; di++) {
            uint16_t r = collision_check(m, di, (uint16_t)gw(m, 0x2772), (uint16_t)(bp - 6 - 4 - 4 - 4 - 2));
            if (r & 4) si |= 0x400;
            if (r & 0x18) return (uint16_t)(si | ((r & 0x10) ? 0x4000 : 0x8000));
        }
    }
    if (qws(m, 4) != gw(m, MR_Z_BOUND)) {
        if (ds[MR_HIT_SLOT] == 0xff && l5) {
            int16_t d = (int16_t)(qws(m, 4) - gw(m, MR_Z_BOUND));
            l3 = (d < 0 ? -d : d) <= cb(m, 0x24);
            if (!l3) {
                if (cws(m, 0xa) == 0 && !(rw(ds, 0x2776) & 0x800) && (rw(ds, 0x2774) & 4)) l3 = 1;
                else l3 = 0;
            } else {
                l3 = 1;
            }
        } else if (l4) {
            l3 = ((prop(m, (uint16_t)gw(m, MR_HIT_ITEM), 3) >> 1) & 1) == 1;
            if (l3) {
                int16_t d = (int16_t)(qws(m, 4) - rec_top(m, (int8_t)ds[MR_HIT_SLOT]));
                l3 = (d < 0 ? -d : d) <= cb(m, 0x24);
                if (l3) si |= 0x80;
            }
        }
        l3 = l3 && ds[MR_TERRAIN_BOUND] && l6 == 0;
        if (l3 && gw(m, MR_Z_BOUND) >= gw(m, MR_SUPPORT)) {
            si &= 0xfeff;
        } else if (!l3 && ds[0x2779] > qws(m, 4)) {
            si |= 0x100;
        }
        if (l3 && gw(m, MR_Z_BOUND) + cb(m, 0x23) > 0x7f) {
            l3 = 0;
            ds[MR_Z_SETTLED] = 1;
            si |= 0x200;
        } else if (l3 && ds[MR_HIT_SLOT] == 0xff && gw(m, MR_Z_BOUND) + cb(m, 0x23) > gw(m, MR_SUPPORT)) {
            l3 = 0;
            ds[MR_Z_SETTLED] = 1;
            si |= 0x400;
        }
        if (!(l3 && (si & 0x400)) && l3 && ds[MR_HIT_SLOT] != 0xff) {
            if (!((prop(m, (uint16_t)gw(m, MR_HIT_ITEM), 3) >> 1) & 1) || l4 == 0) l3 = 0;
        }
        if (l3) {
            int16_t d;
            ds[MR_Z_SETTLED] = 1;
            qww(m, 4, (uint16_t)gw(m, MR_Z_BOUND));
            d = (int16_t)(gw(m, MR_Z_BOUND) - ds[0x2778]);
            if ((d < 0 ? -d : d) <= cb(m, 0x22)) si |= 4;
            else si &= 0xfffb;
        }
    } else if (ds[MR_HIT_SLOT] != 0xff) {
        if (((prop(m, (uint16_t)gw(m, MR_HIT_ITEM), 3) >> 1) & 1) == 1 && l4) {
            si |= 0x80;
            si &= 0xfffb;
        }
    }
    if (ds[MR_Z_SETTLED] && l3) {
        sq_sort(m);
        si &= 0xfbff;
        for (di = 0; ds[0x277d] > di; di++)
            if (collision_check(m, di, (uint16_t)gw(m, 0x2772), (uint16_t)(bp - 6 - 4 - 4 - 4 - 2)) & 4) si |= 0x400;
    }
    if ((si & 0x80) && l4) {
        if ((ds[(uint16_t)(RESULTS + (int8_t)ds[MR_HIT_SLOT] * 6 + 2)] & 0x3f & 0x10)
            || (rw(ds, 0x2774) & 4))
            si &= 0xf7ff;
    }
    if ((rw(ds, 0x2776) & 0x100) && l5 && cws(m, 0xa) == 0
        && qws(m, 4) + cb(m, 0x24) >= ds[0x2779]) {
        si &= 0xfeff;
        qww(m, 4, ds[0x2779]);
        if (qws(m, 4) == ds[0x2778]) si |= 4;
        else si &= 0xfffb;
    }
    if (!(si & 0xfc)) si |= 0x1000;
    if (!(si & 0x80) && qws(m, 4) - cb(m, 0x22) > ds[0x2779]) si |= 0x1000;
    return si;
}

/* motion_substep */
int8_t motion_substep(uw_motion *m, int16_t di, uint16_t bp) {
    uint8_t *ds = m->ds;
    int8_t settled = 0, r;
    if (di == -1) {
        sq_gather(m, 0, 0);
        pick_support(m, 0);
        ds[(uint16_t)(ctx(m) + 0x25)] = ds[MR_FLAGS_SAVED];
        if (ds[MR_Z_SETTLED]) settled = 1;
    } else {
        ds[0x277f] = (uint8_t)(ds[0x277f] - 1);
    }
    r = cws(m, 0xa) != 0 ? step_airborne(m, 0, di, (uint16_t)(bp - 4 - 4 - 4 - 2)) : step_ground(m, 0, di);
    if (settled) {
        uint16_t f = collision_flags(m, (uint16_t)(bp - 4 - 4 - 2));
        ds[MR_FLAGS_SAVED] = cb(m, 0x25);
        ds[(uint16_t)(ctx(m) + 0x25)] = collision_face(m, f);
    }
    return r;
}

/* ==== a creature's tick === */

/* obj_index_from_ptr */
uint16_t obj_index_of(uw_motion *m, uint16_t o) {
    if (o == 0) return 0;
    if (o < rw(m->ds, STATIC_BASE)) return (uint16_t)((uint16_t)(o - rw(m->ds, MOBILE_BASE)) / 0x1b);
    return (uint16_t)((uint16_t)(o - rw(m->ds, STATIC_BASE)) / 8 + 0x100);
}

/* motion_find_blocking_door: the first closed door (ids
 * 0x140..0x147) among the query's group of results, its tile written out. */
static uint16_t find_blocking_door(uw_motion *m) {
    uint8_t *ds = m->ds;
    int si;
    for (si = 0; si < ds[0x277d]; si++) {
        int r = (int8_t)ds[0x277e] + si;
        uint16_t rec = (uint16_t)(RESULTS + r * 6);
        uint16_t obj = obj_at(m, (uint16_t)(rw(ds, (uint16_t)(rec + 2)) >> 6));
        uint16_t id = obj_id(m, obj);
        int16_t local = (int16_t)(rw(ds, (uint16_t)(rec + 4)) & 0x3f);
        ds[AI_DOOR_TILE_X] = (uint8_t)(((rs(ds, Q) >> 3) + local) & 0x3f);
        local = (int16_t)(ds[AI_DOOR_TILE_X] - (rs(ds, Q) >> 3));
        ds[AI_DOOR_TILE_Y] = (uint8_t)(((rs(ds, Q + 2) >> 3)
                                        + (int16_t)(rs(ds, (uint16_t)(rec + 4)) - local) / 0x40) & 0x3f);
        if ((id >> 4) == 0x14 && (id & 0xf) < 8) return obj;
    }
    return 0;
}

/* motion_blocking_object */
static uint16_t blocking_object(uw_motion *m) {
    uint8_t *ds = m->ds;
    if (!ds[0x277d]) return 0;
    return obj_at(m, (uint16_t)(rw(ds, (uint16_t)(RESULTS + (int8_t)ds[0x277e] * 6 + 2)) >> 6));
}

static void set_far(uint8_t *ds, uint16_t at, uint16_t off, uint16_t seg) {
    ww(ds, at, off);
    ww(ds, (uint16_t)(at + 2), off ? seg : 0);
}

/* motion_filter_walker: what a walking creature does with the
 * collision flags motion_run could not settle. 1 stops the motion. */
static int8_t filter_walker(uw_motion *m, uint16_t f) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t npc = rw(ds, CURRENT_NPC), seg = rw(ds, (uint16_t)(CURRENT_NPC + 2));
    if (f & 0x1000) {
        if (rw(ds, BLOCK_WALKER + 0x10) == 0) ww(ds, BLOCK_WALKER + 0x10, 0xfffc);
        ls[(uint16_t)(npc + 0x14)] = (uint8_t)((ls[(uint16_t)(npc + 0x14)] & 0xf8) | 1);
        ds[AI_MOTION_BLOCKED] = 1;
        ds[AI_MAY_MOVE] = 0;
        return 0;
    }
    if (f & 0x10) {
        if ((f & 0xf8) == 0x10) {
            /* Water and nothing else: the splash, and the creature drowns --
             * action 0xc, dying, at its last frame (3). */
            ds[AI_MOTION_BLOCKED] = 1;
            ds[AI_MAY_MOVE] = 0;
            spawn_class7(m, npc, 6, 3, 0, 0, (uint8_t)((int16_t)rw(ds, BLOCK_WALKER) >> 8),
                         (uint8_t)((int16_t)rw(ds, (uint16_t)(BLOCK_WALKER + 2)) >> 8));
            ls[(uint16_t)(npc + 0x15)] = (uint8_t)((ls[(uint16_t)(npc + 0x15)] & 0xc0) | 0xc);
            ww(ls, (uint16_t)(npc + 0xb), (uint16_t)((rw(ls, (uint16_t)(npc + 0xb)) & 0xfff) | 0x3000));
            ls[(uint16_t)(npc + 0x14)] = (uint8_t)((ls[(uint16_t)(npc + 0x14)] & 0xf8) | 1);
            return 1;
        }
        if (!(ls[(uint16_t)(npc + 0x15)] & 0x80)) {
            ds[AI_MOTION_BLOCKED] = 1;
            ww(ds, BLOCK_WALKER + 8, 0);
            ww(ds, BLOCK_WALKER + 6, 0);
            return 1;
        }
    }
    if ((f & 0x800) && !(rw(ds, AI_TERRAIN_FLAGS) & 0x800)) {
        if (ls[(uint16_t)(npc + 0x15)] & 0x80) return 0;
        ds[AI_MOTION_BLOCKED] = 1;
        ww(ds, BLOCK_WALKER + 8, 0);
        ww(ds, BLOCK_WALKER + 6, 0);
        return 1;
    }
    if ((f & 0x20) && !(rw(ds, AI_TERRAIN_FLAGS) & 0x20)) {
        if (ls[(uint16_t)(npc + 0x15)] & 0x80) return 0;
        ds[AI_MOTION_BLOCKED] = 1;
        ww(ds, BLOCK_WALKER + 8, 0);
        ww(ds, BLOCK_WALKER + 6, 0);
        return 1;
    }
    if (f & 0x300) {
        ds[AI_MOTION_BLOCKED] = 1;
        return 0;
    }
    if (f & 0x400) {
        uint16_t door = find_blocking_door(m);
        if (door) {
            set_far(ds, AI_MOTION_BLOCKER, door, seg);
            ds[AI_WALKER_BLOCKED] = 1;
            ds[AI_MOTION_BLOCKED] = 1;
            ds[AI_BLOCKED_ANY] = 1;
        } else {
            ds[AI_MOTION_BLOCKED] = 1;
            ds[AI_BLOCKED_ANY] = 1;
            set_far(ds, AI_MOTION_BLOCKER, blocking_object(m), seg);
        }
    }
    return (int8_t)(ds[AI_MOTION_BLOCKED] && ds[AI_MAY_MOVE]);
}

/* motion_filter_flier: a flier reports a collision and keeps
 * going, and a lip under it lifts it (+0x0a of its block, the vertical
 * velocity, to 0x80). */
static int8_t filter_flier(uw_motion *m, uint16_t f) {
    uint8_t *ds = m->ds;
    ds[AI_MAY_MOVE] = 1;
    if (f & 0x200) {
        ds[AI_MOTION_BLOCKED] = 1;
        return 0;
    }
    if (f & 0x100) {
        ww(ds, BLOCK_FLIER + 0xa, 0x80);
        ds[AI_FLIER_STEP] = 1;
    }
    if (f & 0x400) {
        ds[AI_MOTION_BLOCKED] = 1;
        ds[AI_BLOCKED_ANY] = 1;
        set_far(ds, AI_MOTION_BLOCKER, blocking_object(m), rw(ds, (uint16_t)(CURRENT_NPC + 2)));
    }
    return (int8_t)(ds[AI_MOTION_BLOCKED] && ds[AI_MAY_MOVE]);
}

/* motion_filter_swimmer: a swimmer stops dead. */
static int8_t filter_swimmer(uw_motion *m, uint16_t f) {
    uint8_t *ds = m->ds;
    if (f & 0x300) {
        ww(ds, BLOCK_SWIMMER + 8, 0);
        ww(ds, BLOCK_SWIMMER + 6, 0);
        ds[AI_MOTION_BLOCKED] = 1;
        return 0;
    }
    if (f & 0x400) {
        ww(ds, BLOCK_SWIMMER + 8, 0);
        ww(ds, BLOCK_SWIMMER + 6, 0);
        ds[AI_MOTION_BLOCKED] = 1;
        ds[AI_BLOCKED_ANY] = 1;
        set_far(ds, AI_MOTION_BLOCKER, blocking_object(m), rw(ds, (uint16_t)(CURRENT_NPC + 2)));
    }
    if (f & 8) {
        ww(ds, BLOCK_SWIMMER + 8, 0);
        ww(ds, BLOCK_SWIMMER + 6, 0);
        ds[AI_MOTION_BLOCKED] = 1;
    }
    return (int8_t)(ds[AI_MOTION_BLOCKED] && ds[AI_MAY_MOVE]);
}

/* The filter descriptor's far callback: motion_filter_player for the
 * player, motion_filter_walker for a walking creature. */
static int8_t filter_callback(uw_motion *m, uint16_t *flags) {
    uint8_t *ds = m->ds;
    uint16_t filt = rw(ds, MR_FILTER);
    uint16_t off = rw(ds, (uint16_t)(filt + 8)), seg = rw(ds, (uint16_t)(filt + 10));
    if (off == 0x0bb3 && (seg == 0x2161 || seg == 0x293d)) {
        /* The player's: `mov bx,[bp+6]; test word [bx],0x1000`
         * -- the collision flags the response passes by pointer, bit 12 no
         * support. A player moving under three tenths of full speed who
         * would step off a drop stops instead. */
        if ((*flags & 0x1000) && rw(ds, VERTICAL_VELOCITY) == 0
            && (int16_t)(uint16_t)(rw(ds, MOVEMENT_SPEED) * 10) < (int16_t)(uint16_t)(rw(ds, SPEED_MAX_FORWARD) * 3)) {
            ww(ds, 0x2786, 0);
            ww(ds, 0x2788, 0);
            return 1;
        }
        return 0;
    }
    if (off == 0x0431 && (seg == 0x1aae || seg == 0x228a))
        return filter_walker(m, *flags);
    if (off == 0x05f7 && (seg == 0x1aae || seg == 0x228a))
        return 0;                           /* a thrown object's: `mov al,0` */
    if (off == 0x05fe && (seg == 0x1aae || seg == 0x228a))
        return filter_flier(m, *flags);
    if (off == 0x065e && (seg == 0x1aae || seg == 0x228a))
        return filter_swimmer(m, *flags);
    UW_NOT_CARRIED(m->not_carried);
    return 0;
}

/* motion_collision_response */
static void collision_response(uw_motion *m, uint16_t bp) {
    uint8_t *ds = m->ds;
    uint8_t slid = 0;
    uint16_t f = collision_flags(m, (uint16_t)(bp - 4 - 4 - 2)), filt;
    ds[MR_FLAGS_SAVED] = cb(m, 0x25);
    ds[(uint16_t)(ctx(m) + 0x25)] = collision_face(m, f);
    if (f & 0xc000) {
        motion_substep(m, -1, (uint16_t)(bp - 4 - 2 - 4 - 2));
        if (f & 0x4000) motion_stop(m);
        else gset(m, MR_COUNTER, (uint16_t)(gw(m, MR_STEP_LIMIT) + 1));
        return;
    }
    filt = rw(ds, MR_FILTER);
    f &= (uint16_t)~rw(ds, filt);
    if (f == 0) return;
    if ((f & rw(ds, (uint16_t)(filt + 2))) && filter_callback(m, &f)) {
        motion_substep(m, -1, (uint16_t)(bp - 4 - 2 - 4 - 2));
        gset(m, MR_COUNTER, (uint16_t)(gw(m, MR_STEP_LIMIT) + 1));
        return;
    }
    if (f & 0x700) {
        try_slide(m, (f & 0x400) ? 0 : 1, (uint16_t)(bp - 4 - 2 - 4 - 2));
        slid = 1;
    }
    if ((f & 0x1000) && cws(m, 0x10) == 0) {
        cww(m, 0x10, 0xfffc);
        recalc(m, slid);
    }
}

/* motion_run */
void motion_run(uw_motion *m, uint16_t block, uint16_t filter, uint16_t bp) {
    uint8_t *ds = m->ds;
    int8_t n = 0;
    ww(ds, MR_CTX, block);
    ww(ds, MR_FILTER, filter);
    ww(ds, MR_VELOCITY, (uint16_t)(block + 6));
    ds[MR_FLAGS_SAVED] = ds[(uint16_t)(block + 0x25)];
    ds[0x277f] = 0;
    if (!calc_velocity(m, 1, 1)) return;
    while (gw(m, MR_COUNTER) < gw(m, MR_STEP_LIMIT) + 1) {
        if (n++ == 0x10) {
            cww(m, 0x10, 0);
            cww(m, 0xa, 0);
            cww(m, 0x14, 0);
            return;
        }
        if (motion_substep(m, 1, (uint16_t)(bp - 2 - 2 - 4 - 2))) collision_response(m, (uint16_t)(bp - 2 - 4 - 2));
    }
    load_position(m);
}
