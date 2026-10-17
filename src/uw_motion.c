/* SPDX-License-Identifier: MIT */
/* the pass's frame: dungeon_frame_tick, player_frame_update and
 * player_physics_step -- the player's speed, movement mode and state,
 * the movement input and its sounds -- with the entry points the pass
 * runs each frame (uw_motion_frame, uw_motion_player_frame).
 *
 * One of src/uw_motion*.c: see uw_motion.h and uw_motion_int.h. */
#include "uw_motion_int.h"

#include <stdlib.h>
#include <string.h>

/* ---- player_update_noise ----------------------------------- */

static void update_noise(uw_motion *m, int still) {
    uint8_t *ds = m->ds;
    uint16_t row = rw(ds, CRITTER_ROW_PTR);
    uint8_t cur = (uint8_t)(ds[(uint16_t)(row + 0x1d)] & 0x0f);
    int8_t level;
    if (!still) {
        int16_t speed = rs(ds, MOVEMENT_SPEED);
        level = speed == 0 ? 0
              : (int8_t)((int8_t)((speed * 10) / rs(ds, SPEED_MAX_FORWARD))
                         + (int8_t)ds[NOISE_BASE] - 5);
    } else {
        level = (int8_t)(ds[NOISE_BASE] + 4);
    }
    if (ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0xb8)]) level = (int8_t)(level + 4);
    if (level < 0) level = 0;
    else if (level > 15) level = 15;
    /* It jumps up and leaks down, one a frame, unless the cycle says wait. */
    if ((uint8_t)level < cur) {
        if (ds[NOISE_CYCLE] != 0) goto cycle;
        level = (int8_t)(cur - 1);
    }
    ds[(uint16_t)(row + 0x1d)] = (uint8_t)((ds[(uint16_t)(row + 0x1d)] & 0xf0) | (level & 0x0f));
cycle:
    ds[NOISE_CYCLE] = (uint8_t)((ds[NOISE_CYCLE] + 1) % 8);
    ds[(uint16_t)(row + 0x1d)] = (uint8_t)((ds[(uint16_t)(row + 0x1d)] & 0x0f)
                                           | (ds[NOISE_LEVEL] << 4));
}

/* ---- player_speed_for_mode, from the instructions ---------- */

/* `imul` of two words then `cwd; idiv` -- the product's high word thrown
 * away before the divide. */
static int16_t heading_turn(uint8_t *ds, uint16_t dt) {
    int16_t a = (int16_t)(uint16_t)((uint16_t)dt * rw(ds, TURN_RATE));
    int16_t b = (int16_t)(rs(ds, TURN_INPUT) / 4);
    return (int16_t)((int16_t)(uint16_t)(a * b) / 4);
}

static void speed_for_mode(uw_motion *m, uint16_t mode, uint16_t dt, int16_t *target) {
    uint8_t *ds = m->ds;
    uint16_t di = rw(ds, PLAYER_HEADING);
    if (mode > 13) { ww(ds, MOVE_HEADING, di); return; }
    switch (mode) {
    case 0:                                         /* d31: no heading stored */
        *target = 0;
        return;
    case 1:
        ww(ds, PLAYER_HEADING, (uint16_t)(rw(ds, PLAYER_HEADING) + heading_turn(ds, dt)));
        di = rw(ds, PLAYER_HEADING);
        ww(ds, MOVE_HEADING, di);
        *target = (int16_t)((int16_t)(uint16_t)((rs(ds, FORWARD_INPUT) >> 2)
                                                * rs(ds, SPEED_MAX_FORWARD)) / 32);
        ww(ds, HEADING_OFFSET, 0);
        break;
    case 10:                                        /* c4d */
        di = (uint16_t)(di + 0x4000);
        *target = rs(ds, SPEED_MAX_STRAFE);
        ww(ds, HEADING_OFFSET, 1);
        break;
    case 9:                                         /* c5f */
        di = (uint16_t)(di - 0x4000);
        *target = rs(ds, SPEED_MAX_STRAFE);
        ww(ds, HEADING_OFFSET, 0xffff);
        break;
    case 8:                                         /* c71 */
        di = (uint16_t)(di - 0x8000);
        *target = rs(ds, SPEED_MAX_BACKWARD);
        ww(ds, HEADING_OFFSET, 0xfffe);
        break;
    case 6:                                         /* c86: a standing jump */
        if (rw(ds, VERTICAL_VELOCITY) || rw(ds, VERTICAL_GRAVITY) || rw(ds, MOVEMENT_SPEED))
            break;
        di = rw(ds, PLAYER_HEADING);
        ww(ds, MOVE_HEADING, di);
        *target = (int16_t)(rs(ds, SPEED_MAX_FORWARD) / 2);
        ww(ds, MOVEMENT_SPEED, (uint16_t)*target);
        ww(ds, HEADING_OFFSET, 0);
        /* the impulse */
        /* FALLTHROUGH */
    case 7: {                                       /* cc0 */
        int16_t vv = 0x263;
        if (rs(ds, PLAYER_Z) > 0x280) {
            vv = (int16_t)((int16_t)(uint16_t)(vv * 5) / 6);
            if (rs(ds, PLAYER_Z) > 0x2c0)
                vv = (int16_t)((int16_t)(uint16_t)(vv << 1) / 3);
        }
        ww(ds, VERTICAL_VELOCITY, (uint16_t)vv);
        ww(ds, VERTICAL_GRAVITY, (ds[BLOCK_FLAGS] & 1) ? 0xfffe : 0xfffc);
        return;                                     /* d3b */
    }
    case 12:
        ww(ds, HEADING_OFFSET, 0);
        ww(ds, VERTICAL_VELOCITY, 0x8d);
        ww(ds, VERTICAL_GRAVITY, 0);
        break;
    case 13:
        ww(ds, HEADING_OFFSET, 0);
        ww(ds, VERTICAL_VELOCITY, 0xff73);
        ww(ds, VERTICAL_GRAVITY, 0);
        break;
    default:                                        /* 2..5, 11 */
        break;
    }
    ww(ds, MOVE_HEADING, di);                       /* d37 */
}

/* ---- player_speed_update, from the instructions ------------- */

