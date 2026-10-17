/* SPDX-License-Identifier: MIT */
/* dungeon_tick_update, the pass's second standing handler,
 * from the instructions: experience and levels, the game step
 * (player_update_step and its regeneration, hunger, spell expiry, light
 * sources and fatigue) and uw_motion_tick_update itself.
 *
 * One of src/uw_motion*.c: see uw_motion.h and uw_motion_int.h. */
#include "uw_motion_int.h"
#include "uw_scroll.h"

/* ---- dungeon_tick_update, from the instructions --------------- */

/* ==== experience: award_kill_exp, player_gain_experience ==== */

enum { EXP_LEVEL_THRESHOLDS = 0x097f };   /* experience / 500 a level needs, by level */

/* player_gain_experience(delta), from the instructions. A loss
 * takes the 32-bit total at rec +0x4e down, to 0 if it would go below. A
 * gain is refused once the total is past 0x17700 (96,000),
 * so the award that crosses it still lands; halved (plus one) when the
 * character's level (rec +0x3d) is past twice the dungeon level plus two;
 * the skill points (rec +0x52) grow by the new total / 3000 past the last
 * such count (rec +0x53); the level-up loop walks the thresholds for
 * total / 500 and counts player_level_up. The panels are refreshed when the
 * new total >> 4 compares above the old one's low word sign-extended
 * (an unsigned 32-bit compare) -- below the cap, whenever a gain changes
 * total >> 4. */
enum { LEVEL_UP_DIGITS = 0x1c92 };  /* the level's two digits, the tail of message 0x93 */

/* player_recompute_maxima(fill): max hit points
 * 30 + level * strength / 5 into the critter row, max mana (Mana + 1) *
 * intelligence >> 3 into player_record+0x38 -- +0xb0 on level 7, where the
 * mana is withheld -- the carrying capacity strength * 20, and with `fill`
 * the mana topped up. */
void player_recompute_maxima(uw_motion *m, int fill) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), crow = rw(ds, CRITTER_ROW_PTR);
    uint8_t mana;
    ds[(uint16_t)(crow + 4)] = (uint8_t)((int16_t)(ds[(uint16_t)(rec + 0x3d)] * ds[(uint16_t)(crow + 5)]) / 5 + 0x1e);
    mana = (uint8_t)((int16_t)((ds[(uint16_t)(rec + 0x28)] + 1) * ds[(uint16_t)(crow + 7)]) >> 3);
    if (rw(ds, CURRENT_LEVEL_WORD) == 7) ds[(uint16_t)(rec + 0xb0)] = mana;
    else ds[(uint16_t)(rec + 0x38)] = mana;
    ww(ds, (uint16_t)(rec + 0x4c), (uint16_t)(ds[(uint16_t)(crow + 5)] * 0x14));
    if (fill) ds[(uint16_t)(rec + 0x37)] = ds[(uint16_t)(rec + 0x38)];
}

/* player_level_up(levels): the level raised, its
 * two digits (a space for the tens under 10) written into the tail of the
 * message "You have attained experience level " and that tail
 * printed, the training credits raised as much, the maxima recomputed and
 * the stats panel refreshed. */
static void player_level_up(uw_motion *m, int16_t levels) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    uint8_t lvl;
    ds[(uint16_t)(rec + 0x3d)] = (uint8_t)(ds[(uint16_t)(rec + 0x3d)] + levels);
    lvl = ds[(uint16_t)(rec + 0x3d)];
    ds[LEVEL_UP_DIGITS] = (uint8_t)(lvl < 10 ? 0x20 : lvl / 10 + 0x30);
    ds[(uint16_t)(LEVEL_UP_DIGITS + 1)] = (uint8_t)(lvl % 10 + 0x30);
    print_message(m, 0x93);
    if (m->scroll) {
        char text[0x41];
        size_t n = 0;
        while (n < 0x40 && ds[(uint16_t)(LEVEL_UP_DIGITS + n)]) {
            text[n] = (char)ds[(uint16_t)(LEVEL_UP_DIGITS + n)];
            n++;
        }
        text[n] = 0;
        uw_scroll_print(m->scroll, text);
    } else {
        UW_NOT_CARRIED(m->not_carried);
    }
    ds[(uint16_t)(rec + 0x52)] = (uint8_t)(ds[(uint16_t)(rec + 0x52)] + levels);
    player_recompute_maxima(m, 0);
    stats_panel_refresh(m);                 /* panel_redraw in mode 2 */
}

void player_gain_experience(uw_motion *m, int16_t delta) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    uint32_t exp = (uint32_t)rw(ds, (uint16_t)(rec + 0x4e)) | (uint32_t)rw(ds, (uint16_t)(rec + 0x50)) << 16;
    uint32_t q;
    uint16_t old_lo;
    int16_t levels = 0;
    int refresh;
    if (delta < 0) {
        uint16_t lo = (uint16_t)-delta, hi = (int16_t)lo < 0 ? 0xffff : 0;
        uint32_t take = (uint32_t)hi << 16 | lo;
        exp = take <= exp ? (uint32_t)(exp + (uint32_t)(int32_t)delta) : 0;
        ww(ds, (uint16_t)(rec + 0x4e), (uint16_t)exp);
        ww(ds, (uint16_t)(rec + 0x50), (uint16_t)(exp >> 16));
        return;
    }
    if ((exp >> 16) > 1 || ((exp >> 16) == 1 && (uint16_t)exp > 0x7700)) return;
    if ((int16_t)(rw(ds, CURRENT_LEVEL_WORD) * 2 + 2) < ds[(uint16_t)(rec + 0x3d)])
        delta = (int16_t)(delta / 2 + 1);
    q = (uint32_t)(exp + (uint32_t)(int32_t)delta) / 3000u;
    if ((int16_t)(uint16_t)q > ds[(uint16_t)(rec + 0x53)]) {
        ds[(uint16_t)(rec + 0x52)] = (uint8_t)(ds[(uint16_t)(rec + 0x52)] + (uint8_t)((uint8_t)q - ds[(uint16_t)(rec + 0x53)]));
        ds[(uint16_t)(rec + 0x53)] = (uint8_t)q;
    }
    old_lo = (uint16_t)(exp >> 4);
    exp += (uint32_t)(int32_t)delta;
    ww(ds, (uint16_t)(rec + 0x4e), (uint16_t)exp);
    ww(ds, (uint16_t)(rec + 0x50), (uint16_t)(exp >> 16));
    refresh = (exp >> 4) > (uint32_t)(int32_t)(int16_t)old_lo;
    q = exp / 500u;
    while (ds[(uint16_t)(EXP_LEVEL_THRESHOLDS + ds[(uint16_t)(rec + 0x3d)] + levels)] <= (int16_t)(uint16_t)q
           && ds[(uint16_t)(rec + 0x3d)] + levels < 0x10)
        levels++;
    if (levels) player_level_up(m, levels);
    if (refresh) panels_refresh(m);
}

