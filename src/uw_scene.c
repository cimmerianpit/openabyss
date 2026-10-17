/* SPDX-License-Identifier: MIT */
/* See uw_scene.h. The loaders follow gr_load_all and its
 * callbacks, gr_load_door_textures, crit_pages_init,
 * lev_load_terrain, shade_set_level and set_render_detail. */
#include "uw_scene.h"
#include "uw_crit.h"
#include "uw_objprops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t rw(const uint8_t *m, uint32_t at) {
    return (uint16_t)(m[at] | (m[at + 1] << 8));
}
static void ww(uint8_t *m, uint16_t at, uint16_t w) {
    m[at] = (uint8_t)w;
    m[(uint16_t)(at + 1)] = (uint8_t)(w >> 8);
}
static uw_blob file_at(const char *dir, const char *sub, const char *name) {
    char path[768];
    snprintf(path, sizeof path, "%s/%s%s", dir, sub, name);
    return uw_read_file(path);
}

/* ---- gr_load_all's EMS packing ------------------------------------------ */

typedef struct { uint16_t next, para, page; } gr_cursor;

/* gr_load_file over one .GR with gr_dest_ems and either
 * gr_register_bitmap_at (by_id 0) or gr_register_bitmap(_based) (by_id 1). */
static void gr_file(gr_cursor *c, const char *dir, const char *name, int by_id,
                    int id_base, int count, uint8_t *r, uint8_t *pool, size_t cap,
                    uint8_t *written) {
    char file[32];
    uw_blob b;
    int n, i;
    snprintf(file, sizeof file, "%s.GR", name);
    b = file_at(dir, "DATA/", file);
    if (!b.data || b.size < 3) { uw_free(&b); return; }
    n = rw(b.data, 1);
    for (i = 0; i < n && (count < 0 || i < count); i++) {
        uint32_t off = uw_u32(b.data + 3 + 4 * (size_t)i);
        uint32_t end = i == n - 1 ? (uint32_t)b.size : uw_u32(b.data + 3 + 4 * (size_t)(i + 1));
        uint16_t len = (uint16_t)(end - off);
        uint16_t w;
        size_t at;
        if ((uint16_t)(c->para * 16 + len) > 0x4000) {       /* gr_dest_ems */
            c->para = 0;
            c->page++;
        }
        at = (size_t)c->page * 0x4000u + (size_t)c->para * 16u;
        if (len && at + len <= cap && (size_t)off + len <= b.size) {
            memcpy(pool + at, b.data + off, len);
            if (written) memset(written + at, 1, len);
        }
        w = (uint16_t)(c->page * 0x1000 + c->para);
        if (!by_id) {                                        /* gr_register_bitmap_at */
            ww(r, (uint16_t)(0xb10f + 2 * (c->next + i)), w);
        } else if (len == 0) {                               /* gr_register_bitmap */
            ww(r, (uint16_t)(0xb90f + 4 * (id_base + i)), 0);
        } else {
            ww(r, (uint16_t)(0xb90f + 4 * (id_base + i)), c->next);
            ww(r, (uint16_t)(0xb10f + 2 * c->next), w);
            c->next++;
        }
        c->para = (uint16_t)(c->para + (len >> 4) + ((len & 0xf) != 0));
    }
    if (!by_id) c->next = (uint16_t)(c->next + n);
    uw_free(&b);
}

uint16_t uw_scene_gr_boot(const char *dir, uint8_t *r, uint8_t *pool, size_t cap,
                          uint8_t *written, uint16_t *texture_base) {
    gr_cursor c = { 0, 0, 4 };
    gr_file(&c, dir, "QUESTION", 0, 0, -1, r, pool, cap, written);
    gr_file(&c, dir, "VIEWS", 0, 0, -1, r, pool, cap, written);
    gr_file(&c, dir, "OBJECTS", 1, 0, -1, r, pool, cap, written);
    ww(r, 0xb0f0, c.next);
    gr_file(&c, dir, "ANIMO", 0, 0, -1, r, pool, cap, written);
    gr_file(&c, dir, "BUTTONS", 0, 0, -1, r, pool, cap, written);
    gr_file(&c, dir, "CURSORS", 0, 0, -1, r, pool, cap, written);
    gr_file(&c, dir, "3DWIN", 0, 0, -1, r, pool, cap, written);
    if (texture_base) *texture_base = c.next;
    gr_file(&c, dir, "TMFLAT", 1, 0x170, 0x10, r, pool, cap, written);
    gr_file(&c, dir, "TMOBJ", 0, 0, -1, r, pool, cap, written);
    return c.next;
}