static void speed_update(uw_motion *m, uint16_t dt) {
    uint8_t *ds = m->ds;
    int16_t target = 0;
    if (rw(ds, VERTICAL_GRAVITY) == 0) {
        speed_for_mode(m, rw(ds, MOVEMENT_MODE), dt, &target);
        if (rw(ds, VERTICAL_GRAVITY) == 0) {
            int16_t d = (int16_t)(uint16_t)((uint16_t)target - rw(ds, MOVEMENT_SPEED));
            int16_t ad = (int16_t)(uint16_t)(d < 0 ? -(uint16_t)d : (uint16_t)d);
            int16_t speed;
            if (ad > rs(ds, SPEED_MAX_ACCEL))
                d = (int16_t)(uint16_t)((d > 0 ? 1 : -1) * rs(ds, SPEED_MAX_ACCEL));
            speed = (int16_t)(uint16_t)(rw(ds, MOVEMENT_SPEED) + (uint16_t)d);
            if (speed > rs(ds, SPEED_MAX_FORWARD)) speed = rs(ds, SPEED_MAX_FORWARD);
            else if (speed < 0) speed = 0;
            ww(ds, MOVEMENT_SPEED, (uint16_t)speed);
        }
    }
    if (rw(ds, VERTICAL_GRAVITY) != 0)
        ww(ds, PLAYER_HEADING, (uint16_t)(rw(ds, PLAYER_HEADING) + heading_turn(ds, dt)));
    ww(ds, SPEED_DT, dt);
    ds[FALL_HARDNESS] = 5;
    ds[GROUNDED] = 0;
    if ((rw(ds, MOVEMENT_INPUT_A) | rw(ds, MOVEMENT_INPUT_B) | rw(ds, VERTICAL_GRAVITY)) == 0)
        ds[GROUNDED] = 0x80;
    if (rw(ds, MOVEMENT_SPEED) == 0) ww(ds, MOVE_HEADING, rw(ds, PLAYER_HEADING));
    ww(ds, HEADING_TARGET, rw(ds, MOVE_HEADING));
    ww(ds, RUN_FLAGS, 0);
    if (ds[BLOCK_FLAGS] & 0x14) {
        ww(ds, RUN_FLAGS, 0x1000);
        ds[GROUNDED] = 0x80;
    }
    ww(ds, FALL_IMPACT, 0);
}

/* ---- player_apply_movement_mode, from the instructions ------ */

void apply_movement_mode(uw_motion *m, int8_t mode) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    uint8_t scale;
    if ((uint8_t)mode == 0xff) {
        mode = (int8_t)(ds[(uint16_t)(rec + 0xb6)] & 7);
    } else {
        ds[(uint16_t)(rec + 0xb8)] = (uint8_t)((ds[(uint16_t)(rec + 0xb8)] & 0xe0)
                                               + ds[(uint16_t)(MODE_FLAGS_TABLE + mode)]);
        ds[(uint16_t)(rec + 0xb6)] = (uint8_t)((ds[(uint16_t)(rec + 0xb6)] & 0xf8) | (mode & 7));
    }
    /* Tables copied seven bytes onto the stack; index 7 would read past. */
    if (mode < 0 || mode > 6) { UW_NOT_CARRIED(m->not_carried); return; }
    scale = ds[(uint16_t)(SPEED_SCALE_TABLE + mode)];
    ww(ds, SPEED_MAX_FORWARD, (uint16_t)((int16_t)(uint16_t)(scale * rw(ds, SPEED_BASE_FORWARD)) / 10));
    ww(ds, SPEED_MAX_STRAFE, (uint16_t)((int16_t)(uint16_t)(scale * rw(ds, SPEED_BASE_STRAFE)) / 10));
    ww(ds, SPEED_MAX_BACKWARD, (uint16_t)((int16_t)(uint16_t)(scale * rw(ds, SPEED_BASE_BACKWARD)) / 10));
    ww(ds, TURN_RATE_EFFECTIVE, mode < 4
       ? (uint16_t)((int16_t)(uint16_t)(scale * rw(ds, TURN_RATE)) / 10) : rw(ds, TURN_RATE));
    if (rw(ds, (uint16_t)(rec + 0x4c)) != 0
        && (uint16_t)(rw(ds, (uint16_t)(rec + 0x4a)) << 1) > rw(ds, (uint16_t)(rec + 0x4c))) {
        uint16_t num = (uint16_t)(rw(ds, (uint16_t)(rec + 0x4a)) * 0x60);
        uint16_t den = (uint16_t)(rw(ds, (uint16_t)(rec + 0x4c)) << 1);
        ww(ds, SPEED_MAX_ACCEL, (uint16_t)(0x60 - (den ? num / den : 0)));
    } else {
        ww(ds, SPEED_MAX_ACCEL, 0x60);
    }
}

/* ---- player_set_movement_state, from the instructions ------- */

void set_movement_state(uw_motion *m, uint16_t flags, int force) {
    uint8_t *ds = m->ds;
    if (!(flags == rw(ds, MOVEMENT_STATE) && !force)) {
        int stowed = 0;
        int8_t mode = 0;
        ww(ds, MOVEMENT_STATE, flags);
        if (flags & 0x22) {
            if (!(ds[BLOCK_FLAGS] & 8)) {
                /* player_stow_and_flag */
                uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
                if (flags & 2) {
                    stowed = 1;
                    ds[(uint16_t)(rec + 0xb9)] = 0x60;
                    weapon_stow(m);             /* weapon_stow: nothing unless the weapon is drawn */
                } else {
                    ds[(uint16_t)(rec + 0xb9)] = 0x10;
                }
                mode = 1;
            }
        } else if (flags & 4) {
            mode = 2;
        } else if (flags & 0x10) {
            if (ds[BLOCK_FLAGS] & 4) mode = 4;
            else if (ds[BLOCK_FLAGS] & 0x10) mode = 5;
            else if (ds[BLOCK_FLAGS] & 2) mode = 6;
        }
        apply_movement_mode(m, mode);
        if (!stowed) ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0xb9)] = 0;
    }
    if (flags & 0x10) {
        if (ds[BLOCK_FLAGS] & 0x14) {
            int16_t vv = rs(ds, VERTICAL_VELOCITY);
            ww(ds, VERTICAL_GRAVITY, 0);
            if ((vv < 0 ? -vv : vv) > 10)
                ww(ds, VERTICAL_VELOCITY, (uint16_t)((int16_t)(uint16_t)(vv << 2) / 5));
            else
                ww(ds, VERTICAL_VELOCITY, 0);
        } else {
            if (rw(ds, VERTICAL_GRAVITY) == 0) ww(ds, VERTICAL_GRAVITY, 0xfffc);
            if ((ds[BLOCK_FLAGS] & 2) && rs(ds, VERTICAL_VELOCITY) <= -0x5e) {
                ww(ds, VERTICAL_VELOCITY, 0xffa2);
                if (rs(ds, MOVEMENT_SPEED) > 0x14)
                    ww(ds, MOVEMENT_SPEED, (uint16_t)(rs(ds, MOVEMENT_SPEED) / 2));
                else
                    ww(ds, MOVEMENT_SPEED, 0);
            }
        }
    }
}

/* ---- player_move_update, from the instructions -------------- */

void uw_motion_cutscene_request(uw_motion *m, uint16_t n) {
    if (m->cutscene_due) UW_NOT_CARRIED(m->not_carried);
    m->cutscene_wanted = n;
    m->cutscene_arg = 0;
    m->cutscene_due = 1;
}

void uw_motion_cutscene_request_numbered(uw_motion *m, uint16_t n, uint16_t arg) {
    uw_motion_cutscene_request(m, n);
    m->cutscene_arg = arg;
}


/* get_string of a packed id through scroll_print, when the text
 * windows are given. */
void print_string(uw_motion *m, uint16_t id) {
    char text[0x200];
    if (!m->scroll || !m->strings || uw_strings_by_id(m->strings, id, text, (int)sizeof text) < 0) {
        UW_NOT_CARRIED(m->not_carried);
        return;
    }
    uw_scroll_print(m->scroll, text);
}

