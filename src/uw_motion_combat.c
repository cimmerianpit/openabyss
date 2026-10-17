/* SPDX-License-Identifier: MIT */
/* combat and what a blow reaches -- damage, death,
 * the hit regions, find_target and execute_attack -- a missile's hit
 * with object_use_dispatch and
 * motion_knockback, and the player's swing (combat_swing).
 *
 * One of src/uw_motion*.c: see uw_motion.h and uw_motion_int.h. */
#include "uw_motion_int.h"

/* ==== combat and what a blow reaches */

/* roll_dice: count dice of `sides`, each rand() * sides / 0x8000
 * plus one. */
int16_t roll_dice(uw_motion *m, int16_t count, int16_t sides) {
    int16_t si = count;
    if (sides <= 0 || count <= 0) return si;
    while (count--)
        si = (int16_t)(si + (int16_t)(((int32_t)rt_rand(m) * sides) / 0x8000));
    return si;
}

/* compute_damage: an object's obj_properties byte +8 resists the
 * damage types it shares with `type`; types 1 and 2 by chance, rand() % 3
 * under the low two bits. */
uint8_t compute_damage(uw_motion *m, uint16_t obj, uint8_t dmg, uint8_t type) {
    uint8_t res = prop(m, obj_id(m, obj), 8);
    if (!(res & type)) return dmg;
    if (type & 3) {
        if (rt_rand(m) % 3 < (res & 3)) return 0;
        type &= 0xfc;
    }
    if (type & res) return 0;
    return dmg;
}

/* The music a hurt creature or player asks for: set_theme_music, and
 * combat_music_time from the clock. */
static void combat_music(uw_motion *m, uint8_t theme) {
    m->ds[MUSIC_TRACK_WANTED] = theme;
    ww(m->ds, COMBAT_MUSIC_TIME, (uint16_t)m->clock);
    ww(m->ds, (uint16_t)(COMBAT_MUSIC_TIME + 2), (uint16_t)(m->clock >> 16));
}

/* use_special_npc, by the creature's whoami (+0x1a). At a death
 * (`removal` 0) Thorlson (0x0b) and the golem (0x16) refuse it and talk
 * instead; at the dead creature's removal (1) Garamon (0x1b) clears
 * player_record+0x62 bit 2, 0x18, 0x6e and 0x8e set bits 6, 4 and 11 of
 * +0x65, and Tyball (0xe7) runs trap_tyball_death. 0 refuses. */
int use_special_npc(uw_motion *m, uint16_t obj, int removal) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), flag = 0;
    switch (ls[(uint16_t)(obj + 0x1a)]) {
    case 0x1b:
        if (removal) ds[(uint16_t)(rec + 0x62)] &= 0xfb;
        return 1;
    case 0xe7:
        if (removal) trap_tyball_death(m);
        return 1;
    case 0x0b:
        if (removal) return 1;
        m->talk_object = obj;               /* conv_begin_with_object: Thorlson talks (the host's) */
        ls[(uint16_t)(obj + 8)] = 0x3c;
        return 0;
    case 0x16:
        if (removal) return 1;
        /* the golem: 500 experience the first time (+0x0a bits 4..6 stamped
         * 1), its owner cleared, combat_reset, idle, and a conversation */
        if (((ls[(uint16_t)(obj + 0xa)] & 0x70) >> 4) == 0) {
            ls[(uint16_t)(obj + 0xa)] = (uint8_t)((ls[(uint16_t)(obj + 0xa)] & 0x8f) | 0x10);
            player_gain_experience(m, 500);
        }
        ls[(uint16_t)(obj + 0x12)] = 0;
        combat_reset(m);
        ls[(uint16_t)(obj + 0x15)] = (uint8_t)((ls[(uint16_t)(obj + 0x15)] & 0xc0) | 0x20);
        ww(ls, (uint16_t)(obj + 0xb), (uint16_t)(rw(ls, (uint16_t)(obj + 0xb)) & 0xfff));
        m->talk_object = obj;
        return 0;
    case 0x18: flag = 0x40; break;
    case 0x6e: flag = 0x10; break;
    case 0x8e: flag = 0x800; break;
    default:
        return 1;
    }
    if (removal) ww(ds, (uint16_t)(rec + 0x65), (uint16_t)(rw(ds, (uint16_t)(rec + 0x65)) | flag));
    return 1;
}

/* creature_process_death: a creature with a whoami (+0x1a) may
 * refuse through use_special_npc(obj, 0); then action 0xc, dying, with the
 * counter cleared, motion mode 4 and no hit points. */
static int creature_process_death(uw_motion *m, uint16_t obj) {
    uint8_t *ls = m->lseg;
    if (ls[(uint16_t)(obj + 0x1a)] && !use_special_npc(m, obj, 0))
        return 0;
    ls[(uint16_t)(obj + 0x15)] = (uint8_t)((ls[(uint16_t)(obj + 0x15)] & 0xc0) | 0xc);
    ww(ls, (uint16_t)(obj + 0xb), (uint16_t)(rw(ls, (uint16_t)(obj + 0xb)) & 0xfff));
    ls[(uint16_t)(obj + 0x14)] = (uint8_t)((ls[(uint16_t)(obj + 0x14)] & 0xf8) | 4);
    ls[(uint16_t)(obj + 8)] = 0;
    return 1;
}

/* creature_die: not twice; creature_process_death; and a sound
 * when the CURRENT tick's critter row (not the dying creature's) has remains
 * kind 1. 1 when it dies. */
static int creature_die(uw_motion *m, uint16_t obj) {
    if ((m->lseg[(uint16_t)(obj + 0x15)] & 0x3f) == 0xc) return 0;
    if (!creature_process_death(m, obj)) return 0;
    if ((crit(m, 8) & 7) == 1)
        play_sound_effect_at_xy(m, 6, rs(m->ds, AI_SELF_FINE_X), rs(m->ds, AI_SELF_FINE_Y), 0);
    return 1;
}

/* creature_take_damage: hit points, the attacker remembered, the
 * assault record when the player struck, death, and the fight music. 1 when
 * it died. */
static int creature_take_damage(uw_motion *m, uint16_t obj, uint8_t dmg, uint16_t attacker) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t row = (uint16_t)(CRITTER_BASE + (rw(ls, obj) & 0x3f) * 0x30);
    uint8_t who = 0;
    ls[(uint16_t)(obj + 0x11)] = (uint8_t)(ls[(uint16_t)(obj + 0x11)] + dmg);
    if (attacker) {
        if (((rw(ls, attacker) & 0x1c0) >> 6) != 1) {
            who = ls[(uint16_t)(attacker + 0x12)];
        } else {
            uint16_t idx = obj_index_of(m, attacker);
            who = (uint8_t)(idx >= 0x100 ? 0 : idx);
        }
    }
    if (who) ls[(uint16_t)(obj + 0x12)] = who;
    if (who == 1 && !(ls[(uint16_t)(obj + 0xa)] & 0x80)) {
        uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
        ds[ASSAULT_VICTIM_RACE] = ds[(uint16_t)(row + 9)];
        ds[ASSAULT_VICTIM_INDEX] = (uint8_t)obj_index_of(m, obj);
        ds[ASSAULT_TILE_X] = (uint8_t)(rw(ls, (uint16_t)(obj + 0x16)) >> 10);
        ds[ASSAULT_TILE_Y] = (uint8_t)((rw(ls, (uint16_t)(obj + 0x16)) & 0x3f0) >> 4);
        ds[ASSAULT_Z] = (uint8_t)((rw(ls, (uint16_t)(obj + 2)) & 0x7f) >> 3);
        ww(ds, ASSAULT_TIME, rw(ds, (uint16_t)(rec + 0xce)));
        ww(ds, (uint16_t)(ASSAULT_TIME + 2), rw(ds, (uint16_t)(rec + 0xd0)));
    }
    if (!(ls[(uint16_t)(obj + 8)] > dmg)) {
        ls[(uint16_t)(obj + 8)] = 0;
        if (creature_die(m, obj)) {
            if (who == 1) award_kill_exp(m, obj);
            return 1;
        }
    } else {
        ls[(uint16_t)(obj + 8)] = (uint8_t)(ls[(uint16_t)(obj + 8)] - dmg);
        if (obj == rw(ds, TRACKED_OBJECT)) panels_refresh(m);
    }
    if (who == 1) {
        int16_t f = (int16_t)((ls[(uint16_t)(obj + 8)] << 6) / (ds[(uint16_t)(row + 4)] + 1));
        combat_music(m, (uint8_t)(f < 0x10 ? 5 : 6));
    } else if (obj == rw(ds, TRACKED_OBJECT) && who) {
        uint16_t prow = rw(ds, CRITTER_ROW_PTR), pl = rw(ds, TRACKED_OBJECT);
        int16_t f = (int16_t)((ls[(uint16_t)(pl + 8)] << 6) / (ds[(uint16_t)(prow + 4)] + 1));
        combat_music(m, (uint8_t)(f < 0x10 ? 7 : 6));
    }
    return 0;
}

/* object_damage: a thing's hit points less the damage shifted
 * down by its toughness (obj_properties +6 bits 2..3, 3 unbreakable; word 0
 * bit 13 immune) -- a mobile's +8, a door's +6 bits 1..5 (which never break),
 * anything else's quality. 1 when it broke; a broken static thing's link is
 * triggered at (x, y). */
static int object_damage(uw_motion *m, uint16_t obj, uint16_t attacker, int16_t dmg, int16_t x, int16_t y) {
    uint8_t *ls = m->lseg;
    uint16_t id = obj_id(m, obj);
    int broke = 0, mobile = obj < rw(m->ds, STATIC_BASE);
    int16_t k, v;
    (void)attacker; (void)y;
    if (rw(ls, obj) & 0x2000) return 0;
    k = (int16_t)((prop(m, id, 6) >> 2) & 3);
    if (k == 3) return 0;
    dmg = (int16_t)(dmg >> k);
    if (dmg <= 0) return 0;
    if (mobile) {
        v = (int16_t)(ls[(uint16_t)(obj + 8)] - dmg);
        if (v <= 0) { v = 0; broke = 1; }
        ls[(uint16_t)(obj + 8)] = (uint8_t)v;
    } else if (id >= 0x140 && id <= 0x147 && (ls[(uint16_t)(obj + 6)] & 1) && ((ls[(uint16_t)(obj + 6)] & 0x3f) >> 1)) {
        v = (int16_t)(((ls[(uint16_t)(obj + 6)] & 0x3f) >> 1) - dmg);
        if (v <= 0) v = 0;
        ls[(uint16_t)(obj + 6)] = (uint8_t)((ls[(uint16_t)(obj + 6)] & 0xc0) | (((ls[(uint16_t)(obj + 6)] & 1) | (v << 1)) & 0x3f));
    } else {
        v = (int16_t)((ls[(uint16_t)(obj + 4)] & 0x3f) - dmg);
        if (v <= 0) { broke = 1; v = 0; }
        ls[(uint16_t)(obj + 4)] = (uint8_t)((ls[(uint16_t)(obj + 4)] & 0xc0) | (v & 0x3f));
    }
    if (broke && !mobile && x > -1)
        /* trigger_object_link(attacker, obj, 4, x, y) on its frame: this
         * routine's six bytes of locals and SI, DI under apply_damage's
         * callee frame (`debris_bp`, the same fourteen bytes of arguments),
         * and seven words pushed */
        trigger_object_link_at(m, attacker, obj, 4, (uint16_t)x, (uint16_t)y,
                               m->debris_bp ? (uint16_t)(m->debris_bp - 6 - 4 - 14 - 4 - 2) : (uint16_t)(FRAME_BP - 0x80));   /* a frame of the port's when the caller names none */
    return broke;
}

