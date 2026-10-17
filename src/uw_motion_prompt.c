/* SPDX-License-Identifier: MIT */
/* The use handlers that stand in a prompt -- the shrine's mantra
 * (chant_mantra), the anvil's repair (use_anvil, item_repair with its
 * yes/no question) and the instruments (play_instrument). Each runs to its
 * wait and leaves a flag for the host, which
 * collects the answer as the original's modal loop did and calls the
 * continuation: uw_motion_mantra_answer, uw_motion_yesno_answer with
 * uw_motion_cutscene_finished, uw_motion_instrument_key / _tick / _end. */
#include "uw_motion_int.h"
#include "uw_ail.h"
#include <ctype.h>

/* ---- the shrine ------------------------------------------------------ */

/* skill_governing_attribute: 0 (strength) under skill 7, 2
 * (intelligence) under 10, else 1 (dexterity): an index into the critter
 * row's +5..+7. */
static int skill_governing_attribute(int skill) {
    return skill < 7 ? 0 : skill < 10 ? 2 : 1;
}

/* obj_reset_identification(obj) -> 0: a thing that is not
 * mobile (obj_is_mobile: past the mobiles' records), not a door (class 5),
 * not class 6 and not obj_properties +9 kind 2 has the identification in
 * word 1 bits 7..9 cut to its low two bits -- 3 at most, what a look
 * learns again. */
static void obj_reset_identification(uw_motion *m, uint16_t o) {
    uint8_t *ls = m->lseg;
    uint16_t w0 = rw(ls, o), w1 = rw(ls, (uint16_t)(o + 2));
    if (o < rw(m->ds, STATIC_BASE)) return;
    if ((w0 & 0x1c0) == 0x140 || (w0 & 0x1c0) == 0x180 || (prop(m, (uint16_t)(w0 & 0x1ff), 9) & 3) == 2) return;
    ww(ls, (uint16_t)(o + 2), (uint16_t)((w1 & 0xfc7f) | ((((w1 & 0x380) >> 7) & 3) << 7)));
}

/* run_code_on_object_chain(first, obj_reset_identification):
 * each thing along the chain, and a non-quantity's contents before the
 * next; the callback's 0 never ends the walk. */
static void chain_reset_identification(uw_motion *m, uint16_t o) {
    uint8_t *ls = m->lseg;
    while (o) {
        obj_reset_identification(m, o);
        if (!(rw(ls, o) & 0x8000) && (rw(ls, (uint16_t)(o + 6)) >> 6))
            chain_reset_identification(m, deref_link(m, (uint16_t)(o + 6)));
        if (!(rw(ls, (uint16_t)(o + 4)) >> 6)) break;
        o = deref_link(m, (uint16_t)(o + 4));
    }
}

/* level_reset_identification, whose one callback -- the far
 * pointer it pushes, stub 15 of 69c8 -- is obj_reset_identification:
 * every tile of the 64 by 64 map with a chain, the chain walked. */
static void run_code_on_all_objects_reset_identification(uw_motion *m) {
    uint16_t x, y, tile;
    for (y = 0; y < 0x40; y++)
        for (x = 0; x < 0x40; x++) {
            tile = tile_ptr(m, x, y);
            if (rw(m->lseg, (uint16_t)(tile + 2)) >> 6)
                chain_reset_identification(m, deref_link(m, (uint16_t)(tile + 2)));
        }
}

/* skill_gain(skill): 1 when the skill
 * rose. divisor = divisors[attribute index] (0x19, 0x28, 0x0a); no gain
 * when the skill already exceeds twice the attribute or is 0x1e or more;
 * otherwise +1, a further +1 when the attribute index is non-zero and the
 * skill is still under half the attribute, and while the skill is under
 * the attribute one more with probability (attribute - skill) / divisor,
 * capped at 30. Skill 8, Lore: level_reset_identification with
 * obj_reset_identification -- what lies in the level to be identified
 * again -- and, under level 9, the record's per-level byte at +0xc2 takes
 * the new Lore. */