/* print_message(id): message `id` of STRINGS.PAK block 1. */
void print_message(uw_motion *m, uint16_t id) {
    print_string(m, (uint16_t)(id | 0x200));
}

static void move_update(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t obj = rw(ds, TRACKED_OBJECT);
    uint16_t tile = (uint16_t)((rs(ds, PLAYER_X) >> 8) + ((rs(ds, PLAYER_Y) >> 8) << 6));
    uw_objpool pool;
    memset(&pool, 0, sizeof pool);
    pool.seg = ls;
    if (tile != rw(ds, TRACKED_TILE)) {
        if (rw(ds, TRACKED_TILE) != 0xffff)
            uw_object_list_remove(&pool, (uint16_t)(rw(ds, TILEMAP_PTR)
                                                    + rw(ds, TRACKED_TILE) * 4 + 2), obj);
        ww(ds, TRACKED_TILE, tile);
        uw_object_list_insert(&pool, (uint16_t)(rw(ds, TILEMAP_PTR) + tile * 4 + 2), obj);
        ww(ls, (uint16_t)(obj + 0x16), (uint16_t)((rw(ls, (uint16_t)(obj + 0x16)) & 0x3ff)
                                                   | (((rs(ds, PLAYER_X) >> 8) & 0x3f) << 10)));
        ww(ls, (uint16_t)(obj + 0x16), (uint16_t)((rw(ls, (uint16_t)(obj + 0x16)) & 0xfc0f)
                                                   | (((rs(ds, PLAYER_Y) >> 8) & 0x3f) << 4)));
    }
    ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0x1fff)
                                            | (((rs(ds, PLAYER_X) >> 5) & 7) << 13)));
    ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0xe3ff)
                                            | (((rs(ds, PLAYER_Y) >> 5) & 7) << 10)));
    ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0xff80)
                                            | ((rs(ds, PLAYER_Z) >> 3) & 0x7f)));
    ww(ls, (uint16_t)(obj + 0xb), (uint16_t)((rw(ls, (uint16_t)(obj + 0xb)) & 0x0fff)
                                              | ((((m->clock & 0xff) >> 6) & 0xf) << 12)));
    if (rw(ds, FALL_IMPACT) && rw(ds, HEADING_TARGET) == rw(ds, MOVE_HEADING))
        ww(ds, MOVEMENT_SPEED, 0);
    if (rw(ds, HEADING_TARGET) != rw(ds, MOVE_HEADING)) {
        uint16_t si;
        ww(ds, MOVE_HEADING, rw(ds, HEADING_TARGET));
        si = (uint16_t)(rw(ds, HEADING_TARGET) - (uint16_t)(rw(ds, HEADING_OFFSET) << 14));
        if (ds[GROUNDED] & 0x80) {
            uint16_t d = (uint16_t)(rw(ds, PLAYER_HEADING) - si);
            int16_t ad = (int16_t)(uint16_t)((int16_t)d < 0 ? -(uint16_t)d : d);
            if (ad < 0x400) ww(ds, PLAYER_HEADING, si);
            else if (d < 0x7fff) ww(ds, PLAYER_HEADING, (uint16_t)(rw(ds, PLAYER_HEADING) - 0x400));
            else ww(ds, PLAYER_HEADING, (uint16_t)(rw(ds, PLAYER_HEADING) + 0x400));
        }
    }
    ww(ls, (uint16_t)(obj + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0xfc7f)
                                            | (((rs(ds, PLAYER_HEADING) >> 13) & 7) << 7)));
    ls[(uint16_t)(obj + 0x18)] = (uint8_t)((ls[(uint16_t)(obj + 0x18)] & 0xe0)
                                           | ((rs(ds, PLAYER_HEADING) >> 8) & 0x1f));
    if (rw(ds, FALL_IMPACT)) {
        if (ds[FALL_HARDNESS]) {
            /* Fall damage: the impact's high byte, doubled while still
             * falling, is the roll's difficulty at twice that against
             * player_record+0x32 -- a success scales it by (30 - skill)/30. */
            uint8_t skill = ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x32)];
            int16_t d = (int16_t)(rw(ds, FALL_IMPACT) >> 8);
            if (rw(ds, VERTICAL_VELOCITY)) d = (int16_t)(uint16_t)(d << 1);
            if (check_skill_roll(m, skill, (int16_t)(uint16_t)(d << 1)) > 0)
                d = (int16_t)((int16_t)(uint16_t)(d * (0x1e - skill)) / 0x1e);
            if (d > 3)
                apply_damage(m, obj, 0, 0, 0, (uint8_t)d, 0);
            if (d > 1 || (ds[COLLISION_FACE] & 0x10))
                play_sound_effect(m, 0xf, 0x40, (int8_t)(uint8_t)(d * 4 - 0x3c));
        }
        ww(ds, FALL_IMPACT, 0);
    }
    set_movement_state(m, ds[COLLISION_FACE], 0);
    ds[PLAYER_IN_LIQUID] = 0;
}

/* ---- player_physics_step, from the instructions ------------- */

static void physics_step(uw_motion *m, uint16_t dt) {
    uint8_t *ds = m->ds;
    uint16_t id = (uint16_t)(m->lseg[rw(ds, TRACKED_OBJECT)]
                             | (m->lseg[(uint16_t)(rw(ds, TRACKED_OBJECT) + 1)] << 8));
    uint16_t pr = (uint16_t)(OBJ_PROPERTIES + (id & 0x1ff) * 11);
    ds[PLAYER_RADIUS] = (uint8_t)(ds[(uint16_t)(pr + 1)] & 7);
    ds[PLAYER_HEIGHT] = ds[pr];
    ww(ds, JITTER_PITCH, 0);
    ww(ds, JITTER_YAW, 0);
    ww(ds, SWAY_HORIZONTAL, 0);
    speed_update(m, dt);
    motion_run(m, PLAYER_X, RUN_FLAGS, (uint16_t)(FRAME_BP - 0x12 - 8 - 4 - 4 - 2));
    move_update(m);
    ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 10));
    if (!(ds[COLLISION_FACE] & 0x10)) {
        if ((rs(ds, SPEED_MAX_FORWARD) >> 2) < rs(ds, MOVEMENT_SPEED) && rw(ds, MOVEMENT_MODE) == 1) {
            int8_t cl = (int8_t)((int16_t)(uint16_t)(rw(ds, MOVEMENT_SPEED) << 2)
                                 / (rs(ds, SPEED_MAX_FORWARD) >> 1)) ;
            cl = (int8_t)(cl - 1);
            if (cl < 2) cl = 2;
            ds[STEPPED] = 1;
            ww(ds, SWAY_VERTICAL, (uint16_t)((int8_t)ds[(uint16_t)(0x72a + (ds[SWAY_PHASE] >> 4))] * cl));
        }
        if (rw(ds, MOVEMENT_MODE) == 7) {
            ww(ds, SWAY_VERTICAL, 0xffe0);
            ww(ds, JITTER_YAW, 0xff00);
            ds[STEPPED] = 1;
            ww(ds, MOVEMENT_MODE, 0);
        }
        if (rw(ds, MOVEMENT_MODE) == 9 || rw(ds, MOVEMENT_MODE) == 10) {
            ds[STEPPED] = 1;
            ww(ds, SWAY_VERTICAL, (uint16_t)((int8_t)ds[(uint16_t)(0x73a + (ds[SWAY_PHASE] >> 4))] << 1));
        }
    }
    ww(ds, MOVEMENT_MODE, 0);
}