/* object_damage_debris(obj, attacker, type, x, y): what a thing that broke becomes. A
 * mobile one (x under 0) is left, 1. A door (class 0x14) is opened when shut
 * and its lock removed, a barrel or a chest (0x15b, 0x15d) unlocked and opened,
 * a container (class 8) emptied unless object_cull_test(10) keeps it;
 * object_damage never breaks a door. Anything else: fire
 * (type bit 3) on debris already (0xd5, 0xd6) takes it out of its tile
 * chain -- object_detach, object_remove of the chain not
 * forced, 1 when that removed it -- and on the rest one time in four
 * (rand() & 3 == 0) leaves a class-7 kind 8 (0x1c8) at (x, y) for 6d10
 * steps, spawn_class7_object(obj, 8, roll_dice(6, 10), 0, 0, x, y), and
 * picks the debris id at once. The thing's chain is cleared (word 0 bit
 * 15 clear and +6's link set: object_chain_clear); an id not yet picked is
 * 0xd5 + rand() * 2 / 0x8000 -- 0xd5 or 0xd6 by rand() >= 0x4000; it goes
 * into word 0, and a static thing is placed over the spatial query on
 * placed_object_collision's frame, 0x18 below this one (obj, x, y, 1) -- 1
 * when that removed it, the thing having left the map; else 0. The frame is the
 * caller's to name (`debris_bp`, damage_objects_in_tile's chain); without
 * it a frame of the port's stands in. flame2: the explosions the flame wind made
 * burn in their own fire. */
static int object_damage_debris(uw_motion *m, uint16_t obj, uint16_t attacker, uint8_t type, int16_t x, int16_t y) {
    uint8_t *ls = m->lseg, *ds = m->ds;
    uint16_t w0 = rw(ls, obj), id = (uint16_t)(w0 & 0x1ff);
    int16_t debris = -2;
    (void)attacker;
    if (x < 0) return 1;
    if ((id >> 4) == 0x14) {
        /* a door: a shut one (nibble under 8) opened at the target tile, its
         * locks removed (door_lock_remove(obj, 1)), and it stays */
        if ((id & 0xf) < 8) {
            ww(ds, ACTION_TARGET_TILE_X, (uint16_t)x);
            ww(ds, ACTION_TARGET_TILE_Y, (uint16_t)y);
            door_open(m, attacker, obj);
        }
        door_lock_remove(m, obj, 1);
        debris = -1;
    } else if (id == 0x15d || id == 0x15b) {
        /* a barrel or a chest: the first lock removed and the container
         * opened where it stands (open_container(0, obj, 0)) */
        ww(ds, ACTION_TARGET_TILE_X, (uint16_t)x);
        ww(ds, ACTION_TARGET_TILE_Y, (uint16_t)y);
        door_lock_remove(m, obj, 0);
        open_container(m, 0, obj, 0);
    } else if ((id >> 4) == 8) {
        if (!object_cull_test(m, 10, obj)) debris = -1;
        else container_empty(m, obj, 0);
    } else {
        if (type & 8) {
            if (id == 0xd5 || id == 0xd6) {
                uint16_t link = (uint16_t)(tile_ptr(m, (uint16_t)x, (uint16_t)y) + 2);
                if (obj < rw(ds, STATIC_BASE)) {
                    ls[(uint16_t)(obj + 8)] = 0;
                } else if (object_remove(m, link, obj, 0) == 0) {
                    return 1;
                }
                debris = -1;
            } else if ((rt_rand(m) & 3) == 0) {
                int16_t steps = roll_dice(m, 6, 10);
                spawn_class7(m, obj, 8, steps, 0, 0, (uint8_t)x, (uint8_t)y);
                debris = (int16_t)(0xd5 + (rt_rand(m) >= 0x4000));
            }
        }
        if (!(rw(ls, obj) & 0x8000) && (rw(ls, (uint16_t)(obj + 6)) >> 6))
            object_chain_clear(m, (uint16_t)(obj + 6));
    }
    if (debris < -1) debris = (int16_t)(0xd5 + (rt_rand(m) >= 0x4000));
    if (debris >= 0) {
        id = (uint16_t)(debris & 0x1ff);
        ww(ls, obj, (uint16_t)((rw(ls, obj) & 0xfe00) | id));
        if (obj >= rw(ds, STATIC_BASE)) {
            /* placed_object_collision(obj, x, y, 1) 0x18 under the caller's
             * frame -- a frame of the port's when the caller names none */
            if (placed_object_collision(m, obj, (uint16_t)x, (uint16_t)y, 1,
                                        (uint16_t)((m->debris_bp ? m->debris_bp : (uint16_t)(FRAME_BP - 0x80)) - 0x18)) == 0)
                return 1;
        }
    }
    return 0;
}

/* apply_damage: resisted by compute_damage, then a creature's
 * hit points, or object_damage and, when the thing breaks,
 * object_damage_debris. */
int apply_damage(uw_motion *m, uint16_t obj, uint16_t attacker, int16_t x, int16_t y, uint8_t dmg, uint8_t type) {
    dmg = compute_damage(m, obj, dmg, type);
    if (((rw(m->lseg, obj) & 0x1c0) >> 6) == 1)
        return creature_take_damage(m, obj, dmg, attacker);
    if (!object_damage(m, obj, attacker, dmg, x, y))
        return 0;
    return object_damage_debris(m, obj, attacker, type, x, y);
}

/* player_start_status_effect(bit, ticks): bit 0x20's countdown
 * in status_timer_20, 0x40's in status_timer_40, and the bit into the
 * record's +0xb8; view_apply_impairment ages both and clears the bit. */
void player_start_status_effect(uw_motion *m, uint8_t bit, uint8_t ticks) {
    uint8_t *ds = m->ds;
    if (bit == 0x20) ds[STATUS_TIMER_20] = ticks;
    else if (bit == 0x40) ds[STATUS_TIMER_40] = ticks;
    else return;
    ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0xb8)] |= bit;
}
static void view_start_status(uw_motion *m, uint8_t bit, uint8_t ticks) { player_start_status_effect(m, bit, ticks); }

/* view_apply_impairment: the view's sway and jitter from the
 * player's status bits -- drunk or poisoned (0x11), lava's burn (2: the
 * movement mode lava gives, set_movement_state's state 4 -> mode 2, whose
 * flag byte is 2; a point of type 8 one call in five unless dragon skin
 * boots are worn), bit 3's slow sway, and the two timed shakes (0x40,
 * 0x20) counting down. */
void view_apply_impairment(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    int8_t a = 1, b = 1;
    ds[STEPPED] = 1;
    ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));
    ww(ds, JITTER_PITCH, 0);
    ww(ds, JITTER_YAW, 0);
    ww(ds, SWAY_HORIZONTAL, 0);
    if (ds[(uint16_t)(rec + 0xb8)] & 0x11) {
        ww(ds, SWAY_VERTICAL, (uint16_t)-(int16_t)ds[(uint16_t)(rec + 0xb9)]);
        if (ds[(uint16_t)(rec + 0xb9)] > 0x50) {
            a = (int8_t)(uint8_t)((int16_t)(uint16_t)(rw(ds, MOVEMENT_SPEED) << 2) / (rs(ds, SPEED_MAX_FORWARD) >> 1) - 3);
            if (a < 1) a = 1;
            b = (int8_t)(ds[SWAY_PHASE] >> 4);
            if (rw(ds, MOVEMENT_SPEED) == 0)
                ww(ds, JITTER_PITCH, (uint16_t)((rt_rand(m) & 0x1ff) - 0x100));
            else
                ww(ds, JITTER_PITCH, (uint16_t)((a * (int8_t)ds[(uint16_t)(0x74a + b)]) << 6));
            ww(ds, SWAY_VERTICAL, (uint16_t)(rw(ds, SWAY_VERTICAL)
                                  + (int16_t)(((int8_t)ds[(uint16_t)(0x74a + ((b + 2) & 0xf))] << 1) * a)));
            ww(ds, SWAY_HORIZONTAL, (uint16_t)(((rt_rand(m) & 0x7f) - 0x40) * a));
            ww(ds, JITTER_YAW, (uint16_t)(((rt_rand(m) & 0x7f) - 0x40) * a));
        }
    }
    if ((ds[(uint16_t)(rec + 0xb8)] & 2) && !ds[DRAGON_BOOTS_WORN] && rt_rand(m) % 5 == 0)
        apply_damage(m, rw(ds, TRACKED_OBJECT), 0, 0, 0, 1, 8);
    if (ds[(uint16_t)(rec + 0xb8)] & 8) {
        int16_t v = (int16_t)(0x10 - (ds[SWAY_PHASE] >> 3));
        ww(ds, SWAY_VERTICAL, (uint16_t)((v < 0 ? -v : v) * 3));
    }
    if (!(ds[(uint16_t)(rec + 0xb8)] & 0x60)) return;
    if (ds[(uint16_t)(rec + 0xb8)] & 0x40) {
        uint8_t t = ds[STATUS_TIMER_40];
        ds[STATUS_TIMER_40] = (uint8_t)(t - 1);
        if (t == 0) {
            ds[(uint16_t)(rec + 0xb8)] ^= 0x40;
            ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));
        }
        a = (int8_t)(ds[STATUS_TIMER_40] / 10);
        if (a > 8) a = 8;
    }
    if (ds[(uint16_t)(rec + 0xb8)] & 0x20) {
        uint8_t t = ds[STATUS_TIMER_20];
        ds[STATUS_TIMER_20] = (uint8_t)(t - 1);
        if (t == 0) {
            ds[(uint16_t)(rec + 0xb8)] ^= 0x20;
            ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));
        }
        b = (int8_t)(ds[STATUS_TIMER_20] / 8);
        if (b > 3) b = 3;
    }
    a = (int8_t)(a + b);
    ww(ds, SWAY_HORIZONTAL, (uint16_t)(rw(ds, SWAY_HORIZONTAL) + ((rt_rand(m) & 0xff) - 0x80) * a));
    ww(ds, JITTER_YAW, (uint16_t)(rw(ds, JITTER_YAW) + ((rt_rand(m) & 0x7f) - 0x40) * a));
    ww(ds, JITTER_PITCH, (uint16_t)(rw(ds, JITTER_PITCH) + ((rt_rand(m) & 0x1ff) - 0x100) * a));
}