int skill_gain(uw_motion *m, int skill) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), row = rw(ds, CRITTER_ROW_PTR);
    int ix = skill_governing_attribute(skill);
    unsigned divisor = ds[(uint16_t)(0x1c8c + ix)], attr = ds[(uint16_t)(row + 5 + ix)];
    uint16_t at = (uint16_t)(rec + 0x21 + skill);
    int rose = 1;
    if (attr * 2 < ds[at] || ds[at] > 0x1d) {
        rose = 0;
    } else {
        ds[at]++;
        if (ix && ds[at] < attr / 2) ds[at]++;
        if (ds[at] < attr && (rt_rand(m) % (int)divisor) < (int)(attr - ds[at])) ds[at]++;
        if (ds[at] > 0x1e) ds[at] = 0x1e;
    }
    if (skill == 8) {
        run_code_on_all_objects_reset_identification(m);
        if (rw(ds, CURRENT_LEVEL_WORD) < 9)
            ds[(uint16_t)(rec + 0xc2 + rw(ds, CURRENT_LEVEL_WORD))] = ds[(uint16_t)(rec + 0x29)];
    }
    return rose;
}

/* skill_advance_greatly_message(skill, rose): "You cannot advance
 * any further in that skill." (0x1b), or "You have advanced greatly in ",
 * the skill's name (block 2, 0x1f on) and ".\n". */
static void skill_advance_greatly_message(uw_motion *m, int skill, int rose) {
    char text[0x40];
    if (!rose) { print_message(m, 0x1b); return; }
    print_message(m, 0x1c);
    print_string(m, (uint16_t)((skill + 0x1f) | 0x400));
    scroll_print(m, ds_text(m, 0x1c96, text, sizeof text));
}

/* skill_advance_message(list): "None of your skills improved."
 * (0x1e) for an empty list, else "You have advanced in " and up to four
 * names joined with ", " and " and " before the last,
 * then ".\n". */
static void skill_advance_message(uw_motion *m, const uint8_t *list) {
    char text[0x40];
    int i;
    if (list[0] == 0xff) { print_message(m, 0x1e); return; }
    print_message(m, 0x1d);
    for (i = 0; i < 4 && list[i] != 0xff; i++) {
        if (i == 3 || (i && list[i + 1] == 0xff)) scroll_print(m, ds_text(m, 0x1c99, text, sizeof text));
        else if (i) scroll_print(m, ds_text(m, 0x1c9f, text, sizeof text));
        print_string(m, (uint16_t)((list[i] + 0x1f) | 0x400));
    }
    scroll_print(m, ds_text(m, 0x1c96, text, sizeof text));
}

/* chant_mantra(0), a shrine used: scroll_text_input's prompt
 * "Chant the mantra: " with a field of up to ten characters,
 * which the host edits (mantra_ask) and answers with
 * uw_motion_mantra_answer. */
void chant_mantra(uw_motion *m) {
    char text[0x40];
    if (!m->scroll) { UW_NOT_CARRIED(m->not_carried); return; }
    uw_scroll_print(m->scroll, ds_text(m, 0x1ca2, text, sizeof text));
    m->mantra_ask = 1;
}

/* The chant's end: delay_ticks(0x20), mouse_clear_pending and
 * keyboard_drain -- the latched click cleared, the keys pressed since
 * dropped. */
static void mantra_tail(uw_motion *m) {
    m->delay_ticks = (uint16_t)(m->delay_ticks + 0x20);
    ww(m->ds, 0x0115, 0xffff);
    m->ds[0x011d] = 0;
    uw_motion_keyboard_drain(m);
}

/* The text answered: "\n"; the text upper-cased and compared
 * with block 2's strings 0x33..0x4c, none being "That is not a mantra."
 * (0x19) with player_state_recalc and the tail. A skill mantra (the first
 * twenty): with no advancement owed (record +0x52) "You are not ready to
 * advance." (0x18); else skill_gain twice, and any rise is "Knowledge and
 * Understanding fill you." (0x1a) and one advancement spent, then
 * skill_advance_greatly_message; player_recompute_maxima(0),
 * stats_panel_refresh, player_state_recalc and the tail. INSAHN, unless the
 * record's +0x60 bit 7 says the Cup of Wonder is found, reports its
 * direction from the Avatar's tile to (0x18, 0x2d) on level 3, near 4;
 * FANLO, unless bit 6, spawns item 0xe1 in the hand -- "An object appears
 * ..." wait, message 0x1e -- and sets bit 6; NO is message 0x1f; each
 * with delay_ticks(0x20) alone. SUMM RA, MU AHM and OM CAH draw skills
 * from a group (0 of 7, 7 of 3, 10 of 10; three, two and four draws, and
 * as many attempts as the group has skills): with no advancement owed
 * 0x18; else each attempt picks group + rand * count / 0x8000 -- or, for
 * the second group with Mana (record +0x28) under 8 and rand bit 1, skill
 * 7 outright -- and a rise is listed; skill_advance_message, one
 * advancement spent, and the recompute, refresh, recalc and tail. */
