/* SPDX-License-Identifier: MIT */
/* a creature's paths: the line walk (npc_goto's straight-line test),
 * following a packed path, and the path search pathfind_between_tiles
 * with its slots.
 *
 * One of src/uw_motion*.c: see uw_motion.h and uw_motion_int.h. */
#include "uw_motion_int.h"

/* ---- the line walk: npc_goto's straight-line test ------------------------ */

/* The path cost byte traverse_multiple_tiles steps. path_append_tile and
 * tile_line_walk pass a local they never set, whatever the stack held: a
 * step through theirs is counted, and taken from 0. pathfind_between_tiles
 * sets its own, and is not. */
static uint8_t *cost_garbage(uw_motion *m, uint8_t *cost, int known) {
    if (!known) UW_NOT_CARRIED(m->not_carried);
    return cost;
}

/* traverse_multiple_tiles, from the instructions: may a mover
 * pass from (px,py) through (cx,cy) to (nx,ny)? A zero px means there is
 * no previous tile, a zero nx no next. `floor` is the height carried in,
 * *out the height carried on, *cost the running cost against
 * PATH_COST_LIMIT. f4 and f6 are the mover's filter descriptor words +4 and
 * +6. */
static int8_t traverse_tiles(uw_motion *m, uint8_t px, uint8_t py, uint8_t cx, uint8_t cy,
                             uint8_t nx, uint8_t ny, uint16_t f4, uint16_t f6, uint8_t floor,
                             uint8_t *out, uint8_t *cost, int cost_known) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t tc = tile_ptr(m, cx, cy), tp = tile_ptr(m, px, py), tn = tile_ptr(m, nx, ny);
    uint8_t typc = (uint8_t)(ls[tc] & 0xf), typn = (uint8_t)(ls[tn] & 0xf);
    uint16_t kc = (uint16_t)(rw(ds, (uint16_t)(TERRAIN_KINDS + ((ls[(uint16_t)(tc + 1)] >> 2) & 0xf) * 2)) >> 4);
    uint16_t kn = (uint16_t)(rw(ds, (uint16_t)(TERRAIN_KINDS + ((ls[(uint16_t)(tn + 1)] >> 2) & 0xf) * 2)) >> 4);
    uint8_t Tn = ds[(uint16_t)(TILE_BLOCK_TABLE + typn)], Tc = ds[(uint16_t)(TILE_BLOCK_TABLE + typc)];
    uint8_t l13, l14, l15, l16 = 0;
    int l17 = 0, l18 = 0, l21 = 0, si = 0;
    uint16_t kcbit = (uint16_t)(8u << (kc & 0x1f)), knbit = (uint16_t)(8u << (kn & 0x1f));
    ds[PATH_CLIMB] = 0;