/* angle_to_offset */
void angle_to_offset(uw_motion *m, uint16_t angle, int16_t scale, uint16_t at_x, uint16_t at_y) {
    int16_t s, c;
    uw_sincos((uint16_t)(((0x140 - angle) & 0xff) << 8), &s, &c);
    s = (int16_t)((int16_t)(uint16_t)((s / 0x80) * scale) / 0x100);
    c = (int16_t)((int16_t)(uint16_t)((c / 0x80) * scale) / 0x100);
    if (s > 0) s++; else if (s < 0) s--;
    if (c > 0) c++; else if (c < 0) c--;
    ww(m->ds, at_y, (uint16_t)(rw(m->ds, at_y) + s));
    ww(m->ds, at_x, (uint16_t)(rw(m->ds, at_x) + c));
}

/* combat_choose_hit_region: 0..3 from where the strike's height
 * band meets the target's. */
static int16_t choose_hit_region(uw_motion *m, int16_t lo, int16_t hi, int16_t z0, int16_t z1) {
    int16_t mid_t = (int16_t)((lo + hi) >> 1), mid_s = (int16_t)((z0 + z1) >> 1);
    if (lo + 1 > mid_s) return 2;
    if (hi - 1 < mid_s) return 3;
    if (mid_s < mid_t) {
        if (rt_rand(m) % 2) return 2;
    } else {
        if (rt_rand(m) % 3 == 0) return 3;
    }
    return (int16_t)(rt_rand(m) % 3 ? 0 : 1);
}

/* The target tile a result record names, relative to the query position. */
static void combat_target_tile(uw_motion *m, uint16_t q, int r) {
    uint8_t *ds = m->ds;
    uint16_t tw = rw(ds, (uint16_t)(RESULTS + r * 6 + 4));
    int16_t local = (int16_t)(tw & 0x3f);
    ww(ds, COMBAT_TARGET_TILE_X, (uint16_t)(((rs(ds, q) >> 3) + local) & 0x3f));
    local = (int16_t)(rw(ds, COMBAT_TARGET_TILE_X) - (rs(ds, q) >> 3));
    ww(ds, COMBAT_TARGET_TILE_Y, (uint16_t)(((rs(ds, (uint16_t)(q + 2)) >> 3)
                                            + (int16_t)(tw - local) / 0x40) & 0x3f));
}

/* combat_pick_target_from_query: the nearest struck object in the
 * query's band -- never a trigger or the attacker, and for the player an ally
 * only when nothing else is there. */
static int16_t pick_target_from_query(uw_motion *m, uint16_t q) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    int16_t di = (int8_t)ds[(uint16_t)(q + 0x16)], best = -1;
    int16_t end = (int16_t)(di + ds[(uint16_t)(q + 0x15)]);
    int32_t bestd = 100000;
    uint16_t att = (uint16_t)(rw(ds, MOBILE_BASE) + rw(ds, COMBAT_ATTACKER_OBJ) * 0x1b);
    uint16_t aw16 = rw(ls, (uint16_t)(att + 0x16)), aw2 = rw(ls, (uint16_t)(att + 2));
    int16_t afx = (int16_t)(((aw16 >> 10) << 3) + ((aw2 & 0xe000) >> 13));
    int16_t afy = (int16_t)((((aw16 & 0x3f0) >> 4) << 3) + ((aw2 & 0x1c00) >> 10));
    for (; di < end; di++) {
        uint16_t idx = (uint16_t)((rw(ds, (uint16_t)(RESULTS + di * 6 + 2)) >> 6) & 0x3ff);
        uint16_t o = obj_at(m, idx);
        int16_t dx, dy;
        int32_t d2;
        if (((rw(ls, o) & 0x1c0) >> 6) == 6) continue;
        if (idx == rw(ds, COMBAT_ATTACKER_OBJ)) continue;
        if (rw(ds, COMBAT_ATTACKER_OBJ) == 1 && o && o < rw(ds, STATIC_BASE) && (ls[(uint16_t)(o + 0x19)] & 0x40)
            && !(end - 1 == di && bestd == 100000))
            continue;
        combat_target_tile(m, q, di);
        dx = (int16_t)(afx - ((rw(ds, COMBAT_TARGET_TILE_X) << 3) + (rw(ls, (uint16_t)(o + 2)) >> 13)));
        dy = (int16_t)(afy - ((rw(ds, COMBAT_TARGET_TILE_Y) << 3) + ((rw(ls, (uint16_t)(o + 2)) & 0x1c00) >> 10)));
        d2 = (int16_t)(uint16_t)(dx * dx + dy * dy);
        if (d2 < bestd) { bestd = d2; best = di; }
    }
    if (best >= 0) combat_target_tile(m, q, best);
    return best;
}

/* combat_mark_wall(heading, reach, query), from the
 * instructions: a swing that met no object but a wall walks the query point
 * from the attacker along the heading in steps of 0x10, at most reach + 1
 * of them, until the terrain test blocks (corner flags 0x300); there a
 * static `some_damage` (item 0x1cb) goes at the point, eight above its z,
 * with a level effect of 2 -- freed when the effect list is full -- at the
 * end of its tile's chain. When the Avatar swung, sound effect 7 plays at
 * the point first. `bp` is its frame. */
static void combat_mark_wall(uw_motion *m, uint16_t heading, int16_t reach, uint16_t q, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    int16_t steps = (int16_t)(reach + 1), tx, ty;
    uint16_t o;
    uw_objpool pool;
    ww(ds, SQ_PTR, q);
    ds[(uint16_t)(q + 8)] = 1;
    ww(ds, (uint16_t)(q + 0xa), 0);
    ww(ds, (uint16_t)(bp - 2), (uint16_t)(rw(ds, q) << 4));
    ww(ds, (uint16_t)(bp - 4), (uint16_t)(rw(ds, (uint16_t)(q + 2)) << 4));
    for (;;) {
        sq_terrain(m, 0);
        if ((rw(ds, (uint16_t)(q + 0xc)) | rw(ds, (uint16_t)(q + 0xe))) & 0x300) break;
        angle_to_offset(m, heading, 0x10, (uint16_t)(bp - 2), (uint16_t)(bp - 4));
        ww(ds, q, (uint16_t)(rs(ds, (uint16_t)(bp - 2)) >> 4));
        ww(ds, (uint16_t)(q + 2), (uint16_t)(rs(ds, (uint16_t)(bp - 4)) >> 4));
        if (--steps <= 0) return;
    }
    o = create_object(m, 0x1cb, 0);
    if (!o) return;
    ww(ls, (uint16_t)(o + 2), (uint16_t)((rw(ls, (uint16_t)(o + 2)) & 0x1fff) | ((rw(ds, q) & 7) << 13)));
    ww(ls, (uint16_t)(o + 2), (uint16_t)((rw(ls, (uint16_t)(o + 2)) & 0xe3ff) | ((rw(ds, (uint16_t)(q + 2)) & 7) << 10)));
    tx = (int16_t)(rs(ds, q) >> 3);
    ty = (int16_t)(rs(ds, (uint16_t)(q + 2)) >> 3);
    ww(ls, (uint16_t)(o + 2), (uint16_t)((rw(ls, (uint16_t)(o + 2)) & 0xff80) | ((rw(ds, (uint16_t)(q + 4)) + 8) & 0x7f)));
    if (rw(ds, COMBAT_ATTACKER_OBJ) == 1) play_sound_effect_at_xy(m, 7, rs(ds, q), rs(ds, (uint16_t)(q + 2)), 0);
    if (level_effect_add(m, obj_index_of(m, o), 2, 0, (uint8_t)tx, (uint8_t)ty) == -1) {
        pool_from_ds(m, &pool);
        uw_obj_free(&pool, o);
        pool_to_ds(m, &pool);
        return;
    }
    pool_from_ds(m, &pool);
    uw_object_list_append(&pool, (uint16_t)(tile_ptr(m, (uint16_t)tx, (uint16_t)ty) + 2), o);
    pool_to_ds(m, &pool);
}

/* combat_find_target: a query struct on the stack at the
 * attacker's reach along its heading, gathered and sorted; the nearest in
 * band is the target and its hit region is chosen. `bp` is the function's
 * frame, which the struct sits under. */