void uw_scene_gr_doors(const char *dir, const uint8_t *r, uint8_t *pool, size_t cap,
                       uint8_t *written, uint16_t slot0, const uint8_t *blk, size_t n) {
    uw_blob b = file_at(dir, "DATA/", "DOORS.GR");
    int i;
    if (b.data && b.size >= 3 && n >= 0x7a) {
        int cnt = rw(b.data, 1);
        for (i = 0; i < 6; i++) {
            int e = blk[0x74 + i];
            uint16_t w = rw(r, (uint16_t)(0xb10f + 2 * (slot0 + i)));
            size_t at = (size_t)(w >> 12) * 0x4000u + (size_t)(w & 0xfff) * 16u;
            uint32_t off, end;
            if (e >= cnt) continue;
            off = uw_u32(b.data + 3 + 4 * (size_t)e);
            end = e == cnt - 1 ? (uint32_t)b.size : uw_u32(b.data + 3 + 4 * (size_t)(e + 1));
            if (end > off && at + (end - off) <= cap && end <= b.size) {
                memcpy(pool + at, b.data + off, end - off);
                if (written) memset(written + at, 1, end - off);
            }
        }
    }
    uw_free(&b);
}

void uw_scene_crit_tables(const char *dir, uint8_t *r) {
    char name[32];
    uw_blob a = file_at(dir, "CRIT/", "ASSOC.ANM");
    int page, file;
    if (a.data && a.size >= 0x180) memcpy(r + 0xc10f, a.data + 0x100, 0x80);
    uw_free(&a);
    for (page = 0; page < 32; page++) {
        int end = 0;
        for (file = 0; file < 3; file++) r[0xc28f + page * 3 + file] = 0xa0;
        for (file = 0; end < 0xa0 && file < 16; file++) {
            uw_blob b;
            uw_crit_page_filename(page, file, name, sizeof name);
            b = file_at(dir, "CRIT/", name);
            if (!b.data || b.size < 2) { uw_free(&b); break; }
            end = b.data[0] + b.data[1];
            if (file < 3) r[0xc28f + page * 3 + file] = (uint8_t)end;
            uw_free(&b);
        }
    }
}

/* ---- the executor's hooks ------------------------------------------------ */

/* rast_art_segment: page nibble, paragraph in the low ten bits. */
static const uint8_t *scene_art(void *ctx, uint16_t id, size_t *len) {
    const uw_scene *s = ctx;
    size_t at = (size_t)(id >> 12) * 0x4000u + (size_t)(id & 0x3ff) * 16u;
    if (at + 6 > sizeof s->ems) return NULL;
    *len = sizeof s->ems - at;
    return s->ems + at;
}

/* rast_crit_page_map reads a whole CRIT file into EMS: the file is the page. */
static const uint8_t *scene_crit(void *ctx, int page, int file, size_t *len) {
    uw_scene *s = ctx;
    char name[32];
    int i;
    for (i = 0; i < s->n_crit; i++)
        if (s->crit[i].page == page && s->crit[i].file == file) {
            *len = s->crit[i].b.size;
            return s->crit[i].b.data;
        }
    if (s->n_crit >= 64) return NULL;
    uw_crit_page_filename(page, file, name, sizeof name);
    s->crit[s->n_crit].b = file_at(s->dir, "CRIT/", name);
    if (!s->crit[s->n_crit].b.data) return NULL;
    s->crit[s->n_crit].page = page;
    s->crit[s->n_crit].file = file;
    *len = s->crit[s->n_crit].b.size;
    return s->crit[s->n_crit++].b.data;
}

