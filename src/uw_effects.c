/* SPDX-License-Identifier: MIT */
/* See uw_effects.h. */
#include "uw_effects.h"

#include <string.h>

static uint16_t rw(const uint8_t *m, uint16_t at) {
    return (uint16_t)(m[at] | (m[(uint16_t)(at + 1)] << 8));
}
static void ww(uint8_t *m, uint16_t at, uint16_t w) {
    m[at] = (uint8_t)w;
    m[(uint16_t)(at + 1)] = (uint8_t)(w >> 8);
}

static void parse(uw_effect *r, const uint8_t *b) {
    r->word0 = (uint16_t)(b[0] | (b[1] << 8));
    r->timer = (int16_t)(b[2] | (b[3] << 8));
    r->tile_x = b[4];
    r->tile_y = b[5];
}

bool uw_effects_load(uw_effects *e, const uint8_t *block, size_t len,
                     const uint8_t *props) {
    int i;
    memset(e, 0, sizeof *e);
    if (props) memcpy(e->props, props, sizeof e->props);
    if (!block || len != UW_EFFECTS_BLOCK) return false;
    for (i = 0; i < UW_EFFECTS_MAX; i++) parse(&e->rec[i], block + i * 6);
    /* `while (i < 0x40 && list[i].word0 >> 6 != 0) i++` */
    while (e->count < UW_EFFECTS_MAX && e->rec[e->count].word0 >> 6) e->count++;
    return true;
}

void uw_effects_from_ds(uw_effects *e, const uint8_t *ds) {
    int i;
    memset(e, 0, sizeof *e);
    memcpy(e->props, ds + 0x3658, sizeof e->props);
    for (i = 0; i < UW_EFFECTS_MAX; i++) parse(&e->rec[i], ds + 0x369c + i * 6);
    e->count = (int8_t)ds[0x3656];
    if (e->count < 0) e->count = 0;
    e->moved = ds[0x0aae] != 0;
}

/* obj_deref_link over the record's word 0: mobile objects are 27 bytes from
 * +0x4004, static ones 8 bytes from +0x5b04 counting from index 0x100. */
uint16_t uw_effect_object(const uw_effects *e, int index) {
    uint16_t idx;
    if (index < 0 || index >= UW_EFFECTS_MAX) return 0;
    idx = (uint16_t)(e->rec[index].word0 >> 6);
    if (idx == 0) return 0;
    if (idx < 0x100) return (uint16_t)(0x4004 + idx * 0x1b);
    return (uint16_t)(0x5b04 + (idx - 0x100) * 8);
}

int uw_effect_add(uw_effects *e, uint8_t *level, uint16_t index, int16_t timer,
                  uint8_t seed, uint8_t x, uint8_t y) {
    uw_effect *r;
    uint16_t p;
    if (e->count + 1 > UW_EFFECTS_MAX) return -1;         /* `cmp ax,0x40; jle` */
    r = &e->rec[e->count];
    r->word0 = (uint16_t)((r->word0 & 0x3f) | ((index & 0x3ff) << 6));
    r->timer = timer;
    r->tile_x = x;
    r->tile_y = y;
    p = uw_effect_object(e, e->count);
    if (p && level) {
        int k = level[p] & 0xf, first = (int8_t)e->props[k * 4 + 2];
        int count = e->props[k * 4 + 3];
        if (first >= 0) {
            int frame = count ? first + seed % count : first;
            level[(uint16_t)(p + 6)] = (uint8_t)((level[(uint16_t)(p + 6)] & 0xc0)
                                                 | (frame & 0x3f));
        }
    }
    e->drawn_flag = 1;
    return ++e->count;
}