#define SLOPE_BUMP(f) do { \
        if (typn >= 6 && typn <= 9) { \
            uint16_t ix = (uint16_t)((uint16_t)(nx - cx) * 3 + (uint16_t)(ny - cy)); \
            if (ds[(uint16_t)(SLOPE_TYPE + ds[(uint16_t)(SLOPE_INDEX + ix)])] != typn) (f)++; \
        } } while (0)
    if (px == 0) {
        uint8_t fn = (uint8_t)((ls[tn] >> 4) & 0xf);
        *out = floor;
        if (nx > cx && (Tn & 2)) return 0;
        if (nx < cx && (Tn & 4)) return 0;
        if (ny > cy && (Tn & 8)) return 0;
        if (ny < cy && (Tn & 0x10)) return 0;
        if (nx > cx && (Tc & 4)) return 0;
        if (nx < cx && (Tc & 2)) return 0;
        if (ny > cy && (Tc & 0x10)) return 0;
        if (ny < cy && (Tc & 8)) return 0;
        if (!(f4 & 0x1000)) return 1;
        SLOPE_BUMP(fn);
        if (fn > floor + 1) return 0;
        return 1;
    }
    if (nx == 0) {
        uint16_t link = (uint16_t)(tc + 2);
        uint8_t fc;
        *out = floor;
        if (!(f4 & 0x1000)) return 1;
        while (((rw(ls, link) >> 6) & 0x3ff) && l16 == 0) {
            uint16_t o = obj_at(m, (uint16_t)(rw(ls, link) >> 6));
            uint16_t id = obj_id(m, o);
            if ((prop(m, id, 3) >> 1) & 1)
                l16 = (uint8_t)(((rw(ls, (uint16_t)(o + 2)) & 0x7f) + prop(m, id, 0)) >> 3);
            link = (uint16_t)(o + 4);
        }
        fc = (uint8_t)((ls[tc] >> 4) & 0xf);
        if (l16 > fc) { fc = l16; l18 = 1; }
        if (((ls[tp] >> 4) & 0xf) > fc + 1) {
            *out = fc;
            *cost_garbage(m, cost, cost_known) = (uint8_t)(floor - *out + *cost - 1);
            if (*cost > ds[PATH_COST_LIMIT]) return 0;
        }
        if (!l18) {
            if (kcbit & f4) return 0;
            if (kcbit & f6) {
                *cost_garbage(m, cost, cost_known) = (uint8_t)(*cost + 2);
                if (*cost > ds[PATH_COST_LIMIT]) return 0;
            }
        }
        return 1;
    }
    *out = floor;
    if (nx > cx) {
        if ((Tn & 2) || (Tc & 4)) return 0;
    } else if (nx < cx) {
        if ((Tn & 4) || (Tc & 2)) return 0;
    } else if (ny > cy) {
        if ((Tn & 8) || (Tc & 0x10)) return 0;
    } else if (ny < cy) {
        if ((Tn & 0x10) || (Tc & 8)) return 0;
    }
    {
        uint16_t link = (uint16_t)(tc + 2);
        while (((rw(ls, link) >> 6) & 0x3ff) && l16 == 0) {
            uint16_t o = obj_at(m, (uint16_t)(rw(ls, link) >> 6));
            uint16_t w0 = rw(ls, o), id = (uint16_t)(w0 & 0x1ff), w2 = rw(ls, (uint16_t)(o + 2));
            if (((w0 & 0x1c0) >> 6) == 5 && ((w0 & 0x30) >> 4) == 0 && (w0 & 0xf) < 8) {
                /* A closed door: it blocks exactly the moves that cross its
                 * panel, by the heading's two low bits and its place in
                 * the tile. */
                uint8_t dl = (uint8_t)(((w2 & 0x380) >> 7) & 3);
                uint8_t fx = (uint8_t)(w2 >> 13), fy = (uint8_t)((w2 & 0x1c00) >> 10);
                if (!l21) {
                    if (px < cx) si = cy < ny ? 0 : cy > ny ? 2 : 1;
                    else if (px > cx) si = cy < ny ? 3 : cy > ny ? 5 : 4;
                    else if (py < cy) si = cx < nx ? 8 : cx > nx ? 6 : 7;
                    else si = cx < nx ? 0xb : cx > nx ? 9 : 0xa;
                    l21 = 1;
                }
                switch (si) {
                case 7: case 10: if (dl != 2) return 0; break;
                case 1: case 4: if (dl != 0) return 0; break;
                case 0: case 9:
                    if (dl == 0 && fy > 3) return 0;
                    if (dl == 1) return 0;
                    if (dl == 2 && fx < 4) return 0;
                    break;
                case 2: case 6:
                    if (dl == 0 && fy < 4) return 0;
                    if (dl == 2 && fx < 4) return 0;
                    if (dl == 3) return 0;
                    break;
                case 3: case 11:
                    if (dl == 0 && fy > 3) return 0;
                    if (dl == 2 && fx > 3) return 0;
                    if (dl == 3) return 0;
                    break;
                case 5: case 8:
                    if (dl == 0 && fy < 4) return 0;
                    if (dl == 1) return 0;
                    if (dl == 2 && fx > 3) return 0;
                    break;
                default: break;
                }
            } else if ((prop(m, id, 3) >> 1) & 1) {
                l16 = (uint8_t)(((w2 & 0x7f) + prop(m, id, 0)) >> 3);
            }
            link = (uint16_t)(o + 4);
        }
    }
    if (!(f4 & 0x1000)) {
        *out = (uint8_t)(16 - ((ds[AI_SELF_HEIGHT] + 3) >> 2));
        return 1;
    }
    {
        uint8_t fcur = (uint8_t)((ls[tc] >> 4) & 0xf), fprev = (uint8_t)((ls[tp] >> 4) & 0xf);
        uint8_t fnext = (uint8_t)((ls[tn] >> 4) & 0xf);
        l13 = fcur > fprev ? fcur : fprev;
        l14 = fcur > fnext ? fcur : fnext;
        l15 = l14;
        if (floor > l13) l13 = floor;
        SLOPE_BUMP(l14);
        if (((l13 > l14 ? l13 : l14) << 3) + ds[AI_SELF_HEIGHT] > 0x7f) return 0;
        if (l13 > l14 + 1) {
            if (l13 > l16 + 1) {
                l17 = 1;
                *out = l14 > l16 ? l14 : l16;
                *cost_garbage(m, cost, cost_known) = (uint8_t)(l13 - *out + *cost - 1);
                if (*cost > ds[PATH_COST_LIMIT]) return 0;
            } else {
                l14 = l16;
                l15 = l16;
                l18 = 1;
            }
        } else if (l16 != 0 && l13 >= l16 && l13 <= l16 + 1) {
            l18 = 1;
        }
        if (l14 > l13 + 1) return 0;
        if (l13 > fcur + 1 && !l17 && !l18) {
            if (kcbit & f4) return 0;
            if ((uint16_t)(8u << (kn & 0x1f)) & f6) {
                *cost_garbage(m, cost, cost_known) = (uint8_t)(*cost + 2);
                if (*cost >= ds[PATH_COST_LIMIT]) return 0;
            }
            if (l13 > l16 + 1) {
                *out = l13;
                if (l13 < l14) return 0;
                if ((crit(m, 0xa) >> 5) & 1) {
                    *cost_garbage(m, cost, cost_known) = (uint8_t)(*cost + 1);
                    if (*cost >= ds[PATH_COST_LIMIT]) return 0;
                    ds[PATH_CLIMB] = 1;
                    return 1;
                }
                return 0;
            }
            *out = l15;
            return 1;
        }
        if ((kcbit & f4) && !l18) {
            if (knbit & f4) return 0;
            if (knbit & f6) {
                *cost_garbage(m, cost, cost_known) = (uint8_t)(*cost + 2);
                if (*cost > ds[PATH_COST_LIMIT]) return 0;
            }
            if ((crit(m, 0xa) >> 5) & 1) {
                *cost_garbage(m, cost, cost_known) = (uint8_t)(*cost + 1);
                if (*cost >= ds[PATH_COST_LIMIT]) return 0;
                ds[PATH_CLIMB] = 1;
                return 1;
            }
            return 0;
        }
        *out = l15;
        return 1;
    }