void uw_motion_mantra_answer(uw_motion *m, const char *typed) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    char upper[0x40], text[0x40];
    int i, n, group = 0, count = 0, draws = 0;
    m->mantra_ask = 0;
    scroll_print(m, ds_text(m, 0x1c90, text, sizeof text));
    for (i = 0; typed[i] && i < (int)sizeof upper - 1; i++) upper[i] = (char)toupper((unsigned char)typed[i]);
    upper[i] = 0;
    for (n = 0x33; n < 0x4d; n++) {
        if (m->strings && uw_strings_by_id(m->strings, (uint16_t)(n | 0x400), text, (int)sizeof text) >= 0
            && !strcmp(upper, text)) break;
    }
    if (n == 0x4d) {
        print_message(m, 0x19);
        player_state_recalc(m);
        mantra_tail(m);
        return;
    }
    n -= 0x33;
    if (n < 0x14) {
        if (!ds[(uint16_t)(rec + 0x52)]) {
            print_message(m, 0x18);
        } else {
            int a = skill_gain(m, n), b = skill_gain(m, n);
            if (a || b) { print_message(m, 0x1a); ds[(uint16_t)(rec + 0x52)]--; }
            skill_advance_greatly_message(m, n, a || b);
        }
        goto recompute;
    }
    switch (n - 0x14) {
    case 0:                                 /* INSAHN */
        if (!(ds[(uint16_t)(rec + 0x60)] & 0x80)) {
            uint16_t w = rw(m->lseg, (uint16_t)(rw(ds, TRACKED_OBJECT) + 0x16));
            if (m->strings && uw_strings_by_id(m->strings, 0x223, text, (int)sizeof text) >= 0)
                report_direction_to(m, text, (int16_t)(w >> 10), (int16_t)((w & 0x3f0) >> 4),
                                    (int16_t)rw(ds, CURRENT_LEVEL_WORD), 0x18, 0x2d, 3, 4);
            else UW_NOT_CARRIED(m->not_carried);
        }
        m->delay_ticks = (uint16_t)(m->delay_ticks + 0x20);
        return;
    case 1:                                 /* FANLO */
        if (!(ds[(uint16_t)(rec + 0x60)] & 0x40) && spawn_object_in_hand(m, 0, 0xe1)) {
            print_message(m, 0x1e);
            ds[(uint16_t)(rec + 0x60)] |= 0x40;
        }
        m->delay_ticks = (uint16_t)(m->delay_ticks + 0x20);
        return;
    case 2:                                 /* NO */
        print_message(m, 0x1f);
        m->delay_ticks = (uint16_t)(m->delay_ticks + 0x20);
        return;
    case 3: group = 0; count = 7; draws = 3; break;       /* SUMM RA */
    case 4: group = 7; count = 3; draws = 2; break;       /* MU AHM */
    case 5: group = 10; count = 10; draws = 4; break;     /* OM CAH */
    default: break;
    }
    if (!ds[(uint16_t)(rec + 0x52)]) {
        print_message(m, 0x18);
    } else {
        uint8_t list[4] = { 0xff, 0xff, 0xff, 0xff };
        int listed = 0, attempts = count, left = draws;
        while (left) {
            int skill;
            if (!attempts) break;
            attempts--;
            if (group == 7 && ds[(uint16_t)(rec + 0x28)] < 8 && (rt_rand(m) & 2))
                skill = 7;
            else
                skill = group + (int)(((int32_t)rt_rand(m) * count) / 0x8000);
            if (skill_gain(m, skill) && listed < 4) list[listed++] = (uint8_t)skill;
            left--;
        }
        skill_advance_message(m, list);
        ds[(uint16_t)(rec + 0x52)]--;
    }
recompute:
    player_recompute_maxima(m, 0);
    stats_panel_refresh(m);
    player_state_recalc(m);
    mantra_tail(m);
}

