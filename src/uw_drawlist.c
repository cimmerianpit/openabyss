/* SPDX-License-Identifier: MIT */
#include "uw_drawlist.h"

#include <string.h>
#include "uw_trig.h"

/* ---- reading the program ------------------------------------------------
 *
 * `lodsw` off a 16-bit SI. Every fetch is bounds-checked here, which the
 * original does not do: an overrun in it walks the data segment and this
 * has to stop instead. */
typedef struct stream_ {
    const uint8_t *p;
    size_t         len;
    uint16_t       si;
    int            bad;
} stream;

static uint16_t fetch_w(stream *s) {
    uint16_t v;
    if ((size_t)s->si + 2 > s->len) { s->bad = 1; return 0; }
    v = (uint16_t)(s->p[s->si] | (s->p[s->si + 1] << 8));
    s->si = (uint16_t)(s->si + 2);
    return v;
}

static uint8_t fetch_b(stream *s) {
    if ((size_t)s->si + 1 > s->len) { s->bad = 1; return 0; }
    return s->p[s->si++];
}

/* ---- the pieces the opcodes share -------------------------------------- */

/* `imul; imul; add/adc; add bx,bx; adc cx,cx; jno; jg` -- two Q15 products
 * summed, DOUBLED, and the high word taken, with the overflow SATURATED to
 * 0x7fff or 0x8000 rather than wrapped. The doubling is what makes it Q15
 * rather than Q16, and the saturation is why a basis cannot drift past one. */
static int16_t q15_2mac(int16_t a, int16_t b, int16_t c, int16_t d) {
    int32_t p = (int32_t)a * (int32_t)b + (int32_t)c * (int32_t)d;
    int64_t t = (int64_t)p * 2;
    if (t > 0x7fffffffLL) return 0x7fff;
    if (t < -0x80000000LL) return (int16_t)0x8000;
    {
        int32_t v = (int32_t)t;
        int16_t hi = (int16_t)((uint32_t)v >> 16);
        return hi;
    }
}

/* rast_basis_rotate_y turns the ORIGIN and then the basis about
 * y by an 8.8 angle, through gfx_sincos_lerp_lookup -- SINE IN AX, COSINE IN
 * BX -- which it stores as sin, cos and -sin:
 *
 *     origin:         x' = hi(2(x cos - z sin))     z' = hi(2(x sin + z cos))
 *     row 0:          m0' = sat(2(cos m0 - sin m6))  (and m1, m2 alike)
 *     row 2:          m6' = sat(2(sin m0 + cos m6))  (from the OLD row 0)
 *
 * Row 1, the axis, is left alone, and the origin's doubling WRAPS where the
 * basis's saturates (`shl; rcl` against `add; adc; jno`). */
/* The x and z twins are the same computation over another pair: about x, rows 1 and 2 turn
 * as rows 0 and 2 do about y, and the origin's y and z as its x and z; about
 * z, row 0' = cos r0 + sin r1 and row 1' = cos r1 - sin r0, the origin's
 * x' = cos x + sin y and y' = cos y - sin x -- the sine's sign on the other
 * row. `axis` is 0, 1 or 2. */
static void basis_rotate(int16_t m[9], int16_t origin[3], uint16_t angle,
                         int axis) {
    int16_t sn, cs, t[3];
    int i, a, b;                 /* the two rows, and the two origin words */
    int16_t s_a, s_b;            /* the sine's sign on each */

    uw_sincos_lerp(angle, &sn, &cs);
    if (axis == 0)      { a = 1; b = 2; }
    else if (axis == 1) { a = 0; b = 2; }
    else                { a = 0; b = 1; }
    /* new a = cos a + s_a b, new b = s_b a + cos b */
    if (axis == 2) { s_a = sn; s_b = (int16_t)(0u - (uint16_t)sn); }
    else           { s_a = (int16_t)(0u - (uint16_t)sn); s_b = sn; }
    {
        int16_t oa = origin[a], ob = origin[b];
        int64_t na = (int64_t)oa * cs + (int64_t)ob * s_a;
        int64_t nb = (int64_t)oa * s_b + (int64_t)ob * cs;
        origin[a] = (int16_t)(uint16_t)(((uint32_t)na * 2u) >> 16);
        origin[b] = (int16_t)(uint16_t)(((uint32_t)nb * 2u) >> 16);
    }
    for (i = 0; i < 3; i++)
        t[i] = q15_2mac(cs, m[a * 3 + i], s_a, m[b * 3 + i]);
    for (i = 0; i < 3; i++)
        m[b * 3 + i] = q15_2mac(s_b, m[a * 3 + i], cs, m[b * 3 + i]);
    for (i = 0; i < 3; i++)
        m[a * 3 + i] = t[i];
}

/* rast_origin_patch: the fetch's immediates, BY ADDRESS. */
static void origin_patch(uw_dl *m) {
    m->slots.origin[0] = m->origin[0];
    m->slots.origin[1] = m->origin[2];
    m->slots.origin[2] = m->origin[1];
    m->slots.fetch_shift = m->add_shift;
}

/* call_list_rot_y and its _var form: push the nine basis words
 * and the three origin words, turn, SELECT THE ROTATE SET, patch, run the
 * sub-list, pop, patch -- and not select again. */
static long run(uw_dl *m, struct stream_ *s, int depth);
static void do_call_rel(uw_dl *m, struct stream_ *s, int depth);

static void call_rotated(uw_dl *m, struct stream_ *s, int depth,
                         uint16_t angle, int axis) {
    int16_t save_b[9], save_o[3];
    int i;
    for (i = 0; i < 9; i++) save_b[i] = m->basis[i];
    for (i = 0; i < 3; i++) save_o[i] = m->origin[i];
    basis_rotate(m->basis, m->origin, angle, axis);
    m->slots.axis = uw_rast_select_rotate_set(m->basis);
    origin_patch(m);
    do_call_rel(m, s, depth);
    for (i = 0; i < 9; i++) m->basis[i] = save_b[i];
    for (i = 0; i < 3; i++) m->origin[i] = save_o[i];
    origin_patch(m);
}

/* rast_draw_face's tail: the mapper the list chose, then rast_draw_face_lit
 * over the same projected vertices when the list header's word 4 has a low
 * byte. */
static void draw_face(uw_dl *m, const uw_fb *fb, const uw_rast_svert *sv,
                      int n, uint16_t shader, const uw_rast_texrec *r,
                      int lit, const uint8_t *px, size_t plen) {
    if (!fb || !fb->pixels || !px) return;
    if (m->no_mapper) {
        /* nothing: a harness asking which pixels the lighting pass alone
         * reaches */
    } else if (shader == 0x545)
        uw_gfx_texture_poly_wall(fb, sv, n, r, px, plen, &m->proj,
                                 m->ucol, (int)sizeof m->ucol, &m->wall_dv);
    else
        uw_gfx_texture_poly_affine(fb, sv, n, r, px, plen);
    if (lit && m->light) {
        uw_shade_vert v[UW_CLIP_MAX];
        int i;
        for (i = 0; i < n && i < UW_CLIP_MAX; i++) {
            v[i].x = sv[i].sx;
            v[i].y = sv[i].sy;
            v[i].shade = uw_rast_vertex_shade(&sv[i], &m->light_params);
        }
        (void)uw_gfx_shade_polygon_remap(fb, v, n, m->light,
                                         &m->light_overrun);
    }
}

static void draw_shaded_face(uw_dl *m, const uw_fb *fb,
                             const uw_rast_svert *sv, int n, int unclipped,
                             int gouraud, uint8_t base, uint8_t *colour);

/* A flat face: the projected ring through gfx_draw_polygon, which clips
 * it to the view in place -- so a copy. */
static void draw_flat_face(uw_dl *m, const uw_fb *fb, const uw_rast_svert *sv,
                           int n, uint8_t colour) {
    uw_pt pts[UW_CLIP_MAX_VERTS + 1];
    int i;
    if (!fb || !fb->pixels || n > UW_CLIP_MAX_VERTS) return;
    for (i = 0; i < n; i++) { pts[i].x = sv[i].sx; pts[i].y = sv[i].sy; }
    (void)uw_gfx_draw_polygon(fb, &m->spans, pts, n, &m->view_clip, colour);
}

/* A face through fill_general's routine, gfx_polygon_fill_general. */
static void draw_general_face(uw_dl *m, const uw_fb *fb,
                              const uw_rast_svert *sv, int n, uint8_t colour) {
    uw_pt pts[UW_CLIP_MAX_VERTS + 4];
    int i;
    if (!fb || !fb->pixels || n > UW_CLIP_MAX_VERTS) return;
    for (i = 0; i < n; i++) { pts[i].x = sv[i].sx; pts[i].y = sv[i].sy; }
    (void)uw_gfx_polygon_fill_general(fb, pts, n, &m->view_clip, colour);
}

void uw_dl_render_face(uw_dl *m, const uw_fb *fb, int k, int lit) {
    int n;
    if (k < 0 || k >= m->n_faces) return;
    n = m->face_n[k] < UW_DL_FACEV ? m->face_n[k] : UW_DL_FACEV;
    if (m->face_shader[k] == 0x438d) {
        draw_flat_face(m, fb, m->face[k], n, m->face_colour[k]);
        return;
    }
    if (m->face_shader[k] == 0x2e7c) {
        draw_general_face(m, fb, m->face[k], n, m->face_colour[k]);
        return;
    }
    if (m->face_shader[k] == 0x6146 || m->face_shader[k] == 0x6156) {
        draw_shaded_face(m, fb, m->face[k], n, m->face_shader[k] == 0x6156,
                         m->face_mode[k], m->face_colour[k], NULL);
        return;
    }
    draw_face(m, fb, m->face[k], n, m->face_shader[k], &m->face_rec[k],
              lit && m->face_lit[k], m->face_texels[k], m->face_texlen[k]);
}

/* A sprite: decode its art lit by its level, and draw it where the
 * placement put it. */
static void draw_sprite_event(uw_dl *m, const uw_fb *fb, int k) {
    static uint8_t pix[0x10000];
    const uint8_t *art;
    size_t len = 0, n;
    int w = 0, h = 0;
    if (k < 0 || k >= m->n_sprites || !fb || !fb->pixels) return;
    if (m->sprite[k].creature) {
        const uint8_t *page;
        if (!m->crit_page) return;
        page = m->crit_page(m->art_ctx, m->sprite[k].crit_page,
                            m->sprite[k].crit_file, &len);
        if (!page) return;
        n = uw_creature_decode(page, len, &m->sprite[k].frame, m->light,
                               m->sprite[k].level, pix, sizeof pix);
        if (!n) return;
        (void)uw_gfx_draw_sprite_rows(fb, &m->sprite[k].desc, pix, n, &m->sprite_clip, m->xfer, NULL,
                                      m->sprite[k].blitter ? m->sprite[k].blitter : 1, m->sprite[k].fill);
        return;
    }
    if (!m->art) return;
    art = m->art(m->art_ctx, m->sprite[k].art_id, &len);
    if (!art) return;
    n = uw_sprite_decode(art, len, m->auxpals, m->auxpals_len, m->light,
                         m->sprite[k].level, pix, sizeof pix, &w, &h);
    if (!n) return;
    (void)uw_gfx_draw_sprite_rows(fb, &m->sprite[k].desc, pix, n, &m->sprite_clip, m->xfer, NULL,
                                  m->sprite[k].blitter ? m->sprite[k].blitter : 1, m->sprite[k].fill);
}