#undef SLOPE_BUMP
}

/* tile_traversal_allowed: may a line pass from tile (cx, cy)
 * into (nx, ny) at fine height z? Both tiles' types against the blocking
 * table, each direction of travel its own bit, and the next tile's floor
 * nibble against z >> 3. A previous tile other than the current one with no
 * next tile passes. */
static int tile_traversal_allowed(uw_motion *m, uint8_t px, uint8_t py, uint8_t cx, uint8_t cy,
                                  uint8_t nx, uint8_t ny, uint8_t z) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t tc = tile_ptr(m, cx, cy), tn = tile_ptr(m, nx, ny);
    uint8_t Tc = ds[(uint16_t)(TILE_BLOCK_TABLE + (ls[tc] & 0xf))];
    uint8_t Tn = ds[(uint16_t)(TILE_BLOCK_TABLE + (ls[tn] & 0xf))];
    if (px != 0 && (px != cx || py != cy) && nx == 0) return 1;
    if (nx > cx && (Tn & 2)) return 0;
    if (nx < cx && (Tn & 4)) return 0;
    if (ny > cy && (Tn & 8)) return 0;
    if (ny < cy && (Tn & 0x10)) return 0;
    if (nx > cx && (Tc & 4)) return 0;
    if (nx < cx && (Tc & 2)) return 0;
    if (ny > cy && (Tc & 0x10)) return 0;
    if (ny < cy && (Tc & 8)) return 0;
    return (z >> 3) >= ((ls[tn] >> 4) & 0xf);
}

/* test_between_points, from the instructions: 1 when a line from
 * the first fine position to the second crosses no tile that refuses it.
 * A DDA along the major axis a tile a step, at most ten, the minor axis an
 * 8-bit fraction (bit 7 the carry into the next tile) stepped by
 * (minor << 7) / major and seeded from the start's place in its tile; z the
 * start's low byte plus steps * dz / (major >> 3). The seed's arithmetic is
 * the instructions' in each of the eight octants, asymmetries and all. */
int test_between_points(uw_motion *m, int16_t x0, int16_t y0, int16_t z0,
                               int16_t x1, int16_t y1, int16_t z1) {
    int16_t dx = (int16_t)(x1 - x0), dy = (int16_t)(y1 - y0), dz = (int16_t)(z1 - z0);
    uint8_t cur_x = (uint8_t)(x0 >> 3), cur_y = (uint8_t)(y0 >> 3);
    uint8_t prev_x = cur_x, prev_y = cur_y, nxt_x = cur_x, nxt_y = cur_y;
    uint8_t end_x = (uint8_t)(x1 >> 3), end_y = (uint8_t)(y1 >> 3);
    uint8_t *major, *minor, count, slope, frac, z, steps = 0;
    int8_t mstep, nstep;
    uint16_t kx = (uint16_t)(x0 & 7), ky = (uint16_t)(y0 & 7);
#define SLOPE(num, den) ((uint8_t)(((int32_t)(num) << 7) / (int32_t)(den)))
#define SEED(mul) ((uint8_t)((int16_t)((uint16_t)slope * (mul)) / 8))
    if (dx == 0 && dy == 0) return 1;
    if (dx >= dy) {
        if (-dy <= dx) {
            major = &nxt_x; minor = &nxt_y; count = (uint8_t)(dx >> 3); mstep = 1;
            nstep = (int8_t)(dy > 0 ? 1 : -1);
            if (nstep == 1) { slope = SLOPE(dy, dx); frac = (uint8_t)((ky << 4) + SEED(7 - kx)); }
            else { slope = SLOPE(-dy, dx); frac = (uint8_t)(((7 - ky) << 4) + SEED(7 - kx)); }
        } else {
            major = &nxt_y; minor = &nxt_x; count = (uint8_t)(-dy >> 3); mstep = -1;
            nstep = (int8_t)(dx > 0 ? 1 : -1);
            if (nstep == 1) { slope = SLOPE(dx, -dy); frac = (uint8_t)(((7 - kx) << 4) + SEED(ky)); }
            else { slope = SLOPE(-dx, -dy); frac = (uint8_t)((kx << 4) + SEED(ky)); }
        }
    } else {
        if (-dy <= dx) {
            major = &nxt_y; minor = &nxt_x; count = (uint8_t)(dy >> 3); mstep = 1;
            nstep = (int8_t)(dx > 0 ? 1 : -1);
            if (nstep == 1) { slope = SLOPE(dx, dy); frac = (uint8_t)((kx << 4) + SEED(7 - ky)); }
            else { slope = SLOPE(-dx, dy); frac = (uint8_t)(((7 - kx) << 4) + SEED(7 - ky)); }
        } else {
            major = &nxt_x; minor = &nxt_y; count = (uint8_t)(-dx >> 3); mstep = -1;
            nstep = (int8_t)(dy > 0 ? 1 : -1);
            if (nstep == 1) { slope = SLOPE(dy, -dx); frac = (uint8_t)(((7 - ky) << 4) + SEED(kx)); }
            else { slope = SLOPE(-dy, -dx); frac = (uint8_t)((ky << 4) + SEED(kx)); }
        }
    }
#undef SLOPE
#undef SEED
    z = (uint8_t)z0;
    for (;;) {
        if (frac & 0x80) {
            frac &= 0x7f;
            *minor = (uint8_t)(*minor + nstep);
            if (!tile_traversal_allowed(m, prev_x, prev_y, cur_x, cur_y, nxt_x, nxt_y, z)) return 0;
            if (nxt_x == end_x && nxt_y == end_y)
                return tile_traversal_allowed(m, cur_x, cur_y, nxt_x, nxt_y, 0, 0, z);
            prev_x = cur_x; prev_y = cur_y;
            cur_x = nxt_x; cur_y = nxt_y;
        }
        *major = (uint8_t)(*major + mstep);
        if (++steps > 10) return 0;
        if (count) z = (uint8_t)((uint8_t)z0 + (uint8_t)((int16_t)(uint16_t)(steps * dz) / (int16_t)count));
        if (!tile_traversal_allowed(m, prev_x, prev_y, cur_x, cur_y, nxt_x, nxt_y, z)) return 0;
        if (nxt_x == end_x && nxt_y == end_y)
            return tile_traversal_allowed(m, cur_x, cur_y, nxt_x, nxt_y, 0, 0, z);
        prev_x = cur_x; prev_y = cur_y;
        cur_x = nxt_x; cur_y = nxt_y;
        frac = (uint8_t)(frac + slope);
    }
}

