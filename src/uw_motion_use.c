/* SPDX-License-Identifier: MIT */
/* using a thing: use_object and the dispatch under it -- the special
 * items, readables, food and drink, light sources, keys and lockpicks,
 * the fishing pole and the silver seed.
 *
 * One of src/uw_motion*.c: see uw_motion.h and uw_motion_int.h. */
#include "uw_motion_int.h"

/* A C string of the data segment's. */
const char *ds_text(uw_motion *m, uint16_t at, char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap && m->ds[(uint16_t)(at + n)]) {
        buf[n] = (char)m->ds[(uint16_t)(at + n)];
        n++;
    }
    buf[n] = 0;
    return buf;
}

/* effect_nonlethal_damage(obj, dice), from the instructions: a
 * creature (class 1) above 3 hit points (+8) loses dice d8 of them, but
 * no lower than 3; the Avatar's loss is shown by screen_show_frame(0xa8),
 * which the host runs (uw_motion.screen_flash). */
void effect_nonlethal_damage(uw_motion *m, uint16_t obj, int16_t dice) {
    uint8_t *ls = m->lseg;
    int8_t d;
    if (((rw(ls, obj) & 0x1c0) >> 6) != 1) return;
    d = (int8_t)roll_dice(m, dice, 8);
    if (ls[(uint16_t)(obj + 8)] <= 3) return;
    if ((int16_t)ls[(uint16_t)(obj + 8)] - d <= 3) ls[(uint16_t)(obj + 8)] = 3;
    else ls[(uint16_t)(obj + 8)] = (uint8_t)(ls[(uint16_t)(obj + 8)] - d);
    if (obj == rw(m->ds, TRACKED_OBJECT)) {                   /* screen_show_frame(0xa8) */
        m->screen_flash = 1;
        m->screen_flash_colour = 0xa8;
    }
}

/* use_misc_dispatch(actor, obj, used), object_use_dispatch's class
 * 4 subclass 1: by item id through a table. The orb rock (0x112) used asks
 * what on (use_tyballs_orb); applied, it is the pending object and
 * use_tyballs_orb runs at once (counted). The book (0x114) used is
 * trap_exploding_book (counted). Burning incense (0x115) steps the record's
 * +0x61 vision counter (bits 0..1) while under 3 and plays cutscene 0xb +
 * (3 - the counter), or 0xb + rand() % 3 once it is 3 (the cutscene counted),
 * becomes debris (0xd5), and redraws the panel's container button (used) or
 * posts event 2. Rotworm stew (0x11b) is use_object's. The Gem Cutter, incense,
 * the orb, the broken blade and hilt and the figurine do nothing here. */
void use_misc_dispatch(uw_motion *m, uint16_t a, uint16_t b, int flag) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    switch (rw(ls, b) & 0x1ff) {
    case 0x112:
        if (flag) {
            use_object_with_prompt(m, b, 0x0a12);
        } else {
            ww(ds, 0x2688, b);
            ww(ds, 0x268a, rw(ds, (uint16_t)(MOBILE_BASE + 2)));
            use_tyballs_orb(m, a, 0);       /* (actor, 0): nothing but what the cursor held let go of */
        }
        break;
    case 0x114:
        if (flag) trap_exploding_book(m);
        break;
    case 0x115: {
        /* si = rt_rand() % 3; while the
         * counter is under 3 it is bumped and si = 3 - the bumped value;
         * then cutscene_play(si + 0xb) -- 13, 12, 11 for the first three
         * visions and a random one of the three after */
        uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
        int16_t which = (int16_t)((int16_t)rt_rand(m) % 3);
        if ((ds[(uint16_t)(rec + 0x61)] & 3) < 3) {
            uint8_t bumped = (uint8_t)(((ds[(uint16_t)(rec + 0x61)] & 3) + 1) & 3);
            ds[(uint16_t)(rec + 0x61)] = (uint8_t)((ds[(uint16_t)(rec + 0x61)] & 0xfc) | bumped);
            which = (int16_t)(3 - bumped);
        }
        uw_motion_cutscene_request(m, (uint16_t)(which + 0xb));
        ww(ls, b, (uint16_t)((rw(ls, b) & 0xfe00) | 0xd5));
        if (flag) inventory_panel_container_button(m);
        else ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 2));   /* post_event(2) */
        break;
    }
    case 0x11b:
        use_object(m, a, b, flag);
        break;
    default:
        break;
    }
}

/* use_object_dispatch_misc(obj, used), object_use_dispatch's class
 * 3 subclasses 0 and 1: a skull or bones (0xc2..0xc6) used asks what on;
 * the anvil (0xd7) asks (use_anvil); the pole (0xd8) sets
 * action_reach_scale to 1 and recalculates -- its reach doubled -- before
 * asking (use_pole); a dead rotworm and the two plants (0xd9, 0xce, 0xcf) used are use_object's. */
void use_object_dispatch_misc(uw_motion *m, uint16_t obj, int flag) {
    uint8_t *ds = m->ds;
    uint16_t id = (uint16_t)(rw(m->lseg, obj) & 0x1ff);
    if (id >= 0xc2 && id <= 0xc6) {
        if (flag) use_object_with_prompt(m, obj, 0x0719);
    } else if (id == 0xd7) {
        use_object_with_prompt(m, obj, 0x08cf);
    } else if (id == 0xd8) {
        ds[0x1b00] = 1;
        player_state_recalc(m);
        use_object_with_prompt(m, obj, 0x0882);
    } else if (flag && (id == 0xd9 || id == 0xce || id == 0xcf)) {
        use_object(m, rw(ds, TRACKED_OBJECT), obj, flag);
    }
}

