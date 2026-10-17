/* SPDX-License-Identifier: MIT */
/* The draw-list executor -- rast_execute and the opcode handlers
 * the shipped lists use.
 *
 * A UW frame's 3-D view is a PROGRAM. The game builds a list of 16-bit
 * opcodes per frame, one per tile and per object, and the rasteriser threads
 * through it: every handler ends `lodsw; xchg ax,bx; jmp [bx+0x2738]`, so
 * the next opcode word is a BYTE OFFSET into a dispatch table and there is
 * no loop at all. The table is filled in at run time.
 *
 * WHY THIS MODULE EXISTS. Everything under it -- the vertex fetch, the
 * rotate, the slot opcodes, the clip, the projection, both texture mappers
 * -- is ported. This is the thing that drives them: a frame's own draw
 * list executed against its own tables.
 *
 * WHAT IT COVERS. Every opcode the shipped lists use,
 * the rotate set rast_select_rotate_set installs, the near-wall subdivision,
 * both texture mappers and the lighting pass over them, the sprites and
 * creatures and the shaded untextured faces. One opcode, automap_mark (0xb0)
 * -- do_mouseq, the mouse serviced mid-frame -- is the host's: COUNTED AND
 * SKIPPED, the shell polling once after the draw.
 *
 * TWO COORDINATE ORDERS LIVE IN THIS INSTRUCTION SET, and they are not the
 * same one twice. A VERTEX's three list words are x, z, y -- the fetch
 * writes BX, BP, CX and the rotate reads BX, CX, BP. An ORIGIN's three are
 * x, y, z: set_origin subtracts them from the three camera words in that
 * order, and rast_origin_patch then writes them into the fetch's three
 * `sub ax,imm` immediates BY ADDRESS -- the first takes origin x, the
 * second origin z, the third origin y -- which is where the two orders are
 * reconciled.
 */
#ifndef UW_DRAWLIST_H
#define UW_DRAWLIST_H

#include "uw.h"
#include "uw_rast.h"
#include "uw_texmap.h"
#include "uw_shade.h"
#include "uw_sprite.h"
#include "uw_poly.h"

#define UW_DL_RECS   32      /* shape records, 8 bytes each */
#define UW_DL_TEX    128     /* texture slots: 116 loaded */
#define UW_DL_IMAGE  127     /* the slot load_texture_image's scratch takes */
#define UW_DL_FACES  256     /* faces kept for scoring one at a time */
#define UW_DL_FACEV  100     /* vertices kept per face: gfx_draw_polygon
                              * refuses more than 99, and the shrine draws 24 */
#define UW_DL_OPS    152     /* dispatch slots, opcode >> 1 */
#define UW_DL_SPRITES 128    /* sprites kept for replay */
#define UW_DL_EVENTS (UW_DL_FACES + UW_DL_SPRITES)
#define UW_DL_SREC   21      /* the three 7-word sprite records -- and no
                              * further: other data and the art-id table
                              * follow */
#define UW_DL_FLAGS  16      /* the per-face words */
#define UW_DL_DEPTH  8       /* sub-list nesting; the shipped lists use 2 */