/* path_append_tile: a step into the buffer, and from the
 * second on the traversal test over the last three. */
static int path_append_tile(uw_motion *m, uint8_t x, uint8_t y) {
    uint8_t *ds = m->ds, *e = m->ext;
    uint8_t cost = 0, n;
    uint16_t filt = rw(ds, AI_FILTER_DESC);
    int8_t r;
    ds[PATH_COST_LIMIT] = 0;
    e[(uint16_t)(ds[PATH_LEN] << 2)] = x;
    e[(uint16_t)((ds[PATH_LEN] << 2) + 1)] = y;
    ds[PATH_LEN] = (uint8_t)(ds[PATH_LEN] + 1);
    if (ds[PATH_LEN] > 0x3f) return 0;
    n = ds[PATH_LEN];
    if (n == 2)
        r = traverse_tiles(m, 0, 0, e[0], e[1], e[4], e[5], rw(ds, (uint16_t)(filt + 4)),
                           rw(ds, (uint16_t)(filt + 6)), e[2], &e[6], &cost, 0);
    else
        r = traverse_tiles(m, e[(uint16_t)(n * 4 - 12)], e[(uint16_t)(n * 4 - 11)],
                           e[(uint16_t)(n * 4 - 8)], e[(uint16_t)(n * 4 - 7)],
                           e[(uint16_t)(n * 4 - 4)], e[(uint16_t)(n * 4 - 3)],
                           rw(ds, (uint16_t)(filt + 4)), rw(ds, (uint16_t)(filt + 6)),
                           e[(uint16_t)(n * 4 - 10)], &e[(uint16_t)(n * 4 - 6)], &cost, 0);
    return r && !ds[PATH_CLIMB];
}

/* tile_line_walk: may the creature walk a straight line of
 * tiles from (x0,y0) to (x1,y1)? 1 yes, 0 no, -1 already there. */
int tile_line_walk(uw_motion *m, uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1) {
    uint8_t *ds = m->ds, *e = m->ext, *ls = m->lseg;
    int8_t dx = (int8_t)(uint8_t)(x1 - x0), dy = (int8_t)(uint8_t)(y1 - y0);
    uint8_t xy[2] = { x0, y0 }, acc = 0x40, frac, cost = 0;
    int major, minor;
    int8_t step, mstep;
    uint16_t filt = rw(ds, AI_FILTER_DESC), t0 = tile_ptr(m, x0, y0);
    ds[PATH_COST_LIMIT] = 0;
    if (dx == 0 && dy == 0) return -1;
    if (dx >= dy) {
        if (dx >= -dy) {
            major = 0; minor = 1; frac = (uint8_t)(((int32_t)dy << 7) / dx); step = 1;
            mstep = (int8_t)(dy > 0 ? 1 : -1);
        } else {
            major = 1; minor = 0; frac = (uint8_t)(((int32_t)dx << 7) / dy); step = -1;
            mstep = (int8_t)(dx > 0 ? 1 : -1);
        }
    } else if (dx >= -dy) {
        major = 1; minor = 0; frac = (uint8_t)(((int32_t)dx << 7) / dy); step = 1;
        mstep = (int8_t)(dx > 0 ? 1 : -1);
    } else {
        major = 0; minor = 1; frac = (uint8_t)(((int32_t)dy << 7) / dx); step = -1;
        mstep = (int8_t)(dy > 0 ? 1 : -1);
    }
    e[0] = x0;
    e[1] = y0;
    ds[PATH_LEN] = 1;
    e[2] = (uint8_t)((ls[t0] >> 4) & 0xf);
    do {
        xy[major] = (uint8_t)(xy[major] + step);
        if (!path_append_tile(m, xy[0], xy[1])) return 0;
        acc = (uint8_t)(acc + frac);
        if (acc & 0x80) {
            acc &= 0x7f;
            xy[minor] = (uint8_t)(xy[minor] + mstep);
            if (!path_append_tile(m, xy[0], xy[1])) return 0;
        }
    } while (!(xy[0] == x1 && xy[1] == y1));
    {
        /* The destination once more, with NO NEXT tile -- the last two
         * steps as previous and current, then two zeros. Without the
         * previous tile a lava dweller's destination would never be charged
         * its terrain cost against a limit of 0. */
        uint16_t n4 = (uint16_t)(ds[PATH_LEN] << 2);
        return traverse_tiles(m, e[(uint16_t)(n4 - 8)], e[(uint16_t)(n4 - 7)],
                              e[(uint16_t)(n4 - 4)], e[(uint16_t)(n4 - 3)], 0, 0,
                              rw(ds, (uint16_t)(filt + 4)), rw(ds, (uint16_t)(filt + 6)),
                              e[(uint16_t)(n4 - 6)], &e[(uint16_t)(n4 - 6)], &cost, 0);
    }
}