/* motion_params_init: the motion block from the object and its
 * obj_properties row. A creature's and a static object's sub-fine position
 * is not stored, so its low bits are drawn from rand(). */
void motion_params_init(uw_motion *m, uint16_t obj, uint16_t blk) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t di = (uint16_t)(OBJ_PROPERTIES + (rw(ls, obj) & 0x1ff) * 11);
    int randomise = 1;
    ww(ds, (uint16_t)(blk + 0x20), obj_index_of(m, obj));
    ww(ds, (uint16_t)(blk + 0x18), (uint16_t)((rw(ds, (uint16_t)(di + 1)) >> 4) & 0xfff));
    ds[(uint16_t)(blk + 0x1a)] = (uint8_t)((ds[(uint16_t)(di + 6)] >> 4) & 1);
    ds[(uint16_t)(blk + 0x16)] = (uint8_t)((rw(ds, (uint16_t)(di + 6)) >> 5) & 0xf);
    ds[(uint16_t)(blk + 0x1c)] = ds[(uint16_t)(di + 8)];
    ds[(uint16_t)(blk + 0x1d)] = 0;
    ww(ds, (uint16_t)(blk + 0x1e), (uint16_t)(((rw(ls, (uint16_t)(obj + 2)) & 0x380) >> 7) << 13));
    ds[(uint16_t)(blk + 0x24)] = 0;
    ds[(uint16_t)(blk + 0x22)] = (uint8_t)(ds[(uint16_t)(di + 1)] & 7);
    ds[(uint16_t)(blk + 0x23)] = ds[di];
    ww(ds, blk, (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0xe000) >> 13));
    ww(ds, (uint16_t)(blk + 2), (uint16_t)((rw(ls, (uint16_t)(obj + 2)) & 0x1c00) >> 10));
    ww(ds, (uint16_t)(blk + 4), (uint16_t)(rw(ls, (uint16_t)(obj + 2)) & 0x7f));
    if (obj < rw(ds, STATIC_BASE)) {
        uint16_t w16 = rw(ls, (uint16_t)(obj + 0x16));
        ww(ds, blk, (uint16_t)(rw(ds, blk) + ((w16 >> 10) << 3)));
        ww(ds, (uint16_t)(blk + 2), (uint16_t)(rw(ds, (uint16_t)(blk + 2)) + (((w16 & 0x3f0) >> 4) << 3)));
        ww(ds, (uint16_t)(blk + 0x1e), (uint16_t)(ls[(uint16_t)(obj + 9)] << 8));
        ds[(uint16_t)(blk + 0x25)] = (uint8_t)(1 << ((ls[(uint16_t)(obj + 0xa)] & 0x70) >> 4));
        ww(ds, (uint16_t)(blk + 0xa), (uint16_t)((((ls[(uint16_t)(obj + 0x14)] & 0xf8) >> 3) - 16) << 6));
        ww(ds, (uint16_t)(blk + 0x10), (uint16_t)(((ls[(uint16_t)(obj + 0x13)] & 0x80) >> 7) * -4));
        ds[(uint16_t)(blk + 0x1b)] = ls[(uint16_t)(obj + 8)];
        if (((rw(ls, obj) & 0x1c0) >> 6) != 1) {
            randomise = 0;
            ww(ds, blk, rw(ls, (uint16_t)(obj + 0xb)));
            ww(ds, (uint16_t)(blk + 2), rw(ls, (uint16_t)(obj + 0xd)));
            ww(ds, (uint16_t)(blk + 4), rw(ls, (uint16_t)(obj + 0xf)));
        }
        ww(ds, (uint16_t)(blk + 0x14), (uint16_t)(ls[(uint16_t)(obj + 0x13)] & 0x7f));
        if (((rw(ls, obj) & 0x1c0) >> 6) != 1 && (rw(ds, (uint16_t)(blk + 0x10)) | rw(ds, (uint16_t)(blk + 0xa))) == 0
            && !((ds[(uint16_t)(di + 3)] >> 3) & 1)) {
            int8_t b1a = (int8_t)ds[(uint16_t)(blk + 0x1a)];
            if (b1a * 2 + 2 < rs(ds, (uint16_t)(blk + 0x14)))
                ww(ds, (uint16_t)(blk + 0x14), (uint16_t)((ls[(uint16_t)(obj + 0x13)] & 0x7f) * (b1a * 4 + 0x29)));
            else
                ww(ds, (uint16_t)(blk + 0x14), 0);
        } else {
            ww(ds, (uint16_t)(blk + 0x14), (uint16_t)(rs(ds, (uint16_t)(blk + 0x14)) * 0x2f));
            if (((rw(ls, obj) & 0x1c0) >> 6) == 1) ds[(uint16_t)(blk + 0x24)] = 8;
        }
    } else {
        ww(ds, (uint16_t)(blk + 0xa), 0);
        ww(ds, (uint16_t)(blk + 0x10), 0);
        ww(ds, (uint16_t)(blk + 0x14), 0);
        ds[(uint16_t)(blk + 0x1b)] = (uint8_t)(ls[(uint16_t)(obj + 4)] & 0x3f);
        ww(ds, blk, (uint16_t)(rw(ds, blk) + (rw(ds, MOTION_TILE_X) << 3)));
        ww(ds, (uint16_t)(blk + 2), (uint16_t)(rw(ds, (uint16_t)(blk + 2)) + (rw(ds, MOTION_TILE_Y) << 3)));
    }
    if (randomise) {
        ww(ds, blk, (uint16_t)((rw(ds, blk) << 5) + (rt_rand(m) & 0x1f)));
        ww(ds, (uint16_t)(blk + 2), (uint16_t)((rw(ds, (uint16_t)(blk + 2)) << 5) + (rt_rand(m) & 0x1f)));
        ww(ds, (uint16_t)(blk + 4), (uint16_t)((rw(ds, (uint16_t)(blk + 4)) << 3) + (rt_rand(m) & 7)));
    }
    ww(ds, (uint16_t)(blk + 0x26), 0);
}

/* object_terrain_test: the terrain-only query for an object where
 * it stands, through a query struct on the stack. */
uint16_t object_terrain_test(uw_motion *m, uint16_t obj, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t q = (uint16_t)(bp - 0x18), id = (uint16_t)(rw(ls, obj) & 0x1ff);
    uint16_t w16 = rw(ls, (uint16_t)(obj + 0x16)), w2 = rw(ls, (uint16_t)(obj + 2));
    ww(ds, SQ_PTR, q);
    ww(ds, (uint16_t)(q + 0xa), obj_index_of(m, obj));
    ds[(uint16_t)(q + 8)] = (uint8_t)(prop(m, id, 1) & 7);
    ds[(uint16_t)(q + 9)] = prop(m, id, 0);
    ww(ds, q, (uint16_t)(((w16 >> 10) << 3) + ((w2 & 0xe000) >> 13)));
    ww(ds, (uint16_t)(q + 2), (uint16_t)((((w16 & 0x3f0) >> 4) << 3) + ((w2 & 0x1c00) >> 10)));
    ww(ds, (uint16_t)(q + 4), (uint16_t)(w2 & 0x7f));
    sq_terrain(m, 8);
    return (uint16_t)(rw(ds, (uint16_t)(q + 0xc)) | rw(ds, (uint16_t)(q + 0xe)));
}