/* ---- opening, levels, the eye ------------------------------------------- */

bool uw_scene_open(uw_scene *s, const char *dir) {
    static const char *tr_names[4] = { "W64.TR", "F32.TR", "W16.TR", "F16.TR" };
    uw_blob exe, b;
    size_t hdr;
    int i;

    memset(s, 0, sizeof *s);
    snprintf(s->dir, sizeof s->dir, "%s", dir);
    exe = file_at(dir, "", "UW.EXE");
    if (!exe.data || exe.size < 0x20) { uw_free(&exe); return false; }
    hdr = (size_t)rw(exe.data, 8) * 16u;
    if (hdr + (0x6aac - 0x1000) * 16u + 0x10000u > exe.size) { uw_free(&exe); return false; }
    memcpy(s->ds, exe.data + hdr + (0x6aac - 0x1000) * 16u, 0x10000);
    memcpy(s->rast, exe.data + hdr + (0x5723 - 0x1000) * 16u, 0x10000);
    memcpy(s->gfx, exe.data + hdr + (0x4963 - 0x1000) * 16u, 0x10000);
    memcpy(s->priv, exe.data + hdr + (0x6624 - 0x1000) * 16u, 0x10000);
    uw_free(&exe);

    b = file_at(dir, "DATA/", "COMOBJ.DAT");                /* obj_properties */
    if (b.data && b.size > 2)
        memcpy(s->ds + 0x5b6e, b.data + 2, b.size - 2 < 0x200 * 11 ? b.size - 2 : 0x200 * 11);
    uw_free(&b);
    {
        /* obj_properties_load's class-7 section, animation_props, which
         * level_effect_animate reads (and 0x3658 holds). */
        uw_objprops op;
        char opath[768];
        snprintf(opath, sizeof opath, "%s/DATA/OBJECTS.DAT", dir);
        if (uw_objprops_open(&op, opath)) {
            size_t n;
            const uint8_t *a = uw_objprop_section_data(&op, UW_SEC_ANIMATION, &n);
            if (a && n >= sizeof s->animation_props) {
                memcpy(s->animation_props, a, sizeof s->animation_props);
                memcpy(s->ds + 0x3658, a, sizeof s->animation_props);
            }
            uw_objprops_close(&op);
        }
    }
    uw_rng_init(&s->rng);
    (void)uw_view_overlay_load(s->overlay, dir);
    b = file_at(dir, "DATA/", "ALLPALS.DAT");               /* gr_aux_palettes */
    if (b.data) {
        memcpy(s->auxpals, b.data, b.size < sizeof s->auxpals ? b.size : sizeof s->auxpals);
        memcpy(s->ds + 0x573e, b.data, b.size < 0x200 ? b.size : 0x200);
    }
    uw_free(&b);
    b = file_at(dir, "DATA/", "XFER.DAT");
    if (b.data) memcpy(s->xfer, b.data, b.size < sizeof s->xfer ? b.size : sizeof s->xfer);
    uw_free(&b);

    /* drawlist_reset: labels 0..0x5f undefined, and the model
     * routines' predefined labels 0x60..0x9f from drawlist_predef_labels at
     * drawlist_base - 0x98, halved -- byte offsets to words. */
    for (i = 0; i < 0xa0; i++) {
        uint16_t t = i < 0x60 ? 0xffff : rw(s->rast, (uint32_t)(0x28a0 + (i - 0x60) * 2));
        ww(s->ds, (uint16_t)(0x7856 + i * 2),
           i < 0x60 || t == 0xffff ? 0xffff : (uint16_t)((int16_t)t >> 1));
    }
    s->images = uw_scene_gr_boot(dir, s->rast, s->ems, sizeof s->ems, NULL, &s->texture_base);
    ww(s->ds, 0x15e2, s->texture_base);
    uw_scene_crit_tables(dir, s->rast);
    for (i = 0; i < 4; i++) {
        char path[768];
        snprintf(path, sizeof path, "%s/DATA/%s", dir, tr_names[i]);
        if (!uw_tr_open(&s->tr[i], path)) return false;
    }
    s->shades_file = file_at(dir, "DATA/", "SHADES.DAT");
    s->light_file = file_at(dir, "DATA/", "LIGHT.DAT");
    s->mono_file = file_at(dir, "DATA/", "MONO.DAT");
    s->view_width = 172;
    s->view_height = 113;
    s->projection_scale = 25000;
    s->level_no = -1;
    return true;
}