void uw_motion_chant_mantra(uw_motion *m) { chant_mantra(m); }
int  uw_motion_skill_gain(uw_motion *m, int skill) { return skill_gain(m, skill); }

/* ---- the anvil ------------------------------------------------------- */

/* get_item_durability(obj), from the instructions: a class-0
 * object's durability -- a melee weapon's (subclass 0) from
 * melee_weapon_props +7 by its low nibble, armour's (subclass 2 and 3)
 * from armour_props +1 by (id - 0x20); anything else -1. */
static int get_item_durability(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds;
    uint16_t w0 = rw(m->lseg, obj);
    if (w0 & 0x1c0) return -1;
    switch ((w0 & 0x30) >> 4) {
    case 0: return (int8_t)ds[(uint16_t)(MELEE_WEAPON_PROPS + (w0 & 0xf) * 8 + 7)];
    case 2: case 3: return (int8_t)ds[(uint16_t)(ARMOUR_PROPS + ((w0 & 0x3f) - 0x20) * 4 + 1)];
    default: return -1;
    }
}

/* item_repair_apply(obj, skill, &difficulty): 0 for a thing with no
 * durability. difficulty = max(durability * 3 - skill - quality / 2, 0xf),
 * left for the caller -- the time item_repair charges, and nothing else;
 * check_skill_roll(skill, durability) -- the durability, not the
 * difficulty: 0 is no effect (1); 1 adds skill / 5 +
 * 3; 2 adds 0x40; -1 destroys the thing (-2, its quality left as it was)
 * when rand & 0x3f exceeds
 * quality + skill, else takes 4..11 off.
 * Then a sum past 0x3f is quality 0x3f, 3 (fully repaired); a positive sum
 * is the quality, 2 (partially) for a rise and -1 (damaged) for a fall;
 * else quality 0, -2. */
static int item_repair_apply(uw_motion *m, uint16_t obj, int skill, int16_t *difficulty) {
    uint8_t *ls = m->lseg;
    int dur = get_item_durability(m, obj), q, diff, r, si;
    if (dur < 0) return 0;
    q = ls[(uint16_t)(obj + 4)] & 0x3f;
    diff = dur * 3 - skill - q / 2;
    if (diff < 0xf) diff = 0xf;
    *difficulty = (int16_t)diff;
    r = check_skill_roll(m, skill, dur);
    if (r == 0) return 1;
    if (r == 1) si = skill / 5 + 3;
    else if (r == 2) si = 0x40;
    else {
        if ((rt_rand(m) & 0x3f) > q + skill) return -2;
        si = -((rt_rand(m) & 7) + 4);
    }
    if (q + si > 0x3f) {
        ls[(uint16_t)(obj + 4)] = (uint8_t)((ls[(uint16_t)(obj + 4)] & 0xc0) | 0x3f);
        return 3;
    }
    if (q + si > 0) {
        ls[(uint16_t)(obj + 4)] = (uint8_t)((ls[(uint16_t)(obj + 4)] & 0xc0) | ((q + si) & 0x3f));
        return si > 0 ? 2 : -1;
    }
    ls[(uint16_t)(obj + 4)] &= 0xc0;
    return -2;
}

/* item_repair(obj, skill, verbose), from the instructions; the
 * anvil calls it verbose. The name (format_object_name, no article); a
 * thing with no durability is "You cannot repair that." (0x8e). The
 * estimate (durability - skill) + 0xf: under 0 trivial, else up to 0x1e
 * (that / 10 + 1) simple, possible, hard, else very difficult -- "You think
 * it will be ", the word (0xdb on), " to repair the ", the name; then
 * scroll_ask_yes_no(0, 0xda, yes): message 0xda (empty), the echo x and
 * "Yes", the cursor hidden, and the keys until Enter, Escape or a button
 * -- the host's (yesno_ask, uw_motion_yesno_answer). */