/* trap_fishing, from the instructions: the point 0xb fine units
 * ahead of the Avatar (angle_to_offset from its position >> 5 and heading
 * >> 8) must be a tile not solid whose floor is water (floor_terrain, by the
 * tile's floor index, >> 4 is 1), with the Avatar's z >> 3 above its floor
 * height less one -- else "You cannot fish there." (0x65). Then one cast in
 * five catches something (rand() % 5 == 0; else "No luck this time.", 0x64):
 * a fish (item 0xb6) the carried weight has room for is "You catch a lovely
 * fish." (0x63) and 1; none, "You feel a nibble, but the fish gets away."
 * (0x66). The two words of its frame are the stack's, below any label. */
static int trap_fishing(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t bp = (uint16_t)(FRAME_BP - 0x80), rec = rw(ds, PLAYER_RECORD_PTR), tile;
    ww(ds, (uint16_t)(bp - 2), (uint16_t)(rs(ds, 0x2780) >> 5));
    ww(ds, (uint16_t)(bp - 4), (uint16_t)(rs(ds, 0x2782) >> 5));
    angle_to_offset(m, (uint16_t)(rs(ds, 0x727a) >> 8), 0xb, (uint16_t)(bp - 2), (uint16_t)(bp - 4));
    tile = tile_ptr(m, (uint16_t)(rs(ds, (uint16_t)(bp - 2)) >> 3), (uint16_t)(rs(ds, (uint16_t)(bp - 4)) >> 3));
    if (!(ls[tile] & 0xf) || (rw(ds, (uint16_t)(0x7192 + ((ls[(uint16_t)(tile + 1)] >> 2) & 0xf) * 2)) >> 4) != 1
        || (uint16_t)((rw(ls, (uint16_t)(rw(ds, TRACKED_OBJECT) + 2)) & 0x7f) >> 3) <= (uint16_t)(((ls[tile] >> 4) & 0xf) - 1)) {
        print_message(m, 0x65);
        return 0;
    }
    if ((int16_t)rt_rand(m) % 5) {
        print_message(m, 0x64);
        return 0;
    }
    if ((uint16_t)(rw(ds, (uint16_t)(rec + 0x4a)) + ((rw(ds, 0x6341) >> 4) & 0xfff)) >= rw(ds, (uint16_t)(rec + 0x4c))) {
        print_message(m, 0x66);
        return 0;
    }
    print_message(m, 0x63);
    return 1;
}

/* plant_silver_seed: -1 in the void
 * (level 9). Otherwise the point 0xb fine units ahead of the Avatar, as
 * trap_fishing finds it, must be open floor (tile type 1) whose floor
 * texture (the level's texture map by the tile's floor index) lies in
 * 5..11, 18..22, 27..31 or 35..40, with room for a silver tree (0x1ca,
 * item_fits_in_tile at the floor, height * 8, neither slope nor radius) --
 * else 0. The tree is object_create'd there (word 0 bit 13 set) and given a
 * level effect with no timer; the original tests only for 0, which the
 * effect list never answers (full is -1), so the tree goes into the tile
 * and the level's number into the record's +0x5e high nibble, where death
 * returns the Avatar: 1. The locals are the stack's: the point at bp-2 and
 * bp-4, the tile's far pointer at bp-0xc, the tree's at bp-8. */
