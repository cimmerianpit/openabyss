/* SPDX-License-Identifier: MIT */
#include "uw_convbi.h"
#include "uw_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ACTIVE_LIST = 0x273c, ACTIVE_END = 0x2732, CRITTER_RACE = 0x4a5b,
       PLAYER_RECORD_PTR = 0x7270, PLAYER_OBJECT_PTR = 0x7274, TALKER_PTR = 0x4a14, RAND_SEED = 0x207e, CTYPE_TABLE = 0x1e01, ASK_STRING_ID = 0x0ea9,
       CONV_STRING_BLOCK = 0x7c, ASK_MAX = 0x32 };

static uint16_t rw(const uint8_t *s, uint16_t o) { return (uint16_t)(s[o] | s[(uint16_t)(o + 1)] << 8); }
static void ww(uint8_t *s, uint16_t o, uint16_t v) { s[o] = (uint8_t)v; s[(uint16_t)(o + 1)] = (uint8_t)(v >> 8); }

/* conv_mem_get of the argument `k` places below the count. */
static int16_t arg(const uw_convvm *vm, const uint16_t *top, int k) {
    uint16_t at = top[-k];
    return at < vm->mem_words ? (int16_t)vm->mem[at] : 0;
}

/* conv_mem_get and conv_stack_get: an index doubled
 * in sixteen bits, so the top bit falls away. */
static uint16_t mem_get(const uw_convvm *vm, uint16_t i) {
    i &= 0x7fff;
    return i < vm->mem_words ? vm->mem[i] : 0;
}

static uint16_t stack_get(const uw_convvm *vm, uint16_t i) {
    uint32_t at = (uint32_t)vm->memslots + ((uint16_t)(vm->bp + i) & 0x7fff);
    return at < vm->mem_words ? vm->mem[at] : 0;
}

/* Borland's character classes (indexed from -1): 0x01 space,
 * 0x02 digit, 0x04 upper case, 0x40 punctuation. The string builtins index it
 * with a SIGN-EXTENDED char, so a byte from 0x80 up reads the data segment
 * below the table -- carried from `ds` when there is one. */
static uint8_t ctype(const uw_convbi *b, char ch) {
    int c = (signed char)ch;
    if (c < 0) return b->ds ? b->ds[CTYPE_TABLE + c] : 0;
    if (c == ' ') return 0x01;
    if (c >= 9 && c <= 13) return 0x21;
    if (c < 0x20 || c == 0x7f) return 0x20;
    if (c >= '0' && c <= '9') return 0x12;
    if (c >= 'A' && c <= 'Z') return (uint8_t)(c <= 'F' ? 0x14 : 0x04);
    if (c >= 'a' && c <= 'z') return (uint8_t)(c <= 'f' ? 0x18 : 0x08);
    return 0x40;
}

/* strlwr_far, in place. */
static void strlwr_ct(const uw_convbi *b, char *s) {
    for (; *s; s++)
        if (ctype(b, *s) & 0x04) *s = (char)(*s + 0x20);
}

static size_t strnlen_c(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

static char *strdup_c(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    memcpy(p, s, n);
    return p;
}

/* What an id with no string behind it reads as here. */
static char empty_string[1];

/* atol of what strncpy_far copied: at most nineteen characters,
 * the white space skipped (its class test is UNsigned), a sign, the digits;
 * the callers keep the low word. */
static int16_t atoi19(const char *s) {
    char tmp[20];
    const char *p = tmp;
    uint32_t v = 0;
    int neg = 0;
    size_t n = strnlen_c(s, 19);
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    while (*p == ' ' || (*p >= 9 && *p <= 13)) p++;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    for (; *p >= '0' && *p <= '9'; p++) v = v * 10 + (uint32_t)(*p - '0');
    return (int16_t)(neg ? 0u - v : v);
}

static char *get_string(uw_convbi *b, uint16_t id) {
    return b->strings ? uw_strcache_get(b->strings, id) : NULL;
}

/* The number after a kind: its digits (and minus signs) skipped, then an
 * offset when a substitution follows directly -- one less than its value. */
static uint16_t parse_substitution(uw_convbi *b, uw_convvm *vm, const char **src);

static uint16_t substitute_value(uw_convbi *b, uw_convvm *vm, char kind, const char **src) {
    const char *s = *src;
    uint16_t n = (uint16_t)atoi19(s), off = 0;
    while (*s && ((ctype(b, *s) & 0x02) || *s == '-')) s++;
    if (*s == 'G' || *s == 'S' || *s == 'P' || *s == 'C')
        off = (uint16_t)(parse_substitution(b, vm, &s) - 1);
    *src = s;
    switch (kind) {
    case 'G': return mem_get(vm, (uint16_t)(n + off));
    case 'P': return mem_get(vm, (uint16_t)(stack_get(vm, n) + off));
    case 'S': return stack_get(vm, (uint16_t)(n + off));
    default:  return n;
    }
}

/* conv_parse_substitution: a kind, a type (I implied by C), and
 * for an I the value; any other type answers 0 WITHOUT consuming its number,
 * which is then copied out as text. */
static uint16_t parse_substitution(uw_convbi *b, uw_convvm *vm, const char **src) {
    char kind = **src, type = 'I';
    if (!kind) return 0;
    (*src)++;
    if (kind != 'C') {
        type = **src;
        if (!type) return 0;
        (*src)++;
    }
    if (type != 'I') return 0;
    return substitute_value(b, vm, kind, src);
}

typedef struct { char *p; size_t len, cap; } growbuf;

static void put(growbuf *g, const char *s, size_t n) {
    if (g->len + n + 1 > g->cap) {
        while (g->len + n + 1 > g->cap) g->cap *= 2;
        g->p = realloc(g->p, g->cap);
    }
    memcpy(g->p + g->len, s, n);
    g->len += n;
    g->p[g->len] = '\0';
}

/* '@' then a kind -- G a global, S a stack slot from BP, P a pointer read
 * from a stack slot, C a constant -- then a type (I a number printed in
 * decimal, anything else a string id expanded in its turn; C means I), the
 * number and an optional offset substitution; "@@" is a literal '@'. The
 * original writes into (length + 0x40) * 2 bytes unchecked; this grows. */
char *uw_convbi_expand(uw_convbi *b, uw_convvm *vm, char *src) {
    growbuf g;
    const char *s = src;
    if (!src || !strchr(src, '@')) return src;
    g.cap = ((strlen(src) + 0x40) * 2) | 1;
    g.p = malloc(g.cap);
    g.len = 0;
    g.p[0] = '\0';
    while (*s) {
        char kind, type;
        uint16_t v;
        if (*s != '@' || s[1] == '@') {
            s += *s == '@';
            put(&g, s, 1);
            s++;
            continue;
        }
        s++;
        kind = *s;
        if (!kind) break;                         /* the original reads on past the end */
        s++;
        type = 'I';
        if (kind != 'C') {
            type = *s;
            if (!type) break;
            s++;
        }
        v = substitute_value(b, vm, kind, &s);
        if (type == 'I') {
            char num[8];
            int n = snprintf(num, sizeof num, "%d", (int16_t)v);
            put(&g, num, (size_t)n);
        } else {
            char *raw = get_string(b, v);
            if (raw) {
                char *e = uw_convbi_expand(b, vm, raw);
                put(&g, e, strlen(e));
                if (e != raw) free(e);
            }
        }
    }
    return g.p;
}

/* The expansion freed when it was a fresh copy. */
static void free_expansion(char *e, const char *raw) {
    if (e != raw && e != empty_string) free(e);
}

static char *expanded_arg(uw_convbi *b, uw_convvm *vm, uint16_t *top, int k, char **raw) {
    *raw = get_string(b, (uint16_t)arg(vm, top, k));
    return uw_convbi_expand(b, vm, *raw ? *raw : empty_string);
}

/* compare(a, b): 1 when the two expansions match ignoring case --
 * each copied (into 0x110 and 0x100 bytes on the stack, unchecked), both
 * lower-cased, strcmp'd. */
static int16_t bi_compare(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    char *r1, *r2, *e1, *e2, *c1, *c2;
    int differ;
    e1 = expanded_arg(b, vm, top, 1, &r1);
    e2 = expanded_arg(b, vm, top, 2, &r2);
    c1 = strdup_c(e1);
    c2 = strdup_c(e2);
    strlwr_ct(b, c1);
    strlwr_ct(b, c2);
    differ = strcmp(c2, c1);
    free(c1);
    free(c2);
    free_expansion(e2, r2);
    free_expansion(e1, r1);
    return differ == 0;
}

/* strstr_far: an empty needle is never found. */
static char *strstr_far(char *hay, const char *needle) {
    return *needle ? strstr(hay, needle) : NULL;
}

/* contains(text, word): 1 when `word` occurs in `text` as a whole
 * word -- the start of the text or a space or punctuation before it, the end
 * or one after. THE RAW STRINGS ARE LOWER-CASED IN PLACE, after the
 * expansions were taken, so an expansion that copied keeps its case and the
 * typed answer in block 0x7c stays lower-cased afterwards. A hit that fails
 * the test resumes the search the word's length on. The expansions are not
 * freed in the original. */
static int16_t bi_contains(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    char *r1, *r2, *e1, *e2, *p;
    int16_t found = 0;
    e1 = expanded_arg(b, vm, top, 1, &r1);
    e2 = expanded_arg(b, vm, top, 2, &r2);
    if (r2) strlwr_ct(b, r2);
    if (r1) strlwr_ct(b, r1);
    for (p = strstr_far(e1, e2); p; p = strstr_far(p, e2)) {
        size_t n = strlen(e2);
        if ((p == e1 || (ctype(b, p[-1]) & 0x41))
            && (!p[n] || (ctype(b, p[n]) & 0x41))) {
            found = 1;
            break;
        }
        p += n;
    }
    free_expansion(e2, r2);
    free_expansion(e1, r1);
    return found;
}

/* remove_talker(): the talker taken out of its tile's chain,
 * forced; AX is object_remove's, 0 when it went. */
static int16_t bi_remove_talker(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint16_t t = rw(b->ds, TALKER_PTR), w;
    (void)vm; (void)top;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    w = rw(b->level->lseg, (uint16_t)(t + 0x16));
    return (int16_t)uw_motion_object_remove(b->level,
        (uint16_t)(uw_motion_tile_ptr(b->level, (uint16_t)(w >> 10), (uint16_t)((w & 0x3f0) >> 4)) + 2), t, 1);
}

/* set_attitude(whoami, attitude): run_function_on_whoami_list
 * over the active mobiles for the FIRST whose whoami byte is
 * the word given, its +0x0d bits 14..15 set by the callback,
 * conv_set_attitude_handler (whose AL is 0 -- it returns the old word's
 * bits 8..13 and the new attitude, all above the low byte -- so the walk
 * never steps back). AX is what the walk
 * leaves: 0 after a match (its `all` byte sign-extended), else the list
 * cursor, which stops at the list's end. */
static int16_t bi_set_attitude(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint8_t *ds = b->ds;
    uint16_t attitude = (uint16_t)arg(vm, top, 1), whoami = (uint16_t)arg(vm, top, 2), p;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    for (p = rw(ds, ACTIVE_LIST); p < rw(ds, ACTIVE_END); p++) {
        uint8_t *ls = b->level->lseg;
        uint16_t obj = uw_motion_obj_at(b->level, ls[p]);
        if (ls[(uint16_t)(obj + 0x1a)] == whoami) {
            ww(ls, (uint16_t)(obj + 0xd), (uint16_t)((rw(ls, (uint16_t)(obj + 0xd)) & 0x3fff) | ((attitude & 3) << 14)));
            return 0;
        }
    }
    return (int16_t)p;
}

/* set_race_attitude(race, attitude, radius): in the square of
 * tiles `radius` around the talker -- clamped to 1 below, 0x3f above -- every
 * object of the talker's item id whose +0x0a bit 7 is clear and whose
 * critter_properties race byte is the race given has +0x0d bits 14..15 set
 * to the attitude. The talker is one of them. AX is the row cursor the outer
 * loop stops on. */
static int16_t bi_set_race_attitude(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint8_t *ds = b->ds, *ls;
    int16_t radius = arg(vm, top, 1), attitude = arg(vm, top, 2), race = arg(vm, top, 3);
    uint16_t t = rw(ds, TALKER_PTR), id, w;
    int16_t x0, y0, x1, y1, x, y;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    ls = b->level->lseg;
    id = (uint16_t)(rw(ls, t) & 0x1ff);
    w = rw(ls, (uint16_t)(t + 0x16));
    x0 = (int16_t)((w >> 10) - radius);
    if (x0 < 1) x0 = 1;
    y0 = (int16_t)(((w & 0x3f0) >> 4) - radius);
    if (y0 < 1) y0 = 1;
    x1 = (int16_t)((w >> 10) + radius);
    if (x1 >= 0x40) x1 = 0x3f;
    y1 = (int16_t)(((w & 0x3f0) >> 4) + radius);
    if (y1 >= 0x40) y1 = 0x3f;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            uint16_t link = (uint16_t)(uw_motion_tile_ptr(b->level, (uint16_t)x, (uint16_t)y) + 2), o;
            for (o = uw_motion_deref_link(b->level, link); o; o = uw_motion_deref_link(b->level, (uint16_t)(o + 4))) {
                uint16_t w0 = rw(ls, o);
                if ((w0 & 0x1ff) == id && !(ls[(uint16_t)(o + 0xa)] & 0x80)
                    && ds[(uint16_t)(CRITTER_RACE + (w0 & 0x3f) * 0x30)] == (uint16_t)race)
                    ww(ls, (uint16_t)(o + 0xd), (uint16_t)((rw(ls, (uint16_t)(o + 0xd)) & 0x3fff) | ((attitude & 3) << 14)));
            }
        }
    return y;
}