/* award_kill_exp(creature): the Avatar's kill -- the flash
 * (panel 4, 2), theme music 9, and experience: the critter row's +0x28
 * word times four plus 2d(itself), scaled by (24..47)/16 for a creature
 * with +0x0d bit 10. */
void award_kill_exp(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    int16_t xp, total;
    if (((rw(ls, obj) & 0x1c0) >> 6) != 1) return;
    panel_set_value(m, 4, 2);
    ds[MUSIC_TRACK_WANTED] = 9;             /* set_theme_music(9) */
    xp = (int16_t)rw(ds, (uint16_t)(CRITTER_BASE + 0x28 + (rw(ls, obj) & 0x3f) * 0x30));
    total = (int16_t)(roll_dice(m, 2, xp) + (int16_t)(xp << 2));
    if (rw(ls, (uint16_t)(obj + 0xd)) & 0x400) {
        int16_t r = (int16_t)(rt_rand(m) % 0x18 + 0x18);
        total = (int16_t)((int16_t)(total * r) / 0x10);
    }
    player_gain_experience(m, total);
}

/* ==== the game step: player_update_step and its helpers ==== */

/* player_restore_mana(obj, n): n <= 0 adds -n, a byte's
 * arithmetic; n > 0 adds max * (n + rand&3) >> 4, plus one. Capped at the
 * maximum (rec +0x38). The player only. */
void player_restore_mana(uw_motion *m, uint16_t obj, int8_t n) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    if (obj != rw(ds, TRACKED_OBJECT)) return;
    if (n > 0) {
        int16_t r = (int16_t)(rt_rand(m) & 3);
        int16_t v = (int16_t)((int16_t)(n + r) * ds[(uint16_t)(rec + 0x38)]);
        ds[(uint16_t)(rec + 0x37)] = (uint8_t)((uint8_t)(v >> 4) + ds[(uint16_t)(rec + 0x37)] + 1);
    } else {
        ds[(uint16_t)(rec + 0x37)] = (uint8_t)(ds[(uint16_t)(rec + 0x37)] - (uint8_t)n);
    }
    if (ds[(uint16_t)(rec + 0x37)] > ds[(uint16_t)(rec + 0x38)])
        ds[(uint16_t)(rec + 0x37)] = ds[(uint16_t)(rec + 0x38)];
    panels_refresh(m);
}

/* player_restore_vitality(obj, n): the same for hit points
 * (obj +8), the maximum the critter row's +4. */
void player_restore_vitality(uw_motion *m, uint16_t obj, int8_t n) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t row = rw(ds, CRITTER_ROW_PTR);
    uint16_t pl = rw(ds, TRACKED_OBJECT);
    int16_t v;
    if (obj != pl) return;
    if (n > 0) {
        int16_t r = (int16_t)(rt_rand(m) & 3);
        v = (int16_t)((int16_t)((int16_t)(n + r) * ds[(uint16_t)(row + 4)]) >> 4);
        v = (int16_t)(v + ls[(uint16_t)(pl + 8)] + 1);
    } else {
        v = (int16_t)(ls[(uint16_t)(pl + 8)] - n);
    }
    ls[(uint16_t)(pl + 8)] = (int16_t)ds[(uint16_t)(row + 4)] < v ? ds[(uint16_t)(row + 4)] : (uint8_t)v;
    panels_refresh(m);
}

/* change_health(obj, n): hit points plus n, capped at the
 * critter properties' maximum for the object's id & 0x3f. */
void change_health(uw_motion *m, uint16_t obj, uint8_t n) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint8_t max = ds[(uint16_t)(0x4a56 + (rw(ls, obj) & 0x3f) * 0x30)];
    ls[(uint16_t)(obj + 8)] = (uint16_t)(ls[(uint16_t)(obj + 8)] + n) > max ? max
                              : (uint8_t)(ls[(uint16_t)(obj + 8)] + n);
    if (obj == rw(ds, TRACKED_OBJECT)) panels_refresh(m);
}

/* change_hunger(n): satiety (rec +0x39) plus n, floored
 * at 0 and refused past 0xff; a meal (n > 0) heals the stored
 * rec +0x3b / 8, at most 8, and clears it. */
int change_hunger(uw_motion *m, int16_t n) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    int16_t v = (int16_t)(ds[(uint16_t)(rec + 0x39)] + n);
    if (v > 0xff) return 0;
    ds[(uint16_t)(rec + 0x39)] = v < 0 ? 0 : (uint8_t)v;
    if (n > 0) {
        int16_t heal = (int16_t)(ds[(uint16_t)(rec + 0x3b)] / 8);
        if (heal > 8) heal = 8;
        change_health(m, rw(ds, TRACKED_OBJECT), (uint8_t)heal);
        ds[(uint16_t)(rec + 0x3b)] = 0;
    }
    return 1;
}

/* npc_randomise_hunger: every active mobile's +0x19
 * bit 7 a coin, and bit 6 kept one time in four. */
static void npc_randomise_hunger(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t p;
    for (p = rw(ds, ACTIVE_LIST); p < rw(ds, ACTIVE_END); p++) {
        uint16_t obj = (uint16_t)(rw(ds, MOBILE_BASE) + ls[p] * 0x1b);
        ls[(uint16_t)(obj + 0x19)] = (uint8_t)((ls[(uint16_t)(obj + 0x19)] & 0x7f) | ((rt_rand(m) % 2) & 1) << 7);
        if (rt_rand(m) % 4 != 1)
            ls[(uint16_t)(obj + 0x19)] &= 0xbf;
    }
}