static int plant_silver_seed(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t bp = (uint16_t)(FRAME_BP - 0x80), tile, o, z, seg = rw(ds, 0x3122);
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    int16_t x, y, tex;
    uw_objpool pool;
    if (rw(ds, CURRENT_LEVEL_WORD) == 9) return -1;
    ww(ds, (uint16_t)(bp - 2), (uint16_t)(rs(ds, 0x2780) >> 5));
    ww(ds, (uint16_t)(bp - 4), (uint16_t)(rs(ds, 0x2782) >> 5));
    angle_to_offset(m, (uint16_t)(rs(ds, 0x727a) >> 8), 0xb, (uint16_t)(bp - 2), (uint16_t)(bp - 4));
    x = rs(ds, (uint16_t)(bp - 2));
    y = rs(ds, (uint16_t)(bp - 4));
    tile = tile_ptr(m, (uint16_t)(x >> 3), (uint16_t)(y >> 3));
    if (!tile) {
        UW_NOT_CARRIED(m->not_carried);
        return 0;
    }
    ww(ds, (uint16_t)(bp - 0xc), tile);
    ww(ds, (uint16_t)(bp - 0xa), seg);
    if ((ls[tile] & 0xf) != 1) return 0;
    tex = rs(ds, (uint16_t)(0x717c + ((ls[(uint16_t)(tile + 1)] >> 2) & 0xf) * 2));
    if (!((tex >= 5 && tex <= 0xb) || (tex >= 0x12 && tex <= 0x16) || (tex >= 0x1b && tex <= 0x1f)
          || (tex >= 0x23 && tex <= 0x28)))
        return 0;
    z = (uint16_t)(((ls[tile] >> 4) & 0xf) << 3);
    if (!item_fits_in_tile(m, 0x1ca, 0, x, y, (int16_t)z, 0, 0, (uint16_t)(bp - 0xc - 4 - 0xe - 4 - 2)))
        return 0;
    o = create_object(m, 0x1ca, 0);
    if (!o) {
        UW_NOT_CARRIED(m->not_carried);
        return 0;
    }
    ww(ds, (uint16_t)(bp - 8), o);
    ww(ds, (uint16_t)(bp - 6), seg);
    ww(ls, (uint16_t)(o + 2), (uint16_t)((rw(ls, (uint16_t)(o + 2)) & 0xff80) | (z & 0x7f)));
    ww(ls, (uint16_t)(o + 2), (uint16_t)((rw(ls, (uint16_t)(o + 2)) & 0x1fff) | ((x & 7) << 13)));
    ww(ls, (uint16_t)(o + 2), (uint16_t)((rw(ls, (uint16_t)(o + 2)) & 0xe3ff) | ((y & 7) << 10)));
    ww(ls, o, (uint16_t)((rw(ls, o) & 0xdfff) | 0x2000));
    if (!level_effect_add(m, obj_index_of(m, o), -1, 0, (uint8_t)(x >> 3), (uint8_t)(y >> 3))) {
        pool_from_ds(m, &pool);
        uw_obj_free(&pool, o);
        pool_to_ds(m, &pool);
        return 0;
    }
    ds[(uint16_t)(rec + 0x5e)] = (uint8_t)((ds[(uint16_t)(rec + 0x5e)] & 0xf) | ((ds[CURRENT_LEVEL_WORD] & 0xf) << 4));
    pool_from_ds(m, &pool);
    uw_object_list_insert(&pool, (uint16_t)(tile + 2), o);
    pool_to_ds(m, &pool);
    return 1;
}

/* spawn_object_in_hand(obj, id), from the instructions: nothing
 * while something is held (0); the object's own id, or a new object of `id`
 * (object_create), becomes the held thing -- action_state 1, its image the
 * cursor's shape. */
uint16_t spawn_object_in_hand(uw_motion *m, uint16_t obj, uint16_t id) {
    uint8_t *ds = m->ds;
    if (rw(ds, 0x5b06) || rw(ds, 0x5b08)) return 0;
    if (obj) id = (uint16_t)(rw(m->lseg, obj) & 0x1ff);
    else obj = create_object(m, id, 0);
    ww(ds, ACTION_STATE_WORD, 1);
    ww(ds, 0x5b06, obj);
    ww(ds, 0x5b08, obj ? rw(ds, (uint16_t)(MOBILE_BASE + 2)) : 0);
    cursor_shape_push(m, id);
    return obj;
}

/* use_special_item(actor, obj, used): by
 * item id through a table when used -- the bedroll's sleep
 * (0x121, with the event's +8 at 1) and the mandolin and flute (0x123,
 * 0x124, play_instrument) counted; the silver seed (0x122) is
 * plant_silver_seed -- planted (message 12) or vanished in the void (11),
 * both using it up (object_clear, forced), or inert (10); the fishing pole
 * (0x12b) casts
 * (trap_fishing) and a catch is a fish (0xb6) of quality 0x3f in the hand
 * (spawn_object_in_hand) and the release wait; leeches
 * (0x125) clear the Avatar's poison (record +0x5f bits 2..5), deal it 2d8
 * nonlethal damage (effect_nonlethal_damage), are used up (object_clear,
 * forced) and say message 0xe0; the spike (0x127, use_spike_with_prompt),
 * the rock hammer (0x128) and the oil flask (0x12d) ask what to use them on
 * (use_object_with_prompt). Applied rather
 * than used: the cauldron (0x12f) is message 0x112; the glowing rock
 * (0x129, by the Avatar) merges with the glowing rock on the cursor, or
 * one found by inventory_find_by_kind (class 4 subclass 2 type 9, in the
 * slots and then anywhere -- none, and nothing happens), adding the quantities (word 3 bits 6..15), the slot redrawn
 * (inventory_click_slot_index) or the container view refreshed, and is used
 * up (object_clear, forced); the fountain (0x12e) is item_enchantment's
 * effect and magnitude through trap_fire at the target tile, and message
 * 0xf9 (the waters renew your strength) for an effect of 4, else 0xed
 * (the water refreshes you). The frame under the fountain's trap_fire is
 * the port's. */