/* gronk_door(x, y, op): in tile (x, y) the first door (class 5,
 * subclass 0) or else moving door (class 7, subclass 0, type 15); with the
 * action target tile set to (x, y) for the call and put back after, op 0
 * opens it, 1 closes it, 2 toggles it, anything else nothing. 1 when a door
 * was found, 0 when not. */
static int16_t bi_gronk_door(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint8_t *ds = b->ds;
    uint16_t x = (uint16_t)arg(vm, top, 3), y = (uint16_t)arg(vm, top, 2), link, door, sx, sy;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    link = (uint16_t)(uw_motion_tile_ptr(b->level, x, y) + 2);
    door = uw_motion_find_matching(b->level, &link, 0, 5, 0, 0xffff);
    if (!door) door = uw_motion_find_matching(b->level, &link, 0, 7, 0, 0xf);
    if (!door) return 0;
    sx = rw(ds, 0x269a);
    sy = rw(ds, 0x269c);
    ww(ds, 0x269a, x);
    ww(ds, 0x269c, y);
    switch (arg(vm, top, 1)) {
    case 0: uw_motion_door_open(b->level, 0, door); break;   /* door_open(0, door): no actor */
    case 1: uw_motion_door_close(b->level, door); break;
    case 2: uw_motion_door_toggle(b->level, 0, door); break;
    }
    ww(ds, 0x269a, sx);
    ww(ds, 0x269c, sy);
    return 1;
}

static int16_t rand15(uint8_t *ds);

enum { NPC_SLOTS = 0x485e, NPC_SLOT_OFFERED = 0x487a, OBJ_PROPERTIES_VALUE = 0x5b72, EMS_FRAME_CLAIMED = 0x0a4a,
       EMS_FRAME_SEG = 0x24ba };

/* ems_frame_remap's AX, which the barter builtins leave: the
 * EMS page frame's segment while the frame is claimed, else 0.
 * Remapping itself is the far heap's business, not the game's state. */
static int16_t ems_frame_remap(const uw_convbi *b) {
    return (int16_t)((int8_t)b->ds[EMS_FRAME_CLAIMED] ? rw(b->ds, EMS_FRAME_SEG) : 0);
}

/* setup_to_barter(): the talker's goods onto its side of the
 * table. Along its inventory chain, at most 0x28 objects and stopping at the
 * first one it put back: an object is passed over when its obj_properties
 * value is 0, or when it is subclass 0 until one subclass-0 object has been
 * passed over, or -- once the four slots have all been filled -- unless
 * rand() & 7 is 5 or more (three in eight). A taken object leaves the chain
 * for the next NPC slot, whose previous object goes back into
 * the chain; the slots rotate. The slot drawings are the interface's. AX is
 * ems_frame_remap's. */
static int16_t bi_setup_to_barter(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint8_t *ds = b->ds, *ls;
    uint16_t t, cur, first = 0;
    int si = 0, passed = 0, lapped = 0, n = 0;
    (void)vm; (void)top;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    ls = b->level->lseg;
    t = rw(ds, TALKER_PTR);
    uw_motion_spawn_npc_loot(b->level, t);   /* the talker's goods, made once (+0x0d bit 12) */
    cur = uw_motion_deref_link(b->level, (uint16_t)(t + 6));
    while (cur && cur != first && n++ < 0x28) {
        uint16_t next = uw_motion_deref_link(b->level, (uint16_t)(cur + 4)), w0 = rw(ls, cur), old;
        int skip = ((w0 & 0x30) == 0 && !passed)
                   || rw(ds, (uint16_t)(OBJ_PROPERTIES_VALUE + (w0 & 0x1ff) * 0xb)) == 0
                   || (lapped && (rand15(ds) & 7) < 5);
        if (skip) {
            if ((w0 & 0x30) == 0) passed = 1;
        } else {
            uw_motion_object_list_remove(b->level, (uint16_t)(t + 6), cur);
            old = rw(ds, (uint16_t)(NPC_SLOTS + si * 2));
            if (old) {
                if (!first) first = uw_motion_obj_at(b->level, old);
                uw_motion_object_list_insert(b->level, (uint16_t)(t + 6), uw_motion_obj_at(b->level, old));
            }
            ww(ds, (uint16_t)(NPC_SLOTS + si * 2), uw_motion_obj_index(b->level, cur));
            if (++si > 3) {
                si = 0;
                lapped = 1;
            }
        }
        cur = next;
    }
    return ems_frame_remap(b);
}

static void draw_slot_frame(uw_convbi *b, int whose, int slot);

/* imgbuf_restore of a trade slot's background, from barter_setup's buffer.
 * conv_bi_do_decline2, barter_exchange and the give and take builtins draw
 * this between cursor_hide and cursor_show; the pair is here, round the one
 * draw they all make, as the slot's own frame carries its own. */
static void slot_background_restore(uw_convbi *b, uint16_t handle) {
    if (!b->level) return;
    uw_motion_cursor_hide(b->level);
    uw_motion_imgbuf_restore(b->level, handle);
    uw_motion_cursor_show(b->level);
}

/* conv_bi_do_decline2(keep_offered): every NPC slot holding an
 * object -- only the ones not offered when the argument is
 * nonzero -- back into the talker's inventory, the slot's saved background
 * restored, flag and slot cleared and its mark
 * drawn (barter_draw_slot_frame). AX is
 * ems_frame_remap's, and do_decline is decline2(0). */
static int16_t barter_decline(uw_convbi *b, int keep_offered) {
    uint8_t *ds = b->ds;
    uint16_t t = rw(ds, TALKER_PTR);
    int si;
    for (si = 0; si < 4; si++) {
        int16_t idx = (int16_t)rw(ds, (uint16_t)(NPC_SLOTS + si * 2));
        if (idx <= 0 || (keep_offered && ds[(uint16_t)(NPC_SLOT_OFFERED + si)])) continue;
        uw_motion_object_list_insert(b->level, (uint16_t)(t + 6), uw_motion_obj_at(b->level, (uint16_t)idx));
        slot_background_restore(b, rw(ds, (uint16_t)(0x489e + si * 2)));
        ds[(uint16_t)(NPC_SLOT_OFFERED + si)] = 0;
        ww(ds, (uint16_t)(NPC_SLOTS + si * 2), 0);
        draw_slot_frame(b, 0, si);
    }
    return ems_frame_remap(b);
}

void uw_convbi_end_barter(uw_convbi *b) {
    uint8_t *ds = b->ds;
    int si;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return; }
    uw_motion_cursor_hide(b->level);
    for (si = 0; si < 4; si++) {
        int16_t p = (int16_t)rw(ds, (uint16_t)(0x486a + si * 2)), n = (int16_t)rw(ds, (uint16_t)(NPC_SLOTS + si * 2));
        /* The frame's address for the scratch squares: the conversation's
         * refresh runs from the game's pass, whose stack sits near 0x9580.
         * Each occupied slot's background put back. */
        if (p > 0) {
            uw_motion_object_place_at_own_coords(b->level, rw(ds, PLAYER_OBJECT_PTR), uw_motion_obj_at(b->level, (uint16_t)p), 5, 0, 0x9540);
            uw_motion_imgbuf_restore(b->level, rw(ds, (uint16_t)(0x48b6 + si * 2)));
        }
        if (n > 0) {
            uw_motion_object_place_at_own_coords(b->level, rw(ds, TALKER_PTR), uw_motion_obj_at(b->level, (uint16_t)n), 5, 0, 0x9540);
            uw_motion_imgbuf_restore(b->level, rw(ds, (uint16_t)(0x489e + si * 2)));
        }
    }
    uw_motion_cursor_show(b->level);
    /* the eight buffers barter_setup took, given back */
    for (si = 0; si < 4; si++) {
        (void)uw_motion_imgbuf_free(b->level, rw(ds, (uint16_t)(0x48b6 + si * 2)));
        (void)uw_motion_imgbuf_free(b->level, rw(ds, (uint16_t)(0x489e + si * 2)));
    }
}

static int16_t bi_do_decline(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    (void)vm; (void)top;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    return barter_decline(b, 0);
}

enum { PLAYER_SLOTS = 0x486a, PLAYER_SLOT_OFFERED = 0x487e, BARTER_DEAL_DONE = 0x4872, BARTER_PATIENCE = 0x4884,
       BARTER_NPC_JITTER = 0x4882, BARTER_THRESHOLD = 0x488a, BARTER_LAST_PCT = 0x488c, BARTER_LIKES = 0x4876,
       BARTER_DISLIKES = 0x4886, TEXT_SPEAKER = 0x0ea6, CRITTER_ROW = 0x4a52 };

/* conv_bi_say and conv_bi_respond as the barter
 * builtins print through them: the text, unexpanded, to the conversation's
 * windows -- the interface's -- and which side spoke last: say
 * clears it, respond sets it. */
/* The line a text builtin hands scroll_print, into `line`: a prefix, the
 * text, a suffix (the DS strings at 0x10de "\\P", 0x10dc "\n", 0x10e1
 * "\\1", 0x10e4 "\\0\n", 0x10e8 "\\2"). */
static void compose_line(uw_convbi *b, const char *pre, const char *text, const char *post) {
    size_t n = sizeof b->line;
    snprintf(b->line, n, "%s%s%s", pre, text ? text : "", post);
}

/* The line printed: text_window_select_conv_npc (or _conv_menu), scroll_print,
 * text_window_select_scroll -- unless the print stopped at a wait, where a
 * later state stands. */
static void print_line(uw_convbi *b, int npc_window) {
    if (!b->scroll) return;
    if (npc_window) uw_scroll_select_conv_npc(b->scroll);
    else uw_scroll_select_conv_menu(b->scroll);
    uw_scroll_print(b->scroll, b->line);
    if (b->scroll->stopped) return;
    uw_scroll_select_scroll(b->scroll);
}

/* The barter builtins print a string UNEXPANDED through conv_bi_say or
 * conv_bi_respond. */
static void barter_print(uw_convbi *b, int respond, uint16_t id) {
    char *s = get_string(b, id);
    if (respond) compose_line(b, "", s, "\n");
    else compose_line(b, "\\P", s, "\n");
    print_line(b, !respond);
    uw_convbi_spoke(b, respond);
}

void uw_convbi_say(uw_convbi *b, uw_convvm *vm, uint16_t string_id, int respond) {
    char *raw = get_string(b, string_id), *e = uw_convbi_expand(b, vm, raw ? raw : empty_string);
    if (respond) compose_line(b, "", e, "\n");
    else compose_line(b, "\\P", e, "\n");
    free_expansion(e, raw);
    print_line(b, !respond);
    uw_convbi_spoke(b, respond);
}

void uw_convbi_menu_echo(uw_convbi *b, uw_convvm *vm, uint16_t string_id) {
    char *raw = get_string(b, string_id), *e = uw_convbi_expand(b, vm, raw ? raw : empty_string);
    if (b->scroll) {
        /* conv_menu_choose: the menu window cleared and closed */
        ww(b->ds, 0x4948, 0);
        ww(b->ds, 0x4954, 0);
        uw_scroll_select_conv_menu(b->scroll);
        uw_scroll_clear(b->scroll, 1);
        uw_scroll_select_scroll(b->scroll);
        b->ds[0x0a8e] = 0;
        uw_scroll_select_conv_npc(b->scroll);
    }
    compose_line(b, "\\1", e, "\\0\n");
    free_expansion(e, raw);
    print_line(b, 1);                           /* conv_print_npc_line */
    uw_convbi_spoke(b, 1);
    if (b->scroll && !b->scroll->stopped) uw_scroll_select_scroll(b->scroll);
}

void uw_convbi_menu_prints(uw_convbi *b, uw_convvm *vm, uint16_t *top, int fmenu) {
    uint16_t strings = top[-1], flags = fmenu ? top[-2] : 0, n = 1, k, row = 0;
    char *texts[16], *raws[16], line[0xa0];
    ww(b->ds, 0x4948, 1);
    for (k = 0;; k++) {
        uint16_t id = mem_get(vm, (uint16_t)(strings + k));
        char *raw;
        if (!id) break;
        if (fmenu && !mem_get(vm, (uint16_t)(flags + k))) continue;
        if (n >= 16) {
            UW_NOT_CARRIED(b->not_carried);
            break;
        }
        raw = get_string(b, id);
        raws[n] = raw ? raw : empty_string;
        texts[n] = uw_convbi_expand(b, vm, raws[n]);
        ww(b->ds, (uint16_t)(0x4a18 + n * 2), id);
        n++;
    }
    ww(b->ds, 0x4a2c, n);
    if (b->scroll) {
        uw_scroll_select_conv_menu(b->scroll);
        uw_scroll_clear(b->scroll, 1);
    }
    for (k = 0; k < 10; k++) ww(b->ds, (uint16_t)(0x495e + k * 2), 0xffff);
    for (k = 1; k < n; k++) {
        snprintf(line, sizeof line, "%c. %s\n", (char)('0' + k), texts[k]);
        if (b->scroll) {
            int16_t last = uw_scroll_print(b->scroll, line);
            for (; (int16_t)row <= last; row++) ww(b->ds, (uint16_t)(0x495e + row * 2), k);
            row = (uint16_t)(last + 1);
        }
        snprintf(b->menu_text[k], sizeof b->menu_text[k], "%s", texts[k]);
        free_expansion(texts[k], raws[k]);
    }
    if (b->scroll) uw_scroll_select_scroll(b->scroll);
    ww(b->ds, 0x4954, 1);
    b->ds[0x0a8e] = 1;
}

void uw_convbi_menu_relist(uw_convbi *b) {
    char line[0xa0 + 8];
    int16_t k;
    if (b->scroll) {
        uw_scroll_select_conv_menu(b->scroll);
        uw_scroll_clear(b->scroll, 1);
    }
    for (k = 1; k < (int16_t)rw(b->ds, 0x4a2c) && k < 16; k++) {
        snprintf(line, sizeof line, "%c. %s\n", (char)('0' + k), b->menu_text[k]);
        if (b->scroll) (void)uw_scroll_print(b->scroll, line);
    }
    if (b->scroll) uw_scroll_select_scroll(b->scroll);
    b->ds[0x0a8e] = 1;
}