void uw_scene_close(uw_scene *s) {
    int i;
    for (i = 0; i < 4; i++) uw_tr_close(&s->tr[i]);
    for (i = 0; i < s->n_crit; i++) uw_free(&s->crit[i].b);
    uw_free(&s->shades_file);
    uw_free(&s->light_file);
    uw_free(&s->mono_file);
    s->n_crit = 0;
}

bool uw_scene_level(uw_scene *s, int level, const uint8_t *block, size_t len) {
    char path[768];
    uw_ark ark;
    const uint8_t *map, *tex;
    size_t n_map, n_tex;
    uw_blob ter;
    int i;

    snprintf(path, sizeof path, "%s/DATA/LEV.ARK", s->dir);
    if (!uw_ark_open(&ark, path)) return false;
    n_map = uw_ark_block(&ark, level, &map);
    n_tex = uw_ark_block(&ark, 18 + level, &tex);
    if (!block) { block = map; len = n_map; }
    if (!block || n_tex < 0x7a) { uw_ark_close(&ark); return false; }

    /* The level segment: the block at +4, where tile_ptr_from_xy(0, 0)
     * lands, with the object arrays following the tiles. */
    memset(s->level, 0, sizeof s->level);
    memcpy(s->level + 4, block, len < 0xfffc ? len : 0xfffc);
    ww(s->ds, 0x19b4, 0x0004); ww(s->ds, 0x19b6, UW_SCENE_SEG);   /* tilemap_ptr */
    ww(s->ds, 0x272e, 0x4004); ww(s->ds, 0x2730, UW_SCENE_SEG);   /* mobile_obj_base */
    ww(s->ds, 0x275a, 0x5b04); ww(s->ds, 0x275c, UW_SCENE_SEG);   /* static_obj_base */
    ww(s->ds, 0x7274, 0x401f); ww(s->ds, 0x7276, UW_SCENE_SEG);   /* the Avatar, object 1 */
    ww(s->ds, 0x2e10, 0x401f); ww(s->ds, 0x2e12, UW_SCENE_SEG);   /* the camera rides it */
    ww(s->ds, 0x7270, 0x7288);                                     /* the player record */
    ww(s->ds, 0x7278, (uint16_t)(level + 1));                      /* current_level */

    /* The texture slots by the level's texture map. */
    for (i = 0; i < UW_DL_TEX; i++) { s->tex[i] = NULL; s->tex_len[i] = 0; }
    for (i = 0; i < 116; i++) {
        int t = i < 48 ? 0 : i < 58 ? 1 : i < 106 ? 2 : 3;
        int k = t == 0 ? i : t == 1 ? i - 48 : t == 2 ? i - 58 : i - 106;
        int num = t == 0 || t == 2 ? rw(tex, (uint32_t)(k * 2)) : rw(tex, (uint32_t)(0x60 + k * 2));
        s->tex[i] = uw_tr_texture(&s->tr[t], num);
        s->tex_len[i] = s->tex[i] ? (size_t)s->tr[t].dim * (size_t)s->tr[t].dim : 0;
    }
    /* lev_load_terrain: the floor textures' kinds from TERRAIN.DAT's second
     * half, ten words and their count. */
    ter = file_at(s->dir, "DATA/", "TERRAIN.DAT");
    for (i = 0; i < 10; i++) {
        uint32_t at = 0x200u + 2u * rw(tex, (uint32_t)(0x60 + i * 2));
        ww(s->ds, (uint16_t)(0x7192 + 2 * i),
           ter.data && at + 2 <= ter.size ? rw(ter.data, at) : 0);
    }
    ww(s->ds, 0x71a6, 10);
    uw_free(&ter);
    uw_scene_gr_doors(s->dir, s->rast, s->ems, sizeof s->ems, NULL,
                      (uint16_t)(s->texture_base + 0x30), tex, n_tex);
    /* level_effects_load: LEV.ARK slot level + 8 for the 1-based level. */
    {
        const uint8_t *eff;
        size_t n_eff = uw_ark_block(&ark, 9 + level, &eff);
        uw_effects_load(&s->effects, eff, n_eff, s->animation_props);
    }
    uw_objpool_attach(&s->pool, s->level);
    s->effects.pool = &s->pool;
    uw_ark_close(&ark);
    s->level_no = level;
    s->drawn = 0;
    return true;
}