void use_special_item(uw_motion *m, uint16_t a, uint16_t b, int flag) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t id = (uint16_t)(rw(ls, b) & 0x1ff);
    if (!flag) {
        if (id == 0x12f) print_message(m, 0x112);
        else if (id == 0x129 && a == rw(ds, TRACKED_OBJECT)) {
            uint16_t target = rw(ds, CURSOR_OBJECT);
            int16_t slot = -1;
            int where = 0;
            if (!target || (rw(ls, target) & 0x1ff) != 0x129) {
                target = inventory_find_by_kind(m, 4, 2, 9, 2, &slot);
                where = 1;
                /* the same rock (4, 2, 9) again, now anywhere: the last
                 * push is the search's depth, 2 the slots and 4 the
                 * containers too */
                if (!target) { target = inventory_find_by_kind(m, 4, 2, 9, 4, &slot); where = 2; }
            }
            /* no rock to merge with: nothing at all */
            if (!target) return;
            {
                uint16_t q = (uint16_t)((rw(ls, (uint16_t)(target + 6)) >> 6) + (rw(ls, (uint16_t)(b + 6)) >> 6));
                ww(ls, (uint16_t)(target + 6), (uint16_t)((rw(ls, (uint16_t)(target + 6)) & 0x3f) | (q << 6)));
                if (where == 1) inventory_click_slot_index(m, slot);
                else if (where == 2) container_view_refresh(m);
            }
            object_clear(m, b, flag, 1);
        } else if (id == 0x12e) {
            int16_t effect, magnitude;
            int special;
            if (item_enchantment(m, b, &effect, &magnitude, &special)) {
                trap_fire(m, (uint8_t)rw(ds, ACTION_TARGET_TILE_X), (uint8_t)rw(ds, ACTION_TARGET_TILE_Y),
                          b, a, effect, (uint8_t)magnitude, (uint16_t)(FRAME_BP - 0x80));
                print_message(m, effect == 4 ? 0xf9 : 0xed);   /* the effect: cmp [bp-8],4 */
            } else {
                print_message(m, 0xed);
            }
        }
        return;
    }
    switch (id) {
    case 0x121:
        /* the bedroll, used in the dungeon: player_sleep(1). The frame is
         * the port's. */
        if (rw(ds, (uint16_t)(rw(ds, 0x00e2) + 8)) == 1)
            player_sleep(m, 1, (uint16_t)(FRAME_BP - 0x80));
        break;
    case 0x122: {
        int r = plant_silver_seed(m);
        if (r == 0) print_message(m, 10);
        else {
            object_clear(m, b, flag, 1);
            print_message(m, r == 1 ? 12 : 11);
        }
        break;
    }
    case 0x123: case 0x124:
        play_instrument(m, id - 0x123);
        break;
    case 0x12b:
        if (trap_fishing(m)) {
            uint16_t fish = spawn_object_in_hand(m, 0, 0xb6);
            if (!fish) {
                UW_NOT_CARRIED(m->not_carried);
                break;
            }
            m->lseg[(uint16_t)(fish + 4)] = (uint8_t)((m->lseg[(uint16_t)(fish + 4)] & 0xc0) | 0x3f);
        }
        input_wait_button_release(m, 1);    /* after every cast, a catch or not */
        break;
    case 0x125:
        ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x5f)] &= 0xc3;
        effect_nonlethal_damage(m, rw(ds, TRACKED_OBJECT), 2);
        object_clear(m, b, flag, 1);
        print_message(m, 0xe0);
        break;
    case 0x127:
        use_object_with_prompt(m, b, 0x0662);
        break;
    case 0x128:
        use_object_with_prompt(m, b, 0x139e);
        break;
    case 0x12d:
        use_object_with_prompt(m, b, 0x1580);
        break;
    default:
        break;
    }
}

/* use_readable(obj, from_inventory), only from the inventory: the map (0x13b) opens
 * with the event's +8 at 1 (game_change_mode(2), which the shell's
 * src/uw_automap.c is); an enchanted
 * one (word 0 bit 12, not a door) casts and is used up (trigger_object_link,
 * cast_spell_from_object and object_clear, counted); word 0 bit 10 plays
 * cutscene 0x100 on (counted). Otherwise its text index (word 3 bits 6..14)
 * under 0x100 prints "You read the ", its name (format_object_name, no
 * article; "UNNAMED" without one) and "...\n" as one line, then the text --
 * string block 3 at the index -- and "\n"; 0x100 and above is a recipe
 * (cook_mix_ingredients, counted). */
/* cook_mix_ingredients: the recipe's
 * three ingredient ids (0xd9, 0xb8, 0xbe) and their counts, zero; the bowl is inventory_find_by_kind(2, 0, 0xe, 4) -- none is
 * message 0x96 -- and its contents are walked, each counted against the
 * three, anything else ending it with 0x94, which a missing ingredient is
 * too. All three there: the open container panel goes up a level when the
 * bowl is the container it shows (CONTAINER_STACK_TOP's +8, an index), the
 * contents are cleared (object_chain_clear), the bowl becomes item 0x11b,
 * inventory_panel_container_button, message 0x95. 1 when it mixed. */
static int cook_mix_ingredients(uw_motion *m) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t ids[3], bowl, o;
    int counts[3], i;
    int16_t slot;
    for (i = 0; i < 3; i++) { ids[i] = rw(ds, (uint16_t)(0x0e28 + 2 * i)); counts[i] = rw(ds, (uint16_t)(0x0e2e + 2 * i)); }
    bowl = inventory_find_by_kind(m, 2, 0, 0xe, 4, &slot);
    if (!bowl) { print_message(m, 0x96); return 0; }
    for (o = deref_link(m, (uint16_t)(bowl + 6)); o; o = deref_link(m, (uint16_t)(o + 4))) {
        int any = 0;
        for (i = 0; i < 3; i++)
            if (ids[i] == (rw(ls, o) & 0x1ff)) { counts[i]++; any = 1; }
        if (!any) { print_message(m, 0x94); return 0; }
    }
    if (!counts[0] || !counts[1] || !counts[2]) { print_message(m, 0x94); return 0; }
    /* the bowl open in the panel closed: the top node's +8 through its far
     * pointer; with none open the original compares a word of
     * the interrupt table, taken as no match */
    if (rw(ds, CONTAINER_STACK_TOP) | rw(ds, (uint16_t)(CONTAINER_STACK_TOP + 2))) {
        const uint8_t *top = far_bytes(m, rw(ds, CONTAINER_STACK_TOP), rw(ds, (uint16_t)(CONTAINER_STACK_TOP + 2)), 0xc);
        if (!top) UW_NOT_CARRIED(m->not_carried);
        else if (obj_index_of(m, bowl) == ((rw(top, 8) >> 6) & 0x3ff)) container_panel_up(m);
    }
    object_chain_clear(m, (uint16_t)(bowl + 6));
    ww(ls, bowl, (uint16_t)((rw(ls, bowl) & 0xfe00) | 0x11b));
    inventory_panel_container_button(m);
    print_message(m, 0x95);
    return 1;
}