static int find_target(uw_motion *m, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t q = (uint16_t)(bp - 0x1c);
    uint16_t att = (uint16_t)(rw(ds, MOBILE_BASE) + rw(ds, COMBAT_ATTACKER_OBJ) * 0x1b);
    uint8_t h = prop(m, obj_id(m, att), 0);
    uint16_t w16 = rw(ls, (uint16_t)(att + 0x16)), w2 = rw(ls, (uint16_t)(att + 2));
    uint16_t heading;
    int16_t idx;
    ww(ds, SQ_PTR, q);
    ww(ds, (uint16_t)(q + 0xa), rw(ds, COMBAT_ATTACKER_OBJ));
    ds[(uint16_t)(q + 8)] = (uint8_t)(ds[COMBAT_REACH] + 1);
    ds[(uint16_t)(q + 9)] = (uint8_t)(((ds[COMBAT_REACH] << 1) + 1) << 2);
    ww(ds, (uint16_t)(q + 4), (uint16_t)((w2 & 0x7f)
        + (uint16_t)((uint16_t)(h * (rs(ds, COMBAT_SWING_TYPE) / 3)) / 3u)));
    if (rw(ds, COMBAT_ATTACKER_OBJ) == 1)
        ww(ds, (uint16_t)(q + 4), (uint16_t)(rw(ds, (uint16_t)(q + 4)) + rs(ds, 0x3588) / 0x200));
    ds[COMBAT_STRIKE_Z] = (uint8_t)(ds[(uint16_t)(q + 4)] + h / 6);
    ww(ds, q, (uint16_t)(((w16 >> 10) << 3) + ((w2 & 0xe000) >> 13)));
    ww(ds, (uint16_t)(q + 2), (uint16_t)((((w16 & 0x3f0) >> 4) << 3) + ((w2 & 0x1c00) >> 10)));
    heading = (uint16_t)((((w2 & 0x380) >> 7) << 5) + (ls[(uint16_t)(att + 0x18)] & 0x1f));
    angle_to_offset(m, heading, (int16_t)(ds[COMBAT_REACH] + 3), q, (uint16_t)(q + 2));
    sq_gather(m, 0, 1);
    if (ds[(uint16_t)(q + 0x14)]) {
        sq_sort(m);
        if (!ds[(uint16_t)(q + 0x15)]) return 0;
        idx = pick_target_from_query(m, q);
        if (idx < 0) return 0;
        ww(ds, COMBAT_HIT_REGION, (uint16_t)choose_hit_region(m, ds[(uint16_t)(RESULTS + idx * 6 + 1)],
                                                             ds[(uint16_t)(RESULTS + idx * 6)],
                                                             rs(ds, (uint16_t)(q + 4)),
                                                             (int16_t)(rs(ds, (uint16_t)(q + 4)) + ds[(uint16_t)(q + 9)])));
        ww(ds, COMBAT_TARGET_OBJ, (uint16_t)((rw(ds, (uint16_t)(RESULTS + idx * 6 + 2)) >> 6) & 0x3ff));
        return 1;
    }
    sq_terrain(m, 0);
    if ((rw(ds, (uint16_t)(q + 0xc)) | rw(ds, (uint16_t)(q + 0xe))) & 0x300) {
        ww(ds, q, (uint16_t)(((w16 >> 10) << 3) + ((w2 & 0xe000) >> 13)));
        ww(ds, (uint16_t)(q + 2), (uint16_t)((((w16 & 0x3f0) >> 4) << 3) + ((w2 & 0x1c00) >> 10)));
        combat_mark_wall(m, heading, (int16_t)(rw(ds, COMBAT_REACH) + 3), q, (uint16_t)(bp - 0x1c - 4 - 6 - 4 - 2));
    }
    return 0;
}

/* combat_relative_facing: 0..4, how far round the target faces
 * from the attacker. */
static void relative_facing(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t t = obj_at(m, rw(ds, COMBAT_TARGET_OBJ)), a = obj_at(m, rw(ds, COMBAT_ATTACKER_OBJ));
    uint8_t v = (uint8_t)(((((rw(m->lseg, (uint16_t)(t + 2)) & 0x380) >> 7) + 0xc)
                           - ((rw(m->lseg, (uint16_t)(a + 2)) & 0x380) >> 7)) & 7);
    ds[COMBAT_FACING] = (uint8_t)(v > 4 ? 8 - v : v);
}

/* combat_attack_roll: 0 a hit, 1 a miss, 2 a fumble. Not at a
 * creature, a hit -- and the Avatar striking a door (0x140..0x14f) notches
 * the weapon hand's slot (8 less the handedness bit, record +0x64) by 2d4
 * when rand() * 12 / 0x8000 falls under twice the door's type (id & 7). A
 * critical on the Avatar damages the slot its hit region names -- 0 the
 * head's, 1 and 2 the weapon hand's (7 or 8 by handedness), 3 the body's or
 * one time in five the legs' -- by 2d4; the Avatar's own fumble against a
 * creature whose move class has bit 0 clear notches the weapon by 2d3. */
static int16_t attack_roll(uw_motion *m, uint16_t att_idx, uint16_t tgt_idx) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t t = obj_at(m, tgt_idx);
    uint8_t hand = (uint8_t)(8 - (ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x64)] & 1));
    int16_t r;
    if (((rw(ls, t) & 0x1c0) >> 6) != 1) {
        if (att_idx == 1 && ((rw(ls, t) & 0x1f0) >> 4) == 0x14
            && (uint16_t)(((int32_t)rt_rand(m) * 12) / 0x8000) < (uint16_t)((rw(ls, t) & 7) << 1))
            inventory_damage_slot(m, hand, (uint8_t)roll_dice(m, 2, 4), 4, 0, 1);
        return 0;
    }
    if (rw(ds, COMBAT_TARGET_OBJ) == 1)
        ww(ds, COMBAT_ATTACK_RATING, (uint16_t)(rs(ds, COMBAT_ATTACK_RATING)
                                                - (int8_t)ds[(uint16_t)(REGION_ARMOUR_BONUS + rw(ds, COMBAT_HIT_REGION))]));
    r = (int16_t)check_skill_roll(m, (int16_t)(rs(ds, COMBAT_ATTACK_RATING) + ds[COMBAT_FACING]),
                                  (int8_t)ds[(uint16_t)(CRITTER_BASE + (rw(ls, t) & 0x3f) * 0x30 + 0x12)]);
    ds[COMBAT_CRITICAL] = 0;
    if (r == 2) {
        ds[COMBAT_CRITICAL] = 1;
        ww(ds, COMBAT_DAMAGE, (uint16_t)(rs(ds, COMBAT_DAMAGE) * (int16_t)(((rt_rand(m) & 0x1f) + 0x30) >> 5)));
        if (tgt_idx == 1) {
            int16_t slot = (int16_t)((rw(ds, COMBAT_HIT_REGION) + 1) & 3);
            m->screen_flash = 1;            /* screen_show_frame(0xb8) */
            m->screen_flash_colour = 0xb8;
            if (slot == 3) slot = (int16_t)(slot + (rt_rand(m) % 5 == 0 ? 1 : 0));
            else if (slot == 1 || slot == 2) slot = (int16_t)((ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x64)] & 1) + 7);
            inventory_damage_slot(m, slot, (uint8_t)roll_dice(m, 2, 4), 4, 1, 1);
        }
        return 0;
    }
    if (r == -1 && att_idx == 1
        && !(ds[(uint16_t)(CRITTER_BASE + (rw(ls, obj_at(m, rw(ds, COMBAT_TARGET_OBJ))) & 0x3f) * 0x30 + 0xa)] & 1)) {
        inventory_damage_slot(m, hand, (uint8_t)roll_dice(m, 2, 3), 4, 0, 1);
    }
    return (int16_t)(1 - r);
}

/* combat_apply_damage: dice for the damage, the sound, armour
 * by region, the view shake, blood. */
/* spawn_class7_object through src/uw_effects.c, over the effect
 * list and the pools the data segment holds, written back. */
void spawn_class7(uw_motion *m, uint16_t src, int kind, int16_t timer, uint8_t seed,
                         int16_t height, uint8_t x, uint8_t y) {
    uint8_t *ds = m->ds;
    static uw_effects e;
    uw_objpool pool;
    int i;
    uw_effects_from_ds(&e, ds);
    e.drawn_flag = ds[0x2e1c];
    pool_from_ds(m, &pool);
    e.pool = &pool;
    uw_spawn_class7_object(&e, ds + OBJ_PROPERTIES, src, kind, timer, seed, height, x, y);
    pool_to_ds(m, &pool);
    ds[0x3656] = (uint8_t)e.count;
    ds[0x2e1c] = (uint8_t)e.drawn_flag;
    for (i = 0; i < UW_EFFECTS_MAX; i++) {
        uint16_t at = (uint16_t)(0x369c + i * 6);
        ww(ds, at, e.rec[i].word0);
        ww(ds, (uint16_t)(at + 2), (uint16_t)e.rec[i].timer);
        ds[(uint16_t)(at + 4)] = e.rec[i].tile_x;
        ds[(uint16_t)(at + 5)] = e.rec[i].tile_y;
    }
    m->not_carried += e.unsupported;
}