void uw_dl_render_event(uw_dl *m, const uw_fb *fb, int e, int lit) {
    if (e < 0 || e >= m->n_events) return;
    if (m->event[e] & 0x8000) draw_sprite_event(m, fb, m->event[e] & 0x7fff);
    else uw_dl_render_face(m, fb, m->event[e], lit);
}

/* A face that reached a shader, kept for the harness's one-at-a-time
 * scoring and its replay. */
static void record_face(uw_dl *m, int n, uint16_t shader,
                        const uw_rast_texrec *r, int lit, uint8_t colour) {
    int i;
    m->faces++;
    for (i = 0; i < n && i < UW_CLIP_MAX; i++) m->last_face[i] = m->sv[i];
    m->last_face_n = n;
    for (i = 0; i < 3; i++) m->last_origin[i] = m->origin[i];
    m->last_shift = m->add_shift;
    if (m->n_faces < UW_DL_FACES) {
        int k = m->n_faces++;
        for (i = 0; i < n && i < UW_DL_FACEV; i++)
            m->face[k][i] = m->sv[i];
        m->face_at[k] = m->op_at;
        m->face_n[k] = (uint8_t)n;
        m->face_shader[k] = shader;
        m->face_rec[k] = *r;
        m->face_lit[k] = (uint8_t)lit;
        m->face_colour[k] = colour;
        /* The texels the face was drawn with, captured now: the image
         * scratch (UW_DL_IMAGE) is rewritten by every load_texture_image,
         * and a replay after the run would otherwise draw every earlier
         * face with the last image loaded. */
        m->face_texels[k] = r->tex_seg < UW_DL_TEX ? m->tex[r->tex_seg] : NULL;
        m->face_texlen[k] = r->tex_seg < UW_DL_TEX ? m->tex_len[r->tex_seg] : 0;
        if (m->n_events < UW_DL_EVENTS) m->event[m->n_events++] = (uint16_t)k;
    }
}

/* rast_draw_face over whatever the clip buffer holds: clip, project, the
 * mapper, the lighting pass. */
static void draw_clip_buffer(uw_dl *m) {
    const uw_rast_texrec *r = &m->rec[m->rec_index & (UW_DL_RECS - 1)];
    int n, lit = (m->flag[(0x2928 - 0x2920) / 2] & 0xff) != 0;

    if (!m->noclip && uw_rast_clip_face(&m->clip) == 0) { m->dropped++; return; }
    n = uw_rast_project(&m->clip, &m->proj, m->sv);
    if (n <= 0) { m->dropped++; return; }
    record_face(m, n, m->shader, r, lit, 0);
    if (lit && !m->light) m->approx++;
    if (lit) m->span_gouraud = 0;          /* the remap pair */
    draw_face(m, &m->fb, m->sv, n, m->shader, r, lit,
              r->tex_seg < UW_DL_TEX ? m->tex[r->tex_seg] : NULL,
              r->tex_seg < UW_DL_TEX ? m->tex_len[r->tex_seg] : 0);
}

/* ---- flat faces -----------------------------------------------------------
 *
 * THE PATH, which rast_poly_emit_tail takes for every face when
 * list-header word 8 is set and emit_poly_gathered takes unless
 * the shaded pair is installed. It is not rast_draw_face with another
 * shader: it has its own clip policy and its own projection, and hands the
 * graphics module bare (x, y) pairs.
 *
 *   - AND set: nothing.
 *   - OR clear: project and draw.
 *   - bit 7 (a vertex at or behind the eye): clip against the planes in the
 *     order 4, 8, 1, 2 -- but only until bit 7 is gone. What is left outside
 *     the frustum's sides is left to the viewport clip in 2-D. Still behind
 *     after all four, and the face is dropped.
 *   - otherwise project with every `idiv` trapped and
 *     every `add` of the centre tested with `jno`, and on either overflow
 *     clip whatever is still flagged, in the order 2, 1, 4, 8, and project
 *     again. The original would do that forever for a face that overflows
 *     with nothing left to clip; the port gives up after two rounds and
 *     counts it.
 *
 * The planes are rast_draw_face's own -- eight-byte copies of its clippers,
 * the same arithmetic without u and v -- so this uses the same pass with u and v riding along unused. */

/* The flat projection, unchecked (0) or checked (1). Returns 0 on an
 * overflow. */
static int project_flat(uw_dl *m, int checked) {
    const uw_rast_cvert *v = m->clip.buf[m->clip.src];
    int i;
    for (i = 0; i < m->clip.count; i++) {
        const int16_t c[2] = { v[i].x, v[i].y };
        const int16_t scale[2] = { m->proj.scale_x, m->proj.scale_y };
        const int16_t off[2] = { m->proj.off_x, m->proj.off_y };
        int32_t out[2];
        int a;
        for (a = 0; a < 2; a++) {
            int32_t q;
            if (v[i].z == 0) {
                if (checked) return 0;
                q = 0x7fff;
            } else {
                q = ((int32_t)c[a] * scale[a]) / v[i].z;
                if (q < -32768 || q > 32767) {
                    if (checked) return 0;
                    q = 0x7fff;
                }
            }
            out[a] = q + off[a];
            if (out[a] < -32768 || out[a] > 32767) {
                if (checked) return 0;
                out[a] = (int16_t)(uint16_t)(out[a] & 0xffff);
            }
        }
        m->sv[i].sx = (int16_t)out[0];
        m->sv[i].sy = (int16_t)out[1];
        m->sv[i].x = v[i].x;
        m->sv[i].y = v[i].y;
        m->sv[i].z = v[i].z;
        m->sv[i].u = (int16_t)(uint8_t)v[i].u;   /* the shaded path's +7 */
        m->sv[i].v = 0;
    }
    return 1;
}

/* A Gouraud face (or a shaded one through the remap pair): the six-byte
 * vertices to gfx_draw_polygon_shaded, or -- `unclipped`, rast_draw_shaded's
 * OR-zero branch calling [0x15f8] -- straight to the scan. */
static void draw_shaded_face(uw_dl *m, const uw_fb *fb,
                             const uw_rast_svert *sv, int n, int unclipped,
                             int gouraud, uint8_t base, uint8_t *colour) {
    uw_shade_vert v[UW_CLIP_MAX_VERTS + 1];
    int i;
    if (!fb || !fb->pixels || !m->light || n > UW_CLIP_MAX_VERTS) return;
    for (i = 0; i < n; i++) {
        v[i].x = sv[i].sx;
        v[i].y = sv[i].sy;
        v[i].shade = (int16_t)(uint8_t)sv[i].u;  /* the shade's high byte is 0 */
    }
    if (unclipped) {
        if (gouraud)
            (void)uw_gfx_shade_polygon_gouraud(fb, v, n, m->light, base,
                                               &m->light_overrun, colour);
        else
            (void)uw_gfx_shade_polygon_remap(fb, v, n, m->light,
                                             &m->light_overrun);
    } else {
        (void)uw_gfx_draw_polygon_shaded(fb, v, n, &m->view_clip, m->light,
                                         gouraud, base, &m->light_overrun,
                                         colour);
    }
}

/* The flat path, or with `shaded` rast_draw_shaded -- the same
 * decisions over records that carry a shade, through the shaded passes. */
static void draw_gathered_path(uw_dl *m, int shaded) {
    static const uint8_t near_order[4] = { 4, 8, 1, 2 };
    static const uint8_t fall_order[4] = { 2, 1, 4, 8 };
    static const uw_rast_texrec none;
    uw_rast_clip *c = &m->clip;
    void (*plane)(uw_rast_clip *, uint8_t) =
        shaded ? uw_rast_clip_plane_shaded : uw_rast_clip_plane;
    int k, round, unclipped;

    if (c->and_code) { m->dropped++; return; }
    if (c->or_code & 0x80) {
        for (k = 0; k < 4 && (c->or_code & 0x80); k++) {
            if (!(c->or_code & near_order[k])) continue;
            plane(c, near_order[k]);
            if (c->and_code) { m->dropped++; return; }
        }
        if (c->or_code & 0x80) { m->dropped++; return; }
    }
    unclipped = c->or_code == 0;
    if (unclipped) {
        (void)project_flat(m, 0);
    } else {
        for (round = 0; !project_flat(m, 1); round++) {
            if (round == 2) { m->flat_unsupported++; return; }
            for (k = 0; k < 4; k++) {
                if (!(c->or_code & fall_order[k])) continue;
                plane(c, fall_order[k]);
                if (c->and_code) { m->dropped++; return; }
            }
        }
    }
    if (c->count > 99) { m->dropped++; return; }   /* cmp cx,0x63; ja */
    if (shaded) {
        uint16_t sh = (uint16_t)(unclipped ? 0x6156 : 0x6146);
        m->shaded_faces++;
        record_face(m, c->count, sh, &none, 0, m->shade_base);
        if (m->n_faces > 0 && m->face_shader[m->n_faces - 1] == sh)
            m->face_mode[m->n_faces - 1] = (uint8_t)m->span_gouraud;
        draw_shaded_face(m, &m->fb, m->sv, c->count, unclipped,
                         m->span_gouraud, m->shade_base, &m->colour);
        return;
    }
    m->flat_faces++;
    if ((m->poly_fn != 0x438d && m->poly_fn != 0x2e7c)
        || m->fill_fn != 0x0d2d) {
        m->flat_unsupported++;
        return;
    }
    record_face(m, c->count, m->poly_fn, &none, 0, m->colour);
    if (m->poly_fn == 0x2e7c)
        draw_general_face(m, &m->fb, m->sv, c->count, m->colour);
    else
        draw_flat_face(m, &m->fb, m->sv, c->count, m->colour);
}

/* emit_poly_gathered: the shaded pair goes to rast_draw_shaded;
 * anything else down the flat path. */
static void emit_gathered(uw_dl *m) {
    m->clip.src = 0;                                /* mov [0x307],0x309 */
    draw_gathered_path(m, m->poly_fn == 0x6146);
}

/* gather_first / gather_next: a slot's x, y, z and its outcode's low byte
 * onto the buffer's end. */