/* ---- following a packed path ------------------------------------------- */

/* npc_pose_from_height: a flier's vertical velocity toward a
 * cruising height 0x14 above the tile's floor (at most 0x78): climb (0x12)
 * below it by more than 8, dive (0xe) above by more than 8 or over 0x78,
 * hover (0xf..0x11) between; a flier lifted by a lip keeps climbing. */
void npc_pose_from_height(uw_motion *m, uint8_t x, uint8_t y) {
    uint8_t *ds = m->ds;
    uint8_t oz, cruise, v;
    if (ds[AI_GOTO_ACTIVE]) return;
    oz = (uint8_t)(nb(m, 2) & 0x7f);
    cruise = (uint8_t)((((m->lseg[tile_ptr(m, x, y)] >> 4) & 0xf) << 3) + 0x14);
    if (cruise > 0x78) cruise = 0x78;
    if ((ds[AI_FLIER_STEP] && oz < 0x78) || (int16_t)oz < (int16_t)cruise - 8)
        v = 0x12;
    else if (oz > 0x78 || (int16_t)oz > (int16_t)cruise + 8)
        v = 0xe;
    else
        v = (uint8_t)(rt_rand(m) % 3 + 0xf);
    nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 7) | ((v & 0x1f) << 3)));
}

/* path_face_next_step: at a flagged node, a step that climbs.
 * Within two eighths of the node's entry point the creature faces the next
 * step, marks ai_faced_path_step and JUMPS -- vertical velocity 0xb0 >> 3 =
 * 22, speed 11, motion mode 1; otherwise it faces the entry point. */
static void path_face_next_step(uw_motion *m, uint16_t rec) {
    uint8_t *ds = m->ds, *e = m->ext;
    int16_t ax = (int16_t)((e[rec] << 3) - 2), ay = (int16_t)((e[(uint16_t)(rec + 1)] << 3) - 2);
    int16_t dx, dy;
    uint8_t h;
    if (e[rec] == ds[AI_SELF_TILE_X]) ax = (int16_t)(ax + 6);
    else if (e[rec] < ds[AI_SELF_TILE_X]) ax = (int16_t)(ax + 0xb);
    if (e[(uint16_t)(rec + 1)] == ds[AI_SELF_TILE_Y]) ay = (int16_t)(ay + 6);
    else if (e[(uint16_t)(rec + 1)] < ds[AI_SELF_TILE_Y]) ay = (int16_t)(ay + 0xb);
    dx = (int16_t)(ax - rs(ds, AI_SELF_FINE_X));
    dy = (int16_t)(ay - rs(ds, AI_SELF_FINE_Y));
    if ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) < 3) {
        uint8_t cur = (uint8_t)(e[(uint16_t)(rec + 2)] & 0x7f);
        uint8_t dir = (uint8_t)((e[(uint16_t)(rec + 4 + cur / 4)] >> ((cur % 4) * 2)) & 3);
        int16_t tx = (int16_t)(e[rec] + (int8_t)ds[(uint16_t)(PATH_NEIGHBOURS + dir * 2)]);
        int16_t ty = (int16_t)(e[(uint16_t)(rec + 1)] + (int8_t)ds[(uint16_t)(PATH_NEIGHBOURS + dir * 2 + 1)]);
        h = vector_to_heading((int8_t)(uint8_t)(tx - (nw(m, 0x16) >> 10)),
                              (int8_t)(uint8_t)(ty - ((nw(m, 0x16) & 0x3f0) >> 4)));
        ds[AI_FACED_PATH_STEP] = 1;
        nbset(m, 9, (uint8_t)(h << 5));
        nwset(m, 2, (uint16_t)((nw(m, 2) & 0xfc7f) | ((h & 7) << 7)));
        nbset(m, 0x18, (uint8_t)(nb(m, 0x18) & 0xe0));
        nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 0xf8) | 1));
        nbset(m, 0x14, (uint8_t)((nb(m, 0x14) & 7) | 0xb0));
        nbset(m, 0x13, (uint8_t)((nb(m, 0x13) & 0x80) | 0xb));
        return;
    }
    h = vector_to_heading((int8_t)(uint8_t)dx, (int8_t)(uint8_t)dy);
    nbset(m, 9, (uint8_t)(h << 5));
    nwset(m, 2, (uint16_t)((nw(m, 2) & 0xfc7f) | ((h & 7) << 7)));
    nbset(m, 0x18, (uint8_t)(nb(m, 0x18) & 0xe0));
}