static void combat_apply_damage(uw_motion *m, uint8_t type) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t t = obj_at(m, rw(ds, COMBAT_TARGET_OBJ)), id = obj_id(m, t);
    int16_t sixes, rest, dmg, di;
    if (rs(ds, COMBAT_DAMAGE) < 2) ww(ds, COMBAT_DAMAGE, 2);
    sixes = (int16_t)(rs(ds, COMBAT_DAMAGE) / 6);
    rest = (int16_t)(rs(ds, COMBAT_DAMAGE) % 6);
    ww(ds, COMBAT_DAMAGE, 0);
    if (sixes) ww(ds, COMBAT_DAMAGE, (uint16_t)roll_dice(m, sixes, 6));
    if (rest) ww(ds, COMBAT_DAMAGE, (uint16_t)(rs(ds, COMBAT_DAMAGE) + roll_dice(m, 1, rest)));
    dmg = (int16_t)((int16_t)(uint16_t)(ds[COMBAT_DAMAGE_SCALE] * rs(ds, COMBAT_DAMAGE)) >> 7);
    dmg = (int16_t)(dmg + ds[COMBAT_FACING]);
    /* the blow's sound, louder by the damage (its byte, four times): the
     * Avatar's own, or effect 4 at the target's place in eighths */
    if (rw(ds, COMBAT_TARGET_OBJ) == 1)
        play_sound_effect(m, 3, 0x40, (int8_t)(uint8_t)(dmg * 4));
    else
        play_sound_effect_at_xy(m, 4, (int16_t)(ds[COMBAT_TARGET_TILE_X] * 8 + (rw(m->lseg, (uint16_t)(t + 2)) >> 13)),
                                (int16_t)(ds[COMBAT_TARGET_TILE_Y] * 8 + ((rw(m->lseg, (uint16_t)(t + 2)) & 0x1c00) >> 10)),
                                (int8_t)(uint8_t)(dmg * 4));
    if ((id >> 6) == 1) {
        uint16_t row = (uint16_t)((id & 0x3f) * 0x30);
        int16_t armour = ds[(uint16_t)(CRITTER_BASE + row + rs(ds, COMBAT_HIT_REGION) % 4)];
        if (armour == 0xff) {
            ww(ds, COMBAT_HIT_REGION, (uint16_t)(rw(ds, COMBAT_HIT_REGION) & 4));
            armour = ds[(uint16_t)(CRITTER_BASE + row)];
        }
        if (rw(ds, COMBAT_TARGET_OBJ) != 1 && (rw(ls, (uint16_t)(t + 0xd)) & 0x400))
            armour = (int16_t)(armour * 5 / 3);
        if (armour > dmg) dmg = 0;
        else dmg = (int16_t)(dmg - armour);
    }
    di = (int16_t)(dmg / 4);
    if (di >= 4) di = 3;
    if (rw(ds, COMBAT_TARGET_OBJ) == 1 && ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0xb4)])
        dmg = (int16_t)(dmg >> 1);
    {
        int died = apply_damage(m, t, obj_at(m, rw(ds, COMBAT_ATTACKER_OBJ)), rs(ds, COMBAT_TARGET_TILE_X),
                                rs(ds, COMBAT_TARGET_TILE_Y), (uint8_t)dmg, type);
        if (dmg == 0 || rw(ds, COMBAT_ATTACKER_OBJ) == 0xffff) return;
        if (rs(ds, COMBAT_HIT_REGION) >= 4) {
            ds[0x024a] = (uint8_t)-(int8_t)ds[COMBAT_STRIKE_Z];
            ww(ds, COMBAT_HIT_REGION, 4);
        }
        if (rw(ds, VIEW_CAMERA_OBJECT) >= rw(ds, MOBILE_BASE)
            && (uint16_t)((rw(ds, VIEW_CAMERA_OBJECT) - rw(ds, MOBILE_BASE)) / 0x1b) == rw(ds, COMBAT_TARGET_OBJ)) {
            view_start_status(m, 0x20, (uint8_t)(di * 5));
            return;
        }
        if ((id >> 6) == 1) {
            if (rw(ds, COMBAT_ATTACKER_OBJ) == 1) {
                /* The Avatar's target's health in thirds for the panel:
                 * 3 - min(hp * 3 / max, 2). */
                uint8_t max = ds[(uint16_t)(CRITTER_BASE + 4 + (id & 0x3f) * 0x30)];
                int16_t third = max ? (int16_t)((int16_t)(ls[(uint16_t)(t + 8)] * 3) / max) : 0;
                if (third >= 3) third = 2;
                panel_set_value(m, 7, (uint16_t)(3 - third));
            }
            if ((ds[(uint16_t)(CRITTER_BASE + (id & 0x3f) * 0x30 + 8)] >> 3) & 3) {
                /* The blood at the struck region's height (a byte table by
                 * region), and for the Avatar's critical a second splash a
                 * little above or below it. */
                int16_t h = (int8_t)ds[(uint16_t)(0x0246 + rw(ds, COMBAT_HIT_REGION))];
                spawn_class7(m, t, 0, 1, (uint8_t)di, h, ds[COMBAT_TARGET_TILE_X], ds[COMBAT_TARGET_TILE_Y]);
                if (ds[COMBAT_CRITICAL] && rw(ds, COMBAT_ATTACKER_OBJ) == 1) {
                    h = (int16_t)((int8_t)ds[(uint16_t)(0x0246 + rw(ds, COMBAT_HIT_REGION))] + (rt_rand(m) & 1) * 5 - 2);
                    spawn_class7(m, t, 0, 1, (uint8_t)di, h, ds[COMBAT_TARGET_TILE_X], ds[COMBAT_TARGET_TILE_Y]);
                }
                return;
            }
            died = 0;
        }
        if (died) t = 0;
        if (((id & 0x1f0) >> 4) == 0x14 || id == 0x1cf) {
            if (!t) {
                UW_NOT_CARRIED(m->not_carried);
            } else if ((uint16_t)(rw(ls, (uint16_t)(t + 2)) & 0x7f) > (uint16_t)(int16_t)(int8_t)ds[COMBAT_STRIKE_Z]) {
                ds[COMBAT_STRIKE_Z] = (uint8_t)((rw(ls, (uint16_t)(t + 2)) & 0x7f) + 2);
            }
        }
        /* The spark off a thing that does not bleed. */
        spawn_class7(m, t, 0xb, 1, (uint8_t)di, (int16_t)-(int8_t)ds[COMBAT_STRIKE_Z],
                     ds[COMBAT_TARGET_TILE_X], ds[COMBAT_TARGET_TILE_Y]);
    }
}

/* combat_hit_feedback, from the instructions: the sound of a
 * miss (effect 10 at the attacker), or of the blow (at the target): 7 when
 * the attacker's weapon is heavy -- the attacker's item id 1 or past 0xff,
 * else its creature row's +0x10 bits 6..7 -- and what it strikes gives: the
 * Avatar's armour in slot (region + 1) & 3 unless it is none or one of the
 * five leathers (0x20, 0x23, 0x26, 0x29, 0x2c), a static nothing, a
 * creature bits 4..5 of +0x10 of the ATTACKER's row, as the code reads it;
 * else 8. It overwrites combat_attacker_obj with the attacker's item id on
 * the way, and the tests read that. */
static void hit_feedback(uw_motion *m, int16_t result) {
    uint8_t *ds = m->ds;
    uint8_t heavy, hard;
    uint16_t id;
    if (result == 0) {
        play_sound_effect_at_object(m, 10, obj_at(m, rw(ds, COMBAT_ATTACKER_OBJ)), 0);
        return;
    }
    ww(ds, COMBAT_ATTACKER_OBJ, obj_id(m, obj_at(m, rw(ds, COMBAT_ATTACKER_OBJ))));
    id = rw(ds, COMBAT_ATTACKER_OBJ);
    if (id == 1 || (int16_t)id >= 0x100) heavy = 1;
    else heavy = (uint8_t)(ds[(uint16_t)(CRITTER_BASE + (id & 0x3f) * 0x30 + 0x10)] >> 6);
    if (rw(ds, COMBAT_TARGET_OBJ) == 1) {
        uint16_t o = inventory_slot_object(m, (int16_t)((ds[COMBAT_HIT_REGION] + 1) & 3));
        uint16_t a = o ? obj_id(m, o) : 0;
        hard = !(!o || a == 0x20 || a == 0x23 || a == 0x26 || a == 0x29 || a == 0x2c);
    } else if ((int16_t)rw(ds, COMBAT_TARGET_OBJ) >= 0x100) {
        hard = 0;
    } else {
        hard = (uint8_t)((ds[(uint16_t)(CRITTER_BASE + (id & 0x3f) * 0x30 + 0x10)] & 0x30) >> 4);
    }
    play_sound_effect_at_object(m, heavy == 1 && hard == 1 ? 7 : 8, obj_at(m, rw(ds, COMBAT_TARGET_OBJ)), 0);
}

/* combat_resolve_attack */
static int resolve_attack(uw_motion *m, uint16_t bp) {
    uint8_t *ds = m->ds;
    int16_t r;
    if (!find_target(m, (uint16_t)(bp - 8))) {
        hit_feedback(m, 0);
        return 0;
    }
    if (rw(ds, COMBAT_ATTACKER_OBJ) != 1 && rw(ds, COMBAT_TARGET_OBJ) != 1) {
        uint16_t t = obj_at(m, rw(ds, COMBAT_TARGET_OBJ)), a = obj_at(m, rw(ds, COMBAT_ATTACKER_OBJ));
        if (t && t < rw(ds, STATIC_BASE)
            && !((m->lseg[(uint16_t)(t + 0x19)] ^ m->lseg[(uint16_t)(a + 0x19)]) & 0x40))
            return 0;
    }
    relative_facing(m);
    r = attack_roll(m, rw(ds, COMBAT_ATTACKER_OBJ), rw(ds, COMBAT_TARGET_OBJ));
    if (r) {
        apply_damage(m, obj_at(m, rw(ds, COMBAT_TARGET_OBJ)), obj_at(m, rw(ds, COMBAT_ATTACKER_OBJ)),
                     rs(ds, COMBAT_TARGET_TILE_X), rs(ds, COMBAT_TARGET_TILE_Y), 0, 4);
        hit_feedback(m, r);
        return 0;
    }
    combat_apply_damage(m, 4);
    return 1;
}

/* creature_execute_attack: the attack context from the critter's
 * chosen attack, then the shared resolution, and a poisoning bite. `bp` is
 * creature_ai_update's frame. */
int execute_attack(uw_motion *m, uint16_t obj, int16_t swing, uint8_t scale, int16_t attack,
                          uint16_t poison, uint16_t bp_cai) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t row = (uint16_t)(CRITTER_BASE + (rw(ls, obj) & 0x3f) * 0x30);
    uint16_t bp = (uint16_t)(bp_cai - 0x20);
    int hit;
    ww(ds, COMBAT_REACH, 2);
    ww(ds, COMBAT_ATTACKER_OBJ, obj_index_of(m, obj));
    ww(ds, COMBAT_SWING_TYPE, (uint16_t)swing);
    ds[COMBAT_DAMAGE_SCALE] = scale;
    ww(ds, COMBAT_DAMAGE, (uint16_t)(ds[(uint16_t)(row + 0x14 + attack * 3)] + ds[(uint16_t)(row + 5)] / 5));
    ww(ds, COMBAT_ATTACK_RATING, (uint16_t)((int8_t)ds[(uint16_t)(row + 0x13 + attack * 3)]
                                            + ((int8_t)ds[(uint16_t)(row + 0x11)] >> 1)));
    if (rw(ls, (uint16_t)(obj + 0xd)) & 0x400) {
        ww(ds, COMBAT_ATTACK_RATING, (uint16_t)(rs(ds, COMBAT_ATTACK_RATING) + rt_rand(m) % 6 + 7));
        ww(ds, COMBAT_DAMAGE, (uint16_t)(rs(ds, COMBAT_DAMAGE) + rt_rand(m) % 0xc + 4));
    }
    hit = resolve_attack(m, (uint16_t)(bp - 4 - 6));
    if (hit && rw(ds, COMBAT_TARGET_OBJ) == 1
        && ((ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x5f)] >> 2) & 0xf) < poison) {
        if (compute_damage(m, rw(ds, TRACKED_OBJECT), 1, 0x10)) {
            uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
            ds[(uint16_t)(rec + 0x5f)] = (uint8_t)((ds[(uint16_t)(rec + 0x5f)] & 0xc3) | ((poison & 0xf) << 2));
        }
    }
    return hit;
}

/* ---- a missile's hit ------ */

/* combat_apply_hit: a hit whose damage was decided elsewhere --
 * the impact tile, the strike height at the missile's middle, full scale, the
 * region from how the two bodies overlap, the sound by who was struck, and
 * combat_apply_damage. */