/* active_spell_expire(&i): the player's spell slot i (rec +0x3e,
 * a word each: type nibble, subtype nibble, duration byte) has run out. A
 * type 1 of subtype 3 or 5 steps down to 0x0121; any other leaves the list,
 * the last slot moved into its place and i stepped back to revisit it --
 * type 0xb subtype 1 first restoring the camera (counted) and type 1 setting
 * player_in_liquid. */
int active_spell_expire(uw_motion *m, int16_t *i) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    uint16_t at = (uint16_t)(rec + 0x3e + *i * 2);
    uint16_t e = rw(ds, at), count;
    if ((e & 0xf) == 1 && ((e & 0xf0) >> 4 == 3 || (e & 0xf0) >> 4 == 5)) {
        ww(ds, at, (uint16_t)((e >> 8 << 8) + 0x21));
        ww(ds, at, (uint16_t)((rw(ds, at) & 0xff) + 0x100));
        return 1;
    }
    if ((e & 0xf) == 0xb && (e & 0xf0) >> 4 == 1) {
        debug_camera_set_target(m, 1);      /* Roaming Sight over: the Avatar's eye again */
        ww(ds, ACTION_STATE_WORD, (uint16_t)(rw(ds, ACTION_STATE_WORD) - 8));
    }
    if ((rw(ds, at) & 0xf) == 1) ds[PLAYER_IN_LIQUID] = 1;
    count = (uint16_t)(((rw(ds, (uint16_t)(rec + 0x5f)) >> 6) & 0xf) - 1) & 0xf;
    ww(ds, (uint16_t)(rec + 0x5f), (uint16_t)((rw(ds, (uint16_t)(rec + 0x5f)) & 0xfc3f) | count << 6));
    {
        uint16_t was = (uint16_t)*i;
        (*i)--;
        count = (rw(ds, (uint16_t)(rec + 0x5f)) >> 6) & 0xf;
        if (was < count)
            ww(ds, (uint16_t)(rec + 0x3e + (*i + 1) * 2), rw(ds, (uint16_t)(rec + 0x3e + count * 2)));
    }
    return 1;
}

/* update_light_sources(steps, step): the four light slots'
 * lit lights (ids 0x94..0x97) burn a quality point every burn-rate steps;
 * one burnt out goes dark (id - 4) and its slot is redrawn
 * (inventory_click_slot_index). Nonzero when one went out. */

static int update_light_sources(uw_motion *m, int16_t steps, uint8_t step) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    int out = 0, k;
    for (k = 0; k < 4; k++) {
        int8_t slot = (int8_t)ds[(uint16_t)(LIGHT_SLOT_INDICES + k)];
        uint16_t o = obj_at(m, (uint16_t)((rw(ds, (uint16_t)(INVENTORY_SLOTS + slot * 2)) >> 6) & 0x3ff));
        int16_t q, rate, burn;
        if (!o) continue;
        q = (int16_t)(rw(ls, o) & 0xf);
        if (((rw(ls, o) & 0x1f0) >> 4) != 9 || q < 4 || q >= 8) continue;
        rate = ds[(uint16_t)(LIGHT_BURN_RATES + q * 2)];
        if (!rate) continue;
        burn = (int16_t)(step % rate == 0);
        if (steps > 1) burn = (int16_t)(burn + steps / rate);
        if (!burn) continue;
        if ((uint16_t)(ls[(uint16_t)(o + 4)] & 0x3f) > (uint16_t)burn) {
            ls[(uint16_t)(o + 4)] = (uint8_t)((ls[(uint16_t)(o + 4)] & 0xc0) | (((ls[(uint16_t)(o + 4)] & 0x3f) - burn) & 0x3f));
        } else {
            ls[(uint16_t)(o + 4)] &= 0xc0;
            ww(ls, o, (uint16_t)((rw(ls, o) & 0xfff0) | (((rw(ls, o) & 0xf) - 4) & 0xf)));
            inventory_click_slot_index(m, slot);
            out = 1;
        }
    }
    return out;
}

/* player_drowning_tick: with the swim count (rec +0xb9, the head's dip below
 * the surface) past 0x50, a Swimming roll (rec +0x34 against the load rec
 * +0x4a / +0x4c, in 32nds) adds 3 - roll d4 below 0x8c; past 0x78 a second
 * roll's failure fills the view with colour 0xc6 (screen_flash) and
 * damages the player --
 * drowning. */
static void player_drowning_tick(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    uint8_t load = 0;
    int16_t r, miss;
    if (rw(ds, (uint16_t)(rec + 0x4c)))
        load = (uint8_t)((uint16_t)(rw(ds, (uint16_t)(rec + 0x4a)) << 5) / rw(ds, (uint16_t)(rec + 0x4c)));
    r = (int16_t)check_skill_roll(m, ds[(uint16_t)(rec + 0x34)], load);
    if (r < 1 && ds[(uint16_t)(rec + 0xb9)] < 0x8c)
        ds[(uint16_t)(rec + 0xb9)] = (uint8_t)(ds[(uint16_t)(rec + 0xb9)] + (uint8_t)roll_dice(m, (int16_t)(3 - r), 4));
    if (ds[(uint16_t)(rec + 0xb9)] > 0x78) {
        miss = (int16_t)(2 - check_skill_roll(m, ds[(uint16_t)(rec + 0x34)], load));
        if (miss) {
            m->screen_flash = 1;            /* screen_show_frame(0xc6) */
            m->screen_flash_colour = 0xc6;
            ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));   /* post_event(2) */
            apply_damage(m, rw(ds, TRACKED_OBJECT), 0, 0, 0, (uint8_t)roll_dice(m, 2, (int16_t)(miss + 2)), 0);
        }
    }
}