/* path_step: advance the record's node by the next two-bit
 * direction; its flag bit into [2] bit 7. 0 when the path is spent. */
int path_step(uw_motion *m, uint16_t rec) {
    uint8_t *e = m->ext, *ds = m->ds;
    uint8_t cur = (uint8_t)(e[(uint16_t)(rec + 2)] & 0x7f), dir;
    if (cur >= e[(uint16_t)(rec + 3)]) return 0;
    dir = (uint8_t)((e[(uint16_t)(rec + 4 + cur / 4)] >> ((cur % 4) * 2)) & 3);
    e[rec] = (uint8_t)(e[rec] + ds[(uint16_t)(PATH_NEIGHBOURS + dir * 2)]);
    e[(uint16_t)(rec + 1)] = (uint8_t)(e[(uint16_t)(rec + 1)] + ds[(uint16_t)(PATH_NEIGHBOURS + dir * 2 + 1)]);
    if ((e[(uint16_t)(rec + 0x14 + cur / 8)] >> (cur % 8)) & 1)
        e[(uint16_t)(rec + 2)] |= 0x80;
    else
        e[(uint16_t)(rec + 2)] &= 0x7f;
    e[(uint16_t)(rec + 2)] = (uint8_t)((e[(uint16_t)(rec + 2)] & 0x80) | ((cur + 1) & 0x7f));
    return 1;
}

/* path_step_toward: has the creature reached the node? Without
 * the node's flag, a creature within two eighths of a tile edge counts as in
 * the neighbouring tile it is leaning into. */
static int path_step_toward(int flag, int16_t tx, int16_t ty, int16_t fx, int16_t fy, int16_t nx, int16_t ny) {
    if (!flag) {
        if (fx >= 6 && nx > tx) tx++;
        else if (fx <= 1 && nx < tx) tx--;
        if (fy >= 6 && ny > ty) ty++;
        else if (fy <= 1 && ny < ty) ty--;
    }
    return tx == nx && ty == ny;
}

/* turn_towards_path: step the path when its node is reached,
 * then aim at the node -- its centre on an axis the creature shares, its
 * near edge otherwise. 0 when the path is spent. */
int turn_towards_path(uw_motion *m, uint16_t rec) {
    uint8_t *ds = m->ds, *e = m->ext;
    uint8_t rx = e[rec], ry = e[(uint16_t)(rec + 1)];
    int16_t ax, ay;
    uint8_t h;
    if (path_step_toward((e[(uint16_t)(rec + 2)] >> 7) & 1, ds[AI_SELF_TILE_X], ds[AI_SELF_TILE_Y],
                         (int16_t)(rw(ds, AI_SELF_FINE_X) & 7), (int16_t)(rw(ds, AI_SELF_FINE_Y) & 7),
                         rx, ry)) {
        if (!path_step(m, rec)) return 0;
    } else {
        rx = ds[AI_SELF_TILE_X];
        ry = ds[AI_SELF_TILE_Y];
    }
    if (e[(uint16_t)(rec + 2)] & 0x80) {
        path_face_next_step(m, rec);
        return 1;
    }
    if (crit(m, 0xa) & 0x80)
        npc_pose_from_height(m, (uint8_t)(nw(m, 0xf) & 0x3f), (uint8_t)((nw(m, 0xf) & 0xfc0) >> 6));
    ax = (int16_t)(e[rec] << 3);
    if (e[rec] == rx) ax = (int16_t)(ax + 4);
    else if (e[rec] < rx) ax = (int16_t)(ax + 7);
    ay = (int16_t)(e[(uint16_t)(rec + 1)] << 3);
    if (e[(uint16_t)(rec + 1)] == ry) ay = (int16_t)(ay + 4);
    else if (e[(uint16_t)(rec + 1)] < ry) ay = (int16_t)(ay + 7);
    h = vector_to_heading((int8_t)(uint8_t)(ax - ds[AI_SELF_FINE_X]), (int8_t)(uint8_t)(ay - ds[AI_SELF_FINE_Y]));
    nbset(m, 9, (uint8_t)(h << 5));
    nwset(m, 2, (uint16_t)((nw(m, 2) & 0xfc7f) | ((h & 7) << 7)));
    nbset(m, 0x18, (uint8_t)(nb(m, 0x18) & 0xe0));
    return 1;
}

/* ---- the path search: pathfind_between_tiles and its helpers ----------- */

enum { FRONTIER_A = 0x2c0, FRONTIER_B = 0x340, GRID_STRIDE_X = 0x140 };

static uint16_t grid_index(uint8_t x, uint8_t y) {
    return (uint16_t)((int8_t)x * GRID_STRIDE_X + (int8_t)y * 5);
}

/* path_read_back_from_grid: walk the parents back from the
 * destination into the step buffer, each step's climb flag beside it. */