static void gather_slot(uw_dl *m, uint16_t slot, int first) {
    uw_rast_clip *c = &m->clip;
    int off = slot & (UW_RAST_SLOT_WINDOW - 8);
    int16_t xyz[3];
    uint8_t code;
    uw_rast_cvert *w;
    if (first) { c->src = 0; c->count = 0; }
    if (c->count >= UW_CLIP_MAX) return;
    w = &c->buf[0][c->count++];
    uw_rast_slot_get(&m->slots, off, xyz);
    code = m->slots.outcode[(off + 6) & (UW_RAST_SLOT_WINDOW - 1)];
    w->x = xyz[0]; w->y = xyz[1]; w->z = xyz[2];
    w->outcode = code;
    w->u = (int16_t)m->slots.outcode[(off + 7) & (UW_RAST_SLOT_WINDOW - 1)];
    w->v = 0;
    if (first) { c->or_code = code; c->and_code = code; }
    else { c->or_code |= code; c->and_code &= code; }
}

/* the colour mode set_texture's tail sets:
 * `shl ax,1; jb` refuses a mode with bit 15, the colour byte is the high
 * byte of the graphics module's colour table [mode] and the fill is its fill
 * table [type] for a type word [mode] below 0x14 -- 0x0d2d,
 * gfx_fill_span_list, for every solid colour. */
static uint16_t gfx_word(const uw_dl *m, uint32_t at) {
    return (uint16_t)(m->gfx_ds[at & 0xffff]
                      | (m->gfx_ds[(at + 1) & 0xffff] << 8));
}

static void set_flat_colour(uw_dl *m, uint16_t mode) {
    uint16_t i2 = (uint16_t)(mode << 1), type;
    m->poly_fn = 0x438d;
    if (mode & 0x8000) return;
    if (!m->gfx_ds) {
        m->colour = (uint8_t)mode;
        m->fill_fn = mode < 256 ? 0x0d2d : 0;
        return;
    }
    m->colour = (uint8_t)(gfx_word(m, 0x21eu + i2) >> 8);
    type = gfx_word(m, 8u + i2);
    if (type < 0x14) m->fill_fn = gfx_word(m, 0xaf4u + type);
}

/* The emitters' tail.
 *
 * A gathered quad is SUBDIVIDED rather than drawn when the vertex count
 * says four, the shader is 0x545, the AND of the outcodes is clear (a set
 * AND drops the face without drawing), list-header word 8 is zero, and
 * either the OR of the outcodes is set or some vertex is nearer than
 * 1 << the add shift. Then the four sub-quads go through rast_draw_face one
 * by one -- each clipped, projected, mapped and LIT on its own, which is why
 * a near wall's texture columns and its light gradient are both piecewise.
 *
 * `subdividable` is whether the opcode enters the subdivision at all: emit_poly3/4
 * fall into it and emit_poly4b jumps to it, while the shape emitters and
 * emit_poly4c jump past it to rast_poly_emit_tail. emit_poly4b does not
 * write the vertex count, so its count is whatever the last emit_poly3/4 left.
 *
 * rast_poly_emit_tail sends every face down the flat path (copying the
 * buffer to eight-byte records first) when word 8 is set:
 * the model faces a list colours with set_colour_lit. */
static void emit_face_tail(uw_dl *m, int subdividable) {
    if (subdividable && m->poly_count == 4 && m->shader == 0x545) {
        if (m->clip.and_code) { m->dropped++; return; }       /* 60ef */
        if (m->flag[(0x2930 - 0x2920) / 2] == 0
            && uw_rast_should_subdivide(&m->clip, 0x545, m->add_shift)) {
            uw_rast_cvert q[4][4];
            int k, i;
            uw_rast_subdivide(&m->clip, q);
            m->subdivided++;
            for (k = 0; k < 4; k++) {
                m->clip.src = 0;
                m->clip.count = 4;
                m->clip.or_code = 0;
                m->clip.and_code = 0xff;
                for (i = 0; i < 4; i++) {
                    uint8_t oc = (uint8_t)(q[k][i].outcode & 0xff);
                    m->clip.buf[0][i] = q[k][i];
                    m->clip.or_code |= oc;
                    m->clip.and_code &= oc;
                }
                draw_clip_buffer(m);
            }
            return;
        }
    }
    if (m->flag[(0x2930 - 0x2920) / 2]) { draw_gathered_path(m, 0); return; }
    draw_clip_buffer(m);
}

/* Reading a data-segment word the way the conditional skips do: the port
 * keeps the words a list WRITES in named fields, so a read has to consult
 * those first and fall back to the segment image for everything else.
 *
 * BYTE BY BYTE, and the records are SEVEN. The shape records at 0xb07e end
 * where the sprite records at 0xb0b6 begin, and the art-id table at 0xb10f
 * sits inside the range a thirty-two-record reading claimed -- at an odd
 * address, which that reading answered with some record's mask. */
static uint8_t peek_b(const uw_dl *m, const uint8_t *ds, size_t len,
                      uint16_t a) {
    uint16_t w;
    int lo;
    if (a >= 0x2920 && a < 0x2920 + UW_DL_FLAGS * 2) {
        w = m->flag[(a - 0x2920) / 2];
        lo = !((a - 0x2920) & 1);
    } else if (a >= 0xb0b6 && a < 0xb0b6 + UW_DL_SREC * 2) {
        w = (uint16_t)m->srec[(a - 0xb0b6) / 2];
        lo = !((a - 0xb0b6) & 1);
    } else if (a >= 0xb07e && a < 0xb0b6) {
        const uw_rast_texrec *r = &m->rec[(a - 0xb07e) / 8];
        switch (((a - 0xb07e) % 8) / 2) {
        case 0:  w = (uint16_t)r->width; break;
        case 1:  w = (uint16_t)r->v_max; break;
        case 2:  w = r->tex_seg; break;
        default: w = r->v_mask; break;
        }
        lo = !((a - 0xb07e) & 1);
    } else {
        return a < len ? ds[a] : 0;
    }
    return (uint8_t)(lo ? w : w >> 8);
}

static uint16_t peek(const uw_dl *m, const uint8_t *ds, size_t len,
                     uint16_t addr) {
    return (uint16_t)(peek_b(m, ds, len, addr)
                      | (peek_b(m, ds, len, (uint16_t)(addr + 1)) << 8));
}

/* store_imm's destinations, which the shipped lists keep to thirteen. A
 * record's v_max at 0xb07e + 8k + 2 is by far the commonest -- that is a
 * wall's height in texels, written per face -- and the rest are the
 * 0x2920..0x2936 flag words. An address outside both is counted, because
 * a list that stores somewhere else is a list this executor does not
 * understand and saying so is the point. */
static void store_imm(uw_dl *m, uint16_t addr, uint16_t val) {
    if (addr >= 0xb0b6 && addr < 0xb0b6 + UW_DL_SREC * 2 && !(addr & 1)) {
        m->srec[(addr - 0xb0b6) / 2] = (int16_t)val;
        return;
    }
    if (addr >= 0xb07e && addr < 0xb0b6 && !((addr - 0xb07e) & 1)) {
        int k = (addr - 0xb07e) / 8, off = (addr - 0xb07e) % 8;
        uw_rast_texrec *r = &m->rec[k];
        if (off == 0)      r->width = (int16_t)val;
        else if (off == 2) r->v_max = (int16_t)val;
        else if (off == 4) r->tex_seg = val;
        else               r->v_mask = val;
        return;
    }
    if (addr >= 0x2920 && addr < 0x2920 + UW_DL_FLAGS * 2) {
        m->flag[(addr - 0x2920) / 2] = val;
        return;
    }
    m->bad_store++;
}

/* ---- the operand length of every FIXED-LENGTH opcode ------------------
 *
 * Each width is the handler's, kept as data rather than computed because the
 * widths are a property of the original program. 87 of the 151
 * slots are fixed-length; the rest loop over their operands or branch.
 *
 * WHAT THIS TABLE IS FOR. An opcode this module does not EXECUTE still has to
 * be stepped over, or the walk desynchronises and everything after it is
 * noise -- the instruction set has no boundaries other than the ones its
 * handlers consume. So a fixed-length opcode with no case above is consumed
 * and counted as skipped, which keeps the parse exact while saying plainly
 * that its effect is missing. An opcode that is NOT in this table stops the
 * walk, because there is nothing honest to do with it. */
static const struct { uint16_t op; uint8_t bytes; const char *name; }
uw_dl_oplen[] = {
    { 0x00,  0, "ret" },
    { 0x02,  4, "store_imm" },
    { 0x04,  4, "load_ind" },
    { 0x06, 16, "plane3_call" },
    { 0x08,  2, "plot_vertex" },
    { 0x0a,  4, "load_stack" },
    { 0x0c, 12, "call_pair_plane_yz" },
    { 0x0e, 12, "call_pair_plane_xz" },
    { 0x10, 12, "call_pair_plane_xy" },
    { 0x12,  2, "call_rel" },
    { 0x14,  6, "skip_if_gt" },
    { 0x16,  6, "skip_if_lt" },
    { 0x1a,  2, "call_native" },
    { 0x24,  8, "vertex_lerp" },
    { 0x26,  4, "vertex_copy" },
    { 0x28,  2, "call_list" },
    { 0x2e,  2, "set_texture" },
    { 0x30,  2, "call_list_ind" },
    { 0x32,  2, "jmp_ind" },
    { 0x3c,  2, "rotate_point_block" },
    { 0x3e,  6, "emit_face_47ff" },
    { 0x40,  0, "fill_general" },
    { 0x42,  0, "int3" },
    { 0x44,  0, "fill_convex" },
    { 0x46,  0, "int3" },
    { 0x4a,  6, "origin_sub" },
    { 0x4c,  8, "vertex_set_uvw" },
    { 0x4e,  4, "draw_sprite" },
    { 0x50,  4, "call_list_rot_y" },
    { 0x52, 16, "vertex_line" },
    { 0x54,  0, "push_origin" },
    { 0x56,  0, "pop_origin" },
    { 0x58, 14, "skip_if_behind_xyz" },
    { 0x5c,  4, "set_texture_var" },
    { 0x5e, 10, "skip_if_behind_yz" },
    { 0x60, 10, "skip_if_behind_xz" },
    { 0x62, 10, "skip_if_behind_xy" },
    { 0x64,  6, "skip_if_behind_x" },
    { 0x66,  6, "skip_if_behind_y" },
    { 0x68,  6, "skip_if_behind_z" },
    { 0x6c,  8, "call_pair_by_var" },
    { 0x6e,  8, "call_list_anim_rot_x" },
    { 0x70,  4, "call_list_rot_z" },
    { 0x72,  4, "call_list_rot_x" },
    { 0x74,  8, "call_list_anim_rot_z" },
    { 0x76,  8, "call_list_anim_rot_y" },
    { 0x7c,  2, "gather_first" },
    { 0x80,  0, "emit_poly_gathered" },
    { 0x8e,  2, "gather_next" },
    { 0x98, 12, "load_vertex_block" },
    { 0x9a,  4, "load_vertex_block_cam" },
    { 0xac,  0, "nop" },
    { 0xae,  2, "set_colour" },
    { 0xb0,  0, "automap_mark" },
    { 0xb2,  2, "set_shape_record" },
    { 0xba,  4, "call_list_rot_y_var" },
    { 0xbc,  4, "set_colour_lit" },
    { 0xbe,  4, "store_ind" },
    { 0xc2,  4, "call_list_rot_x_var" },
    { 0xc4,  4, "call_list_rot_z_var" },
    { 0xc8,  2, "set_shaded_colour" },
    { 0xca,  2, "set_shaded_colour_var" },
    { 0xd0,  2, "select_shader" },
    { 0xd6,  0, "set_shaded_gouraud" },
    { 0xd8,  0, "set_shaded_remap" },
    { 0xda,  0, "int2_stub" },
    { 0xe0,  2, "gather_first" },
    { 0xe4,  0, "emit_poly_gathered" },
    { 0xf2,  2, "gather_next" },
    { 0xfc, 12, "load_vertex_block" },
    { 0xfe,  4, "load_vertex_block_cam" },
    { 0x102, 10, "skip10" },
    { 0x104,  8, "emit_vertex_word_noclip" },
    { 0x106,  2, "gather_first_noclip" },
    { 0x10a,  0, "emit_poly_gathered_noclip" },
    { 0x110,  6, "vertex_add_u_noclip" },
    { 0x112,  6, "vertex_add_v_noclip" },
    { 0x114,  6, "vertex_add_w_noclip" },
    { 0x116,  4, "vertex_copy_noclip" },
    { 0x118,  2, "gather_next_noclip" },
    { 0x11a,  8, "vertex_add_uv_noclip" },
    { 0x11c,  8, "vertex_add_uw_noclip" },
    { 0x11e,  8, "vertex_add_vw_noclip" },
    { 0x120,  4, "draw_line_noclip" },
    { 0x122, 12, "load_vertex_block_noclip" },
    { 0x124,  4, "load_vertex_block_cam_noclip" },
    { 0x126,  8, "gather_vertex_pair_noclip" },
};

