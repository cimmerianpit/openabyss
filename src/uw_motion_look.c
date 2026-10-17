/* SPDX-License-Identifier: MIT */
/* looking: look_at and what it says of a texture, an object, a creature,
 * a key, remains or magic equipment; reading; action_look and the
 * search for a trap.
 *
 * One of src/uw_motion*.c: see uw_motion.h and uw_motion_int.h. */
#include "uw_motion_int.h"

/* look_at_texture(kind, surface): for kind 2 and a surface, "You
 * see ", the name in string block 10 (| 0x1400 is block 10's id) -- a wall's by
 * wall_texture_assign, a floor's 0x1fe less its texture word, anything else
 * 0x1ff -- and the sentence's ending; otherwise message kind + 0x98. */
/* The scenery's text: a prefix from string block 8 (none when `prefix` is
 * 0), the epitaph or the writing (block 8 string n through
 * format_article_plural(s, 1, 0)) and a newline, built as one line. */
static void scenery_text(uw_motion *m, uint16_t prefix, uint16_t n) {
    char line[0x140], text[0x100];
    if (!m->strings) { UW_NOT_CARRIED(m->not_carried); return; }
    line[0] = 0;
    if (prefix && uw_strings_by_id(m->strings, (uint16_t)(prefix | 0x1000), text, (int)sizeof text) >= 0)
        strcpy(line, text);
    if (uw_strings_by_id(m->strings, (uint16_t)(n | 0x1000), text, (int)sizeof text - 2) >= 0)
        strncat(line, format_article_plural(text, 1, 0), sizeof line - strlen(line) - 2);
    strcat(line, "\n");
    scroll_print(m, line);
}

/* look_at_scenery(obj, mode): a class-5 object 0x164..0x16f
 * described, with `mode` the look's lore or -1 from a use. 0x164, a bridge:
 * word 0 bits 9..12 of 2 or more look_at_texture(2, bits + 0x2f), else
 * message 0xab. 0x165, a gravestone: the epitaph is block 8 string N --
 * word 3 bits 6..14 when word 0 has bit 15, else word 3's low six bits
 * -- GRAVE.DAT's byte N is a cutscene number, and nothing at
 * all is printed when the file will not open; with the byte non-zero and
 * the epitaph there the text window is cleared first
 * (text_window_clear(1)) and cutscene 0x101 patched with it plays after,
 * and the prefix, block 8's 0x160 + bits 9..12, is printed only when the
 * byte is 0. 0x166, writing: the same text, always the prefix
 * 0x170 + bits 9..12, no file. 0x16e and
 * 0x16f, special tmap objects: mode 0 or more look_at_texture(2, (word 3 &
 * 0x3f) + 1), and a mode above 0 whose texture's word reads 9
 * in its low byte plays cutscene 0x100 patched with the level. The rest of
 * the group does nothing. */
static uint16_t scenery_index(uint16_t w0, uint16_t w3) {
    return (uint16_t)((w0 & 0x8000) ? (w3 >> 6) & 0x1ff : w3 & 0x3f);
}

void look_at_scenery(uw_motion *m, uint16_t obj, int16_t mode) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w0 = rw(ls, obj), w3 = rw(ls, (uint16_t)(obj + 6)), bits = (uint16_t)((w0 >> 9) & 0xf);
    switch (w0 & 0x1ff) {
    case 0x164:
        if (bits >= 2) look_at_texture(m, 2, (int16_t)(bits + 0x2f));
        else print_message(m, 0xab);
        break;
    case 0x165: {
        uint16_t n = scenery_index(w0, w3);
        uint8_t cut;
        char probe[4];
        if (!m->grave) { UW_NOT_CARRIED(m->not_carried); return; }
        if (n >= m->grave_size) return;                     /* the read fails: nothing printed */
        cut = m->grave[n];
        if (cut && m->strings && uw_strings_by_id(m->strings, (uint16_t)(n | 0x1000), probe, (int)sizeof probe) >= 0) {
            if (m->scroll) uw_scroll_clear(m->scroll, 1);
            else UW_NOT_CARRIED(m->not_carried);
        }
        scenery_text(m, cut ? 0 : (uint16_t)(0x160 + bits), n);
        if (cut) uw_motion_cutscene_request_numbered(m, 0x101, cut);  /* cutscene_open_numbered */
        break;
    }
    case 0x166:
        scenery_text(m, (uint16_t)(0x170 + bits), scenery_index(w0, w3));
        break;
    case 0x16e: case 0x16f:
        if (mode >= 0) look_at_texture(m, 2, (int16_t)((w3 & 0x3f) + 1));
        if (mode > 0 && (rw(ds, (uint16_t)(0x720c + 2 * (w3 & 0x3f))) & 0xff) == 9)
            uw_motion_cutscene_request_numbered(m, 0x100, rw(ds, CURRENT_LEVEL_WORD));   /* cutscene_open_for_level */
        break;
    default:
        break;
    }
}