void item_repair(uw_motion *m, uint16_t obj, int skill) {
    int dur = get_item_durability(m, obj), est;
    char name[0x50];
    uint8_t *ls = m->lseg;
    if (!format_object_name(m, name, sizeof name, rw(ls, obj), ls[(uint16_t)(obj + 0x1a)], 0, 0)) name[0] = 0;
    if (dur < 0) { print_message(m, 0x8e); return; }
    est = dur - skill + 0xf;
    if (est < 0) est = 0;
    else if (est <= 0x1e) est = est / 10 + 1;
    else est = 4;
    print_message(m, 0xd8);
    print_message(m, (uint16_t)(0xdb + est));
    print_message(m, 0xd9);
    scroll_print(m, name);
    m->prompt_obj = obj;
    m->prompt_skill = (int16_t)skill;
    scroll_ask_yes_no(m, 0xda, 1, 1);
}

/* scroll_ask_yes_no(0, message, &answer), as a prompt the host
 * answers: the scroll selected with its text colour, the message, the echo
 * x taken, and "Yes" or "No" by the answer given; the cursor hidden and the
 * keys and buttons the host's (yesno_ask = `whose`, uw_motion_yesno_answer
 * with the answer). */
void scroll_ask_yes_no(uw_motion *m, uint16_t message, int initial, uint8_t whose) {
    char text[0x10];
    print_message(m, message);
    if (!m->scroll) { UW_NOT_CARRIED(m->not_carried); return; }
    uw_scroll_field_begin(m->scroll);
    uw_scroll_print(m->scroll, ds_text(m, initial ? 0x0aa2 : 0x0aa6, text, sizeof text));
    m->yesno_ask = whose;
    m->yesno_value = (uint8_t)(initial != 0);
}

void uw_motion_yesno_redraw(uw_motion *m, int yes) {
    char text[0x10];
    m->yesno_value = (uint8_t)(yes != 0);
    if (m->scroll) uw_scroll_echo_text(m->scroll, ds_text(m, yes ? 0x0aa2 : 0x0aa6, text, sizeof text));
}

/* The question answered (the host has redrawn Yes or No as the original's
 * scroll_redraw_yes_no did), by whose it was. item_repair's: "\n",
 * and a no ends it; a yes is cutscene_play(0x104) -- the
 * hammering, in the view -- which the port plays after the pass, the apply
 * waiting on its end (cutscene_then 1, uw_motion_cutscene_finished).
 * action_look's: "\n", disarm_trap with the Traps skill on a yes,
 * and the rest of the look (action_look_rest). */
void uw_motion_yesno_answer(uw_motion *m, int yes) {
    char text[0x10];
    uint8_t whose = m->yesno_ask;
    m->yesno_ask = 0;
    if (whose == 2) {
        scroll_print(m, ds_text(m, 0x02a3, text, sizeof text));
        if (yes) disarm_trap(m, m->prompt_obj, (uint8_t)m->prompt_skill);
        action_look_rest(m, m->prompt_obj);
        return;
    }
    scroll_print(m, ds_text(m, 0x12e9, text, sizeof text));
    if (!yes) return;
    uw_motion_cutscene_request(m, 0x104);
    m->cutscene_then = 1;
}

/* item_repair's verbose tail after the cutscene: item_repair_apply; the
 * critter row's +0x1d low nibble set to 0xf; the record's clock at +0xce
 * moved on by difficulty * 256 * 60 (the minutes it took); a destroyed
 * thing that object_cull_test(10) lets go is taken out of the pack
 * (inventory_remove_one) and removed, one it keeps is no effect (0); the
 * message 0x8e + result -- destroyed, damaged, cannot, no effect,
 * partially, fully -- with the name and "." (0x53) for any but 0;
 * player_state_recalc and post_event(0x200). */
static void item_repair_finish(uw_motion *m, uint16_t obj, int skill) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), row = rw(ds, CRITTER_ROW_PTR);
    int16_t diff = 0;
    int r = item_repair_apply(m, obj, skill, &diff);
    char name[0x50];
    uint32_t t;
    if (!format_object_name(m, name, sizeof name, rw(ls, obj), ls[(uint16_t)(obj + 0x1a)], 0, 0)) name[0] = 0;
    ds[(uint16_t)(row + 0x1d)] = (uint8_t)((ds[(uint16_t)(row + 0x1d)] & 0xf0) | 0xf);
    t = (uint32_t)rw(ds, (uint16_t)(rec + 0xce)) | ((uint32_t)rw(ds, (uint16_t)(rec + 0xd0)) << 16);
    t += (uint32_t)diff * 256u * 60u;
    ww(ds, (uint16_t)(rec + 0xce), (uint16_t)t);
    ww(ds, (uint16_t)(rec + 0xd0), (uint16_t)(t >> 16));
    if (r == -2) {
        if (object_cull_test(m, 10, obj)) {
            inventory_remove_quantity(m, obj, 1);       /* inventory_remove_one */
            object_remove(m, 0, obj, 1);
        } else {
            r = 0;
        }
    }
    print_message(m, (uint16_t)(0x8e + r));
    if (r) {
        scroll_print(m, name);
        print_message(m, 0x53);
    }
    player_state_recalc(m);
    ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 0x200));    /* post_event(0x200) */
}