/* rast_sphere_cull: the camera-relative centre -- the three
 * origin words shifted left by `shift` and negated -- through the basis's
 * columns for depth, x and y, and the radius shifted the same way, scaled by
 * the x planes' factor and the y planes' (an unsigned multiply's high word).
 * Wholly outside one plane culls; wholly inside all four and straddling any
 * take different routes to the same key: the depth (0 if negative) shifted
 * right by the shift, divided when the projection's divisor word is
 * negative, halved. Every test is the SIGN of a 16-bit result. Returns 1
 * to cull. */
static int sphere_cull(uw_dl *m, int16_t radius, uint8_t shift, int16_t *key) {
    const int16_t *b = m->basis;
    unsigned sh = shift & 0x1f;
    int16_t R = (int16_t)(sh < 16 ? (uint16_t)radius << sh : 0);
    int16_t X = (int16_t)(0u - (uint16_t)(sh < 16 ? (uint16_t)m->origin[0] << sh : 0));
    int16_t Y = (int16_t)(0u - (uint16_t)(sh < 16 ? (uint16_t)m->origin[1] << sh : 0));
    int16_t Z = (int16_t)(0u - (uint16_t)(sh < 16 ? (uint16_t)m->origin[2] << sh : 0));
    int16_t depth, d;
    int partial = 0, axis;
    int16_t k[2];
#define HI(a, c) ((int16_t)(((int32_t)(a) * (int32_t)(c)) >> 16))
#define ADD(a, c) ((int16_t)((uint16_t)(a) + (uint16_t)(c)))
#define SUB(a, c) ((int16_t)((uint16_t)(a) - (uint16_t)(c)))
    depth = ADD(ADD(HI(X, b[2]), HI(Y, b[5])), HI(Z, b[8]));
    m->sphere_depth = depth;
    if (depth < 0) {
        if (ADD(depth, R) < 0) return 1;
        partial = 1;
    }
    k[0] = (int16_t)(((uint32_t)(uint16_t)R * (uint16_t)m->plane_k[1]) >> 16);
    k[1] = (int16_t)(((uint32_t)(uint16_t)R * (uint16_t)m->plane_k[2]) >> 16);
    for (axis = 0; axis < 2; axis++) {
        int16_t c = axis == 0
            ? ADD(ADD(HI(X, b[0]), HI(Y, b[3])), HI(Z, b[6]))
            : ADD(ADD(HI(X, b[1]), HI(Y, b[4])), HI(Z, b[7]));
        int16_t kk = k[axis];
        /* depth - c: the far plane of the pair */
        d = SUB(depth, c);
        if (d < 0) {
            if (ADD(d, kk) < 0) return 1;
            partial = 1;
        } else if (!partial && SUB(d, kk) < 0) {
            partial = 1;
        }
        /* c + depth: the near plane */
        d = ADD(c, depth);
        if (d < 0) {
            if (ADD(d, kk) < 0) return 1;
            partial = 1;
        } else if (!partial && SUB(d, kk) < 0) {
            partial = 1;
        }
    }
#undef HI
#undef ADD
#undef SUB
    {
        uint16_t ax = depth < 0 ? 0 : (uint16_t)(int16_t)(depth >> (sh & 0xf));
        if (depth >= 0 && m->plane_k[0] < 0) {
            uint16_t cx = (uint16_t)(0u - (uint16_t)m->plane_k[0]);
            int16_t dx = (int16_t)((int16_t)ax >> 1);
            if (dx < (int16_t)cx) {
                /* `cmp dx,cx` leaves the borrow in CF, and `rcr ax,1`
                 * shifts that in: 0x8000 below the halved depth. */
                uint32_t num = ((uint32_t)(uint16_t)dx << 16) | 0x8000u;
                ax = (uint16_t)(num / cx);
            } else {
                ax = 0x7fff;
            }
        }
        *key = (int16_t)(ax >> 1);
    }
    m->sphere_full = !partial;
    return 0;
}

/* ---- the executor ------------------------------------------------------- */

static void do_call_rel(uw_dl *m, stream *s, int depth) {
    uint16_t back;
    int16_t rel = (int16_t)fetch_w(s);
    if (s->bad) return;
    back = s->si;
    s->si = (uint16_t)(s->si + (uint16_t)rel);
    if (depth + 1 >= UW_DL_DEPTH) { s->bad = 1; return; }
    (void)run(m, s, depth + 1);
    s->si = back;
}