bool uw_scene_effects(uw_scene *s, const uint8_t *block, size_t len) {
    bool ok = uw_effects_load(&s->effects, block, len, s->animation_props);
    s->effects.pool = &s->pool;
    return ok;
}

long uw_scene_tick(uw_scene *s, int ticks) {
    long before = s->effects.unsupported;
    uw_effects_tick(&s->effects, s->level, ticks, &s->rng);
    return s->effects.unsupported - before;
}

void uw_scene_place(uw_scene *s, uint16_t x, uint16_t y, uint16_t z,
                    uint16_t heading, int detail, int light) {
    uw_blob sh;
    int rec[6], i;

    ww(s->ds, 0x2780, x);                       /* player_world_x, _y, _z */
    ww(s->ds, 0x2782, y);
    ww(s->ds, 0x2784, z);
    ww(s->ds, 0x727a, heading);                 /* player_heading */
    ww(s->ds, 0x3588, 0);                       /* view_pitch, view_roll */
    ww(s->ds, 0x358a, 0);
    uw_scene_sway(s, 0, 0, 0, 0, 0);            /* no impairment */

    /* set_render_detail: the player record's detail nibble picks
     * the detail flag and two emitter pointers. */
    if (detail < 0) detail = 0;
    if (detail > 3) detail = 3;
    s->ds[0x7288 + 0xb5] = (uint8_t)((s->ds[0x7288 + 0xb5] & 0x0f) | (detail << 4));
    ww(s->ds, 0x54b, (uint16_t)(detail > 0));
    ww(s->ds, 0x555, detail > 1 ? 0x0a4a : 0x079a);
    ww(s->ds, 0x55d, detail > 2 ? 0x0a4a : 0x079a);
    ww(s->ds, 0x54d, 1);

    /* shade_set_level: SHADES.DAT's record for the level, and
     * LIGHT.DAT -- MONO.DAT at level 5. */
    if (light < 0) light = 0;
    sh = s->shades_file;
    for (i = 0; i < 6; i++)
        rec[i] = sh.data && (size_t)(light * 12 + i * 2 + 2) <= sh.size
               ? (int16_t)rw(sh.data, (uint32_t)(light * 12 + i * 2)) : 0;
    ww(s->gfx, 0x64c, (uint16_t)(rec[0] < 2 ? 1 : rec[0]));
    ww(s->gfx, 0x64e, (uint16_t)rec[1]);
    ww(s->gfx, 0x650, (uint16_t)rec[2]);
    ww(s->ds, 0x735a, (uint16_t)rec[3]);
    ww(s->ds, 0x549, (uint16_t)rec[4]);
    ww(s->ds, 0x547, (uint16_t)rec[5]);
    s->ds[0x1c50] = (uint8_t)light;
    sh = light == 5 ? s->mono_file : s->light_file;
    if (sh.data) memcpy(s->light, sh.data, sh.size < sizeof s->light ? sh.size : sizeof s->light);
    if (s->shade_blank) {
        /* shade_reload_or_scramble(1): bytes 0 and 1 of each of the
         * sixteen 256-byte rows zeroed -- the brightest two entries of every
         * shade gone to colour 0. Before that the original copies 4 KB of
         * whatever lies at the bottom of the machine's memory over the
         * table; the port keeps the table it has just loaded. */
        UW_NOT_CARRIED(s->unsupported);
        for (i = 0; i < 16; i++) s->light[i * 0x100] = s->light[i * 0x100 + 1] = 0;
    }
}