static void combat_apply_hit(uw_motion *m, uint8_t owner, uint16_t missile, uint16_t target,
                             uint16_t x, uint16_t y, uint16_t dmg, uint8_t type) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t mz = (uint16_t)(rw(ls, (uint16_t)(missile + 2)) & 0x7f), tz = (uint16_t)(rw(ls, (uint16_t)(target + 2)) & 0x7f);
    ww(ds, COMBAT_TARGET_TILE_X, x);
    ww(ds, COMBAT_TARGET_TILE_Y, y);
    ds[COMBAT_CRITICAL] = 0;
    ds[COMBAT_FACING] = 0;
    ds[COMBAT_STRIKE_Z] = (uint8_t)(mz + (prop(m, obj_id(m, missile), 0) >> 1));
    ds[COMBAT_DAMAGE_SCALE] = 0x80;
    ww(ds, COMBAT_ATTACKER_OBJ, owner);
    ww(ds, COMBAT_TARGET_OBJ, obj_index_of(m, target));
    ww(ds, COMBAT_HIT_REGION, (uint16_t)(choose_hit_region(m, (int16_t)tz, (int16_t)(tz + prop(m, obj_id(m, target), 0)),
                                                           (int16_t)mz, (int16_t)(mz + prop(m, obj_id(m, missile), 0))) + 4));
    ww(ds, COMBAT_DAMAGE, dmg);
    if (is_tracked(m, target)) play_sound_effect(m, 3, 0, 0);
    else if (target < rw(ds, STATIC_BASE)) play_sound_effect_at_object(m, 4, target, 0);   /* obj_is_mobile */
    combat_apply_damage(m, type);
}

/* projectile_impact_damage: the missile's listed damage (the
 * byte before its speed in the missile table), scaled by the Missile skill
 * roll when the Avatar threw it, dealt at the collision's struck end. */
static void projectile_impact_damage(uw_motion *m, uint16_t missile, uint16_t target) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t mp = (uint16_t)(MISSILE_PROPS - 1 + (rw(ls, missile) & 0xf) * 3);
    uint16_t amount = ds[mp];
    uint16_t x, y;
    if (ls[(uint16_t)(missile + 0x12)] == 1 && ds[(uint16_t)(mp + 2)] == 0xc0) {
        uint8_t skill = ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x27)];
        uint16_t base = (uint16_t)(skill * 8 + 0xc0);
        int r = check_skill_roll(m, skill, 10);
        if (r == -1) base = (uint16_t)(base - 0x80);
        else if (r == 2) base = (uint16_t)(base + 0xc0);
        amount = (uint16_t)((uint16_t)(amount * base) >> 8);
    }
    if (ds[0x2760]) { x = ds[0x2761]; y = ds[0x2762]; }
    else { x = ds[0x2763]; y = ds[0x2764]; }
    combat_apply_hit(m, ls[(uint16_t)(missile + 0x12)], missile, target, x, y, amount,
                     (uint8_t)-(int8_t)ds[(uint16_t)(mp + 2)]);
}

/* object_find_matching */

/* cast_spell_from_object(x, y, caster, obj, flag), from the
 * instructions: nothing unless item_enchantment finds a spell of the wide
 * kind (`special`). With the flag set the CASTER fires it, at most once a
 * cooldown -- enchantment_cooldown (32 bits) against the record's
 * clock at +0xce: early, sound 0x15 and 0; else the cooldown moved to the
 * clock + 0x2fd. With it clear the OBJECT fires it -- except the player's
 * own wands (0x98..0x9b), which cast only the first way. Firing is
 * trap_fire(x, y, the firer, caster, effect, magnitude) and
 * magic_charge_update(obj); 1. `bp` is this routine's frame. */
int cast_spell_from_object(uw_motion *m, uint8_t x, uint8_t y, uint16_t caster, uint16_t obj, int flag, uint16_t bp) {
    uint8_t *ds = m->ds;
    int16_t effect, magnitude;
    int special = 0;
    uint16_t firer;
    if (!obj || !item_enchantment(m, obj, &effect, &magnitude, &special) || !special) return 0;
    if (flag) {
        uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
        uint32_t now = (uint32_t)rw(ds, (uint16_t)(rec + 0xce)) | (uint32_t)rw(ds, (uint16_t)(rec + 0xd0)) << 16;
        uint32_t due = (uint32_t)rw(ds, 0x09f4) | (uint32_t)rw(ds, 0x09f6) << 16;
        if (now < due) {
            play_sound_effect(m, 0x15, 0x40, 0);
            return 0;
        }
        now += 0x2fd;
        ww(ds, 0x09f4, (uint16_t)now);
        ww(ds, 0x09f6, (uint16_t)(now >> 16));
        firer = caster;
    } else {
        uint16_t id = (uint16_t)(rw(m->lseg, obj) & 0x1ff);
        if (caster == rw(ds, TRACKED_OBJECT) && id >= 0x98 && id <= 0x9b) return 0;
        firer = obj;
    }
    trap_fire(m, x, y, firer, caster, effect, (uint8_t)magnitude, (uint16_t)(bp - 0x10 - 4 - 2));
    magic_charge_update(m, obj);
    return 1;
}

/* magic_charge_update, from the instructions: a non-quantity
 * with contents and a spell (class 4 subclass 2 type 0) of the wide kind
 * (word 0 bit 11) in its chain spends a charge -- the spell's quality less
 * one -- and with none left the spell goes (off the chain and freed) four
 * times in ten, rand() * 10 / 0x8000 under 4. */
void magic_charge_update(uw_motion *m, uint16_t obj) {
    uint8_t *ls = m->lseg;
    uint16_t link = (uint16_t)(obj + 6), spell;
    if ((rw(ls, obj) & 0x8000) || !((rw(ls, link) >> 6) & 0x3ff)) return;
    spell = object_find_matching(m, &link, 0, 4, 2, 0);
    if (!spell || !(rw(ls, spell) & 0x800)) return;
    if (ls[(uint16_t)(spell + 4)] & 0x3f) {
        ls[(uint16_t)(spell + 4)] = (uint8_t)((ls[(uint16_t)(spell + 4)] & 0xc0) | (((ls[(uint16_t)(spell + 4)] & 0x3f) - 1) & 0x3f));
    } else if ((int16_t)(((int32_t)rt_rand(m) * 10) / 0x8000) < 4) {
        uw_objpool pool;
        pool_from_ds(m, &pool);
        uw_object_list_remove(&pool, link, spell);
        uw_obj_free(&pool, spell);
        pool_to_ds(m, &pool);
    }
}

/* object_use_dispatch as a collision reaches it: `b` used on `a`.
 * During a type-4 event only a class-2 subclass-0 thing is used. A missile
 * (class 0, subclass 1) deals projectile_impact_damage to what it struck,
 * class 2 subclass 3 is use_object's, with no tail, subclass 0 open_container's,
 * subclass 1 below nibble 8 use_light_source's; class 4 subclass 0
 * use_key_or_lockpick's, subclass 1 use_misc_dispatch's, subclass 2
 * use_special_item's, subclass 3
 * use_readable's, with no tail; class 3 subclasses 0 and 1
 * use_object_dispatch_misc's, subclass 2's item 0xe7 a prompt; class 5 goes to
 * use_door_furniture_or_switch; class 7 by the id's low nibble --
 * 9 hands the object its +4 links to, when that is 0x12e, to
 * use_special_item; 10, the silver tree, is used up (object_clear, forced)
 * and, gone, says message 9, puts a new silver seed in the hand
 * (spawn_object_in_hand, word 0 bit 13 set), clears the record's +0x5e high
 * nibble -- death's level -- and returns with no tail; 15 by +6's nibble
 * door_close under 8, else door_open. Then trigger_object_link and
 * cast_spell_from_object. */
/* use_wand(obj, flag): used (flag) and
 * not a spent one (nibble 0xc..0xf), trigger_object_link(player, obj, 4, the
 * target tile) and cast_spell_from_object -- the dispatch's tail, run here
 * -- and then, unless the wand is a quantity, no spell (class 4 subclass 2
 * type 0) left in its chain steps the nibble by 4 (its spent form), prints
 * 0x7d and redraws its slot (inventory_click_slot_index of
 * inventory_find_object, a negated slot counted as elsewhere). */
static void use_wand(uw_motion *m, uint16_t obj, int flag) {
    uint8_t *ls = m->lseg;
    uint16_t w0 = rw(ls, obj);
    if (!flag || ((w0 & 0xf) >= 0xc && (w0 & 0xf) <= 0xf)) return;
    /* trigger_object_link(player, wand, 4, the target tile) and
     * cast_spell_from_object(the target tile, player, wand, flag): with the
     * flag set the player casts it; the frames are the port's */
    trigger_object_link_at(m, rw(m->ds, TRACKED_OBJECT), obj, 4, m->ds[ACTION_TARGET_TILE_X], m->ds[ACTION_TARGET_TILE_Y],
                           (uint16_t)(FRAME_BP - 0x80));
    cast_spell_from_object(m, m->ds[ACTION_TARGET_TILE_X], m->ds[ACTION_TARGET_TILE_Y], rw(m->ds, TRACKED_OBJECT), obj, flag,
                           (uint16_t)(FRAME_BP - 0x80));
    w0 = rw(ls, obj);
    if (!(w0 & 0x8000)) {
        uint16_t link = (uint16_t)(obj + 6);
        if (!object_find_matching(m, &link, 0, 4, 2, 0)) {
            int16_t slot;
            ww(ls, obj, (uint16_t)((w0 & 0xfff0) | (((w0 & 0xf) + 4) & 0xf)));
            print_message(m, 0x7d);
            slot = inventory_find_object(m, obj);
            if (slot < 0) UW_NOT_CARRIED(m->not_carried);
            else inventory_click_slot_index(m, slot);
        }
    }
}