void look_at_texture(uw_motion *m, uint8_t kind, int16_t surface) {
    uint8_t *ds = m->ds;
    uint16_t id;
    char text[0x40];
    if (surface <= 0 || kind != 2) {
        print_message(m, (uint16_t)(kind + 0x98));
        return;
    }
    surface--;
    if (surface < 0x30) id = rw(ds, (uint16_t)(0x71aa + surface * 2));
    else if (surface < 0x3a) id = (uint16_t)(0x1fe - rw(ds, (uint16_t)(0x711c + surface * 2)));
    else id = 0x1ff;
    scroll_print(m, ds_text(m, 0x2b1, text, sizeof text));
    print_string(m, (uint16_t)(id | 0x1400));
    scroll_print(m, ds_text(m, 0x2a5, text, sizeof text));
}

/* action_in_reach(max_dist2, obj, tile): the tile's x and y into
 * action_target_tile_x/_y; with a distance, 0 when the object is further
 * than it in eighths of a tile, squared, or its z more than (scale + 1) * 12
 * below the Avatar's or 24 per step above. */
int action_in_reach(uw_motion *m, int16_t max_dist2, uint16_t obj, uint16_t tile) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t av = rw(ds, TRACKED_OBJECT);
    int32_t index = ((int32_t)tile - rw(ds, TILEMAP_ORIGIN)) / 4;
    int16_t dx, dy, dz, lim;
    ww(ds, ACTION_TARGET_TILE_X, (uint16_t)(index & 0x3f));
    ww(ds, ACTION_TARGET_TILE_Y, (uint16_t)((int16_t)(uint16_t)index >> 6));
    if (!max_dist2) return 1;
    dx = (int16_t)((rw(ds, ACTION_TARGET_TILE_X) << 3) + (rw(ls, (uint16_t)(obj + 2)) >> 13)
                   - (((rw(ls, (uint16_t)(av + 0x16)) >> 10) << 3) + (rw(ls, (uint16_t)(av + 2)) >> 13)));
    dy = (int16_t)((rw(ds, ACTION_TARGET_TILE_Y) << 3) + ((rw(ls, (uint16_t)(obj + 2)) & 0x1c00) >> 10)
                   - ((((rw(ls, (uint16_t)(av + 0x16)) & 0x3f0) >> 4) << 3) + ((rw(ls, (uint16_t)(av + 2)) & 0x1c00) >> 10)));
    if (dx < 0) dx = (int16_t)-dx;
    if (dy < 0) dy = (int16_t)-dy;
    if ((int16_t)(uint16_t)(dx * dx + dy * dy) > max_dist2) return 0;
    dz = (int16_t)((rw(ls, (uint16_t)(av + 2)) & 0x7f) - (rw(ls, (uint16_t)(obj + 2)) & 0x7f));
    lim = (int16_t)(((int8_t)ds[ACTION_REACH_SCALE] + 1) * 12);
    if (lim < dz) return 0;
    lim = (int16_t)(((int8_t)ds[ACTION_REACH_SCALE] + 1) * -24);
    if (lim > dz) return 0;
    return 1;
}

/* strcat into a buffer of `cap` bytes, as the originals' strcat does into
 * their fixed locals. */
void append(char *buf, size_t cap, const char *text) {
    size_t at = strlen(buf), n = strlen(text);
    if (at + 1 >= cap) return;
    if (n > cap - at - 1) n = cap - at - 1;
    memcpy(buf + at, text, n);
    buf[at + n] = '\0';
}