void uw_scene_masks(uw_scene *s, const uint16_t mask[6]) {
    int i;
    for (i = 0; i < 6; i++) {
        ww(s->rast, (uint16_t)(0xb07e + 8 * i + 6), mask[i]);   /* a first frame reads it here */
        s->dl.rec[i].v_mask = mask[i];                        /* and the executor carries it */
    }
}

void uw_scene_shade_blank(uw_scene *s, int blank) { s->shade_blank = blank != 0; }

void uw_scene_floors(uw_scene *s, const uint8_t *assign) {
    int i;
    for (i = 0; i < 10; i++) {
        int num = (int16_t)rw(assign, (uint32_t)(i * 2));
        s->tex[48 + i] = num >= 0 ? uw_tr_texture(&s->tr[1], num) : NULL;
        s->tex_len[48 + i] = s->tex[48 + i] ? (size_t)s->tr[1].dim * (size_t)s->tr[1].dim : 0;
        s->tex[106 + i] = num >= 0 ? uw_tr_texture(&s->tr[3], num) : NULL;
        s->tex_len[106 + i] = s->tex[106 + i] ? (size_t)s->tr[3].dim * (size_t)s->tr[3].dim : 0;
    }
}

void uw_scene_sway(uw_scene *s, int on, int16_t height, int16_t heading,
                   int16_t pitch, int16_t roll) {
    s->ds[0x3578] = (uint8_t)(on != 0);
    ww(s->ds, 0x3580, (uint16_t)height);
    ww(s->ds, 0x3582, (uint16_t)heading);
    ww(s->ds, 0x3584, (uint16_t)pitch);
    ww(s->ds, 0x3586, (uint16_t)roll);
}

/* ---- the draw -------------------------------------------------------------- */

static int16_t rs(const uint8_t *m, uint32_t at) { return (int16_t)rw(m, at); }

/* The executor's view of the rasteriser's memory at the start of a run: what
 * rast_execute's setup computed, every draw; what the last frame left, the
 * first time only -- the executor carries it after that, as the original's
 * memory does. */
static void dl_frame_words(uw_scene *s) {
    uw_dl *m = &s->dl;
    const uint8_t *r = s->rast;
    int i;
    for (i = 0; i < 9; i++) m->basis[i] = rs(r, 0x1602u + 2u * (unsigned)i);
    for (i = 0; i < 3; i++) m->cam[i] = (int32_t)uw_u32(r + 0x26ba + 4 * i);
    m->add_shift = r[0x2880];
    m->plane_k[0] = rs(r, 0x2726);
    m->plane_k[1] = rs(r, 0x2728);
    m->plane_k[2] = rs(r, 0x272a);
    m->proj.scale_y = rs(r, 0x26b0);
    m->proj.scale_x = rs(r, 0x26b2);
    m->proj.off_x = rs(r, 0x26b4);
    m->proj.off_y = rs(r, 0x26b6);
    m->depth_sub = s->rf.depth_op == 0x2b;
    m->depth_index = s->rf.depth_axis / 2;
    m->light = s->light;
    m->light_params.scale = rw(s->gfx, 0x64c);
    m->light_params.bias_b = rs(s->gfx, 0x64e);
    m->light_params.bias_a = rs(s->gfx, 0x650);
    for (i = 0; i < UW_DL_TEX; i++) {
        m->tex[i] = s->tex[i];
        m->tex_len[i] = s->tex_len[i];
    }
}