void uw_convbi_spoke(uw_convbi *b, int player) {
    b->ds[TEXT_SPEAKER] = (uint8_t)(player ? 1 : 0);
}

/* barter_side_is_empty(slots, flags): 0 when any slot holds an
 * object (a positive index) with its flag byte positive, else 1. */
static int barter_side_is_empty(const uw_convbi *b, uint16_t slots, uint16_t flags) {
    int i;
    for (i = 0; i < 4; i++)
        if ((int8_t)b->ds[(uint16_t)(flags + i)] > 0 && (int16_t)rw(b->ds, (uint16_t)(slots + i * 2)) > 0) return 0;
    return 1;
}

/* barter_item_appeal(idx): -1 for a thing with no price or an
 * id on the NPC's dislikes list, 1 for an id on its likes list, else what
 * the kind (1000 + id >> 4) entries say -- 1 liked, -1 disliked, the
 * dislikes read last -- or 0. The lists are far pointers into the
 * conversation's memory, which set_likes_dislikes sets; with either set this
 * port counts the call. */
/* The word at entry k of a list whose far pointer is at DS:at, read through
 * the VM's memory -- conv_mem_addr made the pointer from the memory's own far
 * base. */
static int16_t list_entry(const uw_convbi *b, uint16_t at, uint16_t k) {
    uint16_t base = rw(b->ds, 0x4838), i = (uint16_t)((uint16_t)(rw(b->ds, at) - base) / 2 + k);
    return (int16_t)(b->vm && i < b->vm->mem_words ? b->vm->mem[i] : 0xffff);
}

static int16_t barter_item_appeal(uw_convbi *b, uint16_t idx) {
    uint8_t *ds = b->ds;
    uint16_t o = uw_motion_obj_at(b->level, idx), id = (uint16_t)(rw(b->level->lseg, o) & 0x1ff), k;
    int16_t kind = (int16_t)(((int16_t)id >> 4) + 1000), flag = 0, e;
    if (rw(ds, (uint16_t)(OBJ_PROPERTIES_VALUE + id * 0xb)) == 0) return -1;
    if (rw(ds, BARTER_LIKES) | rw(ds, BARTER_LIKES + 2))
        for (k = 0; (e = list_entry(b, BARTER_LIKES, k)) > -1; k++) {
            if (e < 1000) {
                if (e == (int16_t)id) return 1;
            } else if (e == kind) {
                flag = 1;
            }
        }
    if (rw(ds, BARTER_DISLIKES) | rw(ds, BARTER_DISLIKES + 2))
        for (k = 0; (e = list_entry(b, BARTER_DISLIKES, k)) > -1; k++) {
            if (e < 1000) {
                if (e == (int16_t)id) return -1;
            } else if (e == kind) {
                flag = -1;
            }
        }
    return flag;
}

/* set_likes_dislikes(&likes, &dislikes) -> 1: the two lists'
 * far pointers (conv_mem_addr: the memory's base plus twice the index)
 * kept. Each list is item ids below 1000 and kinds (1000 plus
 * id >> 4) at and above, ended by a negative word. */
static int16_t bi_set_likes_dislikes(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint8_t *ds = b->ds;
    (void)vm;
    ww(ds, BARTER_LIKES, (uint16_t)(rw(ds, 0x4838) + top[-2] * 2));
    ww(ds, BARTER_LIKES + 2, rw(ds, 0x483a));
    ww(ds, BARTER_DISLIKES, (uint16_t)(rw(ds, 0x4838) + top[-1] * 2));
    ww(ds, BARTER_DISLIKES + 2, rw(ds, 0x483a));
    return 1;
}

/* rand_percent_jitter(value, lo, hi): value moved by a percentage
 * drawn in [lo, hi) -- lo + rand() * (hi - lo) / 0x8000 in 32 bits -- the
 * product and the division by 100 in sixteen. */
static int16_t rand_percent_jitter(uw_convbi *b, int16_t value, int16_t lo, int16_t hi) {
    int32_t r = rand15(b->ds);
    int16_t pct = (int16_t)(lo + (int16_t)((r * (int32_t)(int16_t)(hi - lo)) / 0x8000));
    return (int16_t)(value + (int16_t)(value * pct) / 100);
}

/* barter_item_price(whose, idx, jitter): the obj_properties value
 * -- for the player's side (whose nonzero) 0 when the NPC dislikes it and
 * half again when it likes it -- times a stack's count, scaled by quality / 64
 * (at least 1; 0 for quality 0), then jittered with rt_rand seeded from the
 * object's index, and rt_rand reseeded from the clock after. */