/* ---- movement_sound_update, from the instructions ------------ */

enum {
    FOOTSTEP_NEXT   = 0x0779,   /* footstep_next_clock, 32 bits */
    FOOTSTEP_FOOT   = 0x077d,
    FOOTSTEP_HANDLE = 0x077e,
    FOOTSTEP_CLOCK  = 0x077f    /* water_sound_clock, 32 bits */
};

uint32_t rd32(const uint8_t *ds, uint16_t at) {
    return (uint32_t)rw(ds, at) | ((uint32_t)rw(ds, (uint16_t)(at + 2)) << 16);
}
void wr32(uint8_t *ds, uint16_t at, uint32_t v) {
    ww(ds, at, (uint16_t)v);
    ww(ds, (uint16_t)(at + 2), (uint16_t)(v >> 16));
}

static void movement_sound_update(uw_motion *m, int force) {
    uint8_t *ds = m->ds;
    uint8_t state = ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0xb8)];
    if (state & 1) {
        /* One sustained effect, restarted every 0x1800 ticks. */
        if (ds[FOOTSTEP_HANDLE] != 0xff && rd32(ds, FOOTSTEP_CLOCK) + 0x1800 <= m->clock) {
            sound_effect_stop(m, ds[FOOTSTEP_HANDLE]);
            ds[FOOTSTEP_HANDLE] = 0xff;
        }
        if (ds[FOOTSTEP_HANDLE] == 0xff) {
            wr32(ds, FOOTSTEP_CLOCK, m->clock);
            ds[FOOTSTEP_HANDLE] = play_sound_effect(m, 0, 0x40, 0);
        }
        return;
    }
    if (ds[FOOTSTEP_HANDLE] != 0xff) {
        sound_effect_stop(m, ds[FOOTSTEP_HANDLE]);
        ds[FOOTSTEP_HANDLE] = 0xff;
    }
    if ((state & 8) || (ds[COLLISION_FACE] & 0x10)) return;
    /* a foot: the left (effect 2, pan 0x38) and the right (1, 0x48) in
     * turn, louder by the speed: (speed >> 5) - 0x10 */
    if (force) {
        play_sound_effect(m, ds[FOOTSTEP_FOOT] ? 1 : 2, ds[FOOTSTEP_FOOT] ? 0x48 : 0x38,
                          (int8_t)(uint8_t)((rw(ds, MOVEMENT_SPEED) >> 5) - 0x10));
        ds[FOOTSTEP_FOOT] = ds[FOOTSTEP_FOOT] ? 0 : 1;
        wr32(ds, FOOTSTEP_NEXT, m->clock + 100);
        return;
    }
    if (rw(ds, MOVEMENT_SPEED) == 0 || m->clock <= rd32(ds, FOOTSTEP_NEXT)) return;
    play_sound_effect(m, ds[FOOTSTEP_FOOT] ? 1 : 2, ds[FOOTSTEP_FOOT] ? 0x48 : 0x38,
                      (int8_t)(uint8_t)((rw(ds, MOVEMENT_SPEED) >> 5) - 0x10));
    ds[FOOTSTEP_FOOT] = ds[FOOTSTEP_FOOT] ? 0 : 1;
    {
        int16_t div = (int16_t)((rs(ds, MOVEMENT_SPEED) >> 2) + 1);
        uint16_t si;
        if (div == 0) {                     /* `idiv` by zero: the original faults */
            UW_NOT_CARRIED(m->not_carried);
            return;
        }
        si = (uint16_t)(6000 / div + 0x40);
        if (si > 200) si = 200;
        wr32(ds, FOOTSTEP_NEXT, m->clock + si);
    }
}

/* ---- movement_mode_from_keys, from the instructions --------- */

static void movement_mode_from_keys(uw_motion *m) {
    uint8_t *ds = m->ds;
    const uint8_t *k = m->keys;
    uint8_t shift, caps;
    int i;
    ww(ds, FORWARD_INPUT, 0);
    ww(ds, TURN_INPUT, 0);
    if (!k) {
        UW_NOT_CARRIED(m->not_carried);
        return;
    }
    shift = k[rw(ds, KEY_SHIFT_PTR)];
    caps = k[rw(ds, KEY_CAPS_PTR)];
    if ((shift != 0) != (caps != 0)) return;
    if (k[rw(ds, KEY_ALT_PTR)] || k[rw(ds, KEY_CTRL_PTR)]) return;
    for (i = 0; i < 9; i++) {
        uint8_t sc = ds[(uint16_t)(MOVEMENT_KEY_SCANCODES + i)];
        if (!k[(uint16_t)(rw(ds, KEY_STATE_PTR) + sc)]) continue;
        switch (sc) {
        case 0x11: ww(ds, FORWARD_INPUT, 0x70); ww(ds, MOVEMENT_MODE, 1); break;
        case 0x1f: ww(ds, FORWARD_INPUT, 0x32); ww(ds, MOVEMENT_MODE, 1); break;
        case 0x1e: ww(ds, TURN_INPUT, 0xffa6); ww(ds, MOVEMENT_MODE, 1); break;
        case 0x20: ww(ds, TURN_INPUT, 0x5a); ww(ds, MOVEMENT_MODE, 1); break;
        case 0x2d: ww(ds, FORWARD_INPUT, 0); ww(ds, TURN_INPUT, 0); ww(ds, MOVEMENT_MODE, 8); break;
        case 0x2c: ww(ds, FORWARD_INPUT, 0); ww(ds, TURN_INPUT, 0); ww(ds, MOVEMENT_MODE, 9); break;
        case 0x2e: ww(ds, FORWARD_INPUT, 0); ww(ds, TURN_INPUT, 0); ww(ds, MOVEMENT_MODE, 10); break;
        case 0x12: ww(ds, MOVEMENT_MODE, (ds[BLOCK_FLAGS] & 0x14) ? 0xc : 0); break;
        case 0x10: ww(ds, MOVEMENT_MODE, (ds[BLOCK_FLAGS] & 0x14) ? 0xd : 0); break;
        default: break;
        }
    }
}

/* ---- movement_set_mode and movement_input_update,
 * from the instructions ------------------------------------------------- */

enum {
    MOVEMENT_FROM_KEYBOARD = 0x0767,
    MOUSE_ZONE_MODES       = 0x0768,  /* three bytes: strafe left, back, strafe right */
    VIEW_HEIGHT            = 0x7280,
    VIEW_X0                = 0x7282,
    VIEW_Y0                = 0x735e,
    VIEW_WIDTH             = 0x7364,
    CURSOR_BOUNDS_COUNTDOWN = 0x0726
};