/* look_at_magic_equipment(obj, lore, buf): for an enchanted
 * object, lore 2 appends "magical " and answers 1; lore 3
 * appends "cursed " for a plain effect 9 and still answers 0. */
static int look_at_magic_equipment(uw_motion *m, uint16_t obj, int16_t lore, char *buf, size_t cap) {
    int16_t effect, magnitude;
    int special;
    char text[0x40];
    if (!item_enchantment(m, obj, &effect, &magnitude, &special)) return 0;
    if (lore == 2) {
        append(buf, cap, ds_text(m, 0x1920, text, sizeof text));
        return 1;
    }
    if (lore == 3 && !special && effect == 9)
        append(buf, cap, ds_text(m, 0x1929, text, sizeof text));
    return 0;
}

/* magic_item_description(obj, lore, buf): at lore 3 only, with
 * enchantment_query_only set around the lookup so a spent spell does not
 * roll, " of " and the spell's name from block 6 -- the magnitude plus 0x1c0
 * for a plain effect 12 (0x10 more past subclass 1), plus 0x100 for a
 * special effect of none, else plus effect * 16; "UNNAMED" for an empty one
 * -- and for a non-quantity whose first linked class 4 subclass 2 object is
 * special, " with N full charge(s)", "no" for none. The curse (effect 9) has
 * no description. 1 when it wrote. */
static int magic_item_description(uw_motion *m, uint16_t obj, int16_t lore, char *buf, size_t cap) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    int16_t effect, magnitude, charges = -1;
    int special, ok;
    uint16_t w0 = rw(ls, obj), index;
    char text[0x60], word[0x20], digits[3];
    ds[0x09f8] = 1;                         /* enchantment_query_only */
    ok = item_enchantment(m, obj, &effect, &magnitude, &special);
    ds[0x09f8] = 0;
    if (!ok || lore != 3 || effect == 9) return 0;
    index = (uint16_t)magnitude;
    if (effect == 0xc) index = (uint16_t)(index + (((w0 & 0x30) >> 4) > 1 ? 0x10 : 0) + 0x1c0);
    else if (special && effect <= 0) index = (uint16_t)(index + 0x100);
    else index = (uint16_t)(index + (effect << 4));
    if (!m->strings || uw_strings_by_id(m->strings, (uint16_t)(index | 0xc00), text, (int)sizeof text) < 0
        || !text[0])
        ds_text(m, 0x1931, text, sizeof text);                   /* "UNNAMED" */
    append(buf, cap, ds_text(m, 0x1939, word, sizeof word));       /* " of " */
    append(buf, cap, text);
    if (w0 & 0x8000) return 1;
    {
        uint16_t link = (uint16_t)(obj + 6), spell = object_find_matching(m, &link, 0, 4, 2, 0);
        if (spell && (rw(ls, spell) & 0x800)) charges = (int16_t)(ls[(uint16_t)(spell + 4)] & 0x3f);
    }
    if (charges < 0) return 1;
    append(buf, cap, ds_text(m, 0x193e, text, sizeof text));       /* " with " */
    if (charges <= 0) {
        append(buf, cap, ds_text(m, 0x1945, text, sizeof text));   /* "no" */
    } else {
        digits[0] = (char)('0' + charges / 10);
        digits[1] = (char)('0' + charges % 10);
        digits[2] = '\0';
        append(buf, cap, charges < 10 ? digits + 1 : digits);
    }
    append(buf, cap, ds_text(m, 0x1948, text, sizeof text));       /* " full charge" */
    if (charges != 1) append(buf, cap, ds_text(m, 0x1955, text, sizeof text));
    return 1;
}

/* read_object(obj, lore), a writing looked at with lore 1 or
 * more: the map (item 0x13b) is message 0x97; a thing whose word 0 bit 12 is
 * set is not read at all unless it is a door; bit 10 plays a cutscene (not
 * carried); otherwise the text is block 3 at word 3's bits 6..14 -- below
 * 0x100 after a line of its own, "You read the ", the name
 * (format_object_name, "UNNAMED" when it has none) and "...\n" -- and a
 * closing newline. */