/* player_update_step, when the tick update's accumulator passes
 * 20 seconds' worth: the spell slots' durations, the lights, the 0x61
 * counter, the regeneration flags, drowning (player_drowning_tick); every
 * third step poison's
 * damage and the mana roll; every 24th hunger, the second 0x61 counter, the
 * create-object traps' chance, the creatures' hunger bits, the three
 * rec +0x3a counters and the vitality roll, and the counter back to 0. */
static void player_update_step(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    uint16_t pl = rw(ds, TRACKED_OBJECT);
    int changed = 0;
    int16_t i;
    ds[GAME_STEP_COUNTER]++;
    for (i = 0; (uint16_t)i < ((rw(ds, (uint16_t)(rec + 0x5f)) >> 6) & 0xf); i++) {
        uint16_t at = (uint16_t)(rec + 0x3e + i * 2);
        uint16_t left = rw(ds, at) >> 8;
        if (left == 1)
            changed = active_spell_expire(m, &i);
        else
            ww(ds, at, (uint16_t)((rw(ds, at) & 0xff) + ((left - 1) << 8)));
    }
    if (update_light_sources(m, 1, ds[GAME_STEP_COUNTER])) changed = 1;
    if ((ds[(uint16_t)(rec + 0x61)] >> 2) & 3) {
        uint8_t n = (uint8_t)((((ds[(uint16_t)(rec + 0x61)] >> 2) & 3) - 1) & 3);
        ds[(uint16_t)(rec + 0x61)] = (uint8_t)((ds[(uint16_t)(rec + 0x61)] & 0xf3) | n << 2);
        if (!n) changed = 1;
    }
    if (changed) {
        player_state_recalc(m);
        ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));   /* post_event(2) */
    }
    if (ds[REGEN_FLAGS]) {
        if (ds[REGEN_FLAGS] & 1) player_restore_vitality(m, pl, -1);
        if (ds[REGEN_FLAGS] & 2) player_restore_mana(m, pl, -1);
    }
    if (ds[(uint16_t)(rec + 0xb9)] > 0x50) player_drowning_tick(m);
    if (ds[GAME_STEP_COUNTER] % 3 == 0) {
        uint16_t w = rw(ds, (uint16_t)(rec + 0x5f));
        int r;
        if ((w >> 2) & 0xf) {
            ww(ds, (uint16_t)(rec + 0x5f), (uint16_t)((w & 0xffc3) | ((w - 4) & 0x3c)));
            apply_damage(m, pl, 0, 0, 0, (uint8_t)((w >> 2) & 0xf), 0x10);
        }
        r = check_skill_roll(m, ds[(uint16_t)(rec + 0x28)], 10);
        if (r > 0) player_restore_mana(m, pl, (int8_t)-r);
    }
    if (ds[GAME_STEP_COUNTER] % 0x18 == 0) {
        uint16_t w;
        change_hunger(m, (int16_t)(-3 - (rt_rand(m) & 3)));
        w = rw(ds, (uint16_t)(rec + 0x61));
        if ((w >> 4) & 0x3f)
            ww(ds, (uint16_t)(rec + 0x61), (uint16_t)((w & 0xfc0f) | ((((w >> 4) & 0x3f) - 1) & 0x3f) << 4));
        if ((rt_rand(m) & 3) == 0)
            trigger_create_object_trap(m, 1, (uint16_t)(FRAME_BP - 0x80));
        npc_randomise_hunger(m);
        for (i = 0; i < 3; i++)
            if (ds[(uint16_t)(rec + 0x3a + i)] < 0xff) ds[(uint16_t)(rec + 0x3a + i)]++;
        if (check_skill_roll(m, ds[(uint16_t)(rw(ds, CRITTER_ROW_PTR) + 5)], 0xf) > 0)
            player_restore_vitality(m, pl, -1);
        ds[GAME_STEP_COUNTER] = 0;
    }
}

/* A call in the tick update returning at `r` ran, when a saved state was
 * not inside it or before it. */
#define TICK_REACHED(stop, r) (!(stop) || (stop) > (r))

void uw_motion_tick_update(uw_motion *m, uint32_t clock, uint16_t stop) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t pl = rw(ds, TRACKED_OBJECT);
    int16_t hp, taken, dt;
    m->clock = clock;                       /* what *clock_ptr reads from here on: a strike's music stamp */
    if (!TICK_REACHED(stop, 0x3b1)) return;
    combat_swing(m, 0);
    hp = ls[(uint16_t)(pl + 8)];
    if (!TICK_REACHED(stop, 0x3ca)) return;
    panel_set_value(m, 0, (uint16_t)hp);
    taken = ls[(uint16_t)(pl + 0x11)];
    if ((int16_t)(taken << 2) > ds[PLAYER_MAX_HP] || (hp < 0x10 && taken > 0)) {
        if (!TICK_REACHED(stop, 0x3ff)) return;
        panel_set_value(m, 4, 3);
    }
    ls[(uint16_t)(pl + 0x11)] = 0;
    if (!TICK_REACHED(stop, 0x421)) return;
    panel_set_value(m, 1, ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x37)]);
    if (rw(ds, CURRENT_LEVEL_WORD) != 9) {
        if (!TICK_REACHED(stop, 0x461)) return;
        panel_set_value(m, 2, (uint16_t)((((((rw(ls, (uint16_t)(pl + 2)) & 0x380) >> 7) << 5)
                                           + (ls[(uint16_t)(pl + 0x18)] & 0x1f) + 8) & 0xff) >> 4));
    }
    if (!TICK_REACHED(stop, 0x473)) return;
    palette_cycle(m, (uint8_t)clock);
    if (ls[(uint16_t)(pl + 8)] == 0) {
        if (!TICK_REACHED(stop, 0x485)) return;
        player_death(m);
    }
    if (!TICK_REACHED(stop, 0x4b6)) return;  /* the two rt_lshr, which keep nothing */
    dt = (int16_t)((uint16_t)(clock >> 8) - rw(ds, LAST_TICK_TIME));
    if (dt) {
        ww(ds, LAST_TICK_TIME, (uint16_t)(clock >> 8));
        ww(ds, (uint16_t)(LAST_TICK_TIME + 2), (uint16_t)(clock >> 24));
        ds[TICK_ACCUMULATOR] = (uint8_t)(ds[TICK_ACCUMULATOR] + (uint8_t)dt);
        if (ds[OBJCHECK_PENDING]) {
            if (!TICK_REACHED(stop, 0x4d2)) return;
            objcheck_quiet(m);
            ds[OBJCHECK_PENDING] = 0;
        }
        if (!TICK_REACHED(stop, 0x4dc)) return;
        sound_update(m);
        if (ds[TICK_ACCUMULATOR] > 0x14) {
            ds[TICK_ACCUMULATOR] = 0;
            if (!TICK_REACHED(stop, 0x4ed)) return;
            player_update_step(m);
        }
    }
    if (rw(ds, CURRENT_LEVEL_WORD) == 9) {
        if (!TICK_REACHED(stop, 0x4f9)) return;
        if ((rt_rand(m) & 0x1f) == 0) {
            if (!TICK_REACHED(stop, 0x503)) return;
            ethereal_void_effect(m);
        }
    }
}