/* movement_set_mode(mode): movement_from_keyboard is mode >= 0.
 * A mode of 0 or more is the keyboard's: movement_mode_from_keys, then 0
 * stops (both magnitudes zeroed), and 6 and 7, the jumps, set the mode --
 * 1 instead while the player is in the air (collision_face bit 4) or
 * swimming (the record's +0xb8 is 1); the
 * rest leave what the keys set. -1 is the mouse's, once a pass: with the left
 * button alone down (or either with the record's +0x5f bit 1) the cursor's
 * place in the view (the event's +0 and +2, rows counted upward) picks the
 * movement -- in the lowest fifth mouse_zone_modes by thirds across (strafe
 * left, back, strafe right) with both magnitudes zero; otherwise mode 1,
 * turning by how far into the left or right third (0x180 over the width,
 * negative to the left) and going forward by how far above two fifths (0xc0
 * over the height). Both buttons down is mode 1, or the jump (7) unless
 * the player is in the air or swimming. */
void movement_set_mode(uw_motion *m, int16_t mode) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), ev, buttons;
    int16_t w, h, x, y;
    ds[MOVEMENT_FROM_KEYBOARD] = mode >= 0;
    if (mode >= 0) {
        movement_mode_from_keys(m);
        if (mode == 0) {
            ww(ds, FORWARD_INPUT, 0);
            ww(ds, TURN_INPUT, 0);
            ww(ds, MOVEMENT_MODE, 0);
        } else if (mode == 6 || mode == 7) {
            ww(ds, MOVEMENT_MODE, (ds[COLLISION_FACE] & 0x10) || ds[(uint16_t)(rec + 0xb8)] == 1 ? 1 : (uint16_t)mode);
        }
        return;
    }
    buttons = mouse_sample_buttons(m);
    if (!(buttons == 1 || ((buttons & 1) && ((ds[(uint16_t)(rec + 0x5f)] >> 1) & 1)))) {
        if (buttons == 3) {
            ww(ds, MOVEMENT_MODE, 1);
            if (!(ds[COLLISION_FACE] & 0x10) && ds[(uint16_t)(rec + 0xb8)] != 1) ww(ds, MOVEMENT_MODE, 7);
        }
        return;
    }
    ww(ds, TURN_INPUT, 0);
    ww(ds, FORWARD_INPUT, 0);
    ev = rw(ds, 0x00e2);
    w = rs(ds, VIEW_WIDTH);
    h = rs(ds, VIEW_HEIGHT);
    x = rs(ds, ev);
    y = rs(ds, (uint16_t)(ev + 2));
    if ((int16_t)(h / 5) > y) {
        ww(ds, MOVEMENT_MODE, ds[(uint16_t)(MOUSE_ZONE_MODES + (int16_t)((int16_t)(x * 3) / w))]);
        return;
    }
    if ((int16_t)(w / 3) > x)
        ww(ds, TURN_INPUT, (uint16_t)((int16_t)-(int16_t)((int16_t)(w / 3 - x) * 0x180) / w));
    if ((int16_t)(w * 2 / 3) < x)
        ww(ds, TURN_INPUT, (uint16_t)((int16_t)((int16_t)(x - w * 2 / 3) * 0x180) / w));
    if ((int16_t)(h * 2 / 5) < y)
        ww(ds, FORWARD_INPUT, (uint16_t)((int16_t)((int16_t)(y - h * 2 / 5) * 0xc0) / h));
    ww(ds, MOVEMENT_MODE, 1);
}

/* ---- the free camera: debug_camera_goto, debug_camera_mouse_drag and
 * debug_camera_set_target. view_place_camera takes the free camera --
 * free_camera_x/y/z, its heading, pitch and roll -- whenever
 * camera_follow_ptr is null. ---- */
enum { FREE_CAM_HEADING = 0x358c, FREE_CAM_PITCH = 0x358e, FREE_CAM_X = 0x3592, FREE_CAM_Y = 0x3594,
       FREE_CAM_Z = 0x3596 };

static int camera_is_free(const uw_motion *m) {
    return !(rw(m->ds, VIEW_CAMERA_OBJECT) | rw(m->ds, (uint16_t)(VIEW_CAMERA_OBJECT + 2)));
}

/* debug_camera_goto(which): 0 and 1 put the free camera on the Avatar's
 * position and heading -- 1 at the eye (z + 0xa4), 0 high above (z 0x458)
 * looking straight down (pitch 0xfc00); 2..0xff at that object, from its
 * own fields. Event 2 when the free camera is the one shown. */
void debug_camera_goto(uw_motion *m, int16_t which) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    if (which < 2) {
        ww(ds, FREE_CAM_X, rw(ds, PLAYER_X));
        ww(ds, FREE_CAM_Y, rw(ds, PLAYER_Y));
        ww(ds, FREE_CAM_HEADING, rw(ds, 0x727a));
        if (which == 1) ww(ds, FREE_CAM_Z, (uint16_t)(rw(ds, PLAYER_Z) + 0xa4));
        else {
            ww(ds, FREE_CAM_Z, 0x458);
            ww(ds, FREE_CAM_PITCH, 0xfc00);
        }
    } else if (which < 0x100) {
        uint16_t o = obj_at(m, (uint16_t)which), w2, w16;
        if (!o) { UW_NOT_CARRIED(m->not_carried); return; }
        w2 = rw(ls, (uint16_t)(o + 2));
        w16 = rw(ls, (uint16_t)(o + 0x16));
        ww(ds, FREE_CAM_X, (uint16_t)((w16 >> 10) * 0x100 + (w2 >> 13) * 0x20));
        ww(ds, FREE_CAM_Y, (uint16_t)(((w16 & 0x3f0) >> 4) * 0x100 + ((w2 & 0x1c00) >> 10) * 0x20));
        ww(ds, FREE_CAM_Z, (uint16_t)((w2 & 0x7f) << 3));
        ww(ds, FREE_CAM_HEADING, (uint16_t)(((w2 & 0x380) >> 7) << 13));
    }
    if (camera_is_free(m)) ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));    /* post_event(2) */
}

/* debug_camera_set_target (attach_eye)(mode), through the five table words
 * at 0c9e by mode + 1: -1 clears camera_follow_ptr, the free camera then
 * shown (no event); 1 puts it back on the Avatar, with event 2 when it was
 * not there. 3 points it at the record two before the mobile list's
 * (the vortex's eye) when it is at or past the one before, and falls into
 * 2, which points it at the one before (the chase view) when it is at or
 * past the list -- offsets compared, as far pointers are -- each with
 * event 2. view_place_camera renders neither in the port: the vortex's
 * frames are the host's (view_hook). 0, a creature's eye, is counted. */