static void dl_first(uw_scene *s) {
    uw_dl *m = &s->dl;
    const uint8_t *r = s->rast;
    uint16_t ri, carried = m->poly_count;
    int i;
    memset(m, 0, sizeof *m);
    for (i = 0; i < 3; i++) m->origin[i] = rs(r, 0x2886u + 2u * (unsigned)i);
    m->cull = rw(r, 0x2882);
    m->shader = rw(r, 0xb006);
    m->shader_sel = rw(r, 0xb00a);
    /* the polygon count emit_poly4b reads: only emit_poly3/4
     * write it, and nothing clears it -- not a level change -- so a scene
     * that has drawn keeps what its last such face left, and a detail-1
     * frame (which has none) inherits it as the original's does */
    m->poly_count = s->ever_drawn ? carried : rw(r, 0xb00c);
    memcpy(m->ucol, s->gfx + 0x7f2, sizeof m->ucol);
    ri = rw(r, 0xb07c);
    m->rec_index = (ri >= 0xb07e && ri < 0xb07e + UW_DL_RECS * 8) ? (ri - 0xb07e) / 8 : 0;
    for (i = 0; i < UW_DL_RECS; i++) {
        uint32_t o = 0xb07eu + 8u * (unsigned)i;
        m->rec[i].width = rs(r, o);
        m->rec[i].v_max = rs(r, o + 2);
        m->rec[i].tex_seg = 0xffff;
        m->rec[i].v_mask = rw(r, o + 6);
    }
    for (i = 0; i < UW_DL_FLAGS; i++) m->flag[i] = rw(r, 0x2920u + 2u * (unsigned)i);
    for (i = 0; i < UW_DL_SREC; i++) m->srec[i] = rs(r, 0xb0b6u + 2u * (unsigned)i);
    m->shift_table = r + 0x1e2;
    m->art = scene_art;
    m->crit_page = scene_crit;
    m->art_ctx = s;
    m->auxpals = s->auxpals;
    m->auxpals_len = sizeof s->auxpals;
    m->xfer = s->xfer;
    m->gfx_ds = s->gfx;
    m->poly_fn = rw(r, 0x15f6);
    m->colour = s->gfx[0x410f];
    m->fixed_colour = s->gfx[0xdc3];
    m->fill_fn = rw(s->gfx, 0x4110);
    uw_spans_load(&m->spans, s->gfx);
    m->view_clip.left = 0;
    m->view_clip.top = 0;
    m->view_clip.right = (int16_t)(s->view_width - 1);
    m->view_clip.bottom = (int16_t)(s->view_height - 1);
    m->sprite_clip.left = m->view_clip.left;
    m->sprite_clip.bottom = m->view_clip.bottom;
    m->sprite_clip.right = m->view_clip.right;
    m->sprite_clip.top = m->view_clip.top;
    for (i = 0; i < UW_RAST_SLOT_WINDOW / 2; i++) m->slots.x[i] = rs(r, 0x1620u + 2u * (unsigned)i);
    for (i = 0; i < UW_RAST_SLOT_WINDOW; i++) m->slots.outcode[i] = r[0x1620 + i];
    m->slots.origin[0] = m->origin[0];
    m->slots.origin[1] = m->origin[2];
    m->slots.origin[2] = m->origin[1];
    m->slots.fetch_shift = m->add_shift;
}

static long scene_render(uw_scene *s, const uw_fb *fb, int pick);

long uw_scene_draw(uw_scene *s, const uw_fb *fb) {
    return scene_render(s, fb, 0);
}

long uw_scene_pick(uw_scene *s, const uw_fb *fb) {
    if (s->drawn == 0) return -1;           /* the map replays a drawn view */
    return scene_render(s, fb, 1);
}