static void path_read_back(uw_motion *m, uint8_t wave, uint8_t x1, uint8_t y1) {
    uint8_t *e = m->ext, *g = m->grid;
    uint8_t cl;
    m->ds[PATH_LEN] = (uint8_t)(wave + 1);
    e[(uint16_t)(wave * 4 + 4)] = x1;
    e[(uint16_t)(wave * 4 + 5)] = y1;
    e[3] = 0;
    for (cl = (uint8_t)(wave + 1); cl > 0; cl--) {
        uint16_t n = (uint16_t)(e[(uint16_t)(cl * 4)] * GRID_STRIDE_X + e[(uint16_t)(cl * 4 + 1)] * 5);
        e[(uint16_t)(cl * 4 - 4)] = g[n];
        e[(uint16_t)(cl * 4 - 3)] = g[(uint16_t)(n + 1)];
        e[(uint16_t)(cl * 4 + 3)] = (uint8_t)(g[(uint16_t)(n + 3)] & 1);
    }
}

/* pathfind_between_tiles, from the instructions: a breadth-first
 * search over the grid (five bytes a tile: parent x, parent y, height,
 * cost << 1 | climb, wave) inside the endpoints' box grown by five, at most
 * 0x20 waves of at most 0x40 tiles, each step tested by
 * traverse_multiple_tiles. A tile already reached is taken again only from
 * a parent of an earlier wave, and only when the height it arrives at is
 * nearer the target's. 1 with the path read back into the step buffer. */
int pathfind_between_tiles(uw_motion *m, uint8_t x0, uint8_t y0, uint8_t z0,
                                  uint8_t x1, uint8_t y1, uint8_t z1, uint8_t limit) {
    uint8_t *ds = m->ds, *e = m->ext, *g = m->grid;
    uint16_t filt = rw(ds, AI_FILTER_DESC), cur = FRONTIER_A, next = FRONTIER_B;
    uint8_t wave = 0, ncur = 0, nnext, i;
    int8_t xmin, ymin, xmax, ymax;
    int si;
    ds[PATH_COST_LIMIT] = limit;
    memset(g, 0, 0x5000);
    {
        int v = (int8_t)((int8_t)x0 < (int8_t)x1 ? x0 : x1) - 5;
        xmin = (int8_t)(v <= 1 ? 1 : v);
        v = (int8_t)((int8_t)y0 < (int8_t)y1 ? y0 : y1) - 5;
        ymin = (int8_t)(v <= 1 ? 1 : v);
        v = (int8_t)((int8_t)x0 > (int8_t)x1 ? x0 : x1) + 5;
        xmax = (int8_t)(v >= 0x40 ? 0x40 : v);
        v = (int8_t)((int8_t)y0 > (int8_t)y1 ? y0 : y1) + 5;
        ymax = (int8_t)(v >= 0x40 ? 0x40 : v);
    }
    g[grid_index(x0, y0)] = x0;
    g[(uint16_t)(grid_index(x0, y0) + 2)] = z0;
    g[(uint16_t)(grid_index(x0, y0) + 4)] = 0;
    for (si = 0; si < 4; si++) {
        uint8_t nx = (uint8_t)(x0 + ds[(uint16_t)(PATH_NEIGHBOURS + si * 2)]);
        uint8_t ny = (uint8_t)(y0 + ds[(uint16_t)(PATH_NEIGHBOURS + si * 2 + 1)]);
        uint16_t n = grid_index(nx, ny);
        uint8_t cost = 0;
        if (!traverse_tiles(m, 0, 0, x0, y0, nx, ny, rw(ds, (uint16_t)(filt + 4)), rw(ds, (uint16_t)(filt + 6)),
                            z0, &g[(uint16_t)(n + 2)], &cost, 1))
            continue;
        if (nx == x1 && ny == y1) {
            ds[PATH_LEN] = 1;
            e[0] = x0; e[1] = y0; e[3] = 0; e[4] = nx; e[5] = ny;
            return 1;
        }
        g[n] = x0;
        g[(uint16_t)(n + 1)] = y0;
        g[(uint16_t)(n + 4)] = 1;
        g[(uint16_t)(n + 3)] = (uint8_t)((g[(uint16_t)(n + 3)] & 1) | ((cost & 0x7f) << 1));
        e[(uint16_t)(cur + ncur * 2)] = nx;
        e[(uint16_t)(cur + ncur * 2 + 1)] = ny;
        ncur++;
    }
    wave = 1;
    while (wave < 0x20 && ncur > 0) {
        uint16_t tmp;
        nnext = 0;
        for (i = 0; i < ncur && nnext < 0x40; i++) {
            uint8_t px = e[(uint16_t)(cur + i * 2)], py = e[(uint16_t)(cur + i * 2 + 1)];
            uint16_t pn = grid_index(px, py);
            for (si = 0; si < 4; si++) {
                uint8_t nx = (uint8_t)(px + ds[(uint16_t)(PATH_NEIGHBOURS + si * 2)]);
                uint8_t ny = (uint8_t)(py + ds[(uint16_t)(PATH_NEIGHBOURS + si * 2 + 1)]);
                uint16_t n;
                uint8_t cost, height = 0;
                int ok, fresh;
                if ((int8_t)nx < xmin || (int8_t)nx > xmax || (int8_t)ny < ymin || (int8_t)ny > ymax) continue;
                n = grid_index(nx, ny);
                cost = (uint8_t)((g[(uint16_t)(pn + 3)] >> 1) & 0x7f);
                if (g[pn] == nx && g[(uint16_t)(pn + 1)] == ny) continue;
                ok = traverse_tiles(m, g[pn], g[(uint16_t)(pn + 1)], px, py, nx, ny,
                                    rw(ds, (uint16_t)(filt + 4)), rw(ds, (uint16_t)(filt + 6)),
                                    g[(uint16_t)(pn + 2)], &height, &cost, 1);
                fresh = g[n] == 0;
                if (!ok) continue;
                if (!fresh) {
                    if (!(g[(uint16_t)(pn + 4)] < g[(uint16_t)(n + 4)])) continue;
                    {
                        int dnew = height - (int8_t)z1, dold = g[(uint16_t)(n + 2)] - (int8_t)z1;
                        if (!((dnew < 0 ? -dnew : dnew) < (dold < 0 ? -dold : dold))) continue;
                    }
                }
                g[n] = px;
                g[(uint16_t)(n + 1)] = py;
                g[(uint16_t)(n + 2)] = height;
                g[(uint16_t)(n + 4)] = (uint8_t)(wave + 1);
                g[(uint16_t)(n + 3)] = (uint8_t)((g[(uint16_t)(n + 3)] & 1) | ((cost & 0x7f) << 1));
                g[(uint16_t)(pn + 3)] = (uint8_t)((g[(uint16_t)(pn + 3)] & 0xfe) | (ds[PATH_CLIMB] & 1));
                if (fresh) {
                    e[(uint16_t)(next + nnext * 2)] = nx;
                    e[(uint16_t)(next + nnext * 2 + 1)] = ny;
                    nnext++;
                }
                if (nx == x1 && ny == y1
                    && traverse_tiles(m, px, py, nx, ny, 0, 0, rw(ds, (uint16_t)(filt + 4)), rw(ds, (uint16_t)(filt + 6)),
                                      g[(uint16_t)(n + 2)], &g[(uint16_t)(n + 2)], &cost, 1)) {
                    path_read_back(m, wave, x1, y1);
                    return 1;
                }
            }
        }
        tmp = cur; cur = next; next = tmp;
        ncur = nnext;
        wave++;
    }
    return 0;
}