static int16_t barter_item_price(uw_convbi *b, int whose, uint16_t idx, int16_t jitter) {
    uint8_t *ds = b->ds, *ls = b->level->lseg;
    uint16_t o = uw_motion_obj_at(b->level, idx), w0 = rw(ls, o);
    uint16_t q = (uint16_t)((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff);
    int16_t price, n, qual;
    if (whose) {
        int16_t appeal = barter_item_appeal(b, idx);
        if (appeal == -1) return 0;
        price = (int16_t)rw(ds, (uint16_t)(OBJ_PROPERTIES_VALUE + (w0 & 0x1ff) * 0xb));
        if (appeal) price = (int16_t)((int16_t)(price * 3) >> 1);
    } else {
        price = (int16_t)rw(ds, (uint16_t)(OBJ_PROPERTIES_VALUE + (w0 & 0x1ff) * 0xb));
    }
    n = (int16_t)((w0 & 0x8000) && !(q & 0x200) ? q : 1);
    price = (int16_t)(price * n);
    qual = (int16_t)(ls[(uint16_t)(o + 4)] & 0x3f);
    if (price > 0) {
        if (qual > 0) {
            price = (int16_t)((int16_t)(price * qual) >> 6);
            if (!price) price = 1;
        } else {
            price = 0;
        }
    }
    ww(ds, RAND_SEED, idx);                    /* rt_srand(idx) */
    ww(ds, RAND_SEED + 2, 0);
    price = rand_percent_jitter(b, price, (int16_t)-jitter, jitter);
    ww(ds, RAND_SEED, b->clock_low);           /* rt_srand(time(0)) */
    ww(ds, RAND_SEED + 2, 0);
    return price;
}

/* barter_side_total(whose, slots, flags, cache, jitter): the sum
 * of the prices of the side's flagged objects, each priced once into its
 * cache word (0xffff empty). */
static int16_t barter_side_total(uw_convbi *b, int whose, uint16_t slots, uint16_t flags, uint16_t cache, int16_t jitter) {
    uint8_t *ds = b->ds;
    int16_t total = 0;
    int i;
    for (i = 0; i < 4; i++) {
        int16_t idx = (int16_t)rw(ds, (uint16_t)(slots + i * 2));
        if ((int8_t)ds[(uint16_t)(flags + i)] <= 0 || idx <= 0) continue;
        if (rw(ds, (uint16_t)(cache + i * 2)) == 0xffff)
            ww(ds, (uint16_t)(cache + i * 2), (uint16_t)barter_item_price(b, whose, (uint16_t)idx, jitter));
        total = (int16_t)(total + (int16_t)rw(ds, (uint16_t)(cache + i * 2)));
    }
    return total;
}

static int16_t barter_decline(uw_convbi *b, int keep_offered);
static int16_t barter_exchange(uw_convbi *b);
static void get_variable(const uw_convbi *b, const uw_convvm *vm, const char *name, int16_t *out);

/* barter_add_to_npc_inventory(obj): into the talker's inventory
 * -- gold (item 0xa1) merged into the first stack of gold in the chain whose
 * count and its own are plain counts (bit 9 clear) adding up to under 999,
 * the given stack freed; anything else inserted at the chain's head. */
static void barter_add_to_npc_inventory(uw_convbi *b, uint16_t o) {
    uint8_t *ls = b->level->lseg;
    uint16_t t = rw(b->ds, TALKER_PTR), cur;
    if ((rw(ls, o) & 0x1ff) == 0xa1) {
        for (cur = uw_motion_deref_link(b->level, (uint16_t)(t + 6)); cur;
             cur = uw_motion_deref_link(b->level, (uint16_t)(cur + 4))) {
            uint16_t qo = (uint16_t)((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff);
            uint16_t qc = (uint16_t)((rw(ls, (uint16_t)(cur + 6)) >> 6) & 0x3ff);
            if (!(rw(ls, o) & 0x8000) || !(rw(ls, cur) & 0x8000) || (qo & 0x200) || (qc & 0x200)
                || (rw(ls, o) & 0x1ff) != (rw(ls, cur) & 0x1ff) || (uint16_t)(qo + qc) >= 999)
                continue;
            ww(ls, (uint16_t)(cur + 6), (uint16_t)((rw(ls, (uint16_t)(cur + 6)) & 0x3f) | (((qo + qc) & 0x3ff) << 6)));
            uw_motion_obj_free(b->level, o);
            return;
        }
    }
    uw_motion_object_list_insert(b->level, (uint16_t)(t + 6), o);
}

/* barter_exchange: what the player put on the table and marked
 * (a nonzero flag) goes into the talker's inventory the way
 * barter_add_to_npc_inventory puts it, unless the NPC will not deal in it
 * (barter_item_appeal -1, which leaves it on the table). The slot's saved
 * background is restored, the slot and its flag
 * cleared and its mark drawn. There is no NPC-to-player half. AX is
 * ems_frame_remap's. */
static int16_t barter_exchange(uw_convbi *b) {
    uint8_t *ds = b->ds;
    int si;
    for (si = 0; si < 4; si++) {
        int16_t idx = (int16_t)rw(ds, (uint16_t)(PLAYER_SLOTS + si * 2));
        if (idx <= 0 || !ds[(uint16_t)(PLAYER_SLOT_OFFERED + si)]) continue;
        if (barter_item_appeal(b, (uint16_t)idx) == -1) continue;
        barter_add_to_npc_inventory(b, uw_motion_obj_at(b->level, (uint16_t)idx));
        slot_background_restore(b, rw(ds, (uint16_t)(0x48b6 + si * 2)));
        ds[(uint16_t)(PLAYER_SLOT_OFFERED + si)] = 0;
        ww(ds, (uint16_t)(PLAYER_SLOTS + si * 2), 0);
        draw_slot_frame(b, 1, si);
    }
    return ems_frame_remap(b);
}

/* barter_collect_selected(ids, indices) -> how many: for each of
 * the player's four slots whose flag is set, its object index and item id,
 * packed from 0. The slot's index is not checked for being there. */
static int barter_collect_selected(uw_convbi *b, uint16_t *ids, uint16_t *indices) {
    uint8_t *ds = b->ds;
    int si, n = 0;
    for (si = 0; si < 4; si++) {
        uint16_t idx;
        if (!ds[(uint16_t)(PLAYER_SLOT_OFFERED + si)]) continue;
        idx = rw(ds, (uint16_t)(PLAYER_SLOTS + si * 2));
        indices[n] = idx;
        ids[n] = (uint16_t)(rw(b->level->lseg, uw_motion_obj_at(b->level, idx)) & 0x1ff);
        n++;
    }
    return n;
}

/* conv_mem_set: an index doubled in sixteen bits. */
static void mem_set(uw_convvm *vm, uint16_t i, uint16_t v) {
    i &= 0x7fff;
    if (i < vm->mem_words) vm->mem[i] = v;
}

/* show_inv(&ids, &indices) -> the count: the player's marked
 * goods, item ids into the first array and object indices into the second,
 * the rest of the four entries zeroed in both. */
static int16_t bi_show_inv(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint16_t ids[4], indices[4];
    int n, si;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    n = barter_collect_selected(b, ids, indices);
    for (si = 0; si < 4; si++) {
        if (si < n) {
            mem_set(vm, (uint16_t)(top[-2] + si), ids[si]);
            mem_set(vm, (uint16_t)(top[-1] + si), indices[si]);
        } else {
            mem_set(vm, (uint16_t)(top[-1] + si), 0);
            mem_set(vm, (uint16_t)(top[-2] + si), 0);
        }
    }
    return (int16_t)n;
}

/* barter_give_slot_item_to_npc(idx): the object into the
 * talker's inventory, and every player slot holding that index emptied --
 * its saved background restored, slot and flag cleared and its mark
 * drawn. AX is ems_frame_remap's. */
static int16_t barter_give_slot_item_to_npc(uw_convbi *b, uint16_t idx) {
    uint8_t *ds = b->ds;
    int si;
    barter_add_to_npc_inventory(b, uw_motion_obj_at(b->level, idx));
    for (si = 0; si < 4; si++) {
        if (rw(ds, (uint16_t)(PLAYER_SLOTS + si * 2)) != idx) continue;
        slot_background_restore(b, rw(ds, (uint16_t)(0x48b6 + si * 2)));
        ww(ds, (uint16_t)(PLAYER_SLOTS + si * 2), 0);
        ds[(uint16_t)(PLAYER_SLOT_OFFERED + si)] = 0;
        draw_slot_frame(b, 1, si);
    }
    return ems_frame_remap(b);
}

/* give_to_npc(count, &indices) -> 1 when the player's marked
 * goods include EACH listed OBJECT INDEX (the list is show_inv's second
 * array, not item ids), every listed one matched to a distinct slot, and then
 * each is given to the talker; 0, and nothing given, when fewer are marked
 * than listed or one is not there. */
static int16_t bi_give_to_npc(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint16_t ids[4], indices[4];
    int16_t slot_of[4] = { -1, -1, -1, -1 }, list_of[4] = { -1, -1, -1, -1 };
    int n, sel, di, si;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    n = arg(vm, top, 2);
    sel = barter_collect_selected(b, ids, indices);
    if (sel < n) return 0;
    for (di = 0; di < n && di < 4; di++) {
        for (si = 0; si < sel; si++) {
            if (mem_get(vm, (uint16_t)(top[-1] + di)) == indices[si] && slot_of[si] == -1) {
                list_of[di] = (int16_t)si;
                slot_of[si] = (int16_t)di;
                break;
            }
        }
        if (list_of[di] == -1) return 0;
    }
    for (si = 0; si < n; si++)
        barter_give_slot_item_to_npc(b, mem_get(vm, (uint16_t)(top[-1] + si)));
    return 1;
}

/* give_ptr_npc(idx, quantity): an object among the player's
 * marked goods is given with barter_give_slot_item_to_npc: 1; any other
 * comes out of the player's inventory with
 * inventory_remove_quantity (the quantity for a plain stack when it is not
 * negative, else all of it) and into the talker's
 * (barter_add_to_npc_inventory): 1, or 0 when the removal failed. The slot
 * scan reads all four entries of barter_collect_selected's arrays, the ones
 * past the count being whatever the stack held; here they read as empty. */
static int16_t bi_give_ptr_npc(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint16_t ids[4] = { 0, 0, 0, 0 }, indices[4] = { 0, 0, 0, 0 };
    uint16_t idx = (uint16_t)arg(vm, top, 2), o, q;
    int16_t count = arg(vm, top, 1);
    int si;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    barter_collect_selected(b, ids, indices);
    for (si = 0; si < 4; si++)
        if (ids[si] && indices[si] == idx) {
            barter_give_slot_item_to_npc(b, idx);
            return 1;
        }
    o = uw_motion_obj_at(b->level, idx);
    q = (uint16_t)((rw(b->level->lseg, (uint16_t)(o + 6)) >> 6) & 0x3ff);
    if (count < 0 || !(rw(b->level->lseg, o) & 0x8000) || (q & 0x200)) count = -1;
    if (!uw_motion_inventory_remove_quantity(b->level, o, count)) return 0;
    barter_add_to_npc_inventory(b, o);
    return 1;
}

/* place_object(idx, x, y) -> 1 when placed: the object taken out
 * of the talker's inventory chain when it is there, then -- for x under 0 --
 * set down at the player's tile (its fine position 0, its height) through
 * object_move_to_coords with a spread of 6, kept, and object_place_scatter
 * set for the call; for x and y in 1..63, raised to that tile's floor and put
 * there when item_fits_in_tile allows it (slopes allowed, a radius of the
 * props' +1 & 7 plus 4): appended to the tile's chain and settled by
 * placed_object_collision, tossed. Anything else, or no room: 0, and the
 * object left out of every chain. */
static int16_t bi_place_object(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    enum { FRAME = 0x9540, LOCALS = 0x0e + 4 };
    uint8_t *ds = b->ds, *ls;
    uint16_t idx = (uint16_t)arg(vm, top, 3), o, at, tp, id, talker = rw(ds, TALKER_PTR);
    int16_t x = arg(vm, top, 2), y = arg(vm, top, 1), floor;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    ls = b->level->lseg;
    o = uw_motion_obj_at(b->level, idx);
    at = (uint16_t)(talker + 6);
    while ((rw(ls, at) >> 6) & 0x3ff && ((rw(ls, at) >> 6) & 0x3ff) != idx)
        at = (uint16_t)(uw_motion_deref_link(b->level, at) + 4);
    if ((rw(ls, at) >> 6) & 0x3ff) uw_motion_object_list_remove(b->level, (uint16_t)(talker + 6), o);
    if (x < 0) {
        uint16_t p = rw(ds, PLAYER_OBJECT_PTR), w16 = rw(ls, (uint16_t)(p + 0x16));
        ds[0x02d5] = 1;                            /* object_place_scatter */
        uw_motion_object_move_to_coords(b->level, (int16_t)((w16 >> 10) << 3), (int16_t)(((w16 & 0x3f0) >> 4) << 3),
                                        (int16_t)(rw(ls, (uint16_t)(p + 2)) & 0x7f), o, 6, 1,
                                        (uint16_t)(FRAME - LOCALS - 14 - 4 - 2));
        ds[0x02d5] = 0;
        return 1;
    }
    if (x < 1 || x >= 0x40 || y < 1 || y >= 0x40) return 0;
    tp = uw_motion_tile_ptr(b->level, (uint16_t)x, (uint16_t)y);
    floor = (int16_t)(((ls[tp] >> 4) & 0xf) << 3);
    ww(ls, (uint16_t)(o + 2), (uint16_t)((rw(ls, (uint16_t)(o + 2)) & 0xff80) | (floor & 0x7f)));
    id = (uint16_t)(rw(ls, o) & 0x1ff);
    if (!uw_motion_item_fits_in_tile(b->level, id, idx, (int16_t)(x << 3), (int16_t)(y << 3), floor, 1,
                                     (uint8_t)((ds[(uint16_t)(0x5b6f + id * 0xb)] & 7) + 4),
                                     (uint16_t)(FRAME - LOCALS - 14 - 4 - 2)))
        return 0;
    uw_motion_object_list_append(b->level, (uint16_t)(tp + 2), o);
    uw_motion_placed_object_collision(b->level, o, (uint16_t)x, (uint16_t)y, 1, (uint16_t)(FRAME - LOCALS - 10 - 4 - 2));
    return 1;
}

/* count_inv(idx) -> a plain stack's count (word 0 bit 15 set,
 * the count's bit 9 clear), else 1. */
static int16_t bi_count_inv(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint8_t *ls;
    uint16_t o, q;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    ls = b->level->lseg;
    o = uw_motion_obj_at(b->level, (uint16_t)arg(vm, top, 1));
    q = (uint16_t)((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff);
    return (int16_t)((rw(ls, o) & 0x8000) && !(q & 0x200) ? q : 1);
}

/* check_inv_quality(idx) -> word 2's low six bits;
 * set_inv_quality(idx, quality) writes them and answers 1. Held
 * by conv34 and conv35, Shak's repair. */
static int16_t bi_check_inv_quality(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    return (int16_t)(b->level->lseg[(uint16_t)(uw_motion_obj_at(b->level, (uint16_t)arg(vm, top, 1)) + 4)] & 0x3f);
}

static int16_t bi_set_inv_quality(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint8_t *ls;
    uint16_t o;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    ls = b->level->lseg;
    o = uw_motion_obj_at(b->level, (uint16_t)arg(vm, top, 2));
    ls[(uint16_t)(o + 4)] = (uint8_t)((ls[(uint16_t)(o + 4)] & 0xc0) | (arg(vm, top, 1) & 0x3f));
    return 1;
}

/* do_inv_create(id) -> barter_npc_create_item: a new
 * object of the id (object_create, not mobile) at quality 63 into the
 * talker's inventory -- a stack merged into a matching plain stack in the
 * chain under 999, as gold is -- and its index, 0 when the pool is full or
 * the new stack was merged away. Held by conv27 and conv28 (Judy's key). */
static int16_t bi_do_inv_create(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint8_t *ls;
    uint16_t t, o, cur;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    ls = b->level->lseg;
    t = rw(b->ds, TALKER_PTR);
    o = uw_motion_object_create(b->level, (uint16_t)arg(vm, top, 1), 0);
    if (!o) return 0;
    ls[(uint16_t)(o + 4)] = (uint8_t)((ls[(uint16_t)(o + 4)] & 0xc0) | 0x3f);
    for (cur = uw_motion_deref_link(b->level, (uint16_t)(t + 6)); cur;
         cur = uw_motion_deref_link(b->level, (uint16_t)(cur + 4))) {
        uint16_t qo = (uint16_t)((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff);
        uint16_t qc = (uint16_t)((rw(ls, (uint16_t)(cur + 6)) >> 6) & 0x3ff);
        if (!(rw(ls, o) & 0x8000) || !(rw(ls, cur) & 0x8000) || (qo & 0x200) || (qc & 0x200)
            || (rw(ls, o) & 0x1ff) != (rw(ls, cur) & 0x1ff) || (uint16_t)(qo + qc) >= 999)
            continue;
        ww(ls, (uint16_t)(cur + 6), (uint16_t)((rw(ls, (uint16_t)(cur + 6)) & 0x3f) | (((qo + qc) & 0x3ff) << 6)));
        uw_motion_obj_free(b->level, o);
        return 0;
    }
    uw_motion_object_list_insert(b->level, (uint16_t)(t + 6), o);
    return (int16_t)uw_motion_obj_index(b->level, o);
}

/* do_inv_delete(id) -> barter_npc_remove_item: the
 * first object of that item id in the talker's inventory (not searched
 * inside containers) unlinked and freed: 1, or 0. */
static int16_t bi_do_inv_delete(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint8_t *ls;
    uint16_t t, id, o;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    ls = b->level->lseg;
    t = rw(b->ds, TALKER_PTR);
    id = (uint16_t)arg(vm, top, 1);
    for (o = uw_motion_deref_link(b->level, (uint16_t)(t + 6)); o; o = uw_motion_deref_link(b->level, (uint16_t)(o + 4))) {
        if ((rw(ls, o) & 0x1ff) != id) continue;
        uw_motion_object_list_remove(b->level, (uint16_t)(t + 6), o);
        uw_motion_obj_free(b->level, o);
        return 1;
    }
    return 0;
}

/* x_obj_stuff(idx, mode, &heading, &owner, &flags, &count, &bit10,
 * &bit9, &quality): each slot not holding -1 is READ into (mode 0) or
 * WRITTEN from (else) the object's field -- the heading word 1 bits 7..9
 * (not for a door or an obj_properties +9 kind 2), word 3's low six bits,
 * word 0 bits 9..12, word 3's bits 6..15 (read masked to nine bits, written
 * with bit 9 set: always a count), word 0 bit 10 and bit 9 (read as the bit
 * itself, 0x400 or 0x200), and word 2's low six bits. AX follows the last
 * value the instructions computed, which CALLI stores. */
static int16_t bi_x_obj_stuff(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint8_t *ds = b->ds, *ls;
    uint16_t p[7], o, w0, ax;
    int k;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    ls = b->level->lseg;
    for (k = 0; k < 7; k++) p[k] = (uint16_t)(top[-7 + k] & 0x7fff);
    o = uw_motion_obj_at(b->level, (uint16_t)arg(vm, top, 9));
    ax = (uint16_t)arg(vm, top, 8);
#define SLOT(k) (p[k] < vm->mem_words ? vm->mem[p[k]] : 0)
#define SET(k, v) do { if (p[k] < vm->mem_words) vm->mem[p[k]] = (uint16_t)(v); } while (0)
    w0 = rw(ls, o);
    if (ax) {
        if (SLOT(0) != 0xffff) {
            ax = (uint16_t)((w0 & 0x1c0) >> 6);
            if (ax != 5) {
                ax = (uint16_t)(ds[(uint16_t)(0x5b77 + (w0 & 0x1ff) * 0xb)] & 3);
                if (ax != 2) {
                    ax = (uint16_t)((rw(ls, (uint16_t)(o + 2)) & 0xfc7f) | ((SLOT(0) & 7) << 7));
                    ww(ls, (uint16_t)(o + 2), ax);
                }
            }
        }
        if (SLOT(1) != 0xffff) {
            ax = (uint16_t)(SLOT(1) & 0x3f);
            ls[(uint16_t)(o + 6)] = (uint8_t)((ls[(uint16_t)(o + 6)] & 0xc0) | ax);
        }
        if (SLOT(2) != 0xffff) {
            ax = (uint16_t)((SLOT(2) & 0xf) << 9);
            ww(ls, o, (uint16_t)((rw(ls, o) & 0xe1ff) | ax));
        }
        if (SLOT(3) != 0xffff) {
            ax = (uint16_t)(((SLOT(3) | 0x200) & 0x3ff) << 6);
            ww(ls, (uint16_t)(o + 6), (uint16_t)((rw(ls, (uint16_t)(o + 6)) & 0x3f) | ax));
        }
        if (SLOT(4) != 0xffff) {
            ax = (uint16_t)((SLOT(4) & 1) << 10);
            ww(ls, o, (uint16_t)((rw(ls, o) & 0xfbff) | ax));
        }
        if (SLOT(5) != 0xffff) {
            ax = (uint16_t)((SLOT(5) & 1) << 9);
            ww(ls, o, (uint16_t)((rw(ls, o) & 0xfdff) | ax));
        }
        if (SLOT(6) != 0xffff) {
            ax = (uint16_t)(SLOT(6) & 0x3f);
            ls[(uint16_t)(o + 4)] = (uint8_t)((ls[(uint16_t)(o + 4)] & 0xc0) | ax);
        }
    } else {
        if (SLOT(0) != 0xffff) {
            ax = (uint16_t)((w0 & 0x1c0) >> 6);
            if (ax != 5) {
                ax = (uint16_t)(ds[(uint16_t)(0x5b77 + (w0 & 0x1ff) * 0xb)] & 3);
                if (ax != 2) {
                    ax = (uint16_t)((rw(ls, (uint16_t)(o + 2)) & 0x380) >> 7);
                    SET(0, ax);
                }
            }
        }
        if (SLOT(1) != 0xffff) { ax = (uint16_t)(ls[(uint16_t)(o + 6)] & 0x3f); SET(1, ax); }
        if (SLOT(2) != 0xffff) { ax = (uint16_t)((w0 & 0x1e00) >> 9); SET(2, ax); }
        if (SLOT(3) != 0xffff) { ax = (uint16_t)((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x1ff); SET(3, ax); }
        if (SLOT(4) != 0xffff) { ax = (uint16_t)(w0 & 0x400); SET(4, ax); }
        if (SLOT(5) != 0xffff) { ax = (uint16_t)(w0 & 0x200); SET(5, ax); }
        if (SLOT(6) != 0xffff) { ax = (uint16_t)(ls[(uint16_t)(o + 4)] & 0x3f); SET(6, ax); }
    }
#undef SLOT
#undef SET
    return (int16_t)ax;
}

/* find_inv(where, what) -> barter_find_inv: the
 * object index of the first thing in the player's inventory (where nonzero)
 * or the talker's that is `what` -- an item id below 1000 (class, subclass
 * and type all matched) or 1000 plus id >> 4 (class and subclass) -- searched
 * with object_find_matching, containers' contents included; 0 when none. */
static int16_t bi_find_inv(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint8_t *ds = b->ds;
    int16_t what = arg(vm, top, 2), where = arg(vm, top, 1);
    uint16_t owner, link, cls, sub, type;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    if (where) {
        owner = rw(ds, PLAYER_OBJECT_PTR);
    } else {
        owner = rw(ds, TALKER_PTR);
        uw_motion_spawn_npc_loot(b->level, owner);   /* the talker's goods, made once */
    }
    link = (uint16_t)(owner + 6);
    if (what > 999) {
        cls = (uint16_t)((int16_t)(what - 1000) >> 2);
        sub = (uint16_t)((what - 1000) & 3);
        type = 0xffff;
    } else {
        cls = (uint16_t)(what >> 6);
        sub = (uint16_t)((what & 0x30) >> 4);
        type = (uint16_t)(what & 0xf);
    }
    return (int16_t)uw_motion_obj_index(b->level, uw_motion_find_matching(b->level, &link, 1, cls, sub, type));
}

/* find_barter(what) -> the object index of the first of the
 * player's marked goods that is `what` -- an item id below 1000, or 1000
 * plus id >> 4, class and subclass -- or 0. */
static int16_t bi_find_barter(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint16_t ids[4], indices[4];
    int16_t what = arg(vm, top, 1);
    int n, si;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    n = barter_collect_selected(b, ids, indices);
    for (si = 0; si < n; si++) {
        if (what < 1000 ? (int16_t)ids[si] == what
                        : ((int16_t)ids[si] >> 6) == ((int16_t)(what - 1000) >> 2)
                          && (((int16_t)ids[si] & 0x30) >> 4) == ((what - 1000) & 3))
            return (int16_t)indices[si];
    }
    return 0;
}

/* find_barter_total(what, &count, indices, &total) -> whether
 * the total is positive: the player's marked goods whose item id is `what`,
 * their object indices written from `indices` on, how many into `count`, and
 * their quantities -- a plain stack's count, else 1 -- summed into `total`.
 * The loop runs only for `what` below 1000, so the kind form its body also
 * tests is never reached: 1000 and up finds nothing. */
static int16_t bi_find_barter_total(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint16_t ids[4], indices[4], found[4];
    int16_t what = arg(vm, top, 4), total = 0;
    int n, si, count = 0;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    n = barter_collect_selected(b, ids, indices);
    if (what < 1000) {
        for (si = 0; si < n; si++) {
            uint8_t *ls = b->level->lseg;
            uint16_t o, q;
            if ((int16_t)ids[si] != what) continue;
            o = uw_motion_obj_at(b->level, indices[si]);
            found[count++] = indices[si];
            q = (uint16_t)((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff);
            total = (int16_t)(total + ((rw(ls, o) & 0x8000) && !(q & 0x200) ? q : 1));
        }
    }
    mem_set(vm, top[-3], (uint16_t)count);
    mem_set(vm, top[-1], (uint16_t)total);
    for (si = 0; si < count; si++)
        mem_set(vm, (uint16_t)(top[-2] + si), found[si]);
    return (int16_t)(total > 0);
}

/* do_offer(nothing, out_of_patience, worse, closer, accept), the
 * last pushed first: patience below 0 says out_of_patience; an empty side
 * says nothing and clears barter_deal_done; else the percentage the player's
 * side (priced for the player) comes to over the NPC's -- 100 when the NPC's
 * is not positive -- at or past the threshold says accept, takes back the
 * NPC's unoffered goods, exchanges (not carried here) and sets the deal:
 * 1. Otherwise, the first offer says closer and costs a point of patience
 * unless twice the percentage reaches the threshold; a later one worse
 * than the last says worse and costs two; a better one says closer and
 * costs one when (threshold - last) * 3 / 2 is past threshold - pct. The
 * percentage is kept: 0. */
static int16_t bi_do_offer(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint8_t *ds = b->ds;
    int16_t pct, offered, wanted, jitter;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    if ((int16_t)rw(ds, BARTER_PATIENCE) < 0) {
        barter_print(b, 0, (uint16_t)arg(vm, top, 2));
        return 0;
    }
    if (barter_side_is_empty(b, PLAYER_SLOTS, PLAYER_SLOT_OFFERED) || barter_side_is_empty(b, NPC_SLOTS, NPC_SLOT_OFFERED)) {
        barter_print(b, 0, (uint16_t)arg(vm, top, 1));
        ds[BARTER_DEAL_DONE] = 0;
        return 0;
    }
    jitter = (int16_t)rw(ds, BARTER_NPC_JITTER);
    offered = barter_side_total(b, 1, PLAYER_SLOTS, PLAYER_SLOT_OFFERED, 0x48ae, jitter);
    wanted = barter_side_total(b, 0, NPC_SLOTS, NPC_SLOT_OFFERED, 0x4896, jitter);
    pct = (int16_t)(wanted > 0 ? (int16_t)((offered - wanted) * 100) / wanted : 100);
    if (pct >= (int16_t)rw(ds, BARTER_THRESHOLD)) {
        barter_print(b, 0, (uint16_t)arg(vm, top, 5));
        barter_decline(b, 1);
        barter_exchange(b);
        ds[BARTER_DEAL_DONE] = 1;
        return 1;
    }
    if (rw(ds, BARTER_LAST_PCT) == 0) {
        if ((int16_t)(pct * 2) < (int16_t)rw(ds, BARTER_THRESHOLD)) {
            barter_print(b, 0, (uint16_t)arg(vm, top, 4));
            ww(ds, BARTER_PATIENCE, (uint16_t)(rw(ds, BARTER_PATIENCE) - 1));
        }
    } else if (pct < (int16_t)rw(ds, BARTER_LAST_PCT)) {
        barter_print(b, 0, (uint16_t)arg(vm, top, 3));
        ww(ds, BARTER_PATIENCE, (uint16_t)(rw(ds, BARTER_PATIENCE) - 2));
    } else if ((int16_t)((int16_t)(rw(ds, BARTER_THRESHOLD) - rw(ds, BARTER_LAST_PCT)) * 3) / 2
               > (int16_t)(rw(ds, BARTER_THRESHOLD) - pct)) {
        barter_print(b, 0, (uint16_t)arg(vm, top, 4));
        ww(ds, BARTER_PATIENCE, (uint16_t)(rw(ds, BARTER_PATIENCE) - 1));
    }
    ww(ds, BARTER_LAST_PCT, (uint16_t)pct);
    return 0;
}

/* do_demand(refuse, accept), the last pushed first. The player's
 * weight is the character level (record +0x3d), the stance bit (+0x5f bit
 * 1), a health term and Charm (+0x30) / 6; the NPC's its critter row's +0x0d
 * low nibble, -1 for an ally or +1 below attitude 2, its own health term and
 * its side of the table / 10. A health term is 2 - 2 * (full - now) / full,
 * or 1 when full is 0 -- the player's full the byte +4 of its critter row
 * and
 * its now the record's +0x36 against the object's +8 (sic, the difference
 * the instructions take). The player heavier, or an ally: accept, the NPC's
 * unoffered goods taken back, the deal done, npc_attitude one lower when
 * positive: 1. Else refuse, all its goods taken back, and the talker's goal
 * made 5 against the Avatar: 0. */
static int16_t bi_do_demand(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint8_t *ds = b->ds, *ls;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), t = rw(ds, TALKER_PTR), row, w0;
    int16_t si, player, npc, total, att = 0, di, full;
    size_t k;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    ls = b->level->lseg;
    w0 = rw(ls, t);
    row = (uint16_t)(CRITTER_ROW + (w0 & 0x3f) * 0x30);
    full = ds[(uint16_t)(rw(ds, 0x7272) + 4)];
    si = (int16_t)(full ? 2 - (int16_t)((int16_t)(ds[(uint16_t)(rec + 0x36)] - ls[(uint16_t)(rw(ds, PLAYER_OBJECT_PTR) + 8)]) * 2) / full : 1);
    player = (int16_t)(ds[(uint16_t)(rec + 0x3d)] + ((ds[(uint16_t)(rec + 0x5f)] >> 1) & 1) + si + ds[(uint16_t)(rec + 0x30)] / 6);
    total = barter_side_total(b, 0, NPC_SLOTS, NPC_SLOT_OFFERED, 0x4896, (int16_t)rw(ds, BARTER_NPC_JITTER));
    full = ds[(uint16_t)(row + 4)];
    si = (int16_t)(full ? 2 - (int16_t)((int16_t)(full - ls[(uint16_t)(t + 8)]) * 2) / full : 1);
    get_variable(b, vm, "npc_attitude", &att);
    if (ls[(uint16_t)(t + 0x19)] & 0x40) di = -1;
    else di = (int16_t)(att < 2 ? 1 : 0);
    npc = (int16_t)((ds[(uint16_t)(row + 0xd)] & 0xf) + di + si + total / 10);
    if (player > npc || (ls[(uint16_t)(t + 0x19)] & 0x40)) {
        barter_print(b, 0, (uint16_t)arg(vm, top, 2));
        barter_decline(b, 1);
        ds[BARTER_DEAL_DONE] = 1;
        if (att > 0) {
            att--;
            for (k = 0; k < b->nvars; k++)       /* conv_set_variable */
                if (!strcmp(b->vars[k].name, "npc_attitude") && b->vars[k].addr < vm->mem_words)
                    vm->mem[b->vars[k].addr] = (uint16_t)att;
        }
        return 1;
    }
    barter_print(b, 0, (uint16_t)arg(vm, top, 1));
    barter_decline(b, 0);
    uw_motion_creature_set_goal_for(b->level, t, 5, 1);
    return 0;
}

/* do_judgement(): both sides priced for the NPC with a jitter of
 * 50 - Appraise (record +0x33) * 45 / 30, the percentage as do_offer takes
 * it, and a line put together from strings 0xe03..0xe07 (the Appraise level
 * in sixes), 0xe02 and 0xe08..0xe10 (the percentage's band) printed as the
 * player's line through conv_print_npc_line. AX is ems_frame_remap's. */
static int16_t bi_do_judgement(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint8_t *ds = b->ds;
    int16_t jitter, offered, wanted, pct, level, band, appraise;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    char text[0x60], *s;
    (void)vm; (void)top;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    appraise = ds[(uint16_t)(rec + 0x33)];
    jitter = (int16_t)(0x32 - (int16_t)(appraise * 0x2d) / 0x1e);
    offered = barter_side_total(b, 0, PLAYER_SLOTS, PLAYER_SLOT_OFFERED, 0x48a6, jitter);
    wanted = barter_side_total(b, 0, NPC_SLOTS, NPC_SLOT_OFFERED, 0x488e, jitter);
    pct = (int16_t)(wanted > 0 ? (int16_t)((offered - wanted) * 100) / wanted : 100);
    level = (int16_t)(appraise < 6 ? 0 : appraise < 12 ? 1 : appraise < 18 ? 2 : appraise < 24 ? 3 : 4);
    band = (int16_t)(pct > 50 ? 0 : pct > 35 ? 1 : pct > 25 ? 2 : pct > 10 ? 3 : pct > -10 ? 4
                     : pct > -25 ? 5 : pct > -35 ? 6 : pct > -50 ? 7 : 8);
    /* three strings of block 7 into a 0x5a-byte stack buffer, unchecked */
    s = get_string(b, (uint16_t)((level + 3) | 0xe00));
    snprintf(text, sizeof text, "%s", s ? s : "");
    s = get_string(b, 0xe02);
    strncat(text, s ? s : "", sizeof text - strlen(text) - 1);
    s = get_string(b, (uint16_t)((band + 8) | 0xe00));
    strncat(text, s ? s : "", sizeof text - strlen(text) - 1);
    compose_line(b, "\\1", text, "\\0\n");     /* conv_print_npc_line */
    print_line(b, 1);
    uw_convbi_spoke(b, 1);
    return ems_frame_remap(b);
}

/* print(string): the string expanded and printed in the NPC's
 * window between the colour escapes \2 and \0 (and a newline) -- the
 * interface's. AX, which CALLI
 * stores, is the expansion's segment: the text's own when there was nothing
 * to expand (the ring's, for a STRINGS.PAK string), farheap_free's when a
 * copy was freed, which depends on the far heap's blocks and is counted.
 * No UW1 conversation reaches the copy: its 53 calls of print each pass a
 * literal string, and none of those has an '@'. */
static int16_t bi_print(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    char *raw = get_string(b, (uint16_t)arg(vm, top, 1)), *e;
    if (!raw) return 0;
    e = uw_convbi_expand(b, vm, raw);
    compose_line(b, "\\2", e, "\\0\n");
    print_line(b, 1);
    if (e != raw) {
        free(e);
        UW_NOT_CARRIED(b->not_carried);
        return 0;
    }
    return (int16_t)uw_strcache_segment(b->strings, raw);
}

/* barter_take_from_npc, the worker of take_from_npc
 * whose AX the builtin leaves: with nothing held, the first object in the
 * talker's inventory that is `what` -- an item id below 1000, or 1000 plus
 * item id >> 4 -- taken out of the chain (unlinked from the talker's +6) and
 * put on the cursor when the player can carry it (held_object_ptr,
 * the cursor's shape pushed, action_state 1): 1.
 * Too heavy, it goes into the first empty player-side trade slot (its flag and cached words reset; 1), else the talker drops it (2 or 3). 0
 * when nothing is held and nothing matched, or something is held. */
static int16_t barter_take_matching(uw_convbi *b, int16_t what, int by_index) {
    uint8_t *ds = b->ds, *ls = b->level->lseg;
    uint16_t t = rw(ds, TALKER_PTR), o;
    int si;
    if (rw(ds, 0x5b06) | rw(ds, 0x5b08)) return 0;
    if (!by_index) uw_motion_spawn_npc_loot(b->level, t);   /* the talker's goods, made once */
    for (o = uw_motion_deref_link(b->level, (uint16_t)(t + 6)); o; o = uw_motion_deref_link(b->level, (uint16_t)(o + 4))) {
        uint16_t w0 = rw(ls, o);
        if (by_index) {
            if (uw_motion_obj_index(b->level, o) != (uint16_t)what) continue;
        } else if (what > 999 ? ((w0 & 0x1f0) >> 4) != (uint16_t)(what - 1000) : (w0 & 0x1ff) != (uint16_t)what) {
            continue;
        }
        uw_motion_object_list_remove(b->level, (uint16_t)(t + 6), o);
        if (uw_motion_inventory_can_carry(b->level, o) & 0xff) {
            ww(ds, 0x5b06, o);
            ww(ds, 0x5b08, rw(ds, 0x3122));   /* the level segment's own */
            /* cursor_hide, cursor_shape_push(the item), action_state 1,
             * cursor_show */
            uw_motion_cursor_hide(b->level);
            uw_motion_cursor_shape_push(b->level, (uint16_t)(rw(ls, o) & 0x1ff));
            ww(ds, 0x26ac, 1);
            uw_motion_cursor_show(b->level);
            return 1;
        }
        for (si = 0; si < 4; si++) {
            if (rw(ds, (uint16_t)(0x486a + si * 2))) continue;
            ww(ds, (uint16_t)(0x486a + si * 2), uw_motion_obj_index(b->level, o));
            ds[(uint16_t)(0x487e + si)] = 0;                    /* not offered */
            ww(ds, (uint16_t)(0x4896 + si * 2), 0xffff);        /* the cached words reset */
            ww(ds, (uint16_t)(0x48a6 + si * 2), 0xffff);
            uw_motion_barter_draw_slot_frame(b->level, 0, si);
            uw_motion_barter_draw_slot(b->level, 1, si);
            return 1;
        }
        /* no slot free: object_place_at_own_coords(talker, obj, 5, 0) puts it
         * at the talker's feet, 2; nowhere, 3. The frame is the port's. */
        return uw_motion_object_place_at_own_coords(b->level, t, o, 5, 0, 0x9540) ? 2 : 3;
    }
    return 0;
}

static int16_t bi_take_from_npc(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    return barter_take_matching(b, arg(vm, top, 1), 0);
}

/* take_id_from_npc(idx) -> barter_take_id_from_npc:
 * barter_take_from_npc for the object with that INDEX, and no loot spawned
 * first. Held by conv30. */
static int16_t bi_take_id_from_npc(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    return barter_take_matching(b, arg(vm, top, 1), 1);
}

/* take_from_npc_inv(n) -> the object index of the n-th link of the
 * talker's inventory chain, the head counting as 0, or 0 when the chain ends
 * first. Takes nothing, whatever the name says. Held by conv30, where
 * Ishtass's take_from_npc_inv(1) names the object behind the scroll she was
 * just given. */
static int16_t bi_take_from_npc_inv(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint8_t *ls;
    uint16_t link;
    int16_t n, si;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    ls = b->level->lseg;
    link = (uint16_t)(rw(b->ds, TALKER_PTR) + 6);
    n = arg(vm, top, 1);
    for (si = 0; si < n && ((rw(ls, link) >> 6) & 0x3ff); si++)
        link = (uint16_t)(uw_motion_deref_link(b->level, link) + 4);
    return (int16_t)((rw(ls, link) >> 6) & 0x3ff);
}

/* conv_get_variable(name, &out, 1): the variable's word, or `out`
 * left as it was when the conversation does not import the name -- which in
 * conv_export_npc_vars means the previous variable's value, one stack word
 * serving them all. */
static void get_variable(const uw_convbi *b, const uw_convvm *vm, const char *name, int16_t *out) {
    size_t i;
    for (i = 0; i < b->nvars; i++)
        if (!strcmp(b->vars[i].name, name)) {
            if (b->vars[i].addr < vm->mem_words) *out = (int16_t)vm->mem[b->vars[i].addr];
            return;
        }
}

/* conv_set_variable(name, &value, 1): the value into the memory
 * word of the import of that name, or nowhere. It lower-cases the name into
 * a local and then compares the name as given. */
static void set_variable(const uw_convbi *b, uw_convvm *vm, const char *name, uint16_t value) {
    size_t i;
    for (i = 0; i < b->nvars; i++)
        if (!strcmp(b->vars[i].name, name)) {
            if (b->vars[i].addr < vm->mem_words) vm->mem[b->vars[i].addr] = value;
            return;
        }
}

/* A creature's health for a conversation: its hit points << 8 over its
 * critter row's +4, SIGNED -- the shifted byte is sign-extended into the
 * division, so 128 points or more come out negative -- or 0x80 for a row
 * of 0. */
static uint16_t health_of(uint8_t hp, uint8_t max) {
    if (!max) return 0x80;
    return (uint16_t)(int16_t)((int32_t)(int16_t)(uint16_t)(hp << 8) / (int32_t)max);
}

void uw_convbi_bind_npc_vars(uw_convbi *b, uw_convvm *vm, uint16_t t) {
    uint8_t *ds = b->ds, *ls;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), p = rw(ds, PLAYER_OBJECT_PTR), crit, w;
    uint32_t clock;
    uint8_t who;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return; }
    ls = b->level->lseg;
#define NAMED(at) ((const char *)ds + (at))
    crit = (uint16_t)((rw(ls, t) & 0x3f) * 0x30 + 0x4a52);
    who = ls[(uint16_t)(t + 0x1a)];
    set_variable(b, vm, NAMED(0x10f2), who);                                          /* npc_whoami */
    set_variable(b, vm, NAMED(0x10fd), (ls[(uint16_t)(t + 0x19)] & 0x80) ? 0x10 : 0xc0);  /* npc_hunger */
    set_variable(b, vm, NAMED(0x1108), health_of(ls[(uint16_t)(t + 8)], ds[(uint16_t)(crit + 4)]));
    set_variable(b, vm, NAMED(0x1113), ls[(uint16_t)(t + 8)]);                          /* npc_hp */
    set_variable(b, vm, NAMED(0x111a), (uint16_t)(int8_t)ds[(uint16_t)(crit + 0x13)]);  /* npc_arms */
    set_variable(b, vm, NAMED(0x1123), (uint16_t)(ds[(uint16_t)(crit + 5)] + ((ds[(uint16_t)(crit + 0x2d)] >> 1) & 0x7f)));
    w = rw(ls, (uint16_t)(t + 0xb));
    set_variable(b, vm, NAMED(0x112d), (uint16_t)(w & 0xf));                              /* npc_goal */
    set_variable(b, vm, NAMED(0x1136), (uint16_t)((w & 0xff0) >> 4));                     /* npc_gtarg */
    set_variable(b, vm, NAMED(0x1140), (uint16_t)((rw(ls, (uint16_t)(t + 0xd)) & 0x2000) >> 13));
    set_variable(b, vm, NAMED(0x114d), (uint16_t)(ds[(uint16_t)(crit + 0xd)] & 0xf));    /* npc_level */
    set_variable(b, vm, NAMED(0x1157), (uint16_t)(ls[(uint16_t)(t + 4)] & 0x3f));        /* npc_xhome */
    set_variable(b, vm, NAMED(0x1161), (uint16_t)(ls[(uint16_t)(t + 6)] & 0x3f));        /* npc_yhome */
    set_variable(b, vm, NAMED(0x116b), who ? (uint16_t)((who + 0x10) | 0xe00) : (uint16_t)((rw(ls, t) & 0x1ff) | 0x800));
    if ((w & 0xf) == 5 && ((w & 0xff0) >> 4) == 1)
        set_variable(b, vm, NAMED(0x1174), 0);                                            /* npc_attitude */
    else if (ls[(uint16_t)(t + 0x19)] & 0x40)
        set_variable(b, vm, NAMED(0x1174), 6);
    else
        set_variable(b, vm, NAMED(0x1174), (uint16_t)(rw(ls, (uint16_t)(t + 0xd)) >> 14));
    crit = (uint16_t)((rw(ls, p) & 0x3f) * 0x30 + 0x4a52);
    set_variable(b, vm, NAMED(0x1181), ds[(uint16_t)(rec + 0x39)]);                      /* play_hunger */
    set_variable(b, vm, NAMED(0x118d), health_of(ls[(uint16_t)(p + 8)], ds[(uint16_t)(crit + 4)]));
    set_variable(b, vm, NAMED(0x1199), ls[(uint16_t)(p + 8)]);                           /* play_hp */
    set_variable(b, vm, NAMED(0x11a1), (uint16_t)(ds[(uint16_t)(rec + 0x21)] + ds[(uint16_t)(rec + 0x1e)]));
    set_variable(b, vm, NAMED(0x11ab), (uint16_t)(ds[(uint16_t)(rec + 0x1f)] + ds[(uint16_t)(rec + 0x37)]
                                                  + ds[(uint16_t)(rec + 0x27)]));
    set_variable(b, vm, NAMED(0x11b6), ds[(uint16_t)(rec + 0x37)]);                      /* play_mana */
    set_variable(b, vm, NAMED(0x11c0), ds[(uint16_t)(rec + 0x3d)]);                      /* play_level */
    set_variable(b, vm, NAMED(0x11cb), rw(ds, 0x7278));
    clock = (uint32_t)rw(ds, (uint16_t)(rec + 0xce)) | (uint32_t)rw(ds, (uint16_t)(rec + 0xd0)) << 16;
    set_variable(b, vm, NAMED(0x11d9), (uint16_t)(clock / 0x3bc4));
    set_variable(b, vm, NAMED(0x11e3), (uint16_t)((clock / 0x3bc4) % 0x5a0));
    set_variable(b, vm, NAMED(0x11ed), (uint16_t)(clock / 0x01502e80));
    set_variable(b, vm, NAMED(0x11f7), 0);
    set_variable(b, vm, NAMED(0x1206), (uint16_t)((ds[(uint16_t)(rec + 0x64)] >> 1) & 1));
    set_variable(b, vm, NAMED(0x120f), (uint16_t)((ds[(uint16_t)(rec + 0x5f)] >> 2) & 0xf));
    set_variable(b, vm, NAMED(0x121b), (uint16_t)((ds[(uint16_t)(rec + 0x5f)] >> 1) & 1));
    set_variable(b, vm, NAMED(0x1226), rw(ds, 0x726e));
#undef NAMED
}

/* barter_draw_slot_frame, the motion port's (src/uw_motion_barter.c). */
static void draw_slot_frame(uw_convbi *b, int whose, int slot) {
    if (b->level && b->level->screen) uw_motion_barter_draw_slot_frame(b->level, whose, slot);
}

void uw_convbi_barter_setup(uw_convbi *b) {
    uint8_t *ds = b->ds;
    uint16_t t = rw(ds, TALKER_PTR), row, charm;
    int k;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return; }
    row = (uint16_t)((rw(b->level->lseg, t) & 0x3f) * 0x30);
    /* the eight slots' backgrounds: a 16 x 16 buffer each (imgbuf_alloc),
     * captured from the screen at the slot's place with gfx_span_variant
     * set, for barter_draw_slot to put back */
    b->level->span_variant = 1;
    for (k = 0; k < 4; k++) {
        ww(ds, (uint16_t)(0x48b6 + k * 2), uw_motion_imgbuf_alloc(b->level, 0x10, 0x10));
        ww(ds, (uint16_t)(0x489e + k * 2), uw_motion_imgbuf_alloc(b->level, 0x10, 0x10));
    }
    for (k = 0; k < 4; k++) {
        uw_motion_imgbuf_capture(b->level, rw(ds, (uint16_t)(0x48b6 + k * 2)),
                                 (int16_t)rw(ds, (uint16_t)(0x0d54 + k * 4)), (int16_t)rw(ds, (uint16_t)(0x0d56 + k * 4)), 0x10, 0x10);
        uw_motion_imgbuf_capture(b->level, rw(ds, (uint16_t)(0x489e + k * 2)),
                                 (int16_t)rw(ds, (uint16_t)(0x0d74 + k * 4)), (int16_t)rw(ds, (uint16_t)(0x0d76 + k * 4)), 0x10, 0x10);
    }
    ww(ds, 0x4868, 0);
    ww(ds, 0x4866, 0);
    for (k = 0; k < 4; k++) {
        ww(ds, (uint16_t)(PLAYER_SLOTS + k * 2), 0);
        ww(ds, (uint16_t)(NPC_SLOTS + k * 2), 0);
        ww(ds, (uint16_t)(0x48a6 + k * 2), 0xffff);
        ww(ds, (uint16_t)(0x488e + k * 2), 0xffff);
        ww(ds, (uint16_t)(0x48ae + k * 2), 0xffff);
        ww(ds, (uint16_t)(0x4896 + k * 2), 0xffff);
        ds[(uint16_t)(PLAYER_SLOT_OFFERED + k)] = 0;
        ds[(uint16_t)(NPC_SLOT_OFFERED + k)] = 0;
        draw_slot_frame(b, 1, k);
        draw_slot_frame(b, 0, k);
    }
    ds[0x4872] = 0;
    ww(ds, RAND_SEED, uw_motion_obj_index(b->level, t));      /* rt_srand */
    ww(ds, RAND_SEED + 2, 0);
    ww(ds, BARTER_THRESHOLD, (uint16_t)rand_percent_jitter(b, (int16_t)((ds[(uint16_t)(row + 0x4a60)] & 0xf) * 6), -25, 25));
    ww(ds, 0x4884, (uint16_t)rand_percent_jitter(b, (int16_t)(ds[(uint16_t)(row + 0x4a60)] >> 4), -20, 100));
    ww(ds, BARTER_NPC_JITTER, (uint16_t)rand_percent_jitter(b, (int16_t)((0xf - (ds[(uint16_t)(row + 0x4a5f)] >> 4)) * 6), -25, 50));
    ww(ds, 0x4874, (uint16_t)rand_percent_jitter(b, (int16_t)(ds[(uint16_t)(row + 0x4a5f)] & 0xf), -20, 20));
    ww(ds, BARTER_LAST_PCT, 0);
    charm = ds[(uint16_t)(rw(ds, PLAYER_RECORD_PTR) + 0x30)];
    ww(ds, BARTER_THRESHOLD, (uint16_t)(rw(ds, BARTER_THRESHOLD) - charm * 2));
    ww(ds, 0x4884, (uint16_t)(rw(ds, 0x4884) + (charm >> 1)));
    ww(ds, 0x4874, (uint16_t)(rw(ds, 0x4874) - charm / 6));
    if ((int16_t)rw(ds, 0x4874) < 1) ww(ds, 0x4874, 1);
    ww(ds, BARTER_LIKES, 0);
    ww(ds, BARTER_LIKES + 2, 0);
    ww(ds, BARTER_DISLIKES, 0);
    ww(ds, BARTER_DISLIKES + 2, 0);
    ww(ds, RAND_SEED, b->clock_low);                          /* rt_srand(time(0)) */
    ww(ds, RAND_SEED + 2, 0);
}

int uw_convbi_export_npc_vars(uw_convbi *b, uw_convvm *vm, uint16_t t) {
    uint8_t *ds = b->ds, *ls;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR), w;
    int16_t v = 0, gtarg = 0;
    int zero = 0;
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    ls = b->level->lseg;
    get_variable(b, vm, "npc_hunger", &v);
    ls[(uint16_t)(t + 0x19)] = (uint8_t)((ls[(uint16_t)(t + 0x19)] & 0x7f) | (v < 0x20 ? 0x80 : 0));
    get_variable(b, vm, "npc_hp", &v);
    ls[(uint16_t)(t + 8)] = (uint8_t)v;
    get_variable(b, vm, "npc_xhome", &v);
    ls[(uint16_t)(t + 4)] = (uint8_t)((ls[(uint16_t)(t + 4)] & 0xc0) | (v & 0x3f));
    get_variable(b, vm, "npc_yhome", &v);
    ls[(uint16_t)(t + 6)] = (uint8_t)((ls[(uint16_t)(t + 6)] & 0xc0) | (v & 0x3f));
    get_variable(b, vm, "npc_goal", &v);
    get_variable(b, vm, "npc_gtarg", &gtarg);
    uw_motion_creature_set_goal_for(b->level, t, (uint8_t)v, gtarg);
    ww(ls, (uint16_t)(t + 0xd), (uint16_t)(rw(ls, (uint16_t)(t + 0xd)) | 0x2000));
    get_variable(b, vm, "npc_attitude", &v);
    w = rw(ls, (uint16_t)(t + 0xd));
    if (v > 3) {
        ww(ls, (uint16_t)(t + 0xd), (uint16_t)(w | 0xc000));
        ls[(uint16_t)(t + 0x19)] |= 0x40;           /* the ally bit */
    } else {
        ww(ls, (uint16_t)(t + 0xd), (uint16_t)((w & 0x3fff) | ((v & 3) << 14)));
    }
    zero = v == 0;
    ww(ls, (uint16_t)(t + 0xd), (uint16_t)(rw(ls, (uint16_t)(t + 0xd)) | 0x2000));
    get_variable(b, vm, "play_hunger", &v);
    ds[(uint16_t)(rec + 0x39)] = (uint8_t)v;
    get_variable(b, vm, "play_hp", &v);
    ls[(uint16_t)(rw(ds, PLAYER_OBJECT_PTR) + 8)] = (uint8_t)v;
    get_variable(b, vm, "play_mana", &v);
    ds[(uint16_t)(rec + 0x37)] = (uint8_t)v;
    get_variable(b, vm, "play_poison", &v);
    ds[(uint16_t)(rec + 0x5f)] = (uint8_t)((ds[(uint16_t)(rec + 0x5f)] & 0xc3) | ((v & 0xf) << 2));
    get_variable(b, vm, "new_player_exp", &v);
    if (v) uw_motion_player_gain_experience(b->level, v);
    return zero;
}

/* The VM overlay's other string intrinsics, from their instructions.
 * plural(n, singular, plural): the plural id when n is over 1. */
static int16_t bi_plural(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    (void)b;
    return arg(vm, top, 3) > 1 ? arg(vm, top, 1) : arg(vm, top, 2);
}

/* length(s): strlen of the string, unexpanded. */
static int16_t bi_length(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    char *s = get_string(b, (uint16_t)arg(vm, top, 1));
    return (int16_t)(s ? strlen(s) : 0);
}

/* val(s): atoi_far -- white space skipped, but the
 * minus sign looked for only at the string's FIRST character, and then
 * stepped over wherever the digits start; decimal digits, sixteen bits. */
static int16_t bi_val(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    const char *s = get_string(b, (uint16_t)arg(vm, top, 1));
    int si = 0;
    uint16_t v = 0;
    if (!s) return 0;
    while (ctype(b, s[si]) & 0x01) si++;
    if (s[0] == '-') si++;
    for (; ctype(b, s[si]) & 0x02; si++) v = (uint16_t)(v * 10 + (s[si] - '0'));
    return (int16_t)(s[0] == '-' ? -(int16_t)v : (int16_t)v);
}

/* find(value, count, &array): the 1-based position of the first
 * of `count` memory words from `array` equal to value, or 0. */
static int16_t bi_find(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    int16_t value = arg(vm, top, 1), count = arg(vm, top, 2), si;
    (void)b;
    for (si = 0; si < count; si++)
        if ((int16_t)mem_get(vm, (uint16_t)(top[-3] + si)) == value) return (int16_t)(si + 1);
    return 0;
}

/* copy(s) and append(a, b): a new string added to
 * block 0x7c -- a copy, or the SECOND argument's text followed by the first's
 * -- and its id. The texts are this port's to keep: they live as long as the
 * process, as the conversation's far heap lives as long as the conversation. */
static int16_t add_runtime_string(uw_convbi *b, const char *text) {
    char *copy = strdup_c(text);
    return (int16_t)(b->strings ? uw_strcache_add(b->strings, copy, CONV_STRING_BLOCK) : 0);
}

static int16_t bi_copy(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    char *s = get_string(b, (uint16_t)arg(vm, top, 1));
    return add_runtime_string(b, s ? s : "");
}

static int16_t bi_append(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    char *a = get_string(b, (uint16_t)arg(vm, top, 1)), *s2 = get_string(b, (uint16_t)arg(vm, top, 2)), *buf;
    size_t la = a ? strlen(a) : 0, lb = s2 ? strlen(s2) : 0;
    int16_t id;
    buf = malloc(la + lb + 1);
    memcpy(buf, s2 ? s2 : "", lb);
    memcpy(buf + lb, a ? a : "", la + 1);
    id = add_runtime_string(b, buf);
    free(buf);
    return id;
}

enum { ENCHANTMENT_QUERY_ONLY = 0x09f8 };

static int item_enchantment(uw_convbi *b, uint16_t o, int16_t *effect, int16_t *magnitude, int *special) {
    return uw_motion_item_enchantment(b->level, o, effect, magnitude, special);
}

/* look_at_magic_equipment(obj, lore, buf): for an enchanted
 * object, lore 2 appends "magical " and answers 1; lore 3 appends "cursed "
 * for a plain effect 9 and still answers 0. */
static int look_at_magic_equipment(uw_convbi *b, uint16_t o, int16_t lore, char *buf) {
    int16_t effect, magnitude;
    int special;
    if (!item_enchantment(b, o, &effect, &magnitude, &special)) return 0;
    if (lore == 2) {
        strcat(buf, "magical ");
        return 1;
    }
    if (lore == 3 && !special && effect == 9) strcat(buf, "cursed ");
    return 0;
}

/* magic_item_description(obj, lore, buf): at lore 3 only, with
 * enchantment_query_only set around the lookup, " of " and the spell's name
 * from block 6 -- magnitude plus 0x1c0 for a plain effect 12 (0x1d0 past
 * subclass 1), plus 0x100 for a special effect of none, else plus effect * 16;
 * "UNNAMED" for an empty one -- and for a non-quantity whose first linked
 * spell is special, " with N full charge(s)", "no" for none. Effect 9, the
 * curse, has no description. 1 when written. */
static int magic_item_description(uw_convbi *b, uint16_t o, int16_t lore, char *buf) {
    uint8_t *ls = b->level->lseg;
    int16_t effect, magnitude, charges = -1;
    int special, ok;
    uint16_t w0 = rw(ls, o), index;
    char *name, digits[3];
    b->ds[ENCHANTMENT_QUERY_ONLY] = 1;
    ok = item_enchantment(b, o, &effect, &magnitude, &special);
    b->ds[ENCHANTMENT_QUERY_ONLY] = 0;
    if (!ok || lore != 3 || effect == 9) return 0;
    index = (uint16_t)magnitude;
    if (effect == 0xc) index = (uint16_t)(index + (((w0 & 0x30) >> 4) > 1 ? 0x10 : 0) + 0x1c0);
    else if (special && effect <= 0) index = (uint16_t)(index + 0x100);
    else index = (uint16_t)(index + (effect << 4));
    name = get_string(b, (uint16_t)(index | 0xc00));
    strcat(buf, " of ");
    strcat(buf, name && *name ? name : "UNNAMED");
    if (w0 & 0x8000) return 1;
    {
        uint16_t link = (uint16_t)(o + 6), spell = uw_motion_find_matching(b->level, &link, 0, 4, 2, 0);
        if (spell && (rw(ls, spell) & 0x800)) charges = (int16_t)(ls[(uint16_t)(spell + 4)] & 0x3f);
    }
    if (charges < 0) return 1;
    strcat(buf, " with ");
    if (charges <= 0) {
        strcat(buf, "no");
    } else {
        digits[0] = (char)('0' + charges / 10);
        digits[1] = (char)('0' + charges % 10);
        digits[2] = '\0';
        strcat(buf, charges < 10 ? digits + 1 : digits);
    }
    strcat(buf, " full charge");
    if (charges != 1) strcat(buf, "s");
    return 1;
}

/* format_article_plural(name, article, plural), in place: a
 * block 4 name is "singular&plural" and "article_noun". The plural form is
 * the part after '&', or the whole with an 's' appended; the singular stops
 * at '&'. Then the article's '_' becomes a space, or without `article` the
 * name starts after it. */
static char *format_article_plural(char *s, int article, int plural) {
    char *amp = strchr(s, '&'), *us;
    if (plural) {
        if (amp) s = amp + 1;
        else strcat(s, "s");
    } else if (amp) {
        *amp = '\0';
    }
    us = strchr(s, '_');
    if (us) {
        if (article) *us = ' ';
        else s = us + 1;
    }
    return s;
}

/* format_object_name(dest, obj, article, plural): a class 1
 * object whose byte +0x1a is 1..0xef takes block 7's name 0x10 on; anything
 * else its block 4 name through format_article_plural. 0, dest untouched,
 * for an empty name. */
static int format_object_name(uw_convbi *b, char *dest, uint16_t o, int article, int plural) {
    uint8_t *ls = b->level->lseg, who = ls[(uint16_t)(o + 0x1a)];
    uint16_t w0 = rw(ls, o);
    char *s;
    if (((w0 & 0x1c0) >> 6) == 1 && who > 0 && who < 0xf0) {
        s = get_string(b, (uint16_t)((who + 0x10) | 0xe00));
        if (!s || !*s) return 0;
    } else {
        s = get_string(b, (uint16_t)((w0 & 0x1ff) | 0x800));
        if (!s || !*s) return 0;
        s = format_article_plural(s, article, plural);
    }
    strcpy(dest, s);
    return 1;
}

/* converse_draw_screen, from the instructions, as far as it draws
 * before the windows' clears: the cursor hidden, then over the page it copied
 * from the screen, the name area filled 0xf1 (x 42..194, rows 199..153),
 * CONVERSE.GR's six images blitted opaque -- the two top bars, the portrait
 * frames, the panel's two rules and the right panel; gfx_blit takes the height
 * before the width -- and a rule at y 152 from x 52 to 220; then, with the
 * panel shown saved and the inventory's put in its place,
 * dungeon_refresh_inventory: the paperdoll, the container corner and the slots
 * (inventory_panel_init, before it, ran in the dungeon and returns at its
 * guard). */
void uw_convbi_draw_screen(uw_convbi *b, const char *data_dir) {
    static const struct { int x, y, image, h, w; } bars[] = {
        { 0x2b, 199, 0, 9, 0x5e }, { 0x8b, 199, 0, 9, 0x5e }, { 0x52, 0xbe, 1, 0x26, 0x37 },
        { 0x8b, 0xbe, 1, 0x26, 0x37 }, { 0x2b, 0xbe, 2, 0x26, 0x26 }, { 0xc3, 0xbe, 2, 0x26, 0x26 },
        { 0x2a, 0x97, 3, 10, 0xc0 }, { 0x2a, 0x49, 4, 10, 0xc0 }, { 0xec, 0xc0, 5, 0x72, 0x54 } };
    uw_motion *m = b->level;
    uint8_t *ds = b->ds;
    size_t k;
    int w, h;
    const uint8_t *px;
    if (!m) return;
    /* ems_frame_claim: the whole page frame as the conversation's scratch,
     * its segment kept, and as a far pointer --
     * which is what converse_refresh tests to take the table down
     * (end_barter), so it runs at the end of every conversation, not only
     * a trade: what is left on the table goes to the owners' feet */
    ds[EMS_FRAME_CLAIMED] = 1;
    ww(ds, 0x494e, rw(ds, EMS_FRAME_SEG));
    ww(ds, 0x494a, 0);
    ww(ds, 0x494c, rw(ds, EMS_FRAME_SEG));
    uw_motion_cursor_hide(m);               /* before the page is copied from the screen */
    uw_motion_fill_rect(m, 0x2a, 199, 0xc2, 0x99, 0xf1);
    for (k = 0; k < sizeof bars / sizeof *bars; k++) {
        px = uw_gr_file_image(data_dir, "CONVERSE", bars[k].image, &w, &h);
        uw_motion_blit(m, px, px ? w : 0, bars[k].x, bars[k].y, bars[k].h, bars[k].w);
    }
    uw_motion_fill_rect(m, 0x34, 0x98, 0xdc, 0x98, 0xf1);
    ds[0x4902] = ds[0x0784];                /* the panel shown, saved; the inventory's now */
    ds[0x0784] = 0;
    ds[0x18a4] = 0;                         /* paperdoll_flip_page: drawn where it stands */
    uw_motion_dungeon_refresh_inventory(m);
    ds[0x18a4] = 1;
}

/* The rest of converse_draw_screen's drawing, after the windows' clears, in
 * font5x6p colour 0x65: the Avatar's head (HEADS.GR by record +0x64's bits) at
 * (197, 188) and name at (144, 197), the talker's head (CHARHEAD.GR by whoami - 1,
 * or GENHEAD.GR by its item) at (45, 188) and name at (48, 197), the heads with
 * colour 0 skipped. */
void uw_convbi_draw_heads(uw_convbi *b, const char *data_dir) {
    uw_motion *m = b->level;
    uint8_t *ds = b->ds;
    int w, h;
    const uint8_t *px;
    char name[0x40], path[512];
    uw_blob font;
    uint16_t talker;
    uint8_t rec64, whoami;
    if (!m || !m->screen) return;
    snprintf(path, sizeof path, "%s/FONT5X6P.SYS", data_dir);
    font = uw_read_file(path);
    rec64 = ds[(uint16_t)(rw(ds, 0x7270) + 0x64)];
    px = uw_gr_file_image(data_dir, "HEADS", ((rec64 >> 1) & 1) * 5 + ((rec64 & 0x1c) >> 2), &w, &h);
    m->span_variant = 1;
    uw_motion_blit(m, px, px ? w : 0, 0xc5, 0xbc, 0x22, 0x22);
    m->span_variant = 0;
    snprintf(name, sizeof name, "%s", (const char *)ds + 0x7288);
    uw_motion_draw_string(m, font.data, font.size, name, 0x90, 0xc5, 0x65);
    talker = rw(ds, 0x4a14);
    whoami = m->lseg[(uint16_t)(talker + 0x1a)];
    px = whoami ? uw_gr_file_image(data_dir, "CHARHEAD", whoami - 1, &w, &h) : NULL;
    if (!px) px = uw_gr_file_image(data_dir, "GENHEAD", rw(m->lseg, talker) & 0x3f, &w, &h);
    m->span_variant = 1;
    uw_motion_blit(m, px, px ? w : 0, 0x2d, 0xbc, 0x22, 0x22);
    m->span_variant = 0;
    if (format_object_name(b, name, talker, 0, 0))
        uw_motion_draw_string(m, font.data, font.size, name, 0x30, 0xc5, 0x65);
    uw_free(&font);
}

/* identify_inv(idx, article, &out, lore) -> the price
 * barter_item_price gives the player's side at the NPC's jitter, and in `out`
 * a new string of block 0x7c: a stack's count and a space, or -- with
 * `article`, a single object and look_at_magic_equipment's word -- "an "
 * before a lower-case vowel and "a " otherwise, either way without the name's
 * own article; then that word ("cursed " comes after an article the name
 * keeps), the name, and magic_item_description. */
static int16_t bi_identify_inv(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint8_t *ls;
    uint16_t idx = (uint16_t)arg(vm, top, 4), o, q;
    int16_t article = arg(vm, top, 3), lore = arg(vm, top, 1), price, count;
    char desc[0x200], word[0x20];
    if (!b->level) { UW_NOT_CARRIED(b->not_carried); return 0; }
    ls = b->level->lseg;
    price = barter_item_price(b, 1, idx, (int16_t)rw(b->ds, BARTER_NPC_JITTER));
    o = uw_motion_obj_at(b->level, idx);
    q = (uint16_t)((rw(ls, (uint16_t)(o + 6)) >> 6) & 0x3ff);
    count = (int16_t)((rw(ls, o) & 0x8000) && !(q & 0x200) ? q : 1);
    desc[0] = word[0] = '\0';
    if (look_at_magic_equipment(b, o, lore, word) && word[0] && article && count == 1) {
        strcpy(desc, strchr("aeiou", word[0]) ? "an " : "a ");
        article = 0;
    } else if (count > 1) {
        sprintf(desc, "%d ", count);
        article = 0;
    }
    if (word[0]) strcat(desc, word);
    format_object_name(b, desc + strlen(desc), o, (int8_t)article, count > 1);
    magic_item_description(b, o, lore, desc);
    mem_set(vm, (uint16_t)top[-2], (uint16_t)add_runtime_string(b, desc));
    return price;
}

/* x_skills(skill, value): value 10000 is skill_gain(skill) (the
 * motion port's, src/uw_motion_prompt.c); 0..30 sets the skill byte at
 * player_record +0x21 + skill (the index unchecked); anything else only
 * reads. AX is the byte after. */
static int16_t bi_x_skills(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint16_t rec = rw(b->ds, PLAYER_RECORD_PTR), skill = (uint16_t)arg(vm, top, 2);
    int16_t value = arg(vm, top, 1);
    uint16_t at = (uint16_t)(rec + skill + 0x21);
    if (value == 10000) {
        if (b->level) uw_motion_skill_gain(b->level, (int)skill);
        else UW_NOT_CARRIED(b->not_carried);
    }
    else if (value >= 0 && value <= 0x1e) b->ds[at] = (uint8_t)value;
    return b->ds[at];
}

/* x_traps(index, value): 0..0x3f stores the byte at player_record
 * +0x70 + index (the index unchecked); anything else only reads. AX is the
 * byte after. */
static int16_t bi_x_traps(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint16_t rec = rw(b->ds, PLAYER_RECORD_PTR), at = (uint16_t)(rec + (uint16_t)arg(vm, top, 2) + 0x70);
    int16_t value = arg(vm, top, 1);
    if (value >= 0 && value <= 0x3f) b->ds[at] = (uint8_t)value;
    return b->ds[at];
}

uint16_t uw_convbi_babl_ask(uw_convbi *b, const char *typed) {
    uint16_t id = rw(b->ds, ASK_STRING_ID);
    size_t n = strnlen_c(typed, ASK_MAX);
    memcpy(b->input, typed, n);
    b->input[n] = '\0';
    if (!b->strings) return id;
    if (id == 0) {
        id = uw_strcache_add(b->strings, b->input, CONV_STRING_BLOCK);
        ww(b->ds, ASK_STRING_ID, id);
    } else if (!uw_strcache_get(b->strings, id)) {
        uw_strcache_set(b->strings, b->input, id);
    }
    return id;
}

int uw_convbi_strcmp_ids(uw_convbi *b, uw_convvm *vm, uint16_t below, uint16_t top) {
    char *rt = get_string(b, top), *et = uw_convbi_expand(b, vm, rt ? rt : empty_string);
    char *rb = get_string(b, below), *eb = uw_convbi_expand(b, vm, rb ? rb : empty_string);
    int r = strcmp(eb, et);
    free_expansion(eb, rb);
    free_expansion(et, rt);
    return r;
}

/* rt_rand over the game's own seed, which the conversation shares. */
static int16_t rand15(uint8_t *ds) {
    uw_rng r;
    int v;
    r.state = (uint32_t)rw(ds, RAND_SEED) | (uint32_t)rw(ds, RAND_SEED + 2) << 16;
    v = uw_rand(&r);
    ww(ds, RAND_SEED, (uint16_t)r.state);
    ww(ds, RAND_SEED + 2, (uint16_t)(r.state >> 16));
    return (int16_t)v;
}

/* sex(male, female): player_record +0x64 bit 1 picks -- set, the
 * last pushed; clear, the one before. */
static int16_t bi_sex(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint16_t rec = rw(b->ds, PLAYER_RECORD_PTR);
    int bit = (b->ds[(uint16_t)(rec + 0x64)] >> 1) & 1;
    return arg(vm, top, 2 - bit);
}

/* random(n): (rt_rand() * n) / 0x8000 + 1, in 32 bits and signed. */
static int16_t bi_random(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    int32_t r = rand15(b->ds);
    int32_t n = arg(vm, top, 1);
    return (int16_t)((int16_t)((r * n) / 0x8000) + 1);
}

/* The 32-bit quest flags at player_record +0x65 as the instructions build a
 * mask: `mov ax,1 / shl ax,cl / cwd` -- a SIXTEEN-bit shift sign-extended into
 * the high word. So flags 16..31 are never set or read (the shift leaves 0),
 * and flag 15 reaches all sixteen high bits at once. */
static uint32_t quest_mask(int16_t id) {
    uint16_t lo = (uint16_t)(id < 16 ? 1u << id : 0);
    return (uint32_t)lo | ((lo & 0x8000) ? 0xffff0000u : 0);
}

/* get_quest(id): a flag for 0..31, the byte at +0x49 + id for
 * 32..35, the byte at +0x6d beyond, 0 for a negative id. */
static int16_t bi_get_quest(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint16_t rec = rw(b->ds, PLAYER_RECORD_PTR);
    int16_t id = arg(vm, top, 1);
    uint32_t flags;
    if (id < 0) return 0;
    if (id < 0x20) {
        flags = (uint32_t)rw(b->ds, (uint16_t)(rec + 0x65)) | (uint32_t)rw(b->ds, (uint16_t)(rec + 0x67)) << 16;
        return (flags & quest_mask(id)) != 0;
    }
    if (id < 0x24) return b->ds[(uint16_t)(rec + 0x49 + id)];
    return b->ds[(uint16_t)(rec + 0x6d)];
}

/* set_quest(id, value): a flag set when value is nonzero, else
 * cleared, for 0..31; the byte at +0x49 + id for 32..35; nothing beyond or
 * for a negative id. It returns nothing, and CALLI stores AX anyway: the mask
 * (1 << id, or its complement when clearing) on the flag paths, the value on
 * the others (conv5's 032: 8 for quest 3). */
static int16_t bi_set_quest(uw_convbi *b, uw_convvm *vm, uint16_t *top) {
    uint16_t rec = rw(b->ds, PLAYER_RECORD_PTR);
    int16_t id = arg(vm, top, 2), value = arg(vm, top, 1);
    if (id < 0) return value;
    if (id < 0x20) {
        uint32_t m = quest_mask(id);
        uint16_t lo = rw(b->ds, (uint16_t)(rec + 0x65)), hi = rw(b->ds, (uint16_t)(rec + 0x67));
        uint16_t shifted = (uint16_t)(id < 16 ? 1u << id : 0);
        if (value) { lo = (uint16_t)(lo | m); hi = (uint16_t)(hi | m >> 16); }
        else       { lo = (uint16_t)(lo & ~m); hi = (uint16_t)(hi & ~m >> 16); }
        ww(b->ds, (uint16_t)(rec + 0x65), lo);
        ww(b->ds, (uint16_t)(rec + 0x67), hi);
        return (int16_t)(value ? shifted : (uint16_t)~shifted);
    }
    if (id < 0x24)
        b->ds[(uint16_t)(rec + 0x49 + id)] = (uint8_t)value;
    return value;
}

int16_t uw_convbi_call(uw_convbi *b, uw_convvm *vm, const char *name, uint16_t *top, int *ported) {
    static const struct { const char *name; int16_t (*fn)(uw_convbi *, uw_convvm *, uint16_t *); } table[] = {
        { "sex", bi_sex }, { "random", bi_random },
        { "get_quest", bi_get_quest }, { "set_quest", bi_set_quest },
        { "compare", bi_compare }, { "contains", bi_contains },
        { "plural", bi_plural }, { "length", bi_length }, { "val", bi_val }, { "find", bi_find },
        { "copy", bi_copy }, { "append", bi_append }, { "identify_inv", bi_identify_inv }, { "x_skills", bi_x_skills }, { "x_traps", bi_x_traps },
        { "remove_talker", bi_remove_talker },
        { "set_attitude", bi_set_attitude }, { "set_race_attitude", bi_set_race_attitude },
        { "gronk_door", bi_gronk_door }, { "take_from_npc", bi_take_from_npc },
        { "take_id_from_npc", bi_take_id_from_npc }, { "take_from_npc_inv", bi_take_from_npc_inv },
        { "setup_to_barter", bi_setup_to_barter }, { "do_decline", bi_do_decline },
        { "print", bi_print }, { "do_offer", bi_do_offer }, { "show_inv", bi_show_inv },
        { "give_to_npc", bi_give_to_npc }, { "x_obj_stuff", bi_x_obj_stuff },
        { "find_inv", bi_find_inv }, { "find_barter", bi_find_barter }, { "find_barter_total", bi_find_barter_total }, { "give_ptr_npc", bi_give_ptr_npc },
        { "set_likes_dislikes", bi_set_likes_dislikes }, { "count_inv", bi_count_inv }, { "check_inv_quality", bi_check_inv_quality },
        { "set_inv_quality", bi_set_inv_quality }, { "place_object", bi_place_object },
        { "do_inv_create", bi_do_inv_create }, { "do_inv_delete", bi_do_inv_delete }, { "do_demand", bi_do_demand }, { "do_judgement", bi_do_judgement },
    };
    size_t i;
    for (i = 0; i < sizeof table / sizeof *table; i++) {
        if (!strcmp(name, table[i].name)) {
            *ported = 1;
            b->vm = vm;
            return table[i].fn(b, vm, top);
        }
    }
    *ported = 0;
    UW_NOT_CARRIED(b->not_carried);
    return 0;
}