/* The cutscene a handler asked for has ended: what it left to run after. */
void uw_motion_cutscene_finished(uw_motion *m) {
    int then = m->cutscene_then;
    m->cutscene_then = 0;
    if (then == 1) item_repair_finish(m, m->prompt_obj, m->prompt_skill);
}

/* use_anvil(obj, how, in_inventory), the anvil's pending handler. It
 * does nothing unless both flags are set, and the view passes
 * in_inventory 0, so the anvil repairs only what is clicked in the
 * pack; the view's caller then drops the cursor itself. Otherwise the
 * cursor's shape popped, the hand emptied and action_state 0, then
 * item_repair(obj, the record's Repair skill (+0x2f), verbose). */
void use_anvil(uw_motion *m, uint16_t obj, int in_inventory) {
    uint8_t *ds = m->ds;
    if (!in_inventory) return;
    cursor_shape_pop(m, 3);
    ww(ds, CURSOR_OBJECT, 0);
    ww(ds, (uint16_t)(CURSOR_OBJECT + 2), 0);
    ww(ds, ACTION_STATE_WORD, 0);
    item_repair(m, obj, ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x2f)]);
}

/* ---- the instruments ------------------------------------------------- */

/* play_instrument(which: 0 the mandolin, 1 the flute). The timbre is
 * timbres[which], the ten pitches of keys 1..9, 0 are a table of their
 * own; the last sixteen notes go round a ring. "You play
 * the instrument.  (Use 0-9 to play, or ESC to return to game)" (0xfa);
 * with a sound driver (its handle not -1) and the program's timbre in its
 * cache (ail_timbre_installed, else snd_install_timbre) a channel is locked
 * (ail_lock_channel, its bit set in snd_channel_alloc_mask, or channel 2
 * outright when the driver's flag is 1) and set up with six channel voice
 * messages -- controller 114 off, the program, 121, volume and expression
 * 0x7f, the pan centred. Then the loop the host runs: a digit key sounds its
 * pitch, Alt an octave up and Ctrl one down (uw_motion_instrument_key), a
 * note ends 0x40 ticks after it began (uw_motion_instrument_tick), Escape
 * ends the playing (uw_motion_instrument_end). */
void play_instrument(uw_motion *m, int which) {
    uint8_t *ds = m->ds;
    uint8_t program = ds[(uint16_t)(0x0164 + which)];
    m->instrument = (uint8_t)(which + 1);
    m->instrument_usable = snd_timbre_ready(m, 0, program) && rw(ds, 0x013c) != 0xffff;
    m->instrument_channel = 0;
    m->instrument_sounding = 0xff;
    m->instrument_playing = 0;
    m->instrument_started = 0;
    memset(m->instrument_ring, 0, sizeof m->instrument_ring);
    m->instrument_ring_at = 0;
    print_message(m, 0xfa);
    if (m->instrument_usable) {
        if (ds[0x013a] == 1) {
            m->instrument_channel = 2;
        } else {
            uint8_t ch = (uint8_t)uw_ail_lock_channel(m->ail);
            m->instrument_channel = ch;
            ww(ds, 0x014c, (uint16_t)(rw(ds, 0x014c) | (1u << (ch & 0x1f))));   /* snd_channel_alloc_mask */
            uw_ail_send_voice(m->ail, (uint8_t)(ch + 0xaf), 0x72, 0);
            uw_ail_send_voice(m->ail, (uint8_t)(ch + 0xbf), program, 0);
            uw_ail_send_voice(m->ail, (uint8_t)(ch + 0xaf), 0x79, 0);
            uw_ail_send_voice(m->ail, (uint8_t)(ch + 0xaf), 7, 0x7f);
            uw_ail_send_voice(m->ail, (uint8_t)(ch + 0xaf), 0xb, 0x7f);
            uw_ail_send_voice(m->ail, (uint8_t)(ch + 0xaf), 10, 0x40);
        }
    }
}