static void read_object(uw_motion *m, uint16_t obj, int16_t lore) {
    uint8_t *ls = m->lseg;
    uint16_t w0 = rw(ls, obj), text;
    char buf[0x64], part[0x100];
    if (lore < 1) return;
    if ((w0 & 0x1ff) == 0x13b) {
        print_message(m, 0x97);
        return;
    }
    if ((w0 & 0x1000) && (w0 & 0x1c0) != 0x140) return;
    text = (uint16_t)((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x1ff);
    if (w0 & 0x400) {
        uw_motion_cutscene_request(m, (uint16_t)(text + 0x100));   /* cutscene_play(text + 0x100) */
        return;
    }
    if (!m->strings) {
        UW_NOT_CARRIED(m->not_carried);
        return;
    }
    if (text < 0x100) {
        ds_text(m, 0x1957, buf, sizeof buf);                        /* "You read the " */
        if (!format_object_name(m, buf + strlen(buf), sizeof buf - strlen(buf), w0, ls[(uint16_t)(obj + 0x1a)], 0, 0))
            append(buf, sizeof buf, ds_text(m, 0x1931, part, sizeof part));   /* "UNNAMED" */
        append(buf, sizeof buf, ds_text(m, 0x1965, part, sizeof part));       /* "...\n" */
        scroll_print(m, buf);
    }
    if (uw_strings_by_id(m->strings, (uint16_t)(text | 0x600), part, (int)sizeof part) >= 0)
        scroll_print(m, part);
    if (text < 0x100) scroll_print(m, ds_text(m, 0x191e, part, sizeof part));  /* "\n" */
}

/* look_describe_key(obj, lore): with any lore, block 5's string
 * 100 on by the key's lock, its +6 low six bits. */
static void look_describe_key(uw_motion *m, uint16_t obj, int16_t lore) {
    char text[0x100];
    if (!lore || !m->strings) return;
    if (uw_strings_by_id(m->strings, (uint16_t)(((m->lseg[(uint16_t)(obj + 6)] & 0x3f) + 100) | 0xa00),
                         text, (int)sizeof text) >= 0 && text[0])
        scroll_print(m, text);
}

/* look_describe_remains(obj, lore): with any lore and a creature
 * in the thing's +6 low six bits -- not 0, not 0x28, and under 0x3c but for
 * 0x3f -- "It looks to be that of " (0x17, the plural, for item 0xc6 or a
 * stack of more than one), then "an adventurer.\n" for 0x3f or the creature
 * named through format_object_name over an object of item id type + 0x40,
 * and ".\n". */
static void look_describe_remains(uw_motion *m, uint16_t obj, int16_t lore) {
    uint8_t *ls = m->lseg;
    uint16_t type = (uint16_t)(ls[(uint16_t)(obj + 6)] & 0x3f);
    char text[0x40], buf[0x28];
    if (!lore || !type || type == 0x28 || (type >= 0x3c && type != 0x3f)) return;
    print_message(m, (uint16_t)((rw(ls, obj) & 0x1ff) == 0xc6 || ((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x3ff) > 1
                                ? 0x17 : 0x16));
    if (type == 0x3f) {
        scroll_print(m, ds_text(m, 0x1979, text, sizeof text));     /* "an adventurer.\n" */
        return;
    }
    buf[0] = '\0';
    format_object_name(m, buf, sizeof buf, (uint16_t)((type + 0x40) & 0x1ff), 0, 1, 0);
    scroll_print(m, buf);
    scroll_print(m, ds_text(m, 0x191d, text, sizeof text));         /* ".\n" */
}

/* look_describe_object(obj), look_at's first move at lore 3:
 * only for a thing whose properties +7 bits 1..4 are 10 -- "You see " and
 * one of block 1's nine messages 0x105 on, by item id (0x136, 0x93, 0x97,
 * 0xbf, 0x11f, 0x37, 0xae, 0xa, 0x36 in that order) -- and 1 for having
 * said it. Another id of that class prints whatever SI held, which the
 * port does not follow. */
static int look_describe_object(uw_motion *m, uint16_t obj) {
    static const uint16_t ids[9] = { 0x136, 0x93, 0x97, 0xbf, 0x11f, 0x37, 0xae, 0x00a, 0x36 };
    uint16_t id = (uint16_t)(rw(m->lseg, obj) & 0x1ff);
    int k;
    if (((prop(m, id, 7) >> 1) & 0xf) != 10) return 0;
    for (k = 0; k < 9; k++)
        if (ids[k] == id) break;
    if (k == 9) {
        UW_NOT_CARRIED(m->not_carried);
        return 1;
    }
    print_message(m, 0x104);
    print_message(m, (uint16_t)(k + 0x105));
    return 1;
}

/* look_at_npc(obj, buf), after look_at's "You see ": the
 * creature's block 4 name, its attitude's word (block 5, 0x60 on, by word
 * +0xd's top two bits) unless its whoami is 0xf0..0xfe, and a whoami's block
 * 7 name (0x10 on). The word with "an " or "a " by its first letter, and a
 * space; the creature's name -- its article only without a word -- unless a
 * whoami names it in lower case (ctype bit 2 clear); then
 * " named " when both stand, and the whoami's name, its article only
 * without the creature's; ".\n", printed. */
static void look_at_npc(uw_motion *m, uint16_t obj, char *buf, size_t cap) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    char kind[0x200], mood[0x200], name[0x200], text[0x40];
    uint8_t who = ls[(uint16_t)(obj + 0x1a)];
    int has_kind, has_mood = 0, has_name = 0, upper = 0;
    if (!m->strings) {
        UW_NOT_CARRIED(m->not_carried);
        return;
    }
    has_kind = uw_strings_by_id(m->strings, (uint16_t)((rw(ls, obj) & 0x1ff) | 0x800), kind, (int)sizeof kind - 2) >= 0
               && kind[0];
    if (who < 0xf0 || who == 0xff)
        has_mood = uw_strings_by_id(m->strings, (uint16_t)(((rw(ls, (uint16_t)(obj + 0xd)) >> 14) + 0x60) | 0xa00),
                                    mood, (int)sizeof mood) >= 0 && mood[0];
    if (who)
        has_name = uw_strings_by_id(m->strings, (uint16_t)((who + 0x10) | 0xe00), name, (int)sizeof name - 2) >= 0
                   && name[0];
    if (has_name) upper = (ds[(uint16_t)(0x1e01 + (int8_t)name[0])] & 4) != 0;
    if (has_kind && has_mood) {
        char c = mood[0];
        strncat(buf, ds_text(m, (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u') ? 0x1908 : 0x190c,
                             text, sizeof text), cap - strlen(buf) - 1);
        strncat(buf, mood, cap - strlen(buf) - 1);
        strncat(buf, ds_text(m, 0x1906, text, sizeof text), cap - strlen(buf) - 1);
    }
    if (has_kind && (!has_name || upper))
        strncat(buf, format_article_plural(kind, !has_mood, 0), cap - strlen(buf) - 1);
    if (has_name) {
        if (has_kind && upper) strncat(buf, ds_text(m, 0x1989, text, sizeof text), cap - strlen(buf) - 1);
        strncat(buf, format_article_plural(name, !has_kind, 0), cap - strlen(buf) - 1);
    }
    strncat(buf, ds_text(m, 0x191d, text, sizeof text), cap - strlen(buf) - 1);
    scroll_print(m, buf);
}

/* look_at(obj, lore), a thing the properties call describable
 * (+0xa bit 4): "You see ", then a creature through look_at_npc; else
 * the quality's word from block 5, a count for a stack of
 * more than one -- or "an " or "a " before a quality word, unless the kind
 * is 0xd -- the name through format_object_name (its own article when there
 * was no word), the enchantment at lore 3 (not carried), an owner's
 * " belonging to" and race, and ".\n", printed; then look_at_object. Lore 3
 * first tries look_describe_object, a thing not describable
 * look_at_scenery for 0x160 (both not carried). */
void look_at(uw_motion *m, uint16_t obj, int16_t lore) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t w0, pr, q;
    int16_t word_ix = 0;
    char buf[0x50], adjective[0x1a], magic[11], text[0x40], first = 0;
    int plural = 0;
    if (!obj) return;
    w0 = rw(ls, obj);
    pr = (uint16_t)(OBJ_PROPERTIES + (w0 & 0x1ff) * 11);
    if (!((ds[(uint16_t)(pr + 0xa)] >> 4) & 1)) {
        if ((w0 & 0x1f0) == 0x160) look_at_scenery(m, obj, lore);
        goto object;
    }
    if (lore == 3 && look_describe_object(m, obj)) return;
    strcpy(buf, ds_text(m, 0x18ff, text, sizeof text));
    magic[0] = 0;
    if (look_at_magic_equipment(m, obj, lore, magic, sizeof magic)) first = magic[0];
    adjective[0] = 0;
    if (((w0 & 0x1c0) >> 6) == 1) {
        look_at_npc(m, obj, buf, sizeof buf);
        return;
    }
    if (ls[(uint16_t)(obj + 4)] & 0x3f)
        word_ix = ((ds[(uint16_t)(pr + 6)] >> 2) & 3) == 3 ? 5 : (int16_t)(((ls[(uint16_t)(obj + 4)] & 0x3f) >> 4) + 1);
    if (m->strings && uw_strings_by_id(m->strings, (uint16_t)(((ds[(uint16_t)(pr + 0xa)] & 0xf) * 6 + word_ix) | 0xa00),
                                       text, (int)sizeof text) >= 0 && text[0]) {
        size_t n = strlen(text);
        if (n >= sizeof adjective) n = sizeof adjective - 1;
        memcpy(adjective, text, n);
        adjective[n] = 0;
        first = text[0];
    }
    q = (uint16_t)((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x3ff);
    if ((w0 & 0x8000) && !(q & 0x200) && q > 1) {
        plural = 1;
        first = 'x';
        itoa10(q, buf + strlen(buf));
        strcat(buf, ds_text(m, 0x1906, text, sizeof text));
    } else if (first && (ds[(uint16_t)(pr + 0xa)] & 0xf) != 0xd) {
        strcat(buf, ds_text(m, (first == 'a' || first == 'e' || first == 'i' || first == 'o' || first == 'u')
                                   ? 0x1908 : 0x190c, text, sizeof text));
    }
    if (adjective[0]) {
        strncat(buf, adjective, sizeof buf - strlen(buf) - 1);
        strncat(buf, ds_text(m, 0x1906, text, sizeof text), sizeof buf - strlen(buf) - 1);
    }
    if (magic[0]) strncat(buf, magic, sizeof buf - strlen(buf) - 1);
    format_object_name(m, buf + strlen(buf), sizeof buf - strlen(buf), w0, ls[(uint16_t)(obj + 0x1a)], !first, plural);
    magic_item_description(m, obj, lore, buf, sizeof buf);
    if (((ds[(uint16_t)(pr + 7)] >> 7) & 1) && (ls[(uint16_t)(obj + 6)] & 0x3f)
        && (ls[(uint16_t)(obj + 6)] & 0x1f) <= 0x1b) {
        strncat(buf, ds_text(m, 0x190f, text, sizeof text), sizeof buf - strlen(buf) - 1);
        if (m->strings && uw_strings_by_id(m->strings, (uint16_t)(((ls[(uint16_t)(obj + 6)] & 0x1f) + 0x172) | 0x200),
                                           text, (int)sizeof text) >= 0)
            strncat(buf, text, sizeof buf - strlen(buf) - 1);
    }
    strncat(buf, ds_text(m, 0x191d, text, sizeof text), sizeof buf - strlen(buf) - 1);
    scroll_print(m, buf);
object:
    /* look_at_object: remains, a key, a writing, a spiked door */
    {
        uint16_t cls = (uint16_t)((w0 & 0x1c0) >> 6), sub = (uint16_t)((w0 & 0x30) >> 4);
        if (cls == 3 && sub == 0 && (w0 & 0x1ff) > 0xc1 && (w0 & 0x1ff) < 199)
            look_describe_remains(m, obj, lore);
        else if (cls == 4 && sub == 3) read_object(m, obj, lore);
        else if (cls == 4 && sub == 0) look_describe_key(m, obj, lore);
        else if (cls == 5 && sub == 0 && (w0 & 0xf) < 8 && (rw(ls, (uint16_t)(obj + 6)) & 1)) print_message(m, 0x83);
    }
}

/* search_for_trap(obj, skill): a
 * non-quantity's first trap or trigger (class 6) among its contents -- a
 * trigger (subclass 2 or more) standing for the first thing it links --
 * whose word 0 low six bits are 0..2 is found by check_skill_roll(skill, 8),
 * whose answer (-1..2) this is; 0 when there is none. */
int search_for_trap(uw_motion *m, uint16_t obj, uint8_t skill) {
    uint8_t *ls = m->lseg;
    uint16_t link, t;
    if (!obj || (rw(ls, obj) & 0x8000) || !((rw(ls, (uint16_t)(obj + 6)) >> 6) & 0x3ff)) return 0;
    link = (uint16_t)(obj + 6);
    t = object_find_matching(m, &link, 0, 6, 0xffff, 0xffff);
    if (!t) return 0;
    if (((rw(ls, t) & 0x30) >> 4) >= 2) {
        t = deref_link(m, (uint16_t)(t + 6));
        if (!t) { UW_NOT_CARRIED(m->not_carried); return 0; }
    }
    if ((rw(ls, t) & 0x3f) >= 3) return 0;
    return check_skill_roll(m, skill, 8);
}

/* action_look: look_at the picked object -- lore 1 when it is in
 * reach (0x48) and cannot be taken, else 0 -- and in look mode a search for
 * a trap in it; the look trigger (trigger_object_link with 5); the release
 * wait in look mode, a drag that picks it up in another (not carried). */
void action_look(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t obj = rw(ds, CURSOR_PICK_OBJECT), rec = rw(ds, PLAYER_RECORD_PTR);
    int reach = action_in_reach(m, 0x48, obj, rw(ds, PICK_TILE));
    look_at(m, obj, reach && !ds[CURSOR_PICK_VALID] ? 1 : 0);
    if (rw(ds, 0x268c) == 3) {
        /* the Search skill finds a trap: "You found a trap!  Do you wish to
         * try to disarm it? " (0xf4) and Yes, the Traps skill's to try --
         * the host's question; the rest of the look after its answer */
        if (search_for_trap(m, obj, ds[(uint16_t)(rec + 0x2c)]) > 0) {
            m->prompt_obj = obj;
            m->prompt_skill = ds[(uint16_t)(rec + 0x2b)];
            scroll_ask_yes_no(m, 0xf4, 1, 2);
            return;
        }
    } else {
        ds[LOOK_IN_PROGRESS] = 1;
    }
    action_look_rest(m, obj);
}

/* action_look's rest: the look trigger -- trigger_object_link(player, obj,
 * 5, the target tile), a trigger in its chain walked for the look kind --
 * then in look mode the release wait; in any other (the default right
 * click) cursor_wait_for_drag, whose drag is action_pickup with the look in
 * progress (the host's: drag_wait with view_drag, uw_motion_view_drag or
 * uw_motion_view_click_end); the look over. */
void action_look_rest(uw_motion *m, uint16_t obj) {
    uint8_t *ds = m->ds;
    if (obj) trigger_object_link_port(m, rw(ds, TRACKED_OBJECT), obj, 5);
    if (rw(ds, 0x268c) == 3) input_wait_button_release(m, 1);
    else if (mouse_sample_buttons(m)) {
        m->drag_wait = 1;
        m->view_drag = 1;
        return;
    }
    ds[LOOK_IN_PROGRESS] = 0;
}

/* The look's drag answered: past six of cursor movement action_pickup, which
 * with the look in progress takes what it can and talks to or uses what it
 * cannot; the button up first, nothing. The look is over either way. */
void uw_motion_view_drag(uw_motion *m) {
    m->view_drag = 0;
    action_pickup(m);
    m->ds[LOOK_IN_PROGRESS] = 0;
}

void uw_motion_view_click_end(uw_motion *m) {
    m->view_drag = 0;
    m->ds[LOOK_IN_PROGRESS] = 0;
}