/* ==== sleep: player_sleep and what it needs ============================= */

/* apply_environment_damage: harm from the tracked object's
 * status flags at record+0xb8, the same byte that gates the view's
 * impairment -- bits 0..1 are lethal, 0xff of damage before the recalc and
 * again after it; bit 3, unless slow falling, levitating or flying (0x16), is
 * (rand % 6) * 10 + 12 of type 0x10. The settle between them is where the
 * two halves of the lethal case are told apart. */
static void apply_environment_damage(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), pl = rw(ds, TRACKED_OBJECT);
    if (ds[(uint16_t)(rec + 0xb8)] & 3) apply_damage(m, pl, 0, 0, 0, 0xff, 0);
    player_state_recalc(m);
    player_settle_motion(m);
    if ((ds[(uint16_t)(rec + 0xb8)] & 8) && !(ds[BLOCK_FLAGS] & 0x16))
        apply_damage(m, pl, 0, 0, 0, (uint8_t)((rt_rand(m) % 6) * 10 + 12), 0x10);
    if (ds[(uint16_t)(rec + 0xb8)] & 3) apply_damage(m, pl, 0, 0, 0, 0xff, 0);
}

/* garamon_dream(bedroll): one of the ten Garamon dreams, ids
 * 0x18..0x21, with the seen ones remembered in player_record+0x6e. In
 * order: dream 0 while its bit is clear; then dream 1 below level 2 -- no,
 * from level 2 on -- while bit 1 is clear; then dream 2 or 3 if a story
 * event has queued one; and failing all of that, one chance in
 * (bedroll * 4 + 4) of a random unseen dream from 4 to 9. Nothing plays
 * while record+0x62 bit 3 is set, which use_bones sets. 1 when one played.
 *
 * The cutscene is the host's; the no-dream path's spin on the timer at
 * 0x2364 for 0x180 ticks, which is what makes the two outcomes take a
 * similar amount of time, is delay_ticks' -- the host lets them pass. */
static int garamon_dream(uw_motion *m, int bedroll) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), flags = rw(ds, (uint16_t)(rec + 0x6e));
    int which = -1;
    if (!(flags & 1)) which = 0;
    else if (rs(ds, CURRENT_LEVEL_WORD) >= 2 && !(flags & 2)) which = 1;
    else if (flags & 4) which = 2;
    else if (flags & 8) which = 3;
    if (which < 0 && rt_rand(m) % (bedroll * 4 + 4) == 0) {
        which = rt_rand(m) % 6 + 4;
        if (flags & (1u << which)) which = -1;
    }
    if (which < 0 || (ds[(uint16_t)(rec + 0x62)] & 8)) {
        m->delay_ticks = (uint16_t)(m->delay_ticks + 0x180);   /* the spin on the timer at 0x2364 */
        return 0;
    }
    uw_motion_cutscene_request(m, (uint16_t)(which + 0x18));   /* cutscene_play(which + 0x18) */
    ww(ds, (uint16_t)(rec + 0x6e), (uint16_t)(flags ^ (1u << which)));
    return 1;
}

/* The play clock, a 32-bit tick count at player_record+0xce, advanced by
 * whole hours. An hour is 0xe1000 ticks -- 3600 seconds at the 256 Hz the
 * clock runs at -- which the light sources agree with: they are
 * stepped 0xb4 times an hour, one step every twenty seconds. */
static void play_time_add(uw_motion *m, uint32_t ticks) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    uint32_t t = (uint32_t)rw(ds, (uint16_t)(rec + 0xce)) | ((uint32_t)rw(ds, (uint16_t)(rec + 0xd0)) << 16);
    t += ticks;
    ww(ds, (uint16_t)(rec + 0xce), (uint16_t)t);
    ww(ds, (uint16_t)(rec + 0xd0), (uint16_t)(t >> 16));
}

/* The fatigue counter at record+0x61 bits 4..9, spent in one block: under
 * `take` it goes to nothing, otherwise `take` comes off it. */
static void sleep_spend_fatigue(uw_motion *m, uint16_t take) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), w = rw(ds, (uint16_t)(rec + 0x61));
    if (((w >> 4) & 0x3f) < take) ww(ds, (uint16_t)(rec + 0x61), (uint16_t)(w & 0xfc0f));
    else ww(ds, (uint16_t)(rec + 0x61),
            (uint16_t)((w & 0xfc0f) | (((((w >> 4) & 0x3f) - take) & 0x3f) << 4)));
}