void debug_camera_set_target(uw_motion *m, int16_t mode) {
    uint8_t *ds = m->ds;
    if (mode == -1) {
        ww(ds, VIEW_CAMERA_OBJECT, 0);
        ww(ds, (uint16_t)(VIEW_CAMERA_OBJECT + 2), 0);
    } else if (mode == 1) {
        if (rw(ds, VIEW_CAMERA_OBJECT) != rw(ds, TRACKED_OBJECT)
            || rw(ds, (uint16_t)(VIEW_CAMERA_OBJECT + 2)) != rw(ds, (uint16_t)(TRACKED_OBJECT + 2))) {
            ww(ds, VIEW_CAMERA_OBJECT, rw(ds, TRACKED_OBJECT));
            ww(ds, (uint16_t)(VIEW_CAMERA_OBJECT + 2), rw(ds, (uint16_t)(TRACKED_OBJECT + 2)));
            ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));    /* post_event(2) */
        }
    } else if (mode == 2 || mode == 3) {
        /* critdata, the mobile list's start: the Avatar is its first record */
        uint16_t crit = (uint16_t)(rw(ds, TRACKED_OBJECT) - 0x1b);
        if (mode == 3 && (uint16_t)(crit - 0x1b) <= rw(ds, VIEW_CAMERA_OBJECT)) {
            ww(ds, VIEW_CAMERA_OBJECT, (uint16_t)(crit - 2 * 0x1b));
            ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));    /* post_event(2) */
        }
        if (rw(ds, VIEW_CAMERA_OBJECT) >= crit) {
            ww(ds, VIEW_CAMERA_OBJECT, (uint16_t)(crit - 0x1b));
            ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));
        }
    } else if (mode == 0) {
        UW_NOT_CARRIED(m->not_carried);
    }
}

/* debug_camera_mouse_drag, movement_input_update's other input: the click's
 * position in the view (the event's +0 and +2) in thirds -- the x third
 * turns the free camera by (third - 1) * 0x400, the y third (bottom-up)
 * moves it back, not at all, or forward along its heading by the sine and
 * cosine (gfx_sincos_lerp) >> 8 -- clamped to 0x180..0x3d80 on both axes;
 * event 2 when the free camera is shown. */
void debug_camera_mouse_drag(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t ev = rw(ds, 0x00e2);
    int16_t along = (int16_t)(rs(ds, (uint16_t)(ev + 2)) * 3 / rs(ds, 0x7280));
    int16_t x, y;
    ww(ds, FREE_CAM_HEADING, (uint16_t)(rw(ds, FREE_CAM_HEADING)
                                        + (rs(ds, ev) * 3 / rs(ds, 0x7364) - 1) * 0x400));
    x = rs(ds, FREE_CAM_X);
    y = rs(ds, FREE_CAM_Y);
    if (along != 1) {
        int16_t s, c;
        uw_sincos_lerp(rw(ds, FREE_CAM_HEADING), &s, &c);
        x = (int16_t)(x + (s >> 8) * (along - 1));
        y = (int16_t)(y + (c >> 8) * (along - 1));
    }
    if (x < 0x180) x = 0x180;
    if (x > 0x3d80) x = 0x3d80;
    if (y < 0x180) y = 0x180;
    if (y > 0x3d80) y = 0x3d80;
    ww(ds, FREE_CAM_X, (uint16_t)x);
    ww(ds, FREE_CAM_Y, (uint16_t)y);
    if (camera_is_free(m)) ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));    /* post_event(2) */
}

/* movement_input_update, the held left button in the view:
 * with movement_input_override set -- Roaming Sight's timed
 * effect sets it -- the free camera flies instead
 * (debug_camera_mouse_drag); otherwise movement_set_mode(-1), and with the
 * bounds countdown at 0 the cursor kept to the viewport (cursor_set_bounds)
 * and the countdown set to 2 -- event_dispatch counts it down and puts the
 * whole screen back (cursor_reset_bounds) when it reaches 0. */
void movement_input_update(uw_motion *m) {
    uint8_t *ds = m->ds;
    if (ds[0x0284]) {
        debug_camera_mouse_drag(m);
        return;
    }
    movement_set_mode(m, -1);
    if (ds[CURSOR_BOUNDS_COUNTDOWN]) return;
    ww(ds, CURSOR_BOUND_X0, rw(ds, VIEW_X0));
    ww(ds, CURSOR_BOUND_Y0, rw(ds, VIEW_Y0));
    ww(ds, CURSOR_BOUND_X1, (uint16_t)(rw(ds, VIEW_X0) + rw(ds, VIEW_WIDTH) - 1));
    ww(ds, CURSOR_BOUND_Y1, (uint16_t)(rw(ds, VIEW_Y0) + rw(ds, VIEW_HEIGHT) - 1));
    ds[CURSOR_BOUNDS_COUNTDOWN] = 2;
}

/* event_dispatch's tail: after the pending events, the
 * bounds countdown stepped, and at 0 cursor_reset_bounds puts
 * the cursor's bounds back to the whole screen. game_loop runs event_dispatch
 * whenever an event is pending and input_tick after it: a frame's pass
 * dispatches the timer's event, and a moving frame's posted events in a
 * second round before the input. */
void uw_motion_dispatch_countdown(uw_motion *m) {
    uint8_t *ds = m->ds;
    if (!ds[CURSOR_BOUNDS_COUNTDOWN] || --ds[CURSOR_BOUNDS_COUNTDOWN]) return;
    cursor_reset_bounds(m);
}

/* cursor_reset_bounds: the cursor's clamp box the whole screen
 * again, (0, 0) to (0x13f, 199). */
void cursor_reset_bounds(uw_motion *m) {
    uint8_t *ds = m->ds;
    ww(ds, CURSOR_BOUND_Y0, 0);
    ww(ds, CURSOR_BOUND_X0, 0);
    ww(ds, CURSOR_BOUND_X1, 0x13f);
    ww(ds, CURSOR_BOUND_Y1, 199);
}

/* ---- player_frame_update ----------------------------------- */

void uw_motion_player_frame(uw_motion *m, uint16_t dt, uint8_t substeps, int still) {
    uint8_t *ds = m->ds;
    ds[STEPPED] = 0;
    ww(ds, SWAY_VERTICAL, 0);
    ds[SWAY_PHASE] = (uint8_t)(ds[SWAY_PHASE] + (uint8_t)dt);
    if (rw(ds, MOVEMENT_MODE) == 0)
        movement_mode_from_keys(m);
    if ((rw(ds, MOVEMENT_MODE) || rw(ds, MOVEMENT_SPEED) || rw(ds, VERTICAL_VELOCITY)
         || rw(ds, VERTICAL_GRAVITY) || rw(ds, MOVEMENT_INPUT_B) || rw(ds, MOVEMENT_INPUT_A)
         || ds[PLAYER_IN_LIQUID]) && !still) {
        m->physics_steps++;
        physics_step(m, dt);
    }
    if (ds[MOBILES_ENABLED] && !ds[TIME_FROZEN] && substeps)
        update_mobile_objects(m, substeps);
    if (ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0xb8)])
        view_apply_impairment(m);
    update_noise(m, still);
    movement_sound_update(m, still);
}

/* ---- dungeon_frame_tick, from the instructions --------------- */

/* The routine reads the clock four times: the elapsed time and play time's
 * increment from the first, the turn phase's 16-tick boundaries from the
 * second, the effect ticks' 64-tick ones from the third -- level_effects_tick
 * runs after it -- and the stamp from the fourth. `clock_first` is the first
 * when the timer ticked between it and the fourth; the second and third fall
 * between them, and are taken from `clock`, which gives the same boundaries
 * whenever the two readings share their 16-tick one. */