static long scene_render(uw_scene *s, const uw_fb *fb, int pick) {
    uw_vl *v = &s->vl;
    uint16_t end, q, prog;
    long r = 0;
    int pass, e, i;

    if (s->level_no < 0) return -1;
    /* The list, from the player's position. */
    v->ds = s->ds;
    v->rast = s->rast;
    v->tiles = s->level + 4;
    v->tiles_origin = 4;
    v->vstate = s->priv + 0x970;
    v->level = s->level;
    v->clock = 0;
    v->lprm = s->gfx + 0x64c;
    for (i = 0; i < UW_VL_ART && i < UW_DL_TEX; i++) v->art[i] = s->tex[i];
    v->shade = s->light;
    /* the pick map's replay keeps the draw's camera and column array */
    v->spans = pick ? 0 : 2;
    v->pick = pick;
    if (pick) {
        v->ds = s->ds;
        v->vstate = s->priv + 0x970;
    }
    v->objects = v->deferred = v->unsupported = v->object_tiles = 0;
    v->newly_seen = 0;                      /* automap_newly_seen, cleared before the sweep */
    memcpy(v->automap, s->ds + 0x3820, sizeof v->automap);
    end = uw_vl_build(v);
    memcpy(s->ds, v->mem, sizeof s->ds);
    memcpy(s->ds + 0x3820, v->automap, sizeof v->automap);
    s->unsupported += v->unsupported;
    for (q = UW_VL_LIST0; q < end; q++) s->rast[q] = v->list[q];

    /* rast_execute's setup, with the view-state block the camera placed and
     * the camera program's depth scale view_set_viewport hands over. */
    prog = rw(s->rast, 0x104);
    ww(s->rast, (uint16_t)(prog + 2), s->projection_scale);
    memcpy(s->rf.rast, s->rast, sizeof s->rf.rast);
    memcpy(s->rf.priv, s->priv, sizeof s->rf.priv);
    memcpy(s->rf.priv + 0x970, v->camera, sizeof v->camera);
    s->rf.view_width = (int16_t)s->view_width;
    s->rf.view_height = (int16_t)s->view_height;
    s->rf.unsupported = s->rf.saturated = 0;
    if (!uw_rast_frame_setup(&s->rf)) s->unsupported++;
    memcpy(s->rast, s->rf.rast, sizeof s->rast);
    memcpy(s->priv, s->rf.priv, sizeof s->priv);

    /* The executor: once to reach what a frame leaves, the first time. */
    if (s->drawn == 0) { dl_first(s); s->ever_drawn = 1; }
    /* gfx_select_row_blitter: 1 from view_build_draw_list, 2 from
     * drawlist_begin_frame */
    s->dl.row_blitter = (uint8_t)(pick ? 2 : 1);
    for (pass = s->drawn == 0 ? 0 : 1; pass < 2; pass++) {
        dl_frame_words(s);
        s->dl.n_faces = 0;
        s->dl.n_events = 0;
        s->dl.image_used = 0;
        r = uw_dl_run(&s->dl, s->rast, sizeof s->rast, rw(s->rast, 0x161c));
        if (r < 0) return -1;
    }
    if (!pick) s->drawn++;
    for (e = 0; e < s->dl.n_events; e++) uw_dl_render_event(&s->dl, fb, e, 1);
    if (!pick) uw_view_overlay_draw(s->overlay, fb);
    return s->dl.n_events;
}

bool uw_view_overlay_load(uw_view_overlay ov[3], const char *dir) {
    static const int at[3][2] = { { 62, 3 }, { 0, 13 }, { 171, 13 } };
    char path[768];
    uw_gr g;
    int i;
    bool ok = true;
    memset(ov, 0, 3 * sizeof *ov);
    snprintf(path, sizeof path, "%s/DATA/3DWIN.GR", dir);
    if (!uw_gr_open(&g, path)) return false;
    for (i = 0; i < 3; i++) {
        uw_gr_info info;
        if (!uw_gr_image(&g, i, NULL, ov[i].px, sizeof ov[i].px, &info)
            || info.width * info.height > (int)sizeof ov[i].px) {
            ok = false;
            continue;
        }
        ov[i].w = info.width;
        ov[i].h = info.height;
        ov[i].x = at[i][0];
        ov[i].y = at[i][1];
    }
    uw_gr_close(&g);
    return ok;
}

void uw_view_overlay_draw(const uw_view_overlay ov[3], const uw_fb *fb) {
    int i, r, c;
    if (!fb || !fb->pixels) return;
    for (i = 0; i < 3; i++)
        for (r = 0; r < ov[i].h; r++) {
            int row = ov[i].y - r;
            if (row < 0 || row >= fb->n_rows) continue;
            for (c = 0; c < ov[i].w; c++) {
                uint8_t v = ov[i].px[r * ov[i].w + c];
                uint32_t at = (uint32_t)fb->row[row] + (uint32_t)(ov[i].x + c);
                if (v && at < fb->size) fb->pixels[at] = v;
            }
        }
}