void object_use_dispatch(uw_motion *m, uint16_t a, uint16_t b, int flag) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w0 = rw(ls, b), cls = (uint16_t)((w0 & 0x1c0) >> 6), si = (uint16_t)((w0 & 0x30) >> 4);
    if (rw(ds, (uint16_t)(rw(ds, 0x00e2) + 8)) == 4 && (cls != 2 || si != 0)) return;
    switch (cls) {
    case 0:
        if (si == 1 && !flag && a) projectile_impact_damage(m, b, a);
        break;
    case 1: case 6:
        break;
    case 2:
        if (si == 3) {
            use_object(m, a, b, flag);
            return;
        }
        if (si == 0) {
            open_container(m, a, b, flag);
            break;
        }
        if (si == 1 && (w0 & 0xf) < 8) {
            use_light_source(m, b, flag);
            break;
        }
        if (si != 2) {
            use_wand(m, b, flag);           /* no tail */
            return;
        }
        break;
    case 4:
        if (si == 3) {
            use_readable(m, b, flag);
            return;
        }
        if (si == 2) {
            use_special_item(m, a, b, flag);
            break;
        }
        if (si == 1) {
            use_misc_dispatch(m, a, b, flag);
            break;
        }
        use_key_or_lockpick(m, b, flag);
        break;
    case 3:
        if (si == 0 || si == 1) use_object_dispatch_misc(m, b, flag);
        else if (si == 2 && (w0 & 0x1ff) == 0xe7 && flag) use_object_with_prompt(m, b, 0x0aff);
        break;
    case 5:
        use_door_furniture_or_switch(m, a, b);
        break;
    case 7:
        if ((w0 & 0xf) == 9) {
            uint16_t link = (uint16_t)((rw(ls, (uint16_t)(b + 4)) >> 6) & 0x3ff), o;
            if (!link) break;
            o = obj_at(m, link);
            if ((rw(ls, o) & 0x1ff) == 0x12e) use_special_item(m, a, o, flag);
        } else if ((w0 & 0xf) == 0xa) {
            uint16_t seed;
            if (!object_clear(m, b, flag, 1)) break;
            print_message(m, 9);
            seed = spawn_object_in_hand(m, 0, 0x122);
            ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x5e)] &= 0xf;
            if (!seed) {
                UW_NOT_CARRIED(m->not_carried);
                return;
            }
            ww(ls, seed, (uint16_t)((rw(ls, seed) & 0xdfff) | 0x2000));
            return;
        } else if ((w0 & 0xf) == 0xf) {
            if ((ls[(uint16_t)(b + 6)] & 0xf) < 8) door_close(m, b);
            else door_open(m, a, b);
        }
        break;
    }
    /* trigger_object_link(a, b, 4, action_target_tile_x, _y) on its frame,
     * 0x1c under this routine's (six bytes of locals and SI, seven words
     * pushed) -- known from action_use (chain1: the pull chain's use
     * trigger and its door trap) */
    trigger_object_link_at(m, a, b, 4, ds[ACTION_TARGET_TILE_X], ds[ACTION_TARGET_TILE_Y],
                           m->use_bp ? (uint16_t)(m->use_bp - 6 - 2 - 14 - 4 - 2) : (uint16_t)(FRAME_BP - 0x80));
    cast_spell_from_object(m, ds[ACTION_TARGET_TILE_X], ds[ACTION_TARGET_TILE_Y], a, b, flag,
                           m->use_bp ? (uint16_t)(m->use_bp - 6 - 2 - 10 - 4 - 2) : (uint16_t)(FRAME_BP - 0x80));
}

/* motion_knockback: the struck thing pushed along the mover's
 * heading -- its own motion block (a local of 0x28 bytes, here a stack
 * address below every frame the port models; no global keeps it) with the
 * mover's heading, speed 0xeb and the mover's vertical velocity scaled by
 * the ratio of their masses (at most 2), written back by
 * projectile_motion_apply. A massless one only loses its speed. */
void motion_knockback(uw_motion *m, uint16_t hit) {
    uint8_t *ds = m->ds;
    uint16_t si = (uint16_t)(FRAME_BP - 0x180);
    int16_t di;
    if (!hit) return;
    ww(ds, MOTION_TILE_X, ds[0x2761]);
    ww(ds, MOTION_TILE_Y, ds[0x2762]);
    motion_params_init(m, hit, si);
    if (rw(ds, (uint16_t)(si + 0x18)) == 0) {
        if (rw(ds, (uint16_t)(si + 0x14))) ww(ds, (uint16_t)(si + 0x14), 0);
        return;
    }
    di = (int16_t)((int16_t)(uint16_t)(rw(ds, (uint16_t)(rw(ds, MR_CTX) + 0x18)) << 6) / rs(ds, (uint16_t)(si + 0x18)));
    if (di > 0x80) di = 0x80;
    ww(ds, (uint16_t)(si + 0x1e), rw(ds, (uint16_t)(rw(ds, MR_CTX) + 0x1e)));
    ww(ds, (uint16_t)(si + 0x14), 0xeb);
    ww(ds, (uint16_t)(si + 0xa), (uint16_t)((int16_t)(uint16_t)(rs(ds, (uint16_t)(rw(ds, MR_CTX) + 0xa)) * di) / 0x40));
    ww(ds, MOTION_TILE_X, ds[0x2761]);
    ww(ds, MOTION_TILE_Y, ds[0x2762]);
    projectile_motion_apply(m, hit, si, (uint16_t)(si + 0x28 - 4 - 0x28 - 6 - 4 - 2));
}

/* ==== the player's swing: combat_swing and its helpers ==== */

/* mouse_sample_buttons: INT 33h's button mask, handed back and
 * left in mouse_button_held; mouse_pending_event cleared to -1 when none is
 * down. `m->buttons` stands for the driver's answer. */
uint16_t mouse_sample_buttons(uw_motion *m) {
    uint8_t *ds = m->ds;
    if (!m->buttons) ww(ds, MOUSE_PENDING, 0xffff);
    ww(ds, MOUSE_HELD, m->buttons);
    return m->buttons;
}

/* combat_show_ready_weapon: the weapon panel back to its ready
 * picture (4 in fight mode, 6 not) and the charge gem emptied. */
void combat_show_ready_weapon(uw_motion *m) {
    panel_set_value(m, 8, (m->ds[(uint16_t)(rw(m->ds, PLAYER_RECORD_PTR) + 0x5f)] >> 1) & 1 ? 4 : 6);
    panel_set_value(m, 3, 0);
}

/* combat_swing_abort: a missile swing put away -- the state to
 * -10, the aiming cursor popped, the weapon panel ready. */
static void combat_swing_abort(uw_motion *m) {
    uint8_t *ds = m->ds;
    ww(ds, SWING_STATE, 0xfff6);
    ww(ds, SWING_BUTTON, 0xffff);
    panel_set_value(m, 3, 0);
    ww(ds, ACTION_STATE_WORD, (uint16_t)(rw(ds, ACTION_STATE_WORD) - 4));
    cursor_shape_pop(m, 3);
    panel_set_value(m, 8, 4);
    ds[COMBAT_SWING_RANGED] = 0;
}

/* input_wait_button_release(keep_running): mouse_button_released
 * -1, then a loop polling the input until the held button comes
 * up -- or another button or a key breaks it, when the buttons still down are
 * latched as a click at the cursor. With keep_running the loop runs the game's
 * passes itself; the port makes the first poll's writes -- text_window_dirty set, the click
 * latch cleared by a mouse_read with nothing pending -- and returns, the
 * mouse state the polls after it leave and the latch after a broken wait not
 * seen. */
void input_wait_button_release(uw_motion *m, int keep_running) {
    (void)keep_running;
    ww(m->ds, 0x011b, 0xffff);
    /* the loop's first poll, which always runs: the text window marked
     * dirty, and mouse_read with nothing pending clearing the latch and
     * keeping the live mask in mouse_button_held (hand1's 060, the wait
     * ended there with the button up) */
    m->ds[0x0a8f] = 1;
    m->ds[0x011d] = 0;
    ww(m->ds, 0x011e, m->buttons);
}

/* combat_select_weapon(&row, &obj): the object in the weapon
 * hand (inventory slot 8, or 7 for rec +0x64 bit 0). A missile weapon
 * (class 0x1_) whose missile_props entry names ammunition 0..15 takes
 * missile_props' row and returns 0 when combat_check_for_ammo finds some --
 * with none it waits for the button's release (counted) and returns -1. A
 * melee weapon (0x0_) takes its eight-byte row and
 * its reach; anything else, or nothing, the fist's. Returns 1. */
static int combat_select_weapon(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    int16_t slot = (int16_t)(8 - (ds[(uint16_t)(rec + 0x64)] & 1));
    uint16_t o = obj_at(m, (uint16_t)((rw(ds, (uint16_t)(INVENTORY_SLOTS + slot * 2)) >> 6) & 0x3ff));
    ww(ds, COMBAT_WEAPON_ROW, 0);
    ww(ds, COMBAT_WEAPON_OBJ, o);
    ww(ds, (uint16_t)(COMBAT_WEAPON_OBJ + 2), o ? rw(ds, (uint16_t)(MOBILE_BASE + 2)) : 0);
    if (o) {
        uint16_t id = (uint16_t)(rw(ls, o) & 0x1ff);
        if ((id >> 4) == 1) {
            int8_t ammo = (int8_t)ds[(uint16_t)(0x5944 + (id & 0xf) * 3)];
            if (ammo >= 0 && ammo < 0x10) {
                if (combat_check_for_ammo(m, (uint16_t)(id & 0xf)) >= 0) {
                    ww(ds, COMBAT_WEAPON_ROW, (uint16_t)(MISSILE_PROPS_ROW + (id & 0xf) * 3));
                    return 0;
                }
                input_wait_button_release(m, 1);
                return -1;
            }
        } else if ((id >> 4) == 0) {
            ww(ds, COMBAT_WEAPON_ROW, (uint16_t)(MELEE_WEAPON_PROPS + (id & 0xf) * 8));
            ww(ds, COMBAT_REACH, ds[(uint16_t)(OBJ_PROPERTIES_1 + id * 0xb)] & 7);
        }
    }
    if (!rw(ds, COMBAT_WEAPON_ROW)) {
        ww(ds, COMBAT_WEAPON_ROW, FIST_ROW);
        ww(ds, COMBAT_REACH, ds[(uint16_t)(OBJ_PROPERTIES_1 + 0xf * 0xb)] & 7);
    }
    return 1;
}

/* item_enchantment(obj, &effect, &magnitude, &special): 1 when
 * the object carries a spell -- its own count word when word 0 has bits 15
 * and 12 and the class is not 5, or else, for a non-quantity with a link, the
 * first class 4 subclass 2 object along its contents. A linked spell at
 * quality 0 fails four times in ten, rt_rand() * 10 / 0x8000 under 4, unless
 * enchantment_query_only is set. Class 6 carries none. The
 * carrier's word 0 bit 11 is `special`: effect bits 6..8 of the count word
 * plus 0xc (0xffff for none) and magnitude bits 0..5; clear, effect bits
 * 4..8 and magnitude bits 0..3. */