void use_readable(uw_motion *m, uint16_t obj, int flag) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w0 = rw(ls, obj), idx = (uint16_t)((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x1ff);
    char line[100], part[0x20];
    size_t n;
    if (!flag) return;
    if ((w0 & 0x1ff) == 0x13b) {
        if (rw(ds, (uint16_t)(rw(ds, 0x00e2) + 8)) == 1) m->map_open = 1;   /* game_change_mode(2) */
        return;
    }
    if ((w0 & 0x1000) && (w0 & 0x1c0) != 0x140) {
        /* an enchanted readable: reading it casts it and uses it up --
         * trigger_object_link(player, obj, 4, the target tile) and
         * cast_spell_from_object(the target tile, player, obj, flag); the
         * frames are the port's */
        trigger_object_link_at(m, rw(ds, TRACKED_OBJECT), obj, 4, ds[ACTION_TARGET_TILE_X], ds[ACTION_TARGET_TILE_Y],
                               (uint16_t)(FRAME_BP - 0x80));
        cast_spell_from_object(m, ds[ACTION_TARGET_TILE_X], ds[ACTION_TARGET_TILE_Y], rw(ds, TRACKED_OBJECT), obj, flag,
                               (uint16_t)(FRAME_BP - 0x80));
        object_clear(m, obj, flag, 0);
        return;
    }
    if (w0 & 0x400) {
        uw_motion_cutscene_request(m, (uint16_t)(idx + 0x100));   /* cutscene_play(0x100 + the index) */
        return;
    }
    if (idx >= 0x100) {
        cook_mix_ingredients(m);
        return;
    }
    ds_text(m, 0x0a17, line, sizeof line);                          /* "You read the " */
    n = strlen(line);
    if (!format_object_name(m, line + n, sizeof line - n, w0, ls[(uint16_t)(obj + 0x1a)], 0, 0))
        strncat(line, ds_text(m, 0x09fe, part, sizeof part), sizeof line - strlen(line) - 1);   /* "UNNAMED" */
    strncat(line, ds_text(m, 0x0a25, part, sizeof part), sizeof line - strlen(line) - 1);       /* "...\n" */
    scroll_print(m, line);
    print_string(m, (uint16_t)(idx | 0x600));
    scroll_print(m, ds_text(m, 0x0a0f, part, sizeof part));                                    /* "\n" */
}

/* use_object(a, b, flag), from the instructions: `b` eaten or
 * drunk. Its count is word 3's bits 6.. for a quantity that is not a special
 * link, else 1. Held on the cursor, more than one is refused (message 0x77,
 * "You can only use those individually.", -2); not held, only a use from the
 * inventory (flag) goes on, else -2. Items 0xb0..0xbf take a signed
 * nutrition from food_props (by the low nibble), anything else
 * 0xff, and by item id a message: the candle (0x92) 0xe6 and leeches (0x125)
 * 0xe5, with no nutrition; the mushroom (0xb8) 0xe8 after a skill roll of
 * the critter row's +7 at 0x14 whose any answer but 0 restores
 * rand() * 3 / 0x8000 mana, and the record's +0x61 bits 2..3 up to 3 and
 * player_state_recalc; the toadstool (0xb9) 0xe7; water (0xbd) 0xed, port
 * (0xbe) 0xee, ale (0xba) 0xef and the two potions (0xbb, 0xbc) 0xf0; four
 * more with nutrition of their own -- 0xce 0xec at 4, 0xcf 0xe9 at 0x17,
 * 0xd9 0xea at 4, 0x11b 0xeb at 0x40; wine (0xbf) is "You are unable to
 * open the wine bottle." and 0; the rest of 0xb0..0xb7 has no message, and
 * anything else with no nutrition is -1. Nutrition above 0 goes to
 * change_hunger (not 0xff), which refusing prints 0x7e, "You are too full
 * to eat that now.", and 0; with no message of its own, "That " and the
 * name without its article ("UNNAMED") are printed and the message is " tasted
 * putrid." .. " tasted great." (0xac + (rand() * 20 / 0x8000 + quality) >> 4,
 * at most 4); a toadstool poisons (record +0x5f bits 2..5: under 4 set to 4,
 * under 13 up two). Nutrition 0 prints its message; below it, drink: the
 * message and, from -2 to -126, drunkenness (record +0x61 bits 4..9) up by
 * the strength, saturating at 0x3f, and a roll of the critter row's +5
 * against it -- -1 collapses asleep (0xf1, player_sleep(-2)) and, alive,
 * wakes (0xf3) with player_start_status_effect(0x40, drunk / 6 + 10); 0 the
 * same effect for drunk / 6; 2 is 0xf2 and player_restore_vitality(-2).
 * Then cast_spell_from_object, trigger_object_link and object_clear(b, flag,
 * forced), which gone with action_state 1 empties the hand. 1. */
