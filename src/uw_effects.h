/* SPDX-License-Identifier: MIT */
/* The level's animated objects: level_effect_list and the
 * routines that keep it, level_effects_tick, level_effect_animate
 * and level_effect_expire.
 *
 * An EFFECT is a record naming an object -- the fountains, fires and
 * shimmering things of item class 7, 0x1c0..0x1cf -- whose frame the game
 * advances: six bytes, the object's index in word 0's top ten bits, a
 * countdown (-1 is permanent) and the tile it stands in. A level's list is
 * LEV.ARK slot 9 + level, 0x180 bytes, a live prefix ended by an index of 0
 * (level_effects_load).
 *
 * THE FRAME IS THE OBJECT'S QUALITY. Byte +6's low six bits, which the draw
 * list's producer adds to 0x1c0 to pick the sprite. What a kind does with it
 * is OBJECTS.DAT's last section, animation_props: sixteen four-byte records
 * of a flag word, a first frame and a frame count. Flag bit 0 steps the
 * frame and wraps to the first; bit 1 picks one at random; bit 2 turns the
 * object and, for a moving door, moves it; bit 7 gives an expiring effect
 * one more animate pass; bit 5 frees the object when its effect expires.
 * Every shipped animation is 0x21, the moving door (0x1cf) 0x84.
 *
 * THE TICK is four a second: dungeon_frame_tick hands
 * level_effects_tick the number of 64-tick boundaries of the 256 Hz clock
 * crossed since the last frame, and a step key hands it one
 * (key_step_or_turn), both only while the effects-on-move flag is set.
 *
 * What this does not carry, counted in `unsupported`: a door swinging
 * shut when no `door_move` is attached (door_closing_check, which asks
 * item_fits_in_tile whether the closed door fits) and the moving door's
 * expiry when no `door_seat` is attached (item_fits_in_tile, the door's own
 * motion and a sound) -- both the game's side's, src/uw_motion_trap.c's
 * level_effect_door_move and _seat -- and freeing an expired effect's
 * object when no pool is attached. */
#ifndef UW_EFFECTS_H
#define UW_EFFECTS_H

#include "uw.h"
#include "uw_objpool.h"

#define UW_EFFECTS_MAX   64
#define UW_EFFECTS_BLOCK 0x180

typedef struct {
    uint16_t word0;              /* object index << 6, and six unread bits */
    int16_t  timer;              /* level_effect_timers: -1 is permanent */
    uint8_t  tile_x, tile_y;     /* level_effect_tiles */
} uw_effect;

typedef struct uw_effects_s {
    uw_effect rec[UW_EFFECTS_MAX];
    int       count;             /* level_effect_count */
    uint8_t   props[64];         /* animation_props */
    int       moved;             /* level_effect_moved */
    uw_objpool *pool;            /* the level's pools, for freeing; may be NULL */
    int       drawn_flag;        /* which level_effect_add sets */
    long      unsupported;
    /* level_effect_expire's kind 0xf, the moving door at the end of its
     * swing: 1 when it was seated as a door and the record may go, 0 when
     * it was turned back and the record stays. NULL: counted. */
    int     (*door_seat)(void *user, struct uw_effects_s *e, int index);
    /* level_effect_animate's turn with the direction bit set (a door
     * swinging shut): door_closing_check(index, step), which turns the swing
     * back, through the record, when the door no longer fits. NULL: counted.
     * `in_expire` is set while level_effect_expire's last pass animates. */
    void    (*door_move)(void *user, struct uw_effects_s *e, int index, int step);
    int       in_expire;
    void     *door_user;
} uw_effects;

/* level_effects_load: the records from a 0x180-byte block, counted to the
 * first index of 0, with `props` the 64 bytes of animation_props (OBJECTS.DAT's
 * last section; NULL leaves them zero). Returns false, with no effects, for
 * a block of any other size. Clears `pool`: attach one after. */
bool uw_effects_load(uw_effects *e, const uint8_t *block, size_t len,
                     const uint8_t *props);

/* The same records read out of a copy of the original's data segment: the
 * count at 0x3656, the list at 0x369c and animation_props at 0x3658. */
void uw_effects_from_ds(uw_effects *e, const uint8_t *ds);

/* level_effects_tick(ticks) over `level`, the level segment as the draw list
 * addresses it (the tile map at +4, mobile objects from +0x4004, static
 * from +0x5b04). `rng` serves bit 1 and may be NULL when no kind has it. */
void uw_effects_tick(uw_effects *e, uint8_t *level, int ticks, uw_rng *rng);

/* level_effect_animate(index, delta) on its own. */
void uw_effect_animate(uw_effects *e, uint8_t *level, int index, int delta,
                       uw_rng *rng);

/* The object an effect names, as an offset into `level`, or 0. */
uint16_t uw_effect_object(const uw_effects *e, int index);

/* level_effect_add, read from the instructions: a record for
 * object `index` with countdown `timer` in tile (x, y), refused past 64
 * (returns -1); the object starts on frame `first + seed % count` of its
 * animation_props record (`first` for a count of 0, untouched for a negative
 * first). Sets `drawn_flag` and returns the new count. */
int uw_effect_add(uw_effects *e, uint8_t *level, uint16_t index, int16_t timer,
                  uint8_t seed, uint8_t x, uint8_t y);

/* spawn_animo_copies, from the instructions: rand() % 3 + 3
 * static copies of the object at `src`, each an item id one or two above it,
 * its fine x and y moved by rand() % 5 - 2 until in 0..7 and its z by
 * (rand() & 15) - 8, put at the head of tile (x, y)'s chain and added as an
 * effect with countdown `2 - a + b` and seed `a`, for a and b two draws of
 * rand() % 3. A refused effect unlinks and frees its copy and ends the loop.
 * Needs `pool`. Returns the last level_effect_add result. */
int uw_spawn_animo_copies(uw_effects *e, uint16_t src, uint8_t x, uint8_t y,
                          uw_rng *rng);

/* spawn_class7_object, from the instructions: object_create of
 * item 0x1c0 + kind; the source's fine position when there is a source; z
 * -height for a negative height, else the source's z plus the low byte of
 * height times the source's COMOBJ.DAT height (byte 0 >> 3, at least 1),
 * else untouched; then the effect, and only then the object APPENDED to
 * tile (x, y) -- or freed when the list is full. Returns whether it was
 * made. `src` may be 0. */
bool uw_spawn_class7_object(uw_effects *e, const uint8_t *comobj, uint16_t src,
                            int kind, int16_t timer, uint8_t seed,
                            int16_t height, uint8_t x, uint8_t y);

#endif