int uw_spawn_animo_copies(uw_effects *e, uint16_t src, uint8_t x, uint8_t y,
                          uw_rng *rng) {
    uw_objpool *p = e->pool;
    uint8_t *seg;
    int left, result = 0;
    if (!p || !rng || !src) { e->unsupported++; return 0; }
    seg = p->seg;
    for (left = uw_rand(rng) % 3 + 2; left >= 0; left--) {
        uint16_t o = uw_obj_alloc(p, 0), w0, w1;
        int fine, d, a, b;
        if (!o) { e->unsupported++; break; }            /* no null test in the original */
        memcpy(seg + o, seg + src, 8);
        w0 = rw(seg, o);
        w0 = (uint16_t)((w0 & 0xfe00) | (((w0 & 0x1ff) + (uw_rand(rng) & 1) + 1) & 0x1ff));
        ww(seg, o, w0);
        w1 = rw(seg, (uint16_t)(o + 2));
        fine = (w1 & 0xe000) >> 13;
        do d = uw_rand(rng) % 5 - 2; while (fine + d < 0 || fine + d > 7);
        w1 = (uint16_t)((w1 & 0x1fff) | (((fine + d) & 7) << 13));
        fine = (w1 & 0x1c00) >> 10;
        do d = uw_rand(rng) % 5 - 2; while (fine + d < 0 || fine + d > 7);
        w1 = (uint16_t)((w1 & 0xe3ff) | (((fine + d) & 7) << 10));
        w1 = (uint16_t)((w1 & 0xff80) | (((w1 & 0x7f) - 8 + (uw_rand(rng) & 0xf)) & 0x7f));
        ww(seg, (uint16_t)(o + 2), w1);
        uw_object_list_insert(p, uw_tile_link(x, y), o);
        a = uw_rand(rng) % 3;
        b = uw_rand(rng) % 3;
        result = uw_effect_add(e, seg, uw_obj_index(o), (int16_t)(2 - a + b),
                               (uint8_t)a, x, y);
        if (result == -1) {
            uw_object_list_remove(p, uw_tile_link(x, y), o);
            uw_obj_free(p, o);
            break;                                      /* `mov di,0xffff` */
        }
    }
    return result;
}

bool uw_spawn_class7_object(uw_effects *e, const uint8_t *comobj, uint16_t src,
                            int kind, int16_t timer, uint8_t seed,
                            int16_t height, uint8_t x, uint8_t y) {
    uw_objpool *p = e->pool;
    uint8_t *seg;
    uint16_t o, w1;
    if (!p) { e->unsupported++; return false; }
    seg = p->seg;
    o = uw_object_create(p, (uint16_t)(kind + 0x1c0), 0, comobj);
    if (!o) return false;
    if (src) {
        w1 = rw(seg, (uint16_t)(o + 2));
        w1 = (uint16_t)((w1 & 0x1fff) | (rw(seg, (uint16_t)(src + 2)) & 0xe000));
        w1 = (uint16_t)((w1 & 0xe3ff) | (rw(seg, (uint16_t)(src + 2)) & 0x1c00));
        ww(seg, (uint16_t)(o + 2), w1);
    }
    if (height < 0) {
        w1 = rw(seg, (uint16_t)(o + 2));
        ww(seg, (uint16_t)(o + 2), (uint16_t)((w1 & 0xff80) | ((-height) & 0x7f)));
    } else if (src) {
        uint8_t k = comobj ? (uint8_t)(comobj[(uint32_t)(rw(seg, src) & 0x1ff) * 11] >> 3) : 1;
        if (k == 0) k = 1;
        k = (uint8_t)(k * height);                      /* `imul si`, AL kept */
        w1 = rw(seg, (uint16_t)(o + 2));
        ww(seg, (uint16_t)(o + 2), (uint16_t)((w1 & 0xff80)
                                               | (((rw(seg, (uint16_t)(src + 2)) & 0x7f) + k) & 0x7f)));
    }
    if (uw_effect_add(e, seg, uw_obj_index(o), timer, seed, x, y) == -1) {
        uw_obj_free(p, o);
        return false;
    }
    uw_object_list_append(p, uw_tile_link(x, y), o);
    return true;
}