/* player_sleep(mode). `mode` is 1 with
 * a bedroll, 0 without one and -2 for a collapse, which skips the refusals
 * and takes the environment's damage on the way down.
 *
 * It refuses -- "You can't go to sleep here!" (0x14) -- with record+0xb8's
 * bits 0, 1 or 3 set, in the air (vertical_gravity), or on level 9; and
 * "There are hostile creatures near!" (0x0e) when something is already
 * hunting. Then "You make camp." (0x0f), the sleep theme, the fade, "You go
 * to sleep." (0x10), the doors closed, and two to six hours pass: the play
 * clock advanced, the light sources stepped 0xb4 times an hour, the
 * poison counter's damage taken as the triangle of its level.
 *
 * Then npc_hunt_sleeper decides how it ended. Something came: the fatigue
 * down 0x20, "Your sleep is interrupted!" (0x15) and a little hunger.
 * Nothing came: the level settled, another seven to ten hours less the
 * first stretch (and one or two more under ten hit points), the vitality
 * and the mana restored by the fatigue's own measure -- doubled, with the
 * mana's a point more, when a bedroll was used and the Avatar is not
 * starving -- or, starving, message 0x11 and two points of damage instead;
 * then the hunger, the fatigue, a dream and "You awaken..." (0x13 without
 * the bedroll's bonus, 0x12 with it).
 *
 * The two fades are the shell's (uw_motion.screen_fade); what is not
 * carried is view_rebuild_and_draw, screen_present and
 * rast_cache_free_stale. */
void player_sleep(uw_motion *m, int16_t mode, uint16_t bp) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), pl = rw(ds, TRACKED_OBJECT);
    int dreamt = 0, bedroll;
    int16_t hours, heal;
    if (mode >= 0) {
        if ((ds[(uint16_t)(rec + 0xb8)] & 0x1b) || rw(ds, VERTICAL_GRAVITY)
            || rw(ds, CURRENT_LEVEL_WORD) == 9) {
            print_message(m, 0x14);
            return;
        }
        if (hostile_creature_nearby(m, bp)) { print_message(m, 0x0e); return; }
        print_message(m, 0x0f);
    }
    /* view_rebuild_and_draw: the view the fade out runs over, which the
     * host renders when it plays the fade (uw_motion.screen_fade) */
    ds[MUSIC_TRACK_WANTED] = 0xd;           /* set_theme_music(0xd) */
    sound_update(m);
    m->screen_fade |= 1;                    /* screen_fade_out, the shell's to run */
    if (mode >= 0) print_message(m, 0x10);
    find_and_close_doors(m, 0);
    obj_reclaim_distant(m, 1, 0x14);
    hours = (int16_t)(rt_rand(m) % 5 + 2);
    play_time_add(m, (uint32_t)(uint16_t)hours * 0xe1000u);
    ww(ds, (uint16_t)(rec + 0x5f), (uint16_t)(rw(ds, (uint16_t)(rec + 0x5f)) & 0xfc3f));
    ds[(uint16_t)(rec + 0x61)] &= 0xf3;
    update_light_sources(m, (int16_t)(hours * 0xb4), 0);
    if (ds[(uint16_t)(rec + 0x5f)] & 0x3c) {
        uint8_t p = (uint8_t)((ds[(uint16_t)(rec + 0x5f)] & 0x3c) >> 2);
        apply_damage(m, pl, 0, 0, 0, (uint8_t)(((p + 1) * p) >> 1), 0x10);
        ds[(uint16_t)(rec + 0x5f)] &= 0xc3;
    }
    if (mode < 0) apply_environment_damage(m);
    if (ls[(uint16_t)(pl + 8)] == 0) {
        ds[MUSIC_TRACK_WANTED] = (uint8_t)(rt_rand(m) % 3 + 2);   /* pick_level_theme_music */
        return;
    }
    if (!npc_hunt_sleeper(m, bp)) {
        npc_settle_level(m, bp);
        trigger_create_object_trap(m, 0, bp);
        hours = (int16_t)((int16_t)(rt_rand(m) % 4 + 7) - hours);
        if (ls[(uint16_t)(pl + 8)] < 10) hours = (int16_t)(hours + rt_rand(m) % 2 + 1);
        play_time_add(m, (uint32_t)(uint16_t)hours * 0xe1000u);
        update_light_sources(m, (int16_t)(hours * 0xb4), 0);
        bedroll = (ds[(uint16_t)(rec + 0x39)] >= 0x41 && mode >= 1) ? 1 : 0;
        heal = (int16_t)(ds[(uint16_t)(rec + 0x3a)] / 2 + 2);
        if (heal > 5) heal = 5;
        ds[(uint16_t)(rec + 0x3a)] = 0;
        if (ds[(uint16_t)(rec + 0x39)] == 0) {
            print_message(m, 0x11);
            apply_damage(m, pl, 0, 0, 0, 2, 0);
        } else {
            player_restore_vitality(m, pl, (int8_t)(heal + heal * bedroll - 1));
            player_restore_mana(m, pl, -6);
            player_restore_mana(m, pl, (int8_t)(heal + (heal + 1) * bedroll - 1));
        }
        change_hunger(m, (int16_t)(-0x18 - (rt_rand(m) & 0x1f)));
        sleep_spend_fatigue(m, 0x20);
        if (mode >= 0) dreamt = garamon_dream(m, bedroll);
        print_message(m, (uint16_t)(0x13 - bedroll));
    } else {
        if (ds[(uint16_t)(rec + 0x3a)] < 0x21) ds[(uint16_t)(rec + 0x3a)] = 0;
        else ds[(uint16_t)(rec + 0x3a)] = (uint8_t)(ds[(uint16_t)(rec + 0x3a)] - 0x20);
        print_message(m, 0x15);
        change_hunger(m, (int16_t)(-0xc - (rt_rand(m) & 0xf)));
        sleep_spend_fatigue(m, 0x10);
    }
    player_state_recalc(m);
    /* rast_cache_free_stale: the rasteriser's EMS cache of textures and
     * sprites let go of what the night aged out; the port's renderer holds
     * every image it has decoded and has nothing to free */
    ww(ds, MOVEMENT_SPEED, 0);
    ww(ds, VERTICAL_GRAVITY, 0);
    ww(ds, MOVEMENT_INPUT_B, 0);
    ww(ds, MOVEMENT_INPUT_A, 0);
    ww(ds, VERTICAL_VELOCITY, 0);
    ww(ds, 0x2788, 0);
    ww(ds, 0x2786, 0);
    stats_panel_refresh(m);
    /* view_rebuild_and_draw, and then a plain screen_present when a dream
     * was shown and screen_fade_in(5) when none was -- an ordinary night,
     * an interrupted one or a collapse (the flag is 1 - (a
     * dream played), and set is the fade): both the host's, rendering the
     * view as it shows it */
    ds[MUSIC_TRACK_WANTED] = (uint8_t)(rt_rand(m) % 3 + 2);   /* pick_level_theme_music */
    m->screen_fade |= dreamt ? 4 : 2;
}