int16_t use_object(uw_motion *m, uint16_t a, uint16_t b, int flag) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w0 = rw(ls, b), id = (uint16_t)(w0 & 0x1ff), rec = rw(ds, PLAYER_RECORD_PTR);
    uint16_t row = rw(ds, CRITTER_ROW_PTR), q = (uint16_t)((rw(ls, (uint16_t)(b + 6)) >> 6) & 0x3ff);
    int16_t count = ((w0 & 0x8000) && !(q & 0x200)) ? (int16_t)q : 1;
    int16_t si = 0, di = 0xff;
    (void)a;
    if (b == rw(ds, 0x5b06) && rw(ds, 0x5b08)) {
        if (count > 1) {
            print_message(m, 0x77);
            return -2;
        }
    } else if (!flag) {
        return -2;
    }
    if (((w0 & 0x1f0) >> 4) == 0xb) di = (int8_t)ds[(uint16_t)(0x5b5a + (w0 & 0xf))];
    switch (id) {
    case 0x92: si = 0xe6; break;
    case 0x125: si = 0xe5; break;
    case 0xb9: si = 0xe7; break;
    case 0xb8:
        if (check_skill_roll(m, ds[(uint16_t)(row + 7)], 0x14))
            player_restore_mana(m, rw(ds, TRACKED_OBJECT), (int8_t)-(int8_t)(((int32_t)rt_rand(m) * 3) / 0x8000));
        if (((ds[(uint16_t)(rec + 0x61)] >> 2) & 3) < 3)
            ds[(uint16_t)(rec + 0x61)] = (uint8_t)((ds[(uint16_t)(rec + 0x61)] & 0xf3)
                                                   | ((((ds[(uint16_t)(rec + 0x61)] >> 2) & 3) + 1) & 3) << 2);
        player_state_recalc(m);
        si = 0xe8;
        break;
    case 0xbd: si = 0xed; break;
    case 0xbe: si = 0xee; break;
    case 0xba: si = 0xef; break;
    case 0xbb: case 0xbc: si = 0xf0; break;
    case 0xce: si = 0xec; di = 4; break;
    case 0xcf: si = 0xe9; di = 0x17; break;
    case 0xd9: si = 0xea; di = 4; break;
    case 0x11b: si = 0xeb; di = 0x40; break;
    case 0xbf:
        print_message(m, 0x7f);
        return 0;
    default:
        if (di == 0xff) return -1;
    }
    if (di > 0) {
        if (di != 0xff && !(change_hunger(m, di) & 0xff)) {
            print_message(m, 0x7e);
            return 0;
        }
        if (!si) {
            char buf[0x4e], part[0x40];
            int16_t taste;
            ds_text(m, 0x0a11, buf, sizeof buf);                            /* "That " */
            if (!format_object_name(m, buf + strlen(buf), sizeof buf - strlen(buf), w0, ls[(uint16_t)(b + 0x1a)], 0, 0))
                append(buf, sizeof buf, ds_text(m, 0x09fe, part, sizeof part));   /* "UNNAMED" */
            taste = (int16_t)((uint16_t)((((int32_t)rt_rand(m) * 20) / 0x8000) + (ls[(uint16_t)(b + 4)] & 0x3f)) >> 4);
            if (taste > 4) taste = 4;
            scroll_print(m, buf);
            si = (int16_t)(taste + 0xac);
        }
        print_message(m, (uint16_t)si);
        if (id == 0xb9) {
            uint8_t p = (uint8_t)((ds[(uint16_t)(rec + 0x5f)] >> 2) & 0xf);
            if (p < 4) ds[(uint16_t)(rec + 0x5f)] = (uint8_t)((ds[(uint16_t)(rec + 0x5f)] & 0xc3) | 0x10);
            else if (p < 0xd) ds[(uint16_t)(rec + 0x5f)] = (uint8_t)((ds[(uint16_t)(rec + 0x5f)] & 0xc3) | ((p + 2) & 0xf) << 2);
        }
    } else {
        print_message(m, (uint16_t)si);
        if (di < -1 && di > -0x7f) {
            uint16_t drunk = (uint16_t)(((rw(ds, (uint16_t)(rec + 0x61)) >> 4) & 0x3f) - di);
            int roll;
            ww(ds, (uint16_t)(rec + 0x61), (uint16_t)((rw(ds, (uint16_t)(rec + 0x61)) & 0xfc0f)
                                                      | (drunk > 0x3f ? 0x3f0 : (drunk & 0x3f) << 4)));
            drunk = (uint16_t)((rw(ds, (uint16_t)(rec + 0x61)) >> 4) & 0x3f);
            roll = check_skill_roll(m, ds[(uint16_t)(row + 5)], drunk);
            if (roll == -1) {
                print_message(m, 0xf1);
                player_sleep(m, -2, (uint16_t)(FRAME_BP - 0x80));   /* the collapse */
                if (ls[(uint16_t)(rw(ds, TRACKED_OBJECT) + 8)]) {
                    print_message(m, 0xf3);
                    player_start_status_effect(m, 0x40, (uint8_t)(drunk / 6 + 10));
                }
            } else if (roll == 0) {
                player_start_status_effect(m, 0x40, (uint8_t)(drunk / 6));
            } else if (roll == 2) {
                print_message(m, 0xf2);
                player_restore_vitality(m, rw(ds, TRACKED_OBJECT), -2);
            }
        }
    }
    /* cast_spell_from_object(the target tile, who, obj, 1) -- the eater or
     * drinker casts what it carries -- on its frame: the same locals as the
     * trigger's below, five words pushed */
    cast_spell_from_object(m, ds[ACTION_TARGET_TILE_X], ds[ACTION_TARGET_TILE_Y], a, b, 1,
                           m->use_bp ? (uint16_t)(m->use_bp - 6 - 2 - 10 - 4 - 2 - 0x4e - 4 - 10 - 4 - 2)
                                     : (uint16_t)(FRAME_BP - 0x80));
    /* trigger_object_link(who, obj, 4, action_target_tile_x, _y) on its
     * frame: object_use_dispatch's six bytes of locals and SI, use_object's
     * five words of arguments, 0x4e of locals and SI, DI, and seven words
     * pushed (chain1: the pull chain's use trigger and its door trap) */
    trigger_object_link_at(m, a, b, 4, ds[ACTION_TARGET_TILE_X], ds[ACTION_TARGET_TILE_Y],
                           m->use_bp ? (uint16_t)(m->use_bp - 6 - 2 - 10 - 4 - 2 - 0x4e - 4 - 14 - 4 - 2)
                                     : (uint16_t)(FRAME_BP - 0x80));
    if (object_clear(m, b, flag, 1) && rw(ds, ACTION_STATE_WORD) == 1) {
        ww(ds, 0x5b06, 0);
        ww(ds, 0x5b08, 0);
    }
    return 1;
}