int item_enchantment(uw_motion *m, uint16_t o, int16_t *effect, int16_t *magnitude, int *special) {
    uint8_t *ls = m->lseg;
    uint16_t w0 = rw(ls, o), carrier = 0, w;
    if (((w0 & 0x1c0) >> 6) == 6) return 0;
    if (!(w0 & 0x8000) && ((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff)) {
        uint16_t link = (uint16_t)(o + 6);
        carrier = object_find_matching(m, &link, 0, 4, 2, 0);
        if (carrier && !(ls[(uint16_t)(carrier + 4)] & 0x3f) && !m->ds[0x09f8]
            && (int16_t)(((int32_t)rt_rand(m) * 10) / 0x8000) < 4)
            return 0;
    } else if ((w0 & 0x8000) && (w0 & 0x1000) && ((w0 & 0x1c0) >> 6) != 5) {
        carrier = o;
    }
    if (!carrier) return 0;
    w = (uint16_t)((rw(ls, (uint16_t)(carrier + 6)) >> 6) & 0x1ff);
    *special = (rw(ls, carrier) & 0x800) != 0;
    if (*special) {
        *effect = (int16_t)(w >> 6 ? (w >> 6) + 0xc : 0xffff);
        *magnitude = (int16_t)(w & 0x3f);
    } else {
        *effect = (int16_t)(w >> 4);
        *magnitude = (int16_t)(w & 0xf);
    }
    return 1;
}

/* combat_damage_calc(row, obj, cell): the player's attack
 * context. The row's +6 names the skill (2..5, else 2: unarmed); the
 * rating is attack / 2 + that skill + dexterity / 7, and 7 more with rec +0xb4;
 * the damage for a fist is unarmed * 2 / 5 + strength / 6 + 4, for a weapon
 * its row's entry for the swing kind + strength / 9. An enchanted weapon --
 * item_enchantment, which may roll for a spent linked spell -- of the plain
 * effect 12 adds (magnitude & 7) + 1 to the damage when magnitude bit 3 is
 * set, to the attack rating when clear. */
static void combat_damage_calc(uw_motion *m, uint16_t row, uint16_t obj, int16_t cell) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    uint8_t str = ds[(uint16_t)(CRITTER_BASE + 5 + (rw(m->lseg, rw(ds, TRACKED_OBJECT)) & 0x3f) * 0x30)];
    int16_t skill = ds[(uint16_t)(row + 6)];
    if (skill >= 6 || skill < 2) skill = 2;
    ww(ds, COMBAT_ATTACK_RATING, (uint16_t)((ds[(uint16_t)(rec + 0x21)] >> 1) + ds[(uint16_t)(rec + 0x21 + skill)]
                                            + ds[(uint16_t)(rec + 0x1f)] / 7));
    if (ds[(uint16_t)(rec + 0xb4)])
        ww(ds, COMBAT_ATTACK_RATING, (uint16_t)(rw(ds, COMBAT_ATTACK_RATING) + 7));
    if (skill == 2)
        ww(ds, COMBAT_DAMAGE, (uint16_t)((int16_t)(ds[(uint16_t)(rec + 0x23)] << 1) / 5 + str / 6 + 4));
    else
        ww(ds, COMBAT_DAMAGE, (uint16_t)(str / 9 + ds[(uint16_t)(row + ds[(uint16_t)(SWING_KIND_BY_CELL + cell)])]));
    ww(ds, COMBAT_ATTACKER_OBJ, 1);
    ww(ds, COMBAT_SWING_TYPE, (uint16_t)cell);
    if (obj) {
        int16_t effect, magnitude;
        int special;
        if (item_enchantment(m, obj, &effect, &magnitude, &special) && !special && effect == 0xc) {
            uint16_t at = (magnitude & 8) ? COMBAT_DAMAGE : COMBAT_ATTACK_RATING;
            ww(ds, at, (uint16_t)(rw(ds, at) + (magnitude & 7) + 1));
        }
    }
}

/* combat_swing(cell): the player's swing, one step a call.
 * action_combat starts it with the view cell clicked; dungeon_tick_update
 * steps it with 0 every pass, keyed off the weapon animation frame the
 * panels advance: before frame 3 a release puts it back; at frame 3 a
 * melee swing charges while the button is held -- a charge step (the row's
 * +4, to 100) each 16 ticks -- and a release plays the strike; at frame 6
 * the charge scales between the row's +3 and +5 and the blow is struck
 * through combat_damage_calc and combat_resolve_attack; past it the weapon
 * is shown ready again. A missile weapon aims at frame 3 -- the crosshair
 * pushed -- and on the release looses a missile when the cursor is over the
 * view (missile_release), putting the swing away either way. `held` is the
 * watched key or, failing
 * that, the right button; when no key is watched (the idle -1) the byte is
 * the stack's, uninitialised -- the frame's call to player_frame_update
 * leaves its return address there, so the idle call reads 0x05
 * and does not sample the mouse. */
/* combat_swing_release -- a swing still charging (state above
 * 0) ended: the state 0, the button -1, panel values 3 and 8 back to 0 and
 * 4 -- has no caller and no stored pointer in the image, so nothing in the
 * game runs it and the port leaves it out; the charge ends in combat_swing
 * itself. */
void combat_swing(uw_motion *m, int16_t cell) {
    uint8_t *ds = m->ds;
    uint16_t bp = (uint16_t)(FRAME_BP - 12);
    int16_t state, frame;
    uint16_t row;
    uint8_t held = ds[(uint16_t)(bp - 3)];
    if (rs(ds, SWING_BUTTON) > 0) {
        if (m->keys_b && rw(ds, SWING_BUTTON) < 0x80) held = m->keys_b[rw(ds, SWING_BUTTON)] != 0;
        else held = m->keys ? m->keys[(uint16_t)(rw(ds, KEY_STATE_PTR) + rw(ds, SWING_BUTTON))] != 0 : 0;
    }
    held = held ? 1 : (mouse_sample_buttons(m) & 2) != 0;
    state = rs(ds, SWING_STATE);
    if (state > 0) return;
    if (state == 0) {
        int r;
        if (!((ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x5f)] >> 1) & 1) || !cell
            || rs(ds, WEAPON_ANIM_FRAME) != -1)
            return;
        ww(ds, COMBAT_SWING_ATTACK, (uint16_t)cell);
        r = combat_select_weapon(m);
        if (r < 0) return;
        ds[COMBAT_SWING_RANGED] = r == 0;
        if (ds[COMBAT_SWING_RANGED]) ww(ds, COMBAT_SWING_ATTACK, 0xffff);
        ww(ds, SWING_STATE, (uint16_t)(0xffff - ds[(uint16_t)(SWING_KIND_BY_CELL + cell)]));
        ww(ds, SWING_BUTTON, ds[(uint16_t)(SWING_BUTTON_BY_ROW + cell / 3)]);
        panel_set_value(m, 8, (uint16_t)(-rs(ds, SWING_STATE) - 1));
        panel_set_value(m, 3, 1);
        ds[COMBAT_CHARGE] = 0;
        ww(ds, COMBAT_CHARGE_TICKS, 0xffff);
        return;
    }
    frame = rs(ds, WEAPON_ANIM_FRAME);
    if (frame < 0 && state <= -10) {
        combat_show_ready_weapon(m);
        ww(ds, SWING_STATE, 0);
        ww(ds, SWING_BUTTON, 0xffff);
        return;
    }
    if (frame > 6) return;
    row = rw(ds, COMBAT_WEAPON_ROW);
    if (frame < 3) {
        if (held) return;
        combat_show_ready_weapon(m);
        ww(ds, SWING_STATE, 0);
        ww(ds, SWING_BUTTON, 0xffff);
        return;
    }
    if (frame == 3) {
        if (ds[COMBAT_SWING_RANGED]) {
            if (held || rs(ds, COMBAT_SWING_ATTACK) < 0) {
                if (rs(ds, COMBAT_SWING_ATTACK) >= 0) return;
                panel_set_value(m, 3, 9);
                ww(ds, COMBAT_SWING_ATTACK, 0);
                ww(ds, ACTION_STATE_WORD, (uint16_t)(rw(ds, ACTION_STATE_WORD) + 4));
                cursor_shape_push(m, 0x1075);       /* the crosshair */
                return;
            }
            if (cursor_over_view_rect(m))
                missile_release(m, (uint16_t)(rw(m->lseg, rw(ds, COMBAT_WEAPON_OBJ)) & 0xf), (uint16_t)(bp - 0x12));
            combat_swing_abort(m);
            return;
        }
        if (held) {
            uint16_t crow = rw(ds, CRITTER_ROW_PTR);
            ds[(uint16_t)(crow + 0x1d)] = (uint8_t)((ds[(uint16_t)(crow + 0x1d)] & 0xf0) | 0xa);
            if (rs(ds, COMBAT_CHARGE_TICKS) < 0) {
                ww(ds, COMBAT_SWING_CLOCK, (uint16_t)m->clock);
                ww(ds, (uint16_t)(COMBAT_SWING_CLOCK + 2), (uint16_t)(m->clock >> 16));
                ww(ds, COMBAT_CHARGE_TICKS, 0);
                return;
            }
            ww(ds, COMBAT_CHARGE_TICKS, (uint16_t)(rw(ds, COMBAT_CHARGE_TICKS)
                                                   + (uint16_t)m->clock - rw(ds, COMBAT_SWING_CLOCK)));
            ww(ds, COMBAT_SWING_CLOCK, (uint16_t)m->clock);
            ww(ds, (uint16_t)(COMBAT_SWING_CLOCK + 2), (uint16_t)(m->clock >> 16));
            while (rs(ds, COMBAT_CHARGE_TICKS) > 0x10) {
                ds[COMBAT_CHARGE] = (uint8_t)(ds[COMBAT_CHARGE] + ds[(uint16_t)(row + 4)]);
                if (ds[COMBAT_CHARGE] > 100) ds[COMBAT_CHARGE] = 100;
                panel_set_value(m, 3, (uint16_t)(ds[COMBAT_CHARGE] / 12 + 1));
                ww(ds, COMBAT_CHARGE_TICKS, (uint16_t)(rw(ds, COMBAT_CHARGE_TICKS) - 0x10));
            }
            return;
        }
        if (state <= -5) return;
        panel_set_value(m, 8, (uint16_t)(-state - 1));
        ww(ds, SWING_STATE, 0xfffb);
        return;
    }
    if (frame != 6 || state <= -10) return;
    panel_set_value(m, 3, 0);
    {
        int16_t d = (int16_t)(ds[(uint16_t)(row + 5)] - ds[(uint16_t)(row + 3)]);
        uint16_t crow = rw(ds, CRITTER_ROW_PTR);
        d = (int16_t)((int16_t)(d * ds[COMBAT_CHARGE]) / 100);
        ds[COMBAT_CHARGE] = (uint8_t)(ds[(uint16_t)(row + 3)] + d);
        ds[(uint16_t)(crow + 0x1d)] = (uint8_t)((ds[(uint16_t)(crow + 0x1d)] & 0xf0) | 0xf);
        ds[COMBAT_DAMAGE_SCALE] = ds[COMBAT_CHARGE];
    }
    combat_damage_calc(m, row, rw(ds, COMBAT_WEAPON_OBJ), rs(ds, COMBAT_SWING_ATTACK));
    resolve_attack(m, (uint16_t)(bp - 16));
    ww(ds, SWING_STATE, 0xfff6);
}