void uw_effect_animate(uw_effects *e, uint8_t *level, int index, int delta,
                       uw_rng *rng) {
    uint16_t p = uw_effect_object(e, index);
    uint16_t w0, flags, bit;
    int k, first, count;

    if (!p || !level) return;
    w0 = rw(level, p);
    if ((w0 & 0x1f0) != 0x1c0) return;              /* class 7, 0x1c0..0x1cf */
    k = w0 & 0xf;
    flags = (uint16_t)(e->props[k * 4] | (e->props[k * 4 + 1] << 8));
    first = (int8_t)e->props[k * 4 + 2];            /* `(int)(char)` */
    count = e->props[k * 4 + 3];

    /* One bit at a time from the bottom: `flags &= ~bit; bit <<= 1`. */
    for (bit = 1; flags; flags = (uint16_t)(flags & ~bit), bit = (uint16_t)(bit << 1)) {
        uint16_t b = (uint16_t)(flags & bit);
        uint8_t q = level[(uint16_t)(p + 6)];
        if (b == 1) {
            /* Step, and past the last frame back to the first. */
            int frame = q & 0x3f;
            int next = frame < first + count - 1 ? frame + 1 : first;
            level[(uint16_t)(p + 6)] = (uint8_t)((q & 0xc0) | (next & 0x3f));
        } else if (b == 2) {
            /* `first + rand() % count`, in a byte. */
            int r = rng ? uw_rand(rng) : 0;
            int pick = count ? r % count : 0;
            level[(uint16_t)(p + 6)] = (uint8_t)((q & 0xc0)
                                                 | ((uint8_t)(first + pick) & 0x3f));
        } else if (b == 4) {
            /* Turn: word 0's bits 9..12, bit 3 of them a direction. The
             * step adds into the low three and carries into the fourth,
             * which is then tested again. */
            uint16_t w = rw(level, p);
            int h = (w >> 9) & 0xf;
            if (h & 8) delta = -delta;
            if ((rw(level, (uint16_t)(p + 6)) & 7) == 6) {
                uint16_t z = rw(level, (uint16_t)(p + 2));
                ww(level, (uint16_t)(p + 2),
                   (uint16_t)((z & 0xff80) | (((z & 0x7f) + delta * 6) & 0x7f)));
            }
            h = ((h & 7) + delta + (h & 8)) & 0xf;
            ww(level, p, (uint16_t)((w & 0xe1ff) | (h << 9)));
            if (h & 8) {
                if (e->door_move) e->door_move(e->door_user, e, index, delta);   /* door_closing_check */
                else e->unsupported++;
            }
        }
    }
}

/* level_effect_expire: one more animate pass for bit 7, the moving door
 * re-seated (or turned back, when the record stays and nothing else is
 * done), the object freed for bit 5 -- and then the LAST live record is
 * copied over this one, so the list stays a prefix. */
static void expire(uw_effects *e, uint8_t *level, int index, uw_rng *rng) {
    uint16_t p = uw_effect_object(e, index);
    uint16_t w0 = p && level ? rw(level, p) : 0;
    uint16_t flags = (uint16_t)(e->props[(w0 & 0xf) * 4]
                                | (e->props[(w0 & 0xf) * 4 + 1] << 8));
    if ((flags & 0x80) && e->rec[index].timer != 0) {
        e->in_expire = 1;
        uw_effect_animate(e, level, index, e->rec[index].timer, rng);
        e->in_expire = 0;
    }
    if ((w0 & 0xf) == 0xf) {
        if (!e->door_seat) e->unsupported++;          /* item_fits_in_tile, door_reverse_motion */
        else if (!e->door_seat(e->door_user, e, index)) return;
    }
    if (flags & 0x20) {
        /* level_effect_free_object: unlink the object from the
         * tile the EFFECT remembers, then free it. */
        if (e->pool && p) {
            uw_object_list_remove(e->pool, uw_tile_link(e->rec[index].tile_x,
                                                        e->rec[index].tile_y), p);
            uw_obj_free(e->pool, p);
        } else {
            e->unsupported++;
        }
    }
    e->count--;
    if (e->count > 0 && e->count != index) e->rec[index] = e->rec[e->count];
}

void uw_effects_tick(uw_effects *e, uint8_t *level, int ticks, uw_rng *rng) {
    int d = e->count > 0 ? ticks : 0, i;
    for (i = 0; i < e->count; i++) {
        if (e->rec[i].timer == -1) {
            uw_effect_animate(e, level, i, d, rng);
        } else {
            int16_t left = (int16_t)(e->rec[i].timer - d);
            if (left < 0) {
                expire(e, level, i, rng);
            } else {
                uw_effect_animate(e, level, i, d, rng);
                /* door_closing_check sets the flag when it rewrote the
                 * countdown itself. */
                if (!e->moved) e->rec[i].timer = left;
                else e->moved = 0;
            }
        }
    }
}