typedef struct {
    /* ---- the rasteriser's data segment, the parts a list touches ------ */
    uw_rast_slots  slots;
    int16_t        basis[9];
    int16_t        origin[3];
    /* rast_patch_depth_axis's rewrite of set_origin_or_skip's behind test
     * (`add ax,[0x2886]` as shipped): which origin word is
     * depth, and whether the radius is added to it or has it subtracted.
     * Zero for both is the shipped instruction. */
    int            depth_index;           /* 0, 1 or 2: (operand - 0x2886) / 2 */
    int            depth_sub;             /* opcode 0x2b rather than 0x03 */
    int32_t        cam[3];
    uint16_t       cull;
    int16_t        plane_k[3];            /* the sphere test's divisor and its
                                           * x- and y-plane radius scales */
    int16_t        depth_key;             /* the last object's */
    int16_t        sphere_depth;
    uint16_t       sphere_shift;
    int            sphere_full;           /* rast_sphere_cull's SI: 0xffff
                                           * wholly inside, 0 straddling */
    int            add_shift;
    uint16_t       shader;                /* the emitter's choice */
    uint16_t       shader_sel;            /* select_shader's */
    int            rec_index;             /* as an index */
    uint16_t       poly_count;            /* written by emit_poly3/4 ONLY --
                                           * emit_poly4b reads it stale */
    uw_rast_texrec rec[UW_DL_RECS];
    uint16_t       flag[UW_DL_FLAGS];

    /* ---- what the host supplies --------------------------------------- */
    const uint8_t *tex[UW_DL_TEX];        /* a texture index's pixels */
    size_t         tex_len[UW_DL_TEX];
    uw_rast_proj   proj;
    const uint8_t *shift_table;           /* 256 bytes */
    uw_fb          fb;
    int            noclip;
    /* The lighting pass (rast_draw_face_lit): the 4096-byte table and the
     * three words of its parameters. With no table the
     * faces are drawn unlit and `approx` says so. */
    const uint8_t *light;
    uw_light_params light_params;
    long           light_overrun;
    int            no_mapper;             /* a harness's probe: light only */
    /* Sprites (emit_sprite, draw_sprite): the art an id names -- in the
     * original a segment in EMS physical page 2, rast_art_segment -- the
     * auxiliary palettes, XFER.DAT as loaded and
     * the clip bounds gfx_draw_sprite_scaled clips to. With no art hook the
     * sprites are counted and skipped, as before. */
    const uint8_t *(*art)(void *ctx, uint16_t art_id, size_t *len);
    /* Creatures (emit_creature): a CRIT\CRxxPAGE.Nyy file by page index and
     * file number, which is what rast_crit_page_map reads into EMS. */
    const uint8_t *(*crit_page)(void *ctx, int page, int file, size_t *len);
    void          *art_ctx;
    const uint8_t *auxpals;
    size_t         auxpals_len;
    const uint8_t *xfer;
    uw_clip_bounds sprite_clip;
    int16_t        srec[UW_DL_SREC];      /* read and written */
    /* Flat faces. `poly_fn` is the polygon routine the gathered path and
     * rast_poly_emit_tail's second path call: 0x438d, gfx_draw_polygon,
     * after set_texture, set_colour_lit and fill_convex; 0x6146, the
     * shaded one, after the set_shaded_* opcodes; 0x2e7c, the general
     * fill. The colour is the graphics module's -- the colour byte and
     * the fill that gfx_set_colour_mode_spans leaves -- and `gfx_ds` is
     * that module's segment image, for the two colour-mode tables and the
     * fixed colour.
     * Without it a colour mode below 256 is its own palette index. The
     * clip is the one view_present sets around the list, (0, view height
     * - 1, view width - 1, 0), which the state after the frame no longer
     * holds. */
    const uint8_t *gfx_ds;
    uint16_t       poly_fn;
    uint8_t        colour;
    uint16_t       fill_fn;
    uint8_t        shade_base;            /* set_vertex_shade's */
    uint8_t        fixed_colour;          /* set_colour's */
    uint8_t        row_blitter;           /* gfx_row_blitter_slot's row, 1..3
                                           * (uw_gfx_draw_sprite_rows); 0 is 1.
                                           * Not in the list: view_build_draw_list
                                           * selects 1 and drawlist_begin_frame,
                                           * the pick map's, 2 */
    int            span_gouraud;          /* 1 the Gouraud pair, 0 the remap
                                           * pair */
    uw_clip_rect   view_clip;             /* during the list */
    uw_spans       spans;

    /* ---- scratch and the tally --------------------------------------- */
    uw_rast_clip   clip;
    uint8_t        image[0x40000];        /* load_texture_image's: an arena
                                           * of every load's copy */
    size_t         image_used;
    uint8_t        ucol[256];             /* the wall mapper's columns */
    int32_t        wall_dv;               /* its span v step */
    uw_rast_svert  sv[UW_CLIP_MAX];
    long           executed;              /* opcodes run */
    long           faces;                 /* faces that reached a shader */
    long           dropped;               /* faces the clip rejected */
    long           skipped;               /* opcodes deliberately not run */
    long           unknown;               /* opcodes with no handler here */
    long           bad_store;             /* store_imm to an unknown address */
    long           approx;                /* ran, but without something the
                                           * host did not supply */
    long           culled;                /* test_box_visible found the box
                                           * wholly outside one plane */
    long           subdivided;            /* near walls drawn as four quads */
    long           flat_faces;            /* faces down the flat path */
    long           flat_unsupported;      /* ...reaching a routine or fill
                                           * this module does not carry */
    long           shaded_faces;          /* through rast_draw_shaded */
    /* The last face handed to a shader, which is what the original keeps
     * -- so the two can be compared face for face rather than
     * pixel for pixel, which is the only way to tell a geometry error from
     * a coverage one. */
    uw_rast_svert  last_face[UW_CLIP_MAX];
    int            last_face_n;
    /* Every face, for a harness that scores them ONE AT A TIME. A whole
     * frame compared as a whole cannot tell a wrong face from a face the
     * original drew over; a face compared alone can. */
    uw_rast_svert  face[UW_DL_FACES][UW_DL_FACEV];
    uint8_t        face_n[UW_DL_FACES];
    uint16_t       face_shader[UW_DL_FACES];
    uw_rast_texrec face_rec[UW_DL_FACES];
    uint8_t        face_lit[UW_DL_FACES]; /* flag[4]'s low byte at the time */
    uint16_t       face_at[UW_DL_FACES];  /* the emitting opcode's offset */
    uint8_t        face_colour[UW_DL_FACES]; /* a flat face's colour, a shaded
                                              * one's base (0x6146, 0x6156) */
    uint8_t        face_mode[UW_DL_FACES];   /* a shaded face's span pair */
    const uint8_t *face_texels[UW_DL_FACES]; /* what a textured face drew with */
    size_t         face_texlen[UW_DL_FACES];
    /* Every sprite placed, in order, and the order faces and sprites were
     * drawn in -- a sprite draws over what is already there, so a replay
     * that draws all faces first is a different picture. */
    struct {
        uint16_t art_id;
        uint8_t  level;
        uint16_t at;
        uw_sprite_desc desc;
        uint8_t  creature;                /* 1: a CRIT frame, below */
        uint8_t  blitter, fill;           /* the row blitter and fixed colour then */
        uint8_t  crit_page, crit_file;
        uw_creature_frame frame;
    }              sprite[UW_DL_SPRITES];
    long           creatures;             /* creatures the drawer ran for */
    int            n_sprites;
    long           sprites;               /* sprites the drawer ran for */
    long           sprites_culled;        /* placed and rejected */
    uint16_t       event[UW_DL_EVENTS];   /* face k, or 0x8000 | sprite k */
    int            n_events;
    uint16_t       op_at;                 /* the opcode being executed */
    int            n_faces;
    /* Per opcode, how often it was EXECUTED and how often it was only
     * STEPPED OVER by its fixed width -- the second is effect the port
     * does not have, and a frame comparison has to be read with it. */
    uint32_t       op_run[UW_DL_OPS];
    uint32_t       op_stepped[UW_DL_OPS];
    int16_t        last_origin[3];        /* and the origin it was under */
    int            last_shift;
    uint16_t       last_unknown;          /* the opcode that stopped it */
    uint16_t       stopped_at;            /* and where */
    /* An optional (offset, opcode) trace, which is how a desynced parse is
     * found: parse the same list independently, and the first offset the
     * two disagree on is the handler whose operand count is wrong. */
    uint16_t      *trace;
    long           n_trace, max_trace;
    /* Stop after this many opcodes (0: no limit).
     *
     * IT IS NOT A BISECT AGAINST A SNAPSHOT OF THE ORIGINAL, and trying to
     * use it as one is a trap worth naming. A snapshot's slot array is the
     * state AFTER the whole frame, and `rast_execute` runs the DEFERRED QUEUE after the
     * list -- more draw lists, with their own origins, rewriting the same
     * 256 slots. So the array only ever agrees at the end of the whole
     * frame, and "the first opcode count at which a slot disagrees" is 6,
     * every time, for every scene. */
    long           stop_after;
} uw_dl;