/* A key in the loop: Escape ends it; a digit (the code's low byte, Alt and
 * Ctrl bits aside) 1..9, 0 picks pitches 0..9, Alt adds twelve and Ctrl
 * takes twelve; a note still sounding is ended (a note off on the
 * channel), the note goes on the ring and begins (a note on, velocity
 * 0x7f), and the clock is stamped. */
void uw_motion_instrument_key(uw_motion *m, uint16_t code) {
    uint8_t *ds = m->ds;
    int n, note;
    if (!m->instrument) return;
    if (code == 0x1b) { uw_motion_instrument_end(m); return; }
    n = code & 0xfcff;
    if (n < '0' || n > '9') return;
    n -= '0';
    if (n == 0) n = 10;
    n--;
    note = ds[(uint16_t)(0x0166 + n)];
    if (code & 0x200) note += 12;
    if (code & 0x100) note -= 12;
    if (m->instrument_playing && m->instrument_sounding != 0xff && m->instrument_usable && m->ail)
        uw_ail_send_voice(m->ail, (uint8_t)(m->instrument_channel + 0x7f), m->instrument_sounding, 0);
    m->instrument_ring[m->instrument_ring_at] = (uint8_t)note;
    m->instrument_ring_at = (uint8_t)((m->instrument_ring_at + 1) & 0xf);
    if (m->instrument_usable && m->ail) uw_ail_send_voice(m->ail, (uint8_t)(m->instrument_channel + 0x8f), (uint8_t)note, 0x7f);
    m->instrument_sounding = (uint8_t)note;
    m->instrument_started = m->clock;
    m->instrument_playing = 1;
}

/* The loop's other half, each poll: a note that began more than 0x40
 * ticks ago is ended. */
void uw_motion_instrument_tick(uw_motion *m) {
    if (!m->instrument || !m->instrument_playing) return;
    if (m->clock - m->instrument_started > 0x40) {
        m->instrument_playing = 0;
        if (m->instrument_usable && m->ail)
            uw_ail_send_voice(m->ail, (uint8_t)(m->instrument_channel + 0x7f), m->instrument_sounding, 0);
        m->instrument_sounding = 0xff;
    }
}

/* The flute's tune (ring): nine notes against
 * the ring's first nine, unless the record's +0x60 bit 7 already says the
 * Cup of Wonder is found; a match spawns item 0xae in the hand (the Cup),
 * "An object appears in the air and falls into your hand..." (0x88) and
 * sets the bit. 1 when it did. */
static int spawn_cup_of_wonder(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    int i;
    if (ds[(uint16_t)(rec + 0x60)] & 0x80) return 0;
    for (i = 0; i < 9; i++)
        if (m->instrument_ring[i] != ds[(uint16_t)(0x0170 + i)]) return 0;
    if (!spawn_object_in_hand(m, 0, 0xae)) return 0;
    print_message(m, 0x88);
    ds[(uint16_t)(rec + 0x60)] |= 0x80;
    return 1;
}

/* Escape: the channel released (ail_release_channel); the flute on
 * level 3 within two tiles of (0x18, 0x2d) -- the Cup of Wonder's spot --
 * playing the tune ends silently, anything else with "You put the
 * instrument down." (0xfb). */
void uw_motion_instrument_end(uw_motion *m) {
    uint8_t *ds = m->ds;
    int which = m->instrument - 1;
    if (!m->instrument) return;
    m->instrument = 0;
    if (m->ail) uw_ail_release_channel(m->ail, m->instrument_channel);   /* its mask bit left set, as the code leaves it */
    if (which == 1 && rw(ds, CURRENT_LEVEL_WORD) == 3) {
        uint16_t w = rw(m->lseg, (uint16_t)(rw(ds, TRACKED_OBJECT) + 0x16));
        int x = w >> 10, y = (w & 0x3f0) >> 4;
        if (abs(x - 0x18) <= 2 && abs(y - 0x2d) <= 2 && spawn_cup_of_wonder(m)) return;
    }
    print_message(m, 0xfb);
}