/* level_effects_tick(ticks) over the list the data segment
 * holds, with the pools' live pointers and the RNG state, and `bp` its
 * frame: level_effect_expire's is 0x10 under it (two bytes of locals, SI,
 * DI, two words of arguments), where a moving door's seat runs. */
struct effects_frame { uw_motion *m; uint16_t bp; };

static int effects_door_seat(void *user, uw_effects *e, int index) {
    struct effects_frame *f = (struct effects_frame *)user;
    return level_effect_door_seat(f->m, e, index, (uint16_t)(f->bp - 2 - 4 - 4 - 4 - 2));
}

/* door_closing_check from level_effect_animate, whose frame is 0x10 under
 * the tick's (two words of arguments, a far return, BP) or, in the
 * expiry's last pass, 0x1c under the expiry's (0xe of locals, SI, DI, two
 * words, the return, BP); the move's own is 0x18 under the animate's (0xa
 * of locals, SI, DI, two words, the return, BP). */
static void effects_door_move(void *user, uw_effects *e, int index, int step) {
    struct effects_frame *f = (struct effects_frame *)user;
    uint16_t let_bp = f->bp, lee_bp = (uint16_t)(let_bp - 2 - 4 - 4 - 4 - 2);
    uint16_t an_bp = e->in_expire ? (uint16_t)(lee_bp - 0xe - 4 - 4 - 4 - 2) : (uint16_t)(let_bp - 2 - 4 - 4 - 4 - 2);
    level_effect_door_move(f->m, e, index, step, (uint16_t)(an_bp - 0xa - 4 - 4 - 4 - 2));
}

void level_effects_tick(uw_motion *m, uint8_t ticks, uint16_t bp) {
    uint8_t *ds = m->ds;
    static uw_effects e;
    struct effects_frame frame;
    uw_objpool pool;
    uw_rng rng;
    int i;
    /* level_effects_tick's first act: with an animated thing in
     * the view (drawlist_object_x, which level_effect_add also sets) the
     * view is asked to redraw, post_event(2) */
    if (ds[0x2e1c]) ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));
    uw_effects_from_ds(&e, ds);
    frame.m = m;
    frame.bp = bp;
    e.door_seat = effects_door_seat;
    e.door_move = effects_door_move;
    e.door_user = &frame;
    memset(&pool, 0, sizeof pool);
    pool.seg = m->lseg;
    pool.mobile_sp = rw(ds, 0x2756);
    pool.static_sp = rw(ds, 0x274a);
    pool.active_end = rw(ds, 0x2732);
    e.pool = &pool;
    rng.state = (uint32_t)rw(ds, 0x207e) | ((uint32_t)rw(ds, 0x2080) << 16);
    uw_effects_tick(&e, m->lseg, ticks, &rng);
    ww(ds, 0x207e, (uint16_t)rng.state);
    ww(ds, 0x2080, (uint16_t)(rng.state >> 16));
    ww(ds, 0x2756, pool.mobile_sp);
    ww(ds, 0x274a, pool.static_sp);
    ww(ds, 0x2732, pool.active_end);
    ds[0x3656] = (uint8_t)e.count;
    ds[0x0aae] = (uint8_t)e.moved;
    for (i = 0; i < UW_EFFECTS_MAX; i++) {
        uint16_t at = (uint16_t)(0x369c + i * 6);
        ww(ds, at, e.rec[i].word0);
        ww(ds, (uint16_t)(at + 2), (uint16_t)e.rec[i].timer);
        ds[(uint16_t)(at + 4)] = e.rec[i].tile_x;
        ds[(uint16_t)(at + 5)] = e.rec[i].tile_y;
    }
    m->not_carried += e.unsupported;
}

/* player_settle_motion: the movement mode cleared, then
 * player_physics_step(0x40) over and over until every velocity component is
 * zero -- sixty-four ticks of simulated time a pass, with no rendering, so
 * the player coasts to a halt at once from the outside. What a level change
 * and a sleep call so that nobody arrives still sliding.
 *
 * Two of the six the loop waits on, the movement inputs, have no writer but
 * zero in this build, so the wait is on the speed, the two velocities and
 * the liquid flag. The cap is the port's: a loop with no exit is not
 * something to leave in a program, and it is counted if it is ever reached. */
void player_settle_motion(uw_motion *m) {
    uint8_t *ds = m->ds;
    int passes = 0;
    ww(ds, MOVEMENT_MODE, 0);
    while (rw(ds, MOVEMENT_SPEED) || rw(ds, VERTICAL_VELOCITY) || rw(ds, VERTICAL_GRAVITY)
           || rw(ds, MOVEMENT_INPUT_B) || rw(ds, MOVEMENT_INPUT_A) || ds[PLAYER_IN_LIQUID]) {
        if (++passes > 0x400) { UW_NOT_CARRIED(m->not_carried); return; }
        physics_step(m, 0x40);
    }
}

void uw_motion_frame(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint32_t stamp = (uint32_t)rw(ds, CLOCK_STAMP) | ((uint32_t)rw(ds, CLOCK_STAMP + 2) << 16);
    uint32_t elapsed = (m->clock_first ? m->clock_first : m->clock) - stamp;
    uint8_t effect_ticks, substeps;

    if (elapsed > 0x40) {                   /* `ja` on the high word, `jbe` on the low */
        elapsed = 0x40;
        ds[TURN_PHASE] = (uint8_t)(ds[TURN_PHASE] + 4);
        effect_ticks = 1;
    } else {
        /* rt_lshr by 4 and by 6 of both clocks, low bytes subtracted. */
        ds[TURN_PHASE] = (uint8_t)(ds[TURN_PHASE]
                                   + (uint8_t)((m->clock >> 4) - (stamp >> 4)));
        effect_ticks = (uint8_t)((m->clock >> 6) - (stamp >> 6));
    }
    if (elapsed == 0) return;
    /* on this routine's frame: six bytes of locals, the argument */
    if (effect_ticks && ds[EFFECTS_ON_MOVE]) level_effects_tick(m, effect_ticks, (uint16_t)(FRAME_BP - 6 - 2 - 4 - 2));
    {
        uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
        uint32_t t = (uint32_t)rw(ds, (uint16_t)(rec + 0xce))
                   | ((uint32_t)rw(ds, (uint16_t)(rec + 0xd0)) << 16);
        t += elapsed;
        ww(ds, (uint16_t)(rec + 0xce), (uint16_t)t);
        ww(ds, (uint16_t)(rec + 0xd0), (uint16_t)(t >> 16));
    }
    ww(ds, CLOCK_STAMP, (uint16_t)m->clock);
    ww(ds, CLOCK_STAMP + 2, (uint16_t)(m->clock >> 16));
    substeps = ds[TURN_PHASE];
    if (ds[TURN_HALVED]) {
        substeps = (uint8_t)(substeps >> 1);
        ds[TURN_PHASE] &= 1;
    } else {
        ds[TURN_PHASE] = 0;
    }
    uw_motion_player_frame(m, (uint16_t)elapsed, substeps, 0);
}