/* Runs the list at `list[pc..]`, `len` bytes long, until `ret` at the
 * outermost level. Returns the number of opcodes executed, or -1 on a
 * malformed list (a target outside the buffer, or nesting past UW_DL_DEPTH).
 *
 * `list` is the raw bytes of the rasteriser's data segment holding the
 * program and `pc` the offset the original starts it at, so relative call
 * targets resolve exactly as the original's do. */
long uw_dl_run(uw_dl *m, const uint8_t *list, size_t len, uint16_t pc);

/* Draws recorded face `k` into `fb` exactly as emit_face did -- the texture
 * mapper the list chose, then the lighting pass if the face was emitted with
 * it and `lit` is non-zero. For a harness that scores faces one at a time. */
void uw_dl_render_face(uw_dl *m, const uw_fb *fb, int k, int lit);

/* Draws event `e` -- a face as uw_dl_render_face does, or a sprite through
 * its art, light and placement exactly as the run drew it. */
void uw_dl_render_event(uw_dl *m, const uw_fb *fb, int e, int lit);

/* The dispatch's own arithmetic, exposed because it is the one thing about
 * the instruction set that is not a table lookup: an opcode word is a BYTE
 * OFFSET into the handler table, so only even values are opcodes. */
#define UW_DL_SLOT(op) ((op) >> 1)

#endif