static long run(uw_dl *m, stream *s, int depth) {
    long n = 0;

    for (;;) {
        uint16_t at, op;
        if (m->stop_after && m->executed >= m->stop_after) return n;
        at = s->si;
        op = fetch_w(s);
        if (s->bad) return -1;
        m->executed++;
        m->op_at = at;
        if ((op >> 1) < UW_DL_OPS && !(op & 1)) m->op_run[op >> 1]++;
        n++;
        if (m->trace && m->n_trace < m->max_trace) {
            m->trace[m->n_trace * 2] = at;
            m->trace[m->n_trace * 2 + 1] = op;
            m->n_trace++;
        }

        switch (op) {
        case 0x0000:                        /* ret */
            return n;

        case 0x0002: {                      /* store_imm (dest, value) */
            uint16_t a = fetch_w(s), v = fetch_w(s);
            if (s->bad) return -1;
            store_imm(m, a, v);
            break;
        }

        case 0x0012:                        /* call_rel */
            do_call_rel(m, s, depth);
            if (s->bad) return -1;
            break;

        case 0x0018: {                      /* set_origin_nocull */
            /* THREE 32-BIT POSITIONS, TESTED ONE AT A TIME -- AND A CULL
             * STOPS READING. The handler subtracts the camera word, checks
             * with `cwd; cmp bx,dx` that the difference fits a signed word,
             * and on a failure jumps to a tail that sets the cull flag and
             * DISPATCHES: so the operand words it had not reached yet are
             * left in the stream and the next one is executed AS AN OPCODE.
             *
             * A port that consumes all six words on the cull path and
             * carries on runs a different program from there. That is not a
             * detail: it is how a culled object's block gets past, and it
             * was worth one afternoon to find. */
            int i;
            m->cull = 0;
            for (i = 0; i < 3; i++) {
                uint16_t lo = fetch_w(s), hi = fetch_w(s);
                int32_t v = (int32_t)((uint32_t)lo | ((uint32_t)hi << 16));
                int32_t d = v - m->cam[i];
                if (s->bad) return -1;
                if (d < -32768 || d > 32767) { m->cull = 0xffff; break; }
                m->origin[i] = (int16_t)(0u - (uint16_t)(int16_t)d);
            }
            if (m->cull) break;             /* the rest stays in the stream */
            /* rast_origin_patch: the fetch's three immediates, by address. */
            m->slots.origin[0] = m->origin[0];
            m->slots.origin[1] = m->origin[2];
            m->slots.origin[2] = m->origin[1];
            m->slots.fetch_shift = m->add_shift;
            break;
        }

        case 0x0036:                        /* emit_poly4, shader 0x1da */
        case 0x0034: {                      /* emit_poly4, shader 0x545 */
            uw_rast_polyv pv[4];
            uint16_t idx = fetch_w(s);
            int i;
            m->shader = (op == 0x0034) ? 0x545 : 0x1da;
            m->rec_index = idx & (UW_DL_RECS - 1);
            for (i = 0; i < 4; i++) {
                pv[i].slot_off = fetch_w(s);
                pv[i].corner = (uint8_t)fetch_w(s);
            }
            if (s->bad) return -1;
            m->poly_count = 4;                /* mov [0xb00c],cx */
            uw_rast_gather_poly(&m->clip, &m->slots,
                                &m->rec[m->rec_index], pv, 4);
            emit_face_tail(m, 1);
            break;
        }

        case 0x003e: {                      /* set_face_texture */
            uint16_t rec = fetch_w(s), tex = fetch_w(s);
            (void)fetch_w(s);               /* read, shifted, never used */
            if (s->bad) return -1;
            /* The handler maps the texture's EMS page, stores its SEGMENT
             * into the record's third word and then copies the texture's
             * FIRST BYTE into list-header word 11. A port has no segments,
             * so the record keeps the index. */
            m->rec[rec & (UW_DL_RECS - 1)].tex_seg = tex;
            if (tex < UW_DL_TEX && m->tex[tex] && m->tex_len[tex])
                m->flag[(0x2936 - 0x2920) / 2] = m->tex[tex][0];
            break;
        }

        case 0x004c: {                      /* vertex_set_uvw */
            int16_t in[3], out[3];
            uint16_t slot;
            int i;
            for (i = 0; i < 3; i++)
                in[i] = (int16_t)((uint16_t)fetch_w(s)
                                  << (m->add_shift & 15));
            slot = fetch_w(s);
            if (s->bad) return -1;
            /* Through the whole basis and into the slot. The original's
             * _do_defdelta leaves the slot's outcode byte alone, where
             * uw_rast_slot_set writes one -- a difference no frame check has
             * shown, the delta slots' codes being set again before a face
             * reads them in every list compared. */
            {
                int16_t v[3] = { in[0], in[2], in[1] };   /* x, z, y */
                uw_rast_rotate_live(&m->slots, v, out);
            }
            uw_rast_slot_set(&m->slots, slot & (UW_RAST_SLOT_WINDOW - 8), out);
            break;
        }

        case 0x0050:                        /* call_list_rot_y */
        case 0x0072: case 0x0070: {         /* call_list_rot_x, _rot_z */
            /* The same push, turn, select, patch, call, pop and patch about
             * each axis. */
            uint16_t angle = fetch_w(s);
            if (s->bad) return -1;
            call_rotated(m, s, depth, angle,
                         op == 0x0050 ? 1 : op == 0x0072 ? 0 : 2);
            if (s->bad) return -1;
            break;
        }

        case 0x007a: {                      /* emit_vertex_word */
            int16_t w3[3];
            uint16_t slot;
            int i;
            for (i = 0; i < 3; i++) w3[i] = (int16_t)fetch_w(s);
            slot = fetch_w(s);
            if (s->bad) return -1;
            uw_rast_emit_vertex_word(&m->slots, w3, slot & (UW_RAST_SLOT_WINDOW - 8));
            break;
        }

        case 0x0082: case 0x00b8: {
            /* emit_vertices_word / emit_vertices: a count
             * and a first slot, then that many vertices into CONSECUTIVE
             * slots, `add [0x271e],8` a time and no mask on the way. The
             * byte form's count and slot are bytes and the slot an index,
             * and nothing rounds the cursor to even after its 2 + 3n bytes:
             * an odd total leaves the next opcode at an odd address. */
            int i, cnt, off;
            if (op == 0x0082) {
                cnt = fetch_w(s);
                off = fetch_w(s);
            } else {
                cnt = fetch_b(s);
                off = fetch_b(s) << 3;
            }
            if (s->bad) return -1;
            if (cnt == 0) cnt = 0x10000;       /* `dec; jne` from zero */
            for (i = 0; i < cnt; i++) {
                int16_t v[3], r3[3];
                if (op == 0x0082) {
                    int16_t w3[3];
                    int k;
                    for (k = 0; k < 3; k++) w3[k] = (int16_t)fetch_w(s);
                    if (s->bad) return -1;
                    uw_rast_fetch_words(w3, m->slots.origin,
                                        m->slots.fetch_shift, v);
                } else {
                    int8_t b3[3];
                    int k;
                    for (k = 0; k < 3; k++) b3[k] = (int8_t)fetch_b(s);
                    if (s->bad) return -1;
                    uw_rast_fetch_bytes(b3, m->slots.origin,
                                        m->slots.fetch_shift, v);
                }
                uw_rast_rotate_live(&m->slots, v, r3);
                uw_rast_slot_set(&m->slots,
                                 (off + 8 * i) & (UW_RAST_SLOT_WINDOW - 8), r3);
            }
            break;
        }

        case 0x00a2:                        /* emit_poly4b, shader 0x545 */
        case 0x00a0: {                      /* emit_poly4b, shader 0x1da */
            uint8_t idx[4];
            uint16_t rec = fetch_w(s);
            int i;
            m->shader = (op == 0x00a2) ? 0x545 : 0x1da;
            m->rec_index = rec & (UW_DL_RECS - 1);
            for (i = 0; i < 4; i++) idx[i] = fetch_b(s);
            if (s->bad) return -1;
            uw_rast_gather_poly_bytes(&m->clip, &m->slots,
                                      &m->rec[m->rec_index], idx, 4);
            emit_face_tail(m, 1);             /* jmp 0x5e73, count stale */
            break;
        }

        case 0x004a: {                      /* origin_sub */
            int i;
            for (i = 0; i < 3; i++) {
                int16_t v = (int16_t)fetch_w(s);
                m->origin[i] = (int16_t)((uint16_t)m->origin[i]
                                         - (uint16_t)v);
            }
            if (s->bad) return -1;
            origin_patch(m);                /* the fetch shift with it */
            break;
        }

        case 0x0078: {                      /* test_box_visible */
            /* A slot, three half-extents and a relative word. The three
             * extents are shifted by the add shift and put through the basis
             * ROW by row, giving three vectors whose eight sign
             * combinations are the box's corners about the slot's vertex.
             *
             * WHICH PASS RUNS IS DECIDED BY THE CENTRE'S OUTCODE, not by
             * the corners (`mov al,[di+0x1626]; or al,al`).
             *
             * Centre inside: the eight corners are tested against the view
             * cone -- z >= 0, x and y within +-z. All eight in arms the
             * no-clip projection (`mov ax,0xffff; call 0x5040`); any one
             * out falls through to be clipped. Either way the relative word
             * is stepped over UNREAD (`add si,0x2`), so an inside centre
             * never culls.
             *
             * Centre outside: its outcode SEEDS the AND (`mov dl,al`), and
             * each corner's outcode is ANDed in; the first time it reaches
             * zero the box straddles a plane and the word is stepped over.
             * If all eight leave it non-zero the box is wholly outside one
             * plane, and only then is the word READ:
             *
             *     lods ax / or ax,ax / je <ret> / add si,ax
             *     ret
             *
             * A ZERO WORD IS A RETURN, not a skip of nothing. That is what
             * the models use -- the door frame at 0x6948 carries 0 -- so a
             * culled model leaves its sub-list at the test, and a port that
             * adds zero to SI runs the whole model and writes slots the
             * original never touched in that frame.
             *
             * THE ARMING NEVER HAPPENS. `mov ax,0xffff; call 0x5040` loads
             * the flag and rast_projection_select's first instruction is
             * `xor ax,ax`, so the routine can only disarm and the no-clip
             * flag reads 0 in every state. It is counted here so a trace shows
             * where the original meant to skip the clip, and nothing
             * else. */
            uint16_t slot = fetch_w(s);
            int16_t h[3];
            int16_t v[3], p[3][3], c[8][3];
            int i, j;
            uint8_t centre;

            for (i = 0; i < 3; i++)
                h[i] = (int16_t)((uint16_t)fetch_w(s)
                                 << (m->add_shift & 15));
            if (s->bad) return -1;
            slot &= (UW_RAST_SLOT_WINDOW - 8);
            uw_rast_slot_get(&m->slots, slot, v);
            centre = m->slots.outcode[slot + 6];
            for (i = 0; i < 3; i++)
                for (j = 0; j < 3; j++)
                    p[i][j] = (int16_t)(((int32_t)h[i]
                                         * (int32_t)m->basis[i * 3 + j])
                                        >> 16);
            for (i = 0; i < 8; i++)
                for (j = 0; j < 3; j++) {
                    int32_t t = v[j];
                    t += (i & 1) ? -p[0][j] : p[0][j];
                    t += (i & 2) ? -p[1][j] : p[1][j];
                    t += (i & 4) ? -p[2][j] : p[2][j];
                    c[i][j] = (int16_t)(uint16_t)(uint32_t)t;
                }

            if (centre == 0) {
                int all_in = 1;
                for (i = 0; i < 8 && all_in; i++) {
                    int16_t neg = (int16_t)(0u - (uint16_t)c[i][2]);
                    if (c[i][2] < 0 || c[i][0] > c[i][2] || c[i][1] > c[i][2]
                        || c[i][0] < neg || c[i][1] < neg) all_in = 0;
                }
                if (all_in) m->approx++;    /* the arming that is discarded */
                (void)fetch_w(s);
                if (s->bad) return -1;
                break;
            }
            {
                uint8_t dl = centre;
                int16_t rel;
                for (i = 0; i < 8 && dl; i++)
                    dl &= uw_rast_outcode(c[i][0], c[i][1], c[i][2]);
                rel = (int16_t)fetch_w(s);
                if (s->bad) return -1;
                if (dl == 0) break;          /* straddles: clip as usual */
                m->culled++;
                if (rel == 0) return n;
                s->si = (uint16_t)(s->si + (uint16_t)rel);
            }
            break;
        }

        case 0x001c: case 0x0088:           /* vertex_add_v -- basis row 1 */
        case 0x002a: case 0x0086:           /* vertex_add_u -- row 0 */
        case 0x002c: case 0x008a: {         /* vertex_add_w -- row 2 */
            /* TWO OPCODES PER AXIS, AND THEY ARE NOT ALIASES AT RUN TIME.
             * The image's table points 0x2a and 0x86 at the same handler,
             * and a reading of the image says so. But
             * rast_select_rotate_set rewrites 0x86/0x88/0x8a (dispatch words
             * 0x27be..0x27c2) to the axis handlers and never touches
             * 0x2a/0x1c/0x2c (0x2762, 0x2754, 0x2764) -- so the high three
             * follow the installed set and the low three are general
             * always. */
            /* (src slot, scale, dest slot). The scale is shifted by
             * the add shift and multiplied through ONE ROW of the basis, where
             * the rotate reads the same nine words by COLUMN. */
            uint16_t src = fetch_w(s);
            int16_t  sc = (int16_t)fetch_w(s);
            uint16_t dst = fetch_w(s);
            int row = (op == 0x002a || op == 0x0086) ? 0
                    : (op == 0x001c || op == 0x0088) ? 1 : 2;
            if (s->bad) return -1;
            if (op >= 0x86 && m->slots.axis)
                uw_rast_vertex_add_axis(&m->slots, src & (UW_RAST_SLOT_WINDOW - 8), sc, row, dst & (UW_RAST_SLOT_WINDOW - 8));
            else
                uw_rast_vertex_add(&m->slots, src & (UW_RAST_SLOT_WINDOW - 8), sc, row, dst & (UW_RAST_SLOT_WINDOW - 8));
            break;
        }

        case 0x0090: case 0x0092: case 0x0094: {
            /* vertex_add_uv / _uw / _vw: two scales, then source and dest.
             * All three are among the dispatch words the axis set rewrites;
             * their aliases 0xf4/0xf6/0xf8 in the template are not. */
            int16_t a = (int16_t)fetch_w(s), b = (int16_t)fetch_w(s);
            uint16_t src = fetch_w(s), dst = fetch_w(s);
            int ra = op == 0x0094 ? 2 : 0;
            int rb = op == 0x0092 ? 2 : 1;
            if (s->bad) return -1;
            uw_rast_vertex_add_pair(&m->slots, src & (UW_RAST_SLOT_WINDOW - 8),
                                    a, ra, b, rb,
                                    dst & (UW_RAST_SLOT_WINDOW - 8),
                                    m->slots.axis);
            break;
        }

        case 0x008c: {                      /* vertex_sum */
            uint16_t a = fetch_w(s), b2 = fetch_w(s), d = fetch_w(s);
            if (s->bad) return -1;
            uw_rast_vertex_sum(&m->slots, a & (UW_RAST_SLOT_WINDOW - 8), b2 & (UW_RAST_SLOT_WINDOW - 8), d & (UW_RAST_SLOT_WINDOW - 8));
            break;
        }

        case 0x0026: {                      /* vertex_copy */
            /* (DEST, source) -- `lodsw; mov di,ax; lodsw; xchg si,ax`, then
             * three `movsw` and a `movsb`: seven bytes, the outcode carried
             * over rather than recomputed. The no-clip form is (source,
             * dest). */
            uint16_t dst = fetch_w(s), src = fetch_w(s);
            if (s->bad) return -1;
            dst &= (UW_RAST_SLOT_WINDOW - 8);
            src &= (UW_RAST_SLOT_WINDOW - 8);
            m->slots.x[dst / 2] = m->slots.x[src / 2];
            m->slots.x[dst / 2 + 1] = m->slots.x[src / 2 + 1];
            m->slots.x[dst / 2 + 2] = m->slots.x[src / 2 + 2];
            m->slots.outcode[dst + 6] = m->slots.outcode[src + 6];
            break;
        }

        case 0x0014: case 0x0016: {
            /* skip_if_gt / skip_if_lt, and the names are the wrong way
             * round: `cmp [bx],ax; jg <a bare dispatch>` CONTINUES when the
             * word is greater and SKIPS when it is not. Three words -- the
             * skip length first, then the address, then the value -- and the
             * comparison is signed.
             *
             * Stepping over these without evaluating them, which the
             * fixed-width table does, makes the port run blocks the original
             * skips. They are how a list switches on a flag another opcode
             * stored: the shipped ones test list-header words 3 and 10. */
            int16_t len = (int16_t)fetch_w(s);
            uint16_t addr = fetch_w(s);
            int16_t val = (int16_t)fetch_w(s);
            int16_t got;
            if (s->bad) return -1;
            got = (int16_t)peek(m, s->p, s->len, addr);
            if (!(op == 0x0014 ? (got > val) : (got < val)))
                s->si = (uint16_t)(s->si + (uint16_t)len);
            break;
        }

        case 0x0064: case 0x0066: case 0x0068: {
            /* skip_if_behind_x / _y / _z -- three of the seven plane tests,
             * one per single axis. `(constant + origin_axis) ^ mask` and a
             * skip when the sign comes out negative: a one-bit normal needs
             * no multiply, which is why these three exist beside the four
             * that do. The skip length comes FIRST, before the mask and the
             * constant. */
            int16_t len = (int16_t)fetch_w(s);
            int16_t mask = (int16_t)fetch_w(s);
            int16_t k = (int16_t)fetch_w(s);
            int ax = (op == 0x0064) ? 0 : (op == 0x0066) ? 1 : 2;
            int16_t v;
            if (s->bad) return -1;
            v = (int16_t)(((uint16_t)k + (uint16_t)m->origin[ax])
                          ^ (uint16_t)mask);
            if (v < 0) s->si = (uint16_t)(s->si + (uint16_t)len);
            break;
        }

        case 0x0006: case 0x000c: case 0x000e: case 0x0010:
        case 0x006c: {
            /* The ORDERING tests: both sub-lists always run, and the sign of
             * the plane -- or, for call_pair_by_var, whether [variable] is
             * at least the threshold, signed -- says which first. plane3_call
             * over x, y and z, call_pair_plane_yz/xz/xy over two axes, then
             * two relative targets, each from the word after its own
             * operand. A non-negative sum runs A then B; a negative one runs
             * B and then A. */
            static const uint8_t axes[4][3] = {
                { 0, 1, 2 }, { 1, 2, 3 }, { 0, 2, 3 }, { 0, 1, 3 } };
            int a_first, i;
            uint16_t a_to, b_to, back;
            if (op == 0x006c) {
                uint16_t var = fetch_w(s);
                int16_t thr = (int16_t)fetch_w(s);
                if (s->bad) return -1;
                a_first = (int16_t)peek(m, s->p, s->len, var) >= thr;
            } else {
                const uint8_t *ax = axes[op == 0x0006 ? 0 : op == 0x000c ? 1
                                        : op == 0x000e ? 2 : 3];
                uint32_t sum = 0;
                for (i = 0; i < 3 && ax[i] < 3; i++) {
                    int16_t nrm = (int16_t)fetch_w(s);
                    int16_t k = (int16_t)fetch_w(s);
                    int16_t v = (int16_t)((uint16_t)k
                                          + (uint16_t)m->origin[ax[i]]);
                    sum += (uint32_t)((int32_t)v * (int32_t)nrm);
                }
                if (s->bad) return -1;
                a_first = (int32_t)sum >= 0;
            }
            a_to = fetch_w(s);
            a_to = (uint16_t)(a_to + s->si);
            b_to = fetch_w(s);
            b_to = (uint16_t)(b_to + s->si);
            if (s->bad) return -1;
            back = s->si;
            if (depth + 1 >= UW_DL_DEPTH) { s->bad = 1; return -1; }
            s->si = a_first ? a_to : b_to;
            (void)run(m, s, depth + 1);
            if (s->bad) return -1;
            s->si = a_first ? b_to : a_to;
            (void)run(m, s, depth + 1);
            if (s->bad) return -1;
            s->si = back;
            break;
        }

        case 0x0058: case 0x005e: case 0x0060: case 0x0062: {
            /* skip_if_behind_xyz / _yz / _xz / _xy: the plane tests that
             * multiply. The skip length, then
             * (normal, constant) per axis; each constant is added to that
             * axis's origin word in sixteen bits, multiplied by its normal,
             * and the products summed in thirty-two -- `add bp,ax; adc
             * cx,dx; jns` -- so the skip is on the sign of the whole sum. */
            static const uint8_t axes[4][3] = {
                { 0, 1, 2 }, { 1, 2, 3 }, { 0, 2, 3 }, { 0, 1, 3 } };
            const uint8_t *ax = axes[op == 0x0058 ? 0 : op == 0x005e ? 1
                                    : op == 0x0060 ? 2 : 3];
            int16_t len = (int16_t)fetch_w(s);
            uint32_t sum = 0;
            int i;
            for (i = 0; i < 3 && ax[i] < 3; i++) {
                int16_t nrm = (int16_t)fetch_w(s);
                int16_t k = (int16_t)fetch_w(s);
                int16_t v = (int16_t)((uint16_t)k + (uint16_t)m->origin[ax[i]]);
                sum += (uint32_t)((int32_t)v * (int32_t)nrm);
            }
            if (s->bad) return -1;
            if ((int32_t)sum < 0) s->si = (uint16_t)(s->si + (uint16_t)len);
            break;
        }

        case 0x0048: {                      /* jmp: a relative skip */
            int16_t rel = (int16_t)fetch_w(s);
            if (s->bad) return -1;
            s->si = (uint16_t)(s->si + (uint16_t)rel);
            break;
        }

        case 0x0028: {                      /* call_list: an absolute target */
            uint16_t target = fetch_w(s);
            uint16_t back;
            if (s->bad) return -1;
            back = s->si;
            s->si = target;
            if (depth + 1 >= UW_DL_DEPTH) { s->bad = 1; return -1; }
            (void)run(m, s, depth + 1);
            s->si = back;
            break;
        }

        case 0x0030: {                      /* call_list_ind: through a word */
            uint16_t tgt_at = fetch_w(s), back;
            if (s->bad) return -1;
            if ((size_t)tgt_at + 2 > s->len) { s->bad = 1; return -1; }
            back = s->si;
            s->si = (uint16_t)(s->p[tgt_at] | (s->p[tgt_at + 1] << 8));
            if (depth + 1 >= UW_DL_DEPTH) { s->bad = 1; return -1; }
            (void)run(m, s, depth + 1);
            s->si = back;
            break;
        }

        case 0x00ba:                        /* call_list_rot_y_var */
        case 0x00c2: case 0x00c4: {         /* _rot_x_var, _rot_z_var */
            /* As 0x50, but the angle is read FROM A WORD the first operand
             * addresses: `lodsw; mov bx,ax; mov bx,[bx]` and then straight
             * into 0x50's body. */
            uint16_t ang_at = fetch_w(s);
            if (s->bad) return -1;
            call_rotated(m, s, depth, peek(m, s->p, s->len, ang_at),
                         op == 0x00ba ? 1 : op == 0x00c2 ? 0 : 2);
            if (s->bad) return -1;
            break;
        }

        case 0x00bc: {                      /* set_colour_lit */
            /* Two words, read with two `lods` on one path and `add si,0x4`
             * on the other. The first ADDRESSES a base colour; the second's
             * low byte is a light level, to which list-header word 10's low
             * byte is added in eight bits and clamped to 15, and its high
             * byte an offset. The colour is LIGHT.DAT[level * 256 + offset +
             * base] -- or, with word 9's low byte set, the fixed colour --
             * and set_texture's tail installs it with the flat pair. */
            uint16_t base_at = fetch_w(s), lw = fetch_w(s);
            if (s->bad) return -1;
            if (m->flag[(0x2932 - 0x2920) / 2] & 0xff) {
                set_flat_colour(m, m->fixed_colour);
            } else {
                uint8_t level = (uint8_t)((lw & 0xff)
                              + (m->flag[(0x2934 - 0x2920) / 2] & 0xff));
                uint16_t idx;
                if (level > 15) level = 15;
                idx = (uint16_t)((level << 8) + (lw >> 8)
                                 + peek(m, s->p, s->len, base_at));
                if (!m->light) { m->approx++; set_flat_colour(m, 0); }
                else if (idx >= 4096) { m->light_overrun++; set_flat_colour(m, 0); }
                else set_flat_colour(m, m->light[idx]);
            }
            break;
        }

        case 0x002e:                        /* set_texture: a colour mode */
            {
                uint16_t mode = fetch_w(s);
                if (s->bad) return -1;
                set_flat_colour(m, mode);
            }
            break;

        case 0x005c: {                      /* set_texture_var */
            uint16_t at_w = fetch_w(s), add = fetch_w(s);
            if (s->bad) return -1;
            set_flat_colour(m, (uint16_t)(add + peek(m, s->p, s->len, at_w)));
            break;
        }

        case 0x00ae: {                      /* set_colour */
            /* The operand's low byte into the fixed colour set_colour_lit
             * and set_shaded_gouraud take when list-header word 9 is set --
             * which is how the pick map paints each object its index: the
             * emitter writes this with the object count only while one is
             * being built. */
            uint16_t c = fetch_w(s);
            if (s->bad) return -1;
            m->fixed_colour = (uint8_t)c;
            break;
        }

        case 0x0044:                        /* fill_convex */
            m->poly_fn = 0x438d;
            break;

        case 0x0040:                        /* fill_general */
            m->poly_fn = 0x2e7c;
            break;

        case 0x00c8: case 0x00ca:           /* set_shaded_colour[_var] */
            /* The shaded pair and a byte stored, and NO span handlers:
             * whichever pair was installed last stays. */
            (void)fetch_w(s);
            if (s->bad) return -1;
            m->poly_fn = 0x6146;
            break;

        case 0x00d6:                        /* set_shaded_gouraud */
            /* The shaded pair and the Gouraud span handlers -- or, with
             * list-header word 9 set, the fixed colour through
             * set_texture's tail. */
            if (m->flag[(0x2932 - 0x2920) / 2] & 0xff) {
                set_flat_colour(m, m->fixed_colour);
            } else {
                m->poly_fn = 0x6146;
                m->span_gouraud = 1;
            }
            break;

        case 0x00d8:                        /* set_shaded_remap */
            m->poly_fn = 0x6146;
            m->span_gouraud = 0;
            break;

        case 0x007c: case 0x00e0:           /* gather_first */
        case 0x008e: case 0x00f2: {         /* gather_next */
            uint16_t slot = fetch_w(s);
            if (s->bad) return -1;
            gather_slot(m, slot, op == 0x007c || op == 0x00e0);
            break;
        }

        case 0x00cc: case 0x00d4: {         /* set_vertex_shade[_raw] */
            /* A count, then for 0xd4 the address of a word whose low byte
             * becomes the Gouraud base colour, then per vertex
             * a slot word and a shade byte into the slot's eighth byte --
             * for 0xd4 plus list-header word 10's low byte in eight bits,
             * clamped to 14. Both end `inc si; and si,0xfffe`: the odd bytes
             * round the cursor up to even before the next opcode. */
            int i, cnt = (int)fetch_w(s);
            uint8_t bias = 0;
            if (s->bad) return -1;
            if (cnt < 0 || cnt > 256) { s->bad = 1; return -1; }
            if (op == 0x00d4) {
                uint16_t at_w = fetch_w(s);
                if (s->bad) return -1;
                m->shade_base = (uint8_t)peek(m, s->p, s->len, at_w);
                bias = (uint8_t)m->flag[(0x2934 - 0x2920) / 2];
            }
            for (i = 0; i < cnt; i++) {
                uint16_t slot = fetch_w(s);
                uint8_t sh = (uint8_t)(fetch_b(s) + bias);
                if (s->bad) return -1;
                if (op == 0x00d4 && sh > 14) sh = 14;
                m->slots.outcode[(slot + 7) & (UW_RAST_SLOT_WINDOW - 1)] = sh;
            }
            if (s->si & 1) (void)fetch_b(s);
            if (s->bad) return -1;
            break;
        }

        case 0x0080: case 0x00e4:           /* emit_poly_gathered */
            emit_gathered(m);
            break;

        case 0x00be: {                      /* store_ind */
            /* `mov di,w0; mov bx,w1; mov bx,[bx]; mov ax,[di]; mov [bx],ax`
             * -- the word AT w0 stored at the address held IN w1. The door
             * frame uses it as `store_ind 0x292e, 0x292c` to put a v_max the
             * list stored in a header word into the shape record whose
             * address another header word holds. */
            uint16_t val_at = fetch_w(s), ptr_at = fetch_w(s);
            if (s->bad) return -1;
            store_imm(m, peek(m, s->p, s->len, ptr_at),
                      peek(m, s->p, s->len, val_at));
            break;
        }

        case 0x0022: case 0x007e: case 0x00e2: {
            /* gather_vertices: a count and that many slot
             * words, copied into the clip buffer as EIGHT-byte records --
             * x, y, z and the outcode, with no u or v. If every vertex is
             * outside one plane (`je` on the last `and bl,dl`) it stops
             * there; otherwise it falls into emit_poly_gathered. */
            int i, cnt = (int)fetch_w(s);
            if (s->bad) return -1;
            if (cnt < 0 || cnt > UW_CLIP_MAX) { s->bad = 1; return -1; }
            for (i = 0; i < cnt; i++) {
                uint16_t slot = fetch_w(s);
                if (s->bad) return -1;
                gather_slot(m, slot, i == 0);
            }
            if (cnt == 0 || m->clip.and_code) { m->dropped++; break; }
            emit_gathered(m);
            break;
        }

        case 0x00a8: case 0x00aa:           /* emit_poly_shape_* */
        case 0x00b4: case 0x00ce: {         /* _record_* and _selected */
            /* A count and then (slot, u, v) per vertex, with u and v
             * NORMALISED and the record scaling them. 0xa8/0xaa read a
             * shape index first; 0xb4 and 0xce use whatever
             * set_shape_record left. */
            uint16_t v3[UW_CLIP_MAX * 3];
            int i, cnt;
            m->shader = (op == 0x00aa) ? 0x545
                      : (op == 0x00ce) ? m->shader_sel : 0x1da;
            if (op == 0x00a8 || op == 0x00aa)
                m->rec_index = fetch_w(s) & (UW_DL_RECS - 1);
            cnt = (int)fetch_w(s);
            if (s->bad) return -1;
            if (cnt < 1 || cnt > UW_CLIP_MAX) { s->bad = 1; return -1; }
            for (i = 0; i < cnt * 3; i++) v3[i] = fetch_w(s);
            if (s->bad) return -1;
            uw_rast_gather_poly_uv(&m->clip, &m->slots,
                                   &m->rec[m->rec_index], v3, cnt);
            emit_face_tail(m, 0);             /* jmp 0x60e0 */
            break;
        }

        case 0x00b2: {                      /* set_shape_record */
            uint16_t idx = fetch_w(s);
            if (s->bad) return -1;
            m->rec_index = idx & (UW_DL_RECS - 1);
            break;
        }

        case 0x00b6: {                      /* emit_vertex */
            int8_t b3[3];
            uint8_t slot;
            int i;
            for (i = 0; i < 3; i++) b3[i] = (int8_t)fetch_b(s);
            slot = fetch_b(s);
            if (s->bad) return -1;
            uw_rast_emit_vertex(&m->slots, b3, slot);
            break;
        }

        case 0x00d0: {                      /* select_shader */
            uint16_t v = fetch_w(s);
            if (s->bad) return -1;
            /* `test al,0xff` -- the LOW BYTE only, so 0x0100 selects 0x1da. */
            m->shader_sel = (v & 0xff) ? 0x545 : 0x1da;
            break;
        }

        /* ---- present in the shipped lists, deliberately not run ------- */
        case 0x003a: case 0x004e: {
            /* emit_sprite: (index, light, slot). An index below
             * 0x1c0 goes through [0xb90f + 4i] & 0x3ff and then
             * [0xb10f + 2*that]; one at or above skips the first table as
             * i - 0x1c0 + [0xb0f0]. The word found is an art id;
             * the decoded image's segment and header bytes 1 and 2 go into
             * the record at 0xb0d2, and the far jump into draw_sprite at 684b
             * brings AX = 0x1c.
             *
             * draw_sprite (0x4e, 6843): (slot, record offset), the same
             * placement on a record already there. */
            uint16_t a1 = fetch_w(s), a2 = fetch_w(s);
            uint16_t slot, rec_off, art_id = 0;
            uint8_t level = 0xff;
            int16_t rec[7];
            int i;
            if (op == 0x3a) {
                const uint8_t *art;
                size_t alen = 0;
                int16_t idx = (int16_t)a1, n2;
                slot = fetch_w(s);
                if (s->bad) return -1;
                level = (uint8_t)a2;
                if (idx >= 0x1c0) n2 = (int16_t)(idx - 0x1c0 + (int16_t)peek(m, s->p, s->len, 0xb0f0));
                else n2 = (int16_t)(peek(m, s->p, s->len, (uint16_t)(0xb90f + 4 * (uint16_t)idx)) & 0x3ff);
                art_id = peek(m, s->p, s->len, (uint16_t)(0xb10f + 2 * (uint16_t)n2));
                if (!m->art) { m->skipped++; break; }
                art = m->art(m->art_ctx, art_id, &alen);
                if (!art || alen < 3) { m->approx++; break; }
                /* `stosw` the segment, `movsb` byte 1, `inc di`, `movsb`
                 * byte 2 -- the high bytes of width and height are left. */
                m->srec[(0xb0d2 - 0xb0b6) / 2] = (int16_t)art_id;
                m->srec[(0xb0d4 - 0xb0b6) / 2] = (int16_t)((m->srec[(0xb0d4 - 0xb0b6) / 2] & 0xff00) | art[1]);
                m->srec[(0xb0d6 - 0xb0b6) / 2] = (int16_t)((m->srec[(0xb0d6 - 0xb0b6) / 2] & 0xff00) | art[2]);
                rec_off = 0x1c;
            } else {
                if (s->bad) return -1;
                slot = a1;
                rec_off = a2;
                m->approx++;              /* no art id: the record's segment */
            }
            for (i = 0; i < 7; i++)
                rec[i] = (int16_t)peek(m, s->p, s->len,
                                       (uint16_t)(0xb0b6 + rec_off + 2 * i));
            {
                uw_sprite_desc desc;
                int draw = uw_rast_sprite_place(&m->slots,
                                                slot & (UW_RAST_SLOT_WINDOW - 2),
                                                rec, (int16_t)peek(m, s->p, s->len, 0x26d0),
                                                m->basis[4], &m->proj, &desc);
                if (!draw) { m->sprites_culled++; break; }
                m->sprites++;
                if (m->n_sprites < UW_DL_SPRITES) {
                    int k = m->n_sprites++;
                    m->sprite[k].art_id = art_id;
                    m->sprite[k].level = level;
                    m->sprite[k].creature = 0;
                    m->sprite[k].blitter = m->row_blitter;
                    m->sprite[k].fill = m->fixed_colour;
                    m->sprite[k].at = m->op_at;
                    m->sprite[k].desc = desc;
                    if (m->n_events < UW_DL_EVENTS)
                        m->event[m->n_events++] = (uint16_t)(0x8000 | k);
                    if (m->fb.pixels && op == 0x3a)
                        draw_sprite_event(m, &m->fb, k);
                }
            }
            break;
        }
        case 0x005a: {
            /* emit_creature: (type, light, frame, direction,
             * slot). The type's two bytes at 0xc10f + 2*type are the page
             * index (0xff: no creature -- the other four words are stepped
             * over) and the palette variant; the frame against the three
             * thresholds at 0xc28f + 3*page picks the file; the page is
             * walked; the frame's width, height and hot spot go into the
             * record at 0xb0c4, and the placement runs on it with
             * AX = 0x0e. */
            uint16_t type = fetch_w(s), light_w, frame_w, dir_w, slot;
            uint8_t page_ix, variant;
            int file = 0, k;
            const uint8_t *page;
            size_t plen = 0;
            uw_creature_frame fr;
            int16_t rec[7];
            if (s->bad) return -1;
            page_ix = (uint8_t)peek(m, s->p, s->len, (uint16_t)(0xc10f + 2 * type));
            variant = (uint8_t)(peek(m, s->p, s->len, (uint16_t)(0xc10f + 2 * type + 1)));
            if (page_ix == 0xff) {                /* 7b00: add si,8 */
                for (k = 0; k < 4; k++) (void)fetch_w(s);
                if (s->bad) return -1;
                m->skipped++;
                break;
            }
            light_w = fetch_w(s);
            frame_w = fetch_w(s);
            for (k = 0; k < 3; k++) {             /* cmp al,[bx]; jb */
                uint8_t thr = (uint8_t)peek(m, s->p, s->len,
                                            (uint16_t)(0xc28f + 3 * page_ix + k));
                if ((uint8_t)frame_w < thr) break;
                file++;
            }
            dir_w = fetch_w(s);
            if (s->bad) return -1;
            if (!m->crit_page) {
                (void)fetch_w(s);
                if (s->bad) return -1;
                m->skipped++;
                break;
            }
            page = m->crit_page(m->art_ctx, page_ix, file, &plen);
            if (!page) {                          /* the pager's failure */
                (void)fetch_w(s);
                if (s->bad) return -1;
                m->approx++;
                break;
            }
            if (!uw_creature_find(page, plen, frame_w, dir_w, variant, &fr)) {
                (void)fetch_w(s);                 /* 7ccf: add si,2 */
                if (s->bad) return -1;
                break;
            }
            slot = fetch_w(s);
            if (s->bad) return -1;
            /* 7d59: `stosw` the segment and `movsb` four header bytes into
             * every other byte from 0xb0c6 -- width, height, hot spot. */
            m->srec[(0xb0c4 - 0xb0b6) / 2] = 0;
            m->srec[(0xb0c6 - 0xb0b6) / 2] = (int16_t)((m->srec[(0xb0c6 - 0xb0b6) / 2] & 0xff00) | fr.width);
            m->srec[(0xb0c8 - 0xb0b6) / 2] = (int16_t)((m->srec[(0xb0c8 - 0xb0b6) / 2] & 0xff00) | fr.height);
            m->srec[(0xb0ca - 0xb0b6) / 2] = (int16_t)((m->srec[(0xb0ca - 0xb0b6) / 2] & 0xff00) | fr.hot_x);
            m->srec[(0xb0cc - 0xb0b6) / 2] = (int16_t)((m->srec[(0xb0cc - 0xb0b6) / 2] & 0xff00) | fr.hot_y);
            for (k = 0; k < 7; k++)
                rec[k] = m->srec[(0xb0c4 - 0xb0b6) / 2 + k];
            {
                uw_sprite_desc desc;
                int draw = uw_rast_sprite_place(&m->slots,
                                                slot & (UW_RAST_SLOT_WINDOW - 2),
                                                rec, (int16_t)peek(m, s->p, s->len, 0x26d0),
                                                m->basis[4], &m->proj, &desc);
                if (!draw) { m->sprites_culled++; break; }
                m->creatures++;
                if (m->n_sprites < UW_DL_SPRITES) {
                    int q = m->n_sprites++;
                    m->sprite[q].creature = 1;
                    m->sprite[q].blitter = m->row_blitter;
                    m->sprite[q].fill = m->fixed_colour;
                    m->sprite[q].crit_page = page_ix;
                    m->sprite[q].crit_file = (uint8_t)file;
                    m->sprite[q].frame = fr;
                    m->sprite[q].level = (uint8_t)light_w;
                    m->sprite[q].at = m->op_at;
                    m->sprite[q].desc = desc;
                    if (m->n_events < UW_DL_EVENTS)
                        m->event[m->n_events++] = (uint16_t)(0x8000 | q);
                    if (m->fb.pixels) draw_sprite_event(m, &m->fb, q);
                }
            }
            break;
        }
        case 0x00b0:                        /* automap_mark, do_mouseq: NO operands; the mouse is the host's */
            m->skipped++;
            break;
        case 0x00c0: {                      /* load_texture_image */
            /* An image index through the art-id table at 0xb10f to a
             * segment (rast_art_segment), and then shape record 6 becomes
             * that image: width the header's byte 1, v_max its size word at
             * +3 less one, the segment the scratch, and the mask (height - 1) << log2(width), with the
             * logarithm counted as `shl al,1` until a bit falls out. The
             * size word's bytes from +5 are copied into the scratch as they
             * are -- unlit, and whatever the image's type. The second word
             * is pushed and popped and nothing reads it. */
            uint16_t idx = fetch_w(s), id;
            const uint8_t *art;
            size_t len = 0, size;
            uw_rast_texrec *r = &m->rec[6];
            int cl = 8;
            uint8_t al;
            (void)fetch_w(s);
            if (s->bad) return -1;
            {
                uint32_t tab = (0xb10fu + 2u * idx) & 0xffff;
                id = (tab + 1 < s->len)
                   ? (uint16_t)(s->p[tab] | (s->p[tab + 1] << 8)) : 0;
            }
            art = m->art ? m->art(m->art_ctx, id, &len) : NULL;
            if (!art || len < 5) { m->skipped++; break; }
            r->width = art[1];
            size = (size_t)(art[3] | (art[4] << 8));
            r->v_max = (int16_t)(size - 1);
            for (al = art[1]; cl > 0;) {    /* dec cl; shl al,1; jae */
                cl--;
                if (al & 0x80) break;
                al = (uint8_t)(al << 1);
            }
            r->v_mask = (uint16_t)(((art[2] - 1) & 0xff) << cl);
            if (size > len - 5) size = len - 5;
            if (size > sizeof m->image) size = sizeof m->image;
            /* One segment in the original, rewritten each time; here each
             * load gets its own copy so the faces drawn from an earlier one
             * keep it (record_face). The arena wraps when full. */
            if (m->image_used + size > sizeof m->image) m->image_used = 0;
            memcpy(m->image + m->image_used, art + 5, size);
            m->tex[UW_DL_IMAGE] = m->image + m->image_used;
            m->tex_len[UW_DL_IMAGE] = size;
            m->image_used += size;
            r->tex_seg = UW_DL_IMAGE;
            break;
        }
        case 0x0038: {                      /* set_origin_or_skip */
            /* Twelve words: a skip target relative to the word
             * after it, a signed pre-shift, the x extent, then x as 32 bits,
             * the y extent, y, the z extent, z, and a radius. Each axis is
             * differenced against the camera in 32 bits and shifted -- a
             * negative count LEFT, a positive one arithmetically right --
             * and must then fit a word, or the cull flag is set and the list
             * resumes at the target. The origin word is the negated
             * difference; the magnitude is x extent + |x|, then ORed with
             * y extent + |y| and z extent + |z|, a signed overflow in any of
             * the three adds culling as the misfit does.
             *
             * Then two quiet rejections that leave the cull flag at zero: the
             * radius plus the origin's x word negative, and
             * rast_sphere_cull. Between them the fetch shift comes from the
             * magnitude through the shift table -- the high byte's
             * entry, or the low byte's plus 8 -- and a second shift from the
             * magnitude ORed with the radius goes to the sphere test. */
            int16_t rel = (int16_t)fetch_w(s);
            uint16_t skip_to = (uint16_t)(s->si + (uint16_t)rel);
            int16_t pre = (int16_t)fetch_w(s);
            uint16_t bp = fetch_w(s);
            uint16_t ext[3] = { 0, 0, 0 }, w8_hi = 0;
            int16_t org[3], radius;
            int i, overflow = 0;

            m->cull = 0;
            for (i = 0; i < 3; i++) {
                uint16_t lo, hi;
                int32_t v;
                if (i) ext[i] = fetch_w(s);
                lo = fetch_w(s); hi = fetch_w(s);
                if (s->bad) return -1;
                v = (int32_t)((uint32_t)lo | ((uint32_t)hi << 16)) - m->cam[i];
                if (pre < 0) {
                    int k;
                    for (k = 0; k < -pre; k++) v = (int32_t)((uint32_t)v << 1);
                } else if (pre > 0) {
                    int k;
                    for (k = 0; k < pre; k++) v >>= 1;
                }
                if (v < -32768 || v > 32767) { overflow = 1; break; }
                org[i] = (int16_t)(0u - (uint16_t)(int16_t)v);   /* neg ax */
                /* stored as soon as it is made, before the next axis's
                 * tests: a cull part way leaves the axes before it
                 * written */
                m->origin[i] = org[i];
                {
                    int16_t a = org[i] < 0 ? (int16_t)(0u - (uint16_t)org[i])
                                           : org[i];
                    int32_t sum = i == 0 ? (int32_t)(int16_t)bp + a
                                         : (int32_t)(int16_t)ext[i] + a;
                    if (sum < -32768 || sum > 32767) { overflow = 1; break; }
                    if (i == 0) bp = (uint16_t)sum;
                    else bp = (uint16_t)(bp | (uint16_t)sum);
                }
            }
            if (overflow) {
                m->cull = 0xffff;
                s->si = skip_to;
                break;
            }
            w8_hi = (uint16_t)(ext[2] >> 8);
            radius = (int16_t)fetch_w(s);
            if (s->bad) return -1;
            for (i = 0; i < 3; i++) m->origin[i] = org[i];
            if ((int16_t)(m->depth_sub
                          ? (uint16_t)radius - (uint16_t)m->origin[m->depth_index % 3]
                          : (uint16_t)radius + (uint16_t)m->origin[m->depth_index % 3]) < 0) {
                m->culled++;                /* 1cd8: js, 0x2882 left clear */
                s->si = skip_to;
                break;
            }
            if (!m->shift_table) { m->approx++; break; }
            {
                uint8_t shift1, shift2;
                int16_t key;
                shift1 = (bp >> 8) ? m->shift_table[bp >> 8]
                                   : (uint8_t)(m->shift_table[bp & 0xff] + 8);
                m->add_shift = shift1;
                m->slots.add_shift = shift1;
                bp = (uint16_t)(bp | (uint16_t)radius);
                shift2 = (bp >> 8) ? m->shift_table[bp >> 8]
                                   : (uint8_t)(m->shift_table[bp & 0xff] + 8);
                m->sphere_shift = (uint16_t)(w8_hi << 8 | shift2);
                if (sphere_cull(m, radius, shift2, &key)) {
                    m->culled++;
                    s->si = skip_to;
                    break;
                }
                m->depth_key = key;
            }
            origin_patch(m);
            break;
        }

        default: {
            /* A fixed-length opcode with no case above is STEPPED OVER and
             * counted; one that is not in the table ends the walk, and says
             * which and where. Guessing a length would misparse everything
             * after it. */
            size_t k;
            for (k = 0; k < sizeof uw_dl_oplen / sizeof uw_dl_oplen[0]; k++)
                if (uw_dl_oplen[k].op == op) {
                    int j;
                    m->op_stepped[op >> 1]++;
                    for (j = 0; j < uw_dl_oplen[k].bytes; j++)
                        (void)fetch_b(s);
                    if (s->bad) return -1;
                    m->skipped++;
                    break;
                }
            if (k < sizeof uw_dl_oplen / sizeof uw_dl_oplen[0]) break;
            m->unknown++;
            m->last_unknown = op;
            m->stopped_at = (uint16_t)(s->si - 2);
            return n;
        }
        }
    }
}

long uw_dl_run(uw_dl *m, const uint8_t *list, size_t len, uint16_t pc) {
    stream s;
    long r;

    if (!m || !list) return -1;
    s.p = list; s.len = len; s.si = pc; s.bad = 0;
    m->slots.basis = m->basis;
    m->slots.add_shift = m->add_shift;
    /* rast_execute selects the rotate set for the frame's basis
     * before it runs anything. */
    m->slots.axis = uw_rast_select_rotate_set(m->basis);
    r = run(m, &s, 0);
    return s.bad ? -1 : r;
}