/* inventory_panel_activate(element): what
 * a click on a panel element does once paperdoll_click has found it no drag,
 * by a table over elements 8..0x18. The weapon hand (element
 * 9 less the record's +0x64 bit 0) holding a melee weapon (ids 0..0xf) or a
 * sling, bow, crossbow or jeweled bow readies or sheathes it (weapon_toggle);
 * the open container's icon closes it a level (container_panel_up) and the
 * arrows page it (0x15 forward, 0x16 back); the barter area (0x18, in a
 * conversation) is barter_click_player_slot_at_cursor's -- the player's
 * slot under the live cursor clicked, so a thing lifted from the paperdoll
 * and let go over the table is placed; the view (0x17) is
 * inventory_panel_activate_view's; anything else -- the other hand, the
 * slots, an element below 8 -- uses what the slot shows through
 * object_use_dispatch(player, it, 1). Something held on entry and not now
 * pops the cursor's shape with action_state 0. */
void inventory_panel_activate(uw_motion *m, int16_t element) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    int was = rw(ds, 0x5b06) || rw(ds, 0x5b08), use = 0;
    uint16_t o;
    if (element == 0x17) {
        inventory_panel_activate_view(m);
        return;
    }
    if (element == 8 || element == 9) {
        if (9 - (ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x64)] & 1) != element) {
            use = 1;
        } else {
            o = inventory_slot_object(m, (int8_t)ds[(uint16_t)(INVENTORY_SEARCH_ORDER + element)]);
            if (!o) UW_NOT_CARRIED(m->not_carried);
            else if (!((rw(ls, o) & 0x1f0) >> 4) || (rw(ls, o) & 0x1ff) == 0x18 || (rw(ls, o) & 0x1ff) == 0x19
                     || (rw(ls, o) & 0x1ff) == 0x1a || (rw(ls, o) & 0x1ff) == 0x1f)
                weapon_toggle(m);
            else use = 1;
        }
    } else if (element == 0x14) {
        container_panel_up(m);
    } else if (element == 0x15) {
        container_page_forward(m);
    } else if (element == 0x16) {
        container_page_back(m);
    } else if (element == 0x18) {
        barter_click_player_slot_at_cursor(m);   /* the trade table's own side */
    } else {
        use = 1;
    }
    if (use) {
        o = inventory_slot_object(m, (int8_t)ds[(uint16_t)(INVENTORY_SEARCH_ORDER + element)]);
        if (o) object_use_dispatch(m, rw(ds, TRACKED_OBJECT), o, 1);
    }
    if (was && !rw(ds, 0x5b06) && !rw(ds, 0x5b08)) {
        cursor_shape_pop(m, 3);
        ww(ds, ACTION_STATE_WORD, 0);
    }
}

/* use_object_with_prompt(obj, handler), from the instructions:
 * "Use ", the thing's name without its article ("UNNAMED") and " on
 * what?\n" printed, its image pushed as the cursor, and it held --
 * action_state 2 -- as pending_action_object with the handler
 * the next pick in the view runs (pending_action_handler: an offset in
 * the use routines' own code segment). */