/* try_sleep, the key: a class 4 subclass 2 type 1 thing in the
 * pack -- a bedroll -- and player_sleep told whether one was found, so
 * sleeping with a bedroll and sleeping without one are the same call with a
 * different flag. */
void try_sleep(uw_motion *m, uint16_t bp) {
    int16_t slot = 0;
    player_sleep(m, (int16_t)(inventory_find_by_kind(m, 4, 2, 1, 4, &slot) != 0), bp);
}


/* ==== player_death, from the instructions ==== */

/* find_teleport_target(level, item) -> whether `level` is the
 * current one. The arrival callback cleared first, whatever the level
 * then on the current level the first object of the item's class,
 * subclass and type in the tile map from (0, 0), and where the search
 * stopped written as the teleport destination -- the search's end, (0,
 * 0x40), when there is none. */
static int find_teleport_target(uw_motion *m, int level, uint16_t item) {
    uint8_t *ds = m->ds;
    int16_t x = 0, y = 0;
    ww(ds, 0x13ba, 0);
    ww(ds, 0x13bc, 0);
    if (level != (int16_t)rw(ds, CURRENT_LEVEL_WORD)) return 0;
    object_find_in_tilemap(m, (uint16_t)((item >> 6) & 7), (uint16_t)((item >> 4) & 3), (uint16_t)(item & 0xf), &x, &y);
    ww(ds, 0x5704, (uint16_t)x);
    ww(ds, 0x5706, (uint16_t)y);
    return 1;
}

/* teleport_to_moonstone: the moonstone's arrival callback --
 * find_teleport_target on the level in the record's +0x5e low nibble, for
 * item 0x126, the moonstone. */
void teleport_to_moonstone(uw_motion *m) {
    uint8_t *ds = m->ds;
    find_teleport_target(m, ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x5e)] & 0xf, 0x126);
}

/* player_resurrect: the silver tree's arrival callback. With
 * the tree on this level and found: the hit points whole below a maximum
 * of 9, else two to four short; the mana full, less max / 8 + 2 above 8;
 * the player object's +0x15 low bits 0x2c; the poison and every active
 * enchantment cleared; player_state_recalc; set_theme_music(4). */
void player_resurrect(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), row = rw(ds, 0x7272), pl = rw(ds, TRACKED_OBJECT);
    if (!find_teleport_target(m, ds[(uint16_t)(rec + 0x5e)] >> 4, 0x1ca)) return;
    if (ds[(uint16_t)(row + 4)] < 9) ls[(uint16_t)(pl + 8)] = ds[(uint16_t)(row + 4)];
    else ls[(uint16_t)(pl + 8)] = (uint8_t)(ds[(uint16_t)(row + 4)] - 2 - (int8_t)(((int32_t)rt_rand(m) * 3) >> 15));
    ds[(uint16_t)(rec + 0x37)] = ds[(uint16_t)(rec + 0x38)];
    if (ds[(uint16_t)(rec + 0x38)] > 8)
        ds[(uint16_t)(rec + 0x37)] = (uint8_t)(ds[(uint16_t)(rec + 0x37)] - (ds[(uint16_t)(rec + 0x38)] / 8 + 2));
    ls[(uint16_t)(pl + 0x15)] = (uint8_t)((ls[(uint16_t)(pl + 0x15)] & 0xc0) | 0x2c);
    ds[(uint16_t)(rec + 0x5f)] &= 0xc3;
    ww(ds, (uint16_t)(rec + 0x5f), (uint16_t)(rw(ds, (uint16_t)(rec + 0x5f)) & 0xfc3f));
    player_state_recalc(m);
    ds[MUSIC_TRACK_WANTED] = 4;             /* set_theme_music(4) */
}

/* player_death: refused while the ritual counter (+0x6d) is 0, the hit
 * points set to 4 instead; else the effects stopped and track 10 loaded
 * (the sound's), an eighth of the experience lost, the view rebuilt and
 * faded out, combat_reset, what the cursor held put down beside the
 * Avatar in action states 0 and 1 and let go of in 2, the remains -- item
 * 0xc2 + rand % 5, the Avatar's low seven bits of word 2 and its two
 * three-bit fields, owner 0x3f -- placed at the Avatar's position and
 * settled; then, with a silver tree planted on a level other than 9, the
 * teleport to it with player_resurrect as the arrival callback and the
 * fades off, perform_pending_teleport -- the host's, since it needs the
 * archive -- and uw_motion_death_teleported after; else
 * game_return_to_menu(1), the host's. */