/* path_pack: the step buffer into a pool record -- the start,
 * the count, two bits of direction a step (from the neighbour index table)
 * and a climb flag a step. It packs whole bytes, reading past the last step. */
void path_pack(uw_motion *m, uint16_t rec) {
    uint8_t *ds = m->ds, *e = m->ext;
    uint8_t i, j;
    e[(uint16_t)(rec + 2)] &= 0x80;
    e[rec] = e[0];
    e[(uint16_t)(rec + 1)] = e[1];
    e[(uint16_t)(rec + 3)] = ds[PATH_LEN];
    for (i = 0; i < ds[PATH_LEN]; i = (uint8_t)(i + 4)) {
        uint8_t b = 0;
        for (j = 0; j < 4; j++) {
            uint16_t k = (uint16_t)((i + j) * 4);
            uint16_t ix = (uint16_t)((uint16_t)(e[(uint16_t)(k + 4)] - e[k]) * 3
                                     + (uint16_t)(e[(uint16_t)(k + 5)] - e[(uint16_t)(k + 1)]));
            b = (uint8_t)(b + ((ds[(uint16_t)(SLOPE_INDEX + ix)] & 3) << (j * 2)));
        }
        e[(uint16_t)(rec + 4 + i / 4)] = b;
    }
    for (i = 0; i < ds[PATH_LEN]; i = (uint8_t)(i + 8)) {
        uint8_t b = 0;
        for (j = 0; j < 8; j++)
            b = (uint8_t)(b + ((e[(uint16_t)((i + j) * 4 + 7)] & 1) << j));
        e[(uint16_t)(rec + 0x14 + i / 8)] = b;
    }
}

/* npc_path_slot_alloc: the first set (free) bit of the mask. */
int path_slot_alloc(uw_motion *m, uint8_t *slot) {
    uint16_t mask = rw(m->ds, NPC_PATH_SLOT_MASK);
    uint8_t i;
    if (!mask) return 0;
    for (i = 0; i < 16; i++)
        if (mask & (1 << i)) { *slot = i; return 1; }
    return 0;
}

/* creature_acceptable_danger: (hp * 4) / max + (critter+0x1c & 0xf) / 4, zero
 * for a creature that is not hostile (attitude other than 0), one with no maximum, word 0 bit 13, and the golem
 * (whoami 0x16) on level 6. The search's cost limit. */
uint8_t creature_acceptable_danger(uw_motion *m) {
    uint8_t *ds = m->ds;
    if ((nw(m, 0xd) >> 14) || crit(m, 4) == 0 || (nw(m, 0) & 0x2000)) return 0;
    if (rw(ds, CURRENT_LEVEL_WORD) == 6 && nb(m, 0x1a) == 0x16) return 0;
    return (uint8_t)((int16_t)(nb(m, 8) << 2) / crit(m, 4) + (crit(m, 0x1c) & 0xf) / 4);
}

/* Release a creature's path slot: its bit back into npc_path_slot_mask and
 * +0x15 bit 7 cleared. */
void release_path_slot(uw_motion *m) {
    ww(m->ds, NPC_PATH_SLOT_MASK, (uint16_t)(rw(m->ds, NPC_PATH_SLOT_MASK) | (1 << (nb(m, 0x16) & 0xf))));
    nbset(m, 0x15, (uint8_t)(nb(m, 0x15) & 0x7f));
}