void use_object_with_prompt(uw_motion *m, uint16_t obj, uint16_t handler) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t seg = rw(ds, (uint16_t)(MOBILE_BASE + 2));
    char buf[0x28], part[0x10];
    ds_text(m, 0x09f9, buf, sizeof buf);                                    /* "Use " */
    if (!format_object_name(m, buf + strlen(buf), sizeof buf - strlen(buf), rw(ls, obj), ls[(uint16_t)(obj + 0x1a)], 0, 0))
        append(buf, sizeof buf, ds_text(m, 0x09fe, part, sizeof part));       /* "UNNAMED" */
    append(buf, sizeof buf, ds_text(m, 0x0a06, part, sizeof part));           /* " on what?\n" */
    scroll_print(m, buf);
    cursor_shape_push(m, (uint16_t)(rw(ls, obj) & 0x1ff));
    ww(ds, 0x5b06, obj);
    ww(ds, 0x5b08, seg);
    ww(ds, ACTION_STATE_WORD, 2);
    ww(ds, 0x2688, obj);
    ww(ds, 0x268a, seg);
    ww(ds, 0x2684, handler);
    ww(ds, 0x2686, 0x3b62);
}

/* use_key_or_lockpick(obj, from_inventory), from the
 * instructions: only from the inventory; the lockpick (0x101) prompts for
 * use_picklock, a key (ids below 0x10f) for use_key,
 * anything else of the subclass nothing. */
void use_key_or_lockpick(uw_motion *m, uint16_t obj, int flag) {
    uint16_t id = (uint16_t)(rw(m->lseg, obj) & 0x1ff);
    if (!flag) return;
    if (id == 0x101) use_object_with_prompt(m, obj, 0x0498);
    else if (id < 0x10f) use_object_with_prompt(m, obj, 0x051d);
}

/* inventory_find_object(obj), from the instructions: the slot
 * holding it, walking the twenty in inventory_search_order's order, or the
 * negated slot of a container there holding it (object_find_link over its
 * contents, a quantity's count never walked, nor the container open in the
 * panel -- the top node's +8), or -1. With no container open the original
 * compares a slot's index with a word of the interrupt table; taken as no
 * match. */
int16_t inventory_find_object(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t idx = obj_index_of(m, obj);
    int16_t i;
    for (i = 0; i < 0x14; i++) {
        int16_t slot = (int8_t)ds[(uint16_t)(INVENTORY_SEARCH_ORDER + i)];
        uint16_t link = (uint16_t)(rw(ds, (uint16_t)(INVENTORY_SLOTS + slot * 2)) >> 6), o;
        if (!link) continue;
        if (link == idx) return slot;
        o = inventory_slot_object(m, slot);
        if (!o || (rw(ls, o) & 0x8000)) continue;
        if ((rw(ds, CONTAINER_STACK_TOP) | rw(ds, (uint16_t)(CONTAINER_STACK_TOP + 2)))) {
            const uint8_t *top = far_bytes(m, rw(ds, CONTAINER_STACK_TOP), rw(ds, (uint16_t)(CONTAINER_STACK_TOP + 2)), 0xc);
            if (!top) {
                UW_NOT_CARRIED(m->not_carried);
                continue;
            }
            if (link == ((rw(top, 8) >> 6) & 0x3ff)) continue;
        }
        if (object_find_link(m, (uint16_t)(o + 6), idx)) return (int16_t)-slot;
    }
    return -1;
}

/* use_light_source(obj, from_inventory), from the instructions:
 * only from the inventory ("Lights may only be used if equipped.", 0x7b);
 * quality 0 is "That light is already used up." (0x7c). In one of the four
 * light slots as a single thing it is simply switched; an unlit
 * light (0x90..0x93) elsewhere first takes the first empty slot of 5..8 --
 * or the one it is already in -- "Your hands are full." (0xf6) with none,
 * taken out of where it was (inventory_remove_quantity of one), added there
 * and the container button redrawn. Switched: the low nibble up four (lit)
 * below 4, down four otherwise; player_state_recalc, and the slot's element
 * redrawn. */
void use_light_source(uw_motion *m, uint16_t obj, int flag) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w0 = rw(ls, obj), id = (uint16_t)(w0 & 0x1ff), q = (uint16_t)((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x3ff);
    int16_t count = ((w0 & 0x8000) && !(q & 0x200)) ? (int16_t)q : 1, slot, i, free = 0;
    if (!flag) {
        print_message(m, 0x7b);
        return;
    }
    if (!(ls[(uint16_t)(obj + 4)] & 0x3f)) {
        print_message(m, 0x7c);
        return;
    }
    slot = inventory_find_object(m, obj);
    for (i = 0; i < 4; i++)
        if ((int8_t)ds[(uint16_t)(0x171e + i)] == slot && count == 1) break;
    if (i == 4 && id >= 0x90 && id <= 0x93) {
        for (i = 5; i <= 8; i++) {
            uint16_t o = inventory_slot_object(m, i);
            if (!o) {
                if (!free) free = i;
            } else if (o == obj && count == 1) {
                free = i;
                break;
            }
        }
        if (!free) {
            print_message(m, 0xf6);
            return;
        }
        inventory_remove_quantity(m, obj, 1);
        inventory_add_object(m, obj, free);
        inventory_panel_container_button(m);
        slot = free;
    }
    w0 = rw(ls, obj);
    ww(ls, obj, (uint16_t)((w0 & 0xfff0) | (((w0 & 0xf) < 4 ? (w0 & 0xf) + 4 : (w0 & 0xf) - 4) & 0xf)));
    player_state_recalc(m);
    if (slot < 0) UW_NOT_CARRIED(m->not_carried);
    else inventory_slot_click(m, (int8_t)ds[(uint16_t)(INVENTORY_CLICK_ORDER + slot)]);
}