void player_death(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), pl = rw(ds, TRACKED_OBJECT), held, o, id;
    uint32_t exp;
    int16_t st;
    enum { BP = 0x9568 };                   /* player_death's frame under the tick's */
    if (ds[(uint16_t)(rec + 0x6d)] == 0) { ls[(uint16_t)(pl + 8)] = 4; return; }
    sound_effect_stop_all(m);
    load_xmi(m, 10, 1);                     /* the death's own music */
    exp = (uint32_t)rw(ds, (uint16_t)(rec + 0x4e)) | (uint32_t)rw(ds, (uint16_t)(rec + 0x50)) << 16;
    player_gain_experience(m, (int16_t)-(int16_t)(uint16_t)(exp >> 3));
    m->screen_fade |= 1;                    /* view_rebuild_and_draw, then screen_fade_out(5): the host's */
    combat_reset(m);
    held = rw(ds, CURSOR_OBJECT);
    st = rs(ds, ACTION_STATE_WORD);
    if (held || rw(ds, (uint16_t)(CURSOR_OBJECT + 2))) {
        if (st == 0 || st == 1) uw_motion_object_place_at_own_coords(m, pl, held, 6, 0, BP);
        if (st == 0 || st == 1 || st == 2) {
            ww(ds, CURSOR_OBJECT, 0);
            ww(ds, (uint16_t)(CURSOR_OBJECT + 2), 0);
            ww(ds, ACTION_STATE_WORD, 0);
            cursor_shape_pop(m, 3);
        }
    }
    id = (uint16_t)((int16_t)rt_rand(m) % 5 + 0xc2);
    o = create_object(m, id, 0);
    if (o && object_move_to_coords(m, (int16_t)(rs(ds, PLAYER_X) >> 5), (int16_t)(rs(ds, PLAYER_Y) >> 5),
                                   (int16_t)(rs(ds, PLAYER_Z) >> 3), o, 0, 1, BP)) {
        uint16_t w2 = rw(ls, (uint16_t)(o + 2)), p2 = rw(ls, (uint16_t)(pl + 2));
        w2 = (uint16_t)((w2 & 0xff80) | (p2 & 0x7f));
        ls[(uint16_t)(o + 6)] = (uint8_t)((ls[(uint16_t)(o + 6)] & 0xc0) | 0x3f);
        w2 = (uint16_t)((w2 & 0x1fff) | (p2 & 0xe000));
        w2 = (uint16_t)((w2 & 0xe3ff) | (p2 & 0x1c00));
        ww(ls, (uint16_t)(o + 2), w2);
        placed_object_collision(m, o, (uint16_t)(rs(ds, PLAYER_X) >> 8), (uint16_t)(rs(ds, PLAYER_Y) >> 8), 1, BP);
    } else if (!o) UW_NOT_CARRIED(m->not_carried);
    if ((ds[(uint16_t)(rec + 0x5e)] >> 4) && rw(ds, CURRENT_LEVEL_WORD) != 9) {
        trap_teleport(m, pl, 0x3f, 0x3f, (int16_t)(ds[(uint16_t)(rec + 0x5e)] >> 4), BP);
        ww(ds, 0x13ba, 0x43);               /* the arrival callback: player_resurrect */
        ww(ds, 0x13bc, 0x629a);
        ds[0x13b2] = 0;
        m->death = 1;                       /* perform_pending_teleport: the host's */
        return;
    }
    m->return_to_menu = 2;                  /* game_return_to_menu(1) */
}

void uw_motion_death_teleported(uw_motion *m, int ok) {
    uint8_t *ds = m->ds;
    ds[0x13b2] = 3;
    m->death = 0;
    if (ok) {
        uw_motion_cutscene_request(m, 0x102);
        m->screen_flash = 1;                /* gfx_clear_work_buffer(0xf1): the view cleared */
        m->screen_flash_colour = 0xf1;
        if (m->scroll) uw_scroll_clear(m->scroll, 1);   /* text_window_clear(1) */
        else UW_NOT_CARRIED(m->not_carried);
        return;
    }
    m->return_to_menu = 2;
}


/* ==== the player's impacts and statuses ==== */

enum { PLAYER_VELOCITY_X = 0x2786, PLAYER_VELOCITY_Y = 0x2788 };

/* player_daze: player_start_status_effect(0x40, 0x1e). */
void player_daze(uw_motion *m) { player_start_status_effect(m, 0x40, 0x1e); }

/* player_knockback_up(n). */
void player_knockback_up(uw_motion *m, int16_t n) {
    uint8_t *ds = m->ds;
    if (rs(ds, VERTICAL_GRAVITY) != -4) ww(ds, VERTICAL_GRAVITY, (uint16_t)-2);
    ww(ds, VERTICAL_VELOCITY, (uint16_t)(int16_t)((n * 0x2f) / 4));
    ww(ds, PLAYER_VELOCITY_X, (uint16_t)(int16_t)(rs(ds, PLAYER_VELOCITY_X) / 2));
    ww(ds, PLAYER_VELOCITY_Y, (uint16_t)(int16_t)(rs(ds, PLAYER_VELOCITY_Y) / 2));
}

/* player_apply_impact(mask, n). */
void player_apply_impact(uw_motion *m, uint16_t mask, int16_t n) {
    if (mask & 1) player_daze(m);
    if (mask & 2) player_knockback_up(m, n);
}

/* player_start_vertical_motion(obj): the face bit 0x10 skips only the
 * velocity, not the gravity. */
void player_start_vertical_motion(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds;
    if (!is_tracked(m, obj)) return;
    if (!(ds[COLLISION_FACE] & 0x10)) ww(ds, VERTICAL_VELOCITY, 0x8d);
    ww(ds, VERTICAL_GRAVITY, 0);
}

/* ethereal_void_effect, the Ethereal Void's, which the tick's level-9 branch
 * runs one frame in 32: screen_show_frame(0xb5) -- the wince,
 * the host's -- then the hit points banded by what is left: from 0x65 up
 * rand % 6 lost, 0x33..0x64 rand % 4, under that one time in four rand % 3
 * when above 0x14, else (or when the roll passed) one time in eight one
 * point when above 1; eleven times in twelve a daze of rand % 0x1e + 0xf
 * ticks; and the heading's panel value set to rand & 0xf. */
void ethereal_void_effect(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t pl = rw(ds, TRACKED_OBJECT);
    uint8_t hp = ls[(uint16_t)(pl + 8)];
    int done = 0;
    m->screen_flash = 1;
    m->screen_flash_colour = 0xb5;
    if (hp < 0x65) {
        if (hp < 0x33) {
            if (hp > 0x14 && (rt_rand(m) & 3) == 0) { hp = (uint8_t)(hp - (uint8_t)((int16_t)rt_rand(m) % 3)); done = 1; }
            if (!done && hp > 1 && (rt_rand(m) & 7) == 0) hp--;
        } else hp = (uint8_t)(hp - (uint8_t)((int16_t)rt_rand(m) % 4));
    } else hp = (uint8_t)(hp - (uint8_t)((int16_t)rt_rand(m) % 6));
    ls[(uint16_t)(pl + 8)] = hp;
    if ((int16_t)rt_rand(m) % 0xc != 0) player_start_status_effect(m, 0x40, (uint8_t)((int16_t)rt_rand(m) % 0x1e + 0xf));
    panel_set_value(m, 2, (uint16_t)(rt_rand(m) & 0xf));
}
