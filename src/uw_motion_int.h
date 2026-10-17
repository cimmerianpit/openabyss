/* SPDX-License-Identifier: MIT */
/* The internal header of src/uw_motion*.c: what the files share.
 *
 * The files are cut by subject. A function another
 * file calls is declared here and is not static, and the enums of
 * data-segment offsets more than one file reads are here too, under the
 * file they came from. Nothing here is for callers outside the port:
 * they include uw_motion.h. */
#ifndef UW_MOTION_INT_H
#define UW_MOTION_INT_H

#include "uw_motion.h"
#include "uw_objpool.h"
#include "uw_trig.h"
#include "uw_effects.h"
#include "uw_viewlist.h"
#include "uw_scroll.h"

#include <stdlib.h>
#include <string.h>

/* ---- the data segment ------------------------------------------------ */

enum {
    CLOCK_STAMP          = 0x0774,   /* last_move_key_clock, 32 bits */
    TURN_PHASE           = 0x0778,   /* mobile_update_advance */
    EFFECTS_ON_MOVE      = 0x0aaf,
    TURN_HALVED          = 0x0283,
    PLAYER_RECORD_PTR    = 0x7270,
    CRITTER_ROW_PTR      = 0x7272,
    STEPPED              = 0x3578,   /* player_stepped_this_frame */
    SWAY_VERTICAL        = 0x3580,
    SWAY_PHASE           = 0x0729,
    MOVEMENT_MODE        = 0x075a,
    MOVEMENT_SPEED       = 0x2794,
    VERTICAL_VELOCITY    = 0x278a,
    VERTICAL_GRAVITY     = 0x2790,
    MOVEMENT_INPUT_A     = 0x278c,
    MOVEMENT_INPUT_B     = 0x278e,
    PLAYER_IN_LIQUID     = 0x00d3,
    MOBILES_ENABLED      = 0x1b36,
    TIME_FROZEN          = 0x0282,
    NOISE_BASE           = 0x1afe,
    NOISE_LEVEL          = 0x1aff,
    NOISE_CYCLE          = 0x0783,
    SPEED_MAX_FORWARD    = 0x2496,
    SPEED_MAX_STRAFE     = 0x2498,
    SPEED_MAX_BACKWARD   = 0x249a,
    TURN_RATE_EFFECTIVE  = 0x2492,
    SPEED_BASE_BACKWARD  = 0x00ca,
    SPEED_BASE_STRAFE    = 0x00cc,
    SPEED_BASE_FORWARD   = 0x00ce,
    SPEED_MAX_ACCEL      = 0x00c8,
    HEADING_OFFSET       = 0x00d0,   /* movement_heading_offset */
    BLOCK_FLAGS          = 0x00d2,   /* motion_spell_bits: 1 Leap, 2 Slow Fall, 4 Levitate, 8 Water Walk, 0x10 Fly */
    SPEED_SCALE_TABLE    = 0x00d4,   /* movement_speed_scale, 7 bytes */
    MODE_FLAGS_TABLE     = 0x00db,   /* movement_mode_flags, 7 bytes */
    TURN_RATE            = 0x075c,
    FORWARD_INPUT        = 0x0763,
    TURN_INPUT           = 0x0765,
    MOVEMENT_STATE       = 0x24a0,   /* player_movement_state */
    TRACKED_TILE         = 0x2494,
    TILEMAP_PTR          = 0x19b4,
    PENDING_EVENTS       = 0x56aa,
    TRACKED_OBJECT       = 0x7274,   /* far: offset, segment */
    PLAYER_HEADING       = 0x727a,
    MOVE_HEADING         = 0x727c,
    PLAYER_X             = 0x2780,
    PLAYER_Y             = 0x2782,
    PLAYER_Z             = 0x2784,
    SPEED_DT             = 0x2792,
    FALL_HARDNESS        = 0x2796,
    GROUNDED             = 0x2797,
    HEADING_TARGET       = 0x279e,
    PLAYER_RADIUS        = 0x27a2,
    PLAYER_HEIGHT        = 0x27a3,
    COLLISION_FACE       = 0x27a5,
    FALL_IMPACT          = 0x27a6,
    RUN_FLAGS            = 0x284e,
    JITTER_PITCH         = 0x3586,
    JITTER_YAW           = 0x3584,
    SWAY_HORIZONTAL      = 0x3582,
    OBJ_PROPERTIES       = 0x5b6e    /* COMOBJ.DAT's 11-byte records */
};

static inline uint16_t rw(const uint8_t *m, uint16_t at) {
    return (uint16_t)(m[at] | (m[(uint16_t)(at + 1)] << 8));
}
static inline int16_t rs(const uint8_t *m, uint16_t at) { return (int16_t)rw(m, at); }
static inline void ww(uint8_t *m, uint16_t at, uint16_t w) {
    m[at] = (uint8_t)w;
    m[(uint16_t)(at + 1)] = (uint8_t)(w >> 8);
}

/* Globals the integrator keeps, as 6aac offsets. */
enum {
    MR_CTX        = 0x284a,   /* motion_ctx: the mover's 0x28-byte block */
    MR_FILTER     = 0x285a,   /* motion_filter: the collision descriptor */
    MR_VELOCITY   = 0x040c,   /* motion_velocity: ctx + 6 */
    MR_QUERY_PTR  = 0x040e,   /* motion_query_pos: 0x2768, never written */
    MR_FRAC_X     = 0x0410, MR_FRAC_Y = 0x0412, MR_FRAC_Z = 0x0414,
    MR_AXIS_STEP  = 0x0416,   /* two words, by axis */
    MR_Z_DIR      = 0x041a,
    MR_MAJOR      = 0x041c, MR_MINOR = 0x041e,
    MR_STEP_LIMIT = 0x0420, MR_STEP_REM = 0x0422,
    MR_TICKS_TILE = 0x0424, MR_COUNTER = 0x0426,
    MR_HIT_SLOT   = 0x0428, MR_HIT_ITEM = 0x0429,
    MR_Z_BOUND    = 0x042b, MR_SUPPORT = 0x042f, MR_Z_PER_TILE = 0x0431,
    MR_DIRS       = 0x0433,   /* eight headings by compass code */
    MR_TERRAIN_BOUND = 0x284c, MR_Z_SETTLED = 0x2848, MR_FLAGS_SAVED = 0x284d,
    SQ_PTR        = 0x2708,   /* spatial_query_ptr */
    Q             = 0x2768,   /* the motion query struct */
    RESULTS       = 0x26c6,   /* nine six-byte records: top, z, link */
    MOBILE_BASE   = 0x272e, STATIC_BASE = 0x275a,
    RAND_SEED     = 0x207e
};

static inline uint16_t ctx(uw_motion *m) { return rw(m->ds, MR_CTX); }
static inline int16_t cws(uw_motion *m, int off) { return rs(m->ds, (uint16_t)(ctx(m) + off)); }
static inline void cww(uw_motion *m, int off, uint16_t v) { ww(m->ds, (uint16_t)(ctx(m) + off), v); }
static inline uint8_t cb(uw_motion *m, int off) { return m->ds[(uint16_t)(ctx(m) + off)]; }
static inline uint16_t qp(uw_motion *m) { return rw(m->ds, MR_QUERY_PTR); }
static inline int16_t qws(uw_motion *m, int off) { return rs(m->ds, (uint16_t)(qp(m) + off)); }
static inline void qww(uw_motion *m, int off, uint16_t v) { ww(m->ds, (uint16_t)(qp(m) + off), v); }
static inline int16_t vel(uw_motion *m, int i) { return rs(m->ds, (uint16_t)(rw(m->ds, MR_VELOCITY) + i * 2)); }
static inline void vset(uw_motion *m, int i, uint16_t v) { ww(m->ds, (uint16_t)(rw(m->ds, MR_VELOCITY) + i * 2), v); }
static inline int16_t gw(uw_motion *m, uint16_t at) { return rs(m->ds, at); }
static inline void gset(uw_motion *m, uint16_t at, uint16_t v) { ww(m->ds, at, v); }
static inline uint8_t rec_top(uw_motion *m, int i) { return m->ds[(uint16_t)(RESULTS + i * 6)]; }
static inline uint8_t rec_z(uw_motion *m, int i) { return m->ds[(uint16_t)(RESULTS + i * 6 + 1)]; }
static inline uint16_t rec_link(uw_motion *m, int i) { return rw(m->ds, (uint16_t)(RESULTS + i * 6 + 2)); }

/* obj_ptr_from_index, an offset into the level segment. */
static inline uint16_t obj_at(uw_motion *m, uint16_t index) {
    if (index == 0) return 0;
    if (index < 0x100) return (uint16_t)(rw(m->ds, MOBILE_BASE) + index * 0x1b);
    return (uint16_t)(rw(m->ds, STATIC_BASE) + index * 8 - 0x800);
}
static inline uint16_t obj_id(uw_motion *m, uint16_t o) { return (uint16_t)(rw(m->lseg, o) & 0x1ff); }
static inline uint8_t prop(uw_motion *m, uint16_t id, int k) {
    return m->ds[(uint16_t)(OBJ_PROPERTIES + id * 11 + k)];
}

/* rt_rand over the state the data segment keeps. */
static inline int rt_rand(uw_motion *m) {
    uint32_t st = (uint32_t)rw(m->ds, RAND_SEED) | ((uint32_t)rw(m->ds, RAND_SEED + 2) << 16);
    st = st * 0x015a4e35u + 1u;
    ww(m->ds, RAND_SEED, (uint16_t)st);
    ww(m->ds, RAND_SEED + 2, (uint16_t)(st >> 16));
    return (int)((st >> 16) & 0x7fff);
}

/* check_skill_roll: (skill - difficulty) + rand() % 31, bucketed
 * -1 below 3, 0 below 16, 1 below 29, else 2. */
static inline int check_skill_roll(uw_motion *m, int skill, int difficulty) {
    int16_t si = (int16_t)(uint16_t)(skill - difficulty);
    si = (int16_t)(uint16_t)(si + rt_rand(m) % 0x1f);
    if (si > 0x1c) return 2;
    if (si > 0xf) return 1;
    if (si > 2) return 0;
    return -1;
}

/* ---- from uw_motion.c --------------------------------------------- */

/* play_sound_effect and play_sound_effect_at_xy both
 * return 0xff before anything else unless sfx_available and sfx_enabled are
 * set; past that gate, the mixer (uw_motion_sound.c). */
enum { SFX_ENABLED = 0x0136, SFX_AVAILABLE = 0x0137 };

enum {
    KEY_SHIFT_PTR = 0x2334, KEY_CAPS_PTR = 0x2338, KEY_ALT_PTR = 0x233c,
    KEY_CTRL_PTR = 0x2340, KEY_STATE_PTR = 0x2344,  /* far pointers */
    MOVEMENT_KEY_SCANCODES = 0x076b                 /* nine */
};

/* ---- from uw_motion_query.c --------------------------------------- */

enum {
    SQ_TILE_WORDS = 0x26f6,   /* nine words, 0x1111 until computed */
    SQ_CORNERS    = 0x270a,   /* five 5-byte records: idx, fx, fy, flags; [4] the centre */
    SQ_TILE_PTR   = 0x2724,   /* far: the centre tile */
    SQ_VALID      = 0x272a,   /* spatial_query_corners_valid */
    SQ_BOX        = 0x26c0,   /* x0 x1 fy y0 y1 (bytes) -- fy at 0x26c2 */
    SQ_FX         = 0x272b,
    NEIGHBOUR_OFF = 0x02ba,   /* nine signed tile offsets, the 3x3 */
    DIAG_MASKS    = 0x02c3,
    CORNER_UNITS  = 0x02c8,
    DIR_TABLE     = 0x02d0,   /* nine compass codes, centred */
    TERRAIN_KINDS = 0x7192,
    TILE_TYPE_FLAGS = 0x1d8a
};

/* ---- from uw_motion_run.c ----------------------------------------- */

enum {
    MOBILE_PHASE      = 0x248d,   /* mobile_update_phase */
    MOBILE_PHASE_PREV = 0x00c4,   /* mobile_update_phase_prev */
    CURRENT_NPC       = 0x245a,   /* far */
    ACTIVE_LIST       = 0x273c,   /* far: active_mobile_list */
    ACTIVE_END        = 0x2732,   /* active_mobile_end */
    VIEW_TILE_X       = 0x248e, VIEW_TILE_Y = 0x2490,
    AI_SELF_INDEX     = 0x244e,
    AI_SELF_CRITTER   = 0x2458,   /* the critter_properties row's DS offset */
    AI_SELF_TILE_X    = 0x245f, AI_SELF_TILE_Y = 0x2460,
    AI_SELF_Z         = 0x2461,
    AI_SELF_FINE_X    = 0x246e, AI_SELF_FINE_Y = 0x2470,
    AI_SELF_HOME_X    = 0x246a, AI_SELF_HOME_Y = 0x246b,
    AI_SELF_HEADING   = 0x2480, AI_SELF_HEADING32 = 0x247c,
    AI_SELF_SPEED     = 0x2477, AI_SELF_HEIGHT = 0x246c,
    AI_MOTION_BLOCK   = 0x2474,   /* ai_motion_block_ptr */
    AI_FILTER_DESC    = 0x2446,   /* ai_motion_filter_desc */
    AI_MOTION_BLOCKED = 0x2452, AI_MAY_MOVE = 0x2440, AI_FLIER_STEP = 0x2462,
    AI_HEADING_CHANGED = 0x2449, AI_BLOCKED_ANY = 0x246d,
    AI_WALKER_BLOCKED = 0x2473, AI_GOTO_ACTIVE = 0x244a,
    AI_TERRAIN_FLAGS  = 0x2438,
    AI_MOTION_BLOCKER = 0x2442,   /* far */
    AI_DOOR_TILE_X    = 0x243a, AI_DOOR_TILE_Y = 0x243b,
    NPC_PATH_SLOT_MASK = 0x00b4,
    MOTION_TILE_X     = 0x247a, MOTION_TILE_Y = 0x247e,
    AI_FACED_PATH_STEP = 0x2472,
    AI_TARGET_PTR     = 0x2466,   /* far */
    AI_TARGET_TILE_X  = 0x2448, AI_TARGET_TILE_Y = 0x2453, AI_TARGET_Z = 0x2478,
    AI_TARGET_FINE_X  = 0x244c, AI_TARGET_FINE_Y = 0x2464,
    AI_TARGET_DX      = 0x243c, AI_TARGET_DY = 0x243e,
    AI_TARGET_TILE_DIST2 = 0x2450, AI_TARGET_FINE_DIST2 = 0x2454,
    ASSAULT_VICTIM_INDEX = 0x00c5, ASSAULT_VICTIM_RACE = 0x00c6,
    ASSAULT_TIME      = 0x2486,   /* 32 bits */
    ASSAULT_TILE_X    = 0x248a, ASSAULT_TILE_Y = 0x248b, ASSAULT_Z = 0x248c,
    COMBAT_MUSIC_TIME = 0x2482,   /* 32 bits */
    PATH_LEN          = 0x245e,   /* the line walk's step count */
    PATH_COST_LIMIT   = 0x2476,
    PATH_CLIMB        = 0x244f,   /* set when a step was allowed as a climb */
    TILE_BLOCK_TABLE  = 0x1d8a,   /* by tile type: 2 +x, 4 -x, 8 +y, 0x10 -y */
    SLOPE_INDEX       = 0x00ba,   /* nine signed tile offsets (NEIGHBOUR_OFF) */
    SLOPE_TYPE        = 0x00bf,
    MUSIC_TRACK_PLAYING = 0x261e, MUSIC_TRACK_WANTED = 0x261f,
    CURRENT_LEVEL_WORD = 0x7278,
    MOTION_STATE_TABLE = 0x03fa,  /* flags +0x25 -> the object's +0x0a bits 4..6 */
    /* The three creature motion blocks and their filter descriptors. */
    BLOCK_WALKER = 0x27a8, BLOCK_FLIER = 0x27d0, BLOCK_SWIMMER = 0x2820,
    FILTER_WALKER = 0x285c, FILTER_FLIER = 0x2868, FILTER_SWIMMER = 0x2880,
    /* dungeon_frame_tick's BP: where the saved registers of a state taken
     * inside it sit (SP = BP - 6), and the stack address a creature's
     * object_terrain_test leaves in spatial_query_ptr (BP - 0x56). The stack
     * is in the data segment. */
    FRAME_BP = 0x958c
};

/* ---- from uw_motion_object.c -------------------------------------- */

enum {
    PLACE_SCATTER   = 0x02d5,   /* object_place_scatter: try 0 is the exact point only when set */
    PLACE_FLOOR     = 0x2728,   /* item_fits_in_tile's floor under the item */
    PLACE_SUPPORT   = 0x272c,
    PLACE_STANDABLE = 0x2765,   /* placed_object_collision: landed on a thing one may stand on */
    PLACE_ON_OPEN   = 0x2766    /* ... on one whose link's bit 4 is set */
};

/* ---- from uw_motion_path.c ---------------------------------------- */

enum { PATH_POOL = 0x100, PATH_RECORD = 0x1c, PATH_NEIGHBOURS = 0x00ac };

/* ---- from uw_motion_combat.c -------------------------------------- */

enum {
    COMBAT_ATTACK_RATING = 0x2654, COMBAT_REACH = 0x2656, COMBAT_SWING_TYPE = 0x2658,
    COMBAT_TARGET_TILE_X = 0x2664, COMBAT_TARGET_TILE_Y = 0x2666,
    COMBAT_TARGET_OBJ = 0x2668, REGION_ARMOUR_BONUS = 0x266a,
    COMBAT_STRIKE_Z = 0x266e, COMBAT_DAMAGE = 0x2670, COMBAT_HIT_REGION = 0x2672,
    COMBAT_FACING = 0x2675, COMBAT_CRITICAL = 0x2676, COMBAT_ATTACKER_OBJ = 0x2678,
    COMBAT_DAMAGE_SCALE = 0x2680,
    INVENTORY_SLOTS = 0x5a92,     /* link words, one a slot */
    STATUS_TIMER_20 = 0x0727, STATUS_TIMER_40 = 0x0728,
    VIEW_CAMERA_OBJECT = 0x2e10,  /* far */
    PANELS_MODE = 0x0784,
    CRITTER_BASE = 0x4a52         /* critter_properties, 0x30 a row */
};

enum {
    COMBAT_SWING_CLOCK   = 0x265a,   /* 32 bits: the clock at the last charge step */
    COMBAT_CHARGE_TICKS  = 0x265e,   /* ticks not yet turned into charge; -1 before the first */
    COMBAT_SWING_ATTACK  = 0x2660,   /* the view cell clicked, 1..9; -1 for a missile weapon */
    COMBAT_CHARGE        = 0x2662,   /* percent, a byte */
    COMBAT_SWING_RANGED  = 0x2674,   /* combat_select_weapon returned 0: a missile weapon */
    COMBAT_WEAPON_OBJ    = 0x267a,   /* far: the wielded object, or 0 */
    COMBAT_WEAPON_ROW    = 0x267e,   /* near: its row -- missile_props, melee_weapon_props, the fist's */
    SWING_KIND_BY_CELL   = 0x0235,   /* combat_swing_kind[cell], cells 1..9 */
    SWING_BUTTON_BY_ROW  = 0x023e,   /* the key watched for release, [cell / 3] */
    MELEE_WEAPON_PROPS   = 0x5972,   /* eight bytes an id 0x00..0x0f */
    MISSILE_PROPS_ROW    = 0x5942,   /* missile_props: three bytes an id 0x10..0x1f, +2 the ammunition */
    FIST_ROW             = 0x59ea,
    OBJ_PROPERTIES_1     = 0x5b6f,   /* obj_properties + 1, eleven bytes an id */
    ACTION_STATE_WORD    = 0x26ac
};

/* ---- from uw_motion_spell.c --------------------------------------- */

enum {
    PROJ_SPEED = 0x26ae, PROJ_TARGET_X = 0x26b0, PROJ_TARGET_Y = 0x26b2,
    PROJ_ITEM = 0x26b4,
    PROJ_FIRER = 0x26b6,          /* far */
    PROJ_HEADING = 0x26ba,        /* projectile_from_firer_tile, then the heading */
    PROJ_AIM_X = 0x26bc, PROJ_AIM_Z = 0x26be,
    MISSILE_PROPS = 0x5943,       /* three bytes by missile kind, +0 the speed */
    CAST_MISSILES = 0x09ab,       /* the missile kind of a class-5 effect, by its argument */
    EFFECT_TARGET_X = 0x3636, EFFECT_TARGET_Y = 0x3637
};

enum { AREA_MATCH_FLAG = 0x1d9a, AREA_MATCH_EXCEPT = 0x7372 };

/* ---- from uw_motion_panel.c --------------------------------------- */

enum {
    PANEL_DIRTY        = 0x0785,   /* the redraw loop's mask, every 64 ticks */
    PANEL_DIRTY_1      = 0x0787,   /* every 32 ticks */
    PANEL_DIRTY_2      = 0x0789,   /* at once, and cleared as drawn */
    PANEL_LAST_CLOCK   = 0x359a,   /* the clock's low byte at the last redraw */
    PANEL_FLASK_SHOWN  = 0x35e2,   /* per element: the level drawn, or a dragon's state (4, 5) */
    PANEL_PREV_VALUES  = 0x35e4,   /* [0] the compass drawn */
    PANEL_VALUES       = 0x3628,   /* per element: what panel_set_value wants shown */
    DRAGON_PHASE       = 0x078b, DRAGON_ELEM = 0x078f, DRAGON_FLICK_PENDING = 0x07da,
    DRAGON_FRAME       = 0x0918, DRAGON_FLICK_FRAME = 0x091c, DRAGON_REPEAT = 0x359b,
    DRAGON_FIRST       = 0x091e, DRAGON_LAST = 0x092a, DRAGON_STILL_ELEM = 0x08db,
    FLASK_BUBBLE       = 0x07d8, FLASK_ART = 0x0908, FLASK_BODY_ELEM = 0x0910,
    ELEMENT3_ELEM      = 0x0936, ELEMENT3_LAST = 0x0938, ELEMENT3_FRAME = 0x093a,
    ELEMENT7_ELEM      = 0x08b3, ELEMENT7_STEP = 0x08b5, ELEMENT7_HOLD = 0x093c,
    ELEMENT7_SHOWN     = 0x35e9,
    VIEW_SWITCHING     = 0x08a3, PANEL_MODE_SHOWN = 0x35e8, PANEL_MODE_WANTED = 0x362e,
    PANEL_FLIP_FRAME_N = 0x08a2,   /* panel_flip_frame, 1..8 while the card turns */
    PANEL_FLIP_TARGET_MODE = 0x35a8,
    WEAPON_STATE       = 0x35ea, WEAPON_WANTED_STATE = 0x3630, WEAPON_FRAME = 0x0796,
    WEAPON_STRUCK      = 0x093d, WEAPON_ART_WANTED = 0x0793, WEAPON_ART_LOADED = 0x0794,
    WEAPON_RESUME      = 0x0795
};

/* ---- from uw_motion_tick.c ---------------------------------------- */

enum {
    SWING_STATE        = 0x0242,   /* combat_swing_state: 0 idle, negative the strike's frames */
    SWING_BUTTON       = 0x0244,   /* combat_swing_button: the key whose release ends the charge */
    WEAPON_ANIM_FRAME  = 0x0796,
    MOUSE_PENDING      = 0x0115,   /* mouse_pending_event */
    MOUSE_HELD         = 0x011e,   /* mouse_button_held */
    PLAYER_MAX_HP      = 0x5626,   /* critter_properties[63].max_hp */
    PANEL_PREV         = 0x35e2,   /* panel_prev_values: the flasks' shown levels first */
    WEAPON_ANIM_WANTED = 0x0793, WEAPON_ANIM_LOADED = 0x0794,
    PALETTE_PHASE      = 0x0234,   /* palette_cycle_phase: the clock's high three bits last seen */
    PALETTE_PHASE_HALF = 0x0235,   /* combat_swing_kind's byte 0, which no view cell indexes */
    PALETTE_PTR        = 0x23bc,   /* far */
    PALETTE_SAVED      = 0x2650,   /* palette_rotate_saved_r/g/b */
    LAST_TICK_TIME     = 0x0289,   /* 32 bits */
    TICK_ACCUMULATOR   = 0x028d,
    OBJCHECK_PENDING   = 0x12b3, OBJCHECK_REPORTED = 0x02d6,
    HOSTILE_NEARBY     = 0x1230,   /* hostile_creature_nearby's answer */
    HUNT_SLEEPER_CAME  = 0x5654,   /* npc_hunt_sleeper's */
    SHELF_SPELL_CAST   = 0x5a90,   /* a spell went off the shelf; the next rune starts a new one */
    MUSIC_ENABLED      = 0x0135, MUSIC_AVAILABLE = 0x0138
};

enum {
    REGEN_FLAGS        = 0x03f8,   /* bit 0 vitality, bit 1 mana, a point a step */
    GAME_STEP_COUNTER  = 0x03f9,
    PANEL_MODE         = 0x0784,
    LIGHT_SLOT_INDICES = 0x171e,   /* four signed inventory slot numbers */
    LIGHT_BURN_RATES   = 0x5b3a    /* a byte every other, by the light's id nibble */
};

/* ---- from uw_motion_cursor.c -------------------------------------- */

enum {
    CURSOR_X            = 0x010e,
    CURSOR_Y            = 0x0110,
    CURSOR_VISIBLE      = 0x0112,   /* cursor_visible_count */
    CURSOR_ON_SCREEN    = 0x0114,
    CURSOR_CACHE_X1     = 0x0120,   /* the region last matched; -1 none */
    CURSOR_REGIONS      = 0x0122,   /* cursor_region_count */
    CURSOR_DEPTH        = 0x0124,   /* cursor_shape_depth, a signed byte */
    CURSOR_SHAPE_COPY   = 0x24ce,
    CURSOR_REGION_Y1    = 0x24d0,   /* the region table, a word a region in each */
    CURSOR_HOT_X        = 0x24f8,
    CURSOR_HOT_Y        = 0x24fa,
    CURSOR_REGION_X1    = 0x24fc,
    CURSOR_REGION_X2    = 0x2528,
    CURSOR_REGION_Y2    = 0x2554,
    CURSOR_H            = 0x257c,
    CURSOR_SHAPE        = 0x2582,
    CURSOR_W            = 0x2586,
    CURSOR_CACHE_Y1     = 0x258c,
    CURSOR_REGION_SHAPE = 0x258e,
    CURSOR_BOUND_X0     = 0x2524,   /* the clamp box */
    CURSOR_BOUND_Y0     = 0x2526,
    CURSOR_BOUND_X1     = 0x257e,
    CURSOR_BOUND_Y1     = 0x2580,
    /* the keyboard's glide (cursor_key_move, cursor_update_position) */
    GLIDE_TARGET_X      = 0x0125,   /* negative: no glide running */
    GLIDE_LAST_CLOCK    = 0x012a,   /* cursor_last_click_clock, two words */
    GLIDE_STEP_X        = 0x2550,
    GLIDE_STEP_Y        = 0x2552,
    GLIDE_SPEED         = 0x2584,
    GLIDE_TARGET_Y      = 0x2588,
    GLIDE_KEY           = 0x258a,   /* the scan code whose release ends it */
    CURSOR_STACK        = 0x25b6,   /* three words */
    CURSOR_CACHE_X2     = 0x25bc,
    CURSOR_CACHE_Y2     = 0x25be,
    CURSOR_VIEW_X       = 0x25c0,   /* the 3-D view: x, y (its larger edge), w, h */
    VIEW_PITCH          = 0x3588,
    VIEW_VIEWPORT_DIRTY = 0x09cf + 0x2430   /* view_set_viewport's, and what
                                             * view_present and screen_present composite
                                             * the weapon over the view by */
};

/* ---- from uw_motion_inventory.c ----------------------------------- */

enum {
    CONTAINER_STACK_TOP   = 0x1726,   /* far: the open container's block in the far heap */
    INVENTORY_CLICK_ORDER = 0x186c,   /* the panel element a slot's redraw is */
    INVENTORY_SEARCH_ORDER = 0x1888,  /* the slot a panel element shows */
    FONT_LOADED           = 0x15bc,
    INVENTORY_PANEL_BG    = 0x5ad6,   /* an image buffer handle an element */
    INVENTORY_PANEL_INIT  = 0x18a3,   /* inventory_panel_init's once-only guard */
    INVENTORY_ELEM_PLACE  = 0x1732,   /* x, y words and two bytes, 14 bytes an element */
    WEIGHT_LEFT_DRAWN     = 0x189f,   /* inventory_weight_left_drawn */
    CONTAINER_CAN_PAGE_FORWARD = 0x18a1,
    CONTAINER_CAN_PAGE_BACK = 0x18a2,
    PANEL_SWITCH_DIRECTION = 0x18a4,  /* the panel being switched in: drawn on the other page */
    PAPERDOLL_ART_NAME    = 0x18ac,   /* "armor_m", its seventh character the sex */
    PAPERDOLL_ART_ITEM    = 0x5aca,   /* a worn slot's art loaded: the item's low five bits + 1 */
    PAPERDOLL_ART_TIER    = 0x5ad0,   /* and its wear tier + 1 */
    CURSOR_OBJECT         = 0x5b06,   /* far: the object on the cursor */
    PROPS_OBJECT          = 0x5b6a,   /* far: the object obj_props_for_object reads */
    CARRIED_WEIGHT        = 0x72d2,   /* tenths of a stone */
    CARRY_CAPACITY        = 0x72d4
};

enum {
    CRITTER_ARMOUR_ADDS  = 0x266a,   /* four bytes, by body region */
    ARMOUR_REGION_BY_SLOT = 0x1af8,  /* a signed byte a worn slot */
    DRAGON_BOOTS_WORN    = 0x1b01,
    LEVEL7_FLOOR_VARIANT = 0x1b02,
    IMPAIRMENT_EFFECT    = 0x1b03,   /* the distortion chosen, -1 none */
    IMPAIRMENT_FORCED    = 0x1b04,
    SPELL_ICON_BASE      = 0x1b0e,   /* the icon a spell's type starts at */
    SHADE_LEVEL          = 0x1c50,
    ARMOUR_PROPS         = 0x59f2,   /* four bytes an id 0x20..0x3f, +0 the protection */
    SPELL_ELEMS          = 0x0944,   /* panel_active_spell_elems, three handles */
    SPELL_ELEM_X         = 0x08bd,
    FLOOR_TEXTURE_4      = 0x7184,
    VIEW_WARP_ON         = 0x04a4
};
void apply_level7_floor_variant(uw_motion *m, uint8_t on);

/* ---- from uw_motion_action.c -------------------------------------- */

enum {
    VIEW_WIDTH_WORD     = 0x2e08,   /* view_width */
    TILEMAP_ORIGIN      = 0x3120,   /* far: the tile map the view's lists index */
    DRAWLIST_OBJ_X      = 0x2e1c,   /* drawlist_object_x: a picked object's tile */
    DRAWLIST_OBJ_INDEX  = 0x2f9c,   /* drawlist_object_index: its object index */
    DRAWLIST_OBJ_COUNT  = 0x311e,
    VIEW_PICK_SURFACE   = 0x2682,
    PICK_TILE           = 0x2696,   /* far */
    CURSOR_PICK_OBJECT  = 0x269e,   /* far */
    PICK_TILE_CHAIN     = 0x26a8,   /* far */
    CURSOR_PICK_VALID   = 0x028e
};

/* ---- from uw_motion_look.c ---------------------------------------- */

enum {
    ACTION_TARGET_TILE_X = 0x269a, ACTION_TARGET_TILE_Y = 0x269c,
    ACTION_REACH_SCALE   = 0x1b00,
    LOOK_IN_PROGRESS     = 0x0288
};

/* ---- from uw_motion_trap.c ---------------------------------------- */

enum {
    OBJECT_FIND_LIST = 0x2736,  /* far: the link heading the chain object_find_link found in */
    OWNED_INDEX      = 0x736e,  /* object_free_with_owned's owner, for its callback */
    OWNED_COUNT      = 0x7370,  /* ... and how many owned things it still expects */
    TRAP_WHO         = 0x7376,  /* far: what set the trap chain off */
    TRAP_WHAT        = 0x737a,  /* far: what it did it with */
    TRIGGER_PROPS    = 0x737e,  /* the event code each trigger type answers */
    TELEPORT_DEST_X     = 0x5704,  /* the pending teleport the event handler performs */
    TELEPORT_DEST_Y     = 0x5706,
    TELEPORT_DEST_LEVEL = 0x5708
};

/* ---- from uw_motion_spell.c --------------------------------------- */

/* The far addresses by which the original passes these routines as
 * callbacks. */
#define AREA_MATCH_FAR 0x62c00061u
#define HOSTILE_NEARBY_FAR 0x61d50052u
#define HUNT_SLEEPER_FAR   0x61d50039u

/* ---- what one file calls in another ---------------------------------- */

/* uw_motion_sound.c: the sound's state machine over the AIL driver */
int  music_track_finished(uw_motion *m);
int  load_xmi(uw_motion *m, uint8_t track, int start);
void music_restart_current(uw_motion *m);
void music_resume(uw_motion *m);
void music_stop(uw_motion *m);
void music_fade(uw_motion *m, int up);
void music_set_enabled(uw_motion *m, int on);
void sfx_set_enabled(uw_motion *m, int on);
void sound_update(uw_motion *m);
uint8_t play_sound_effect(uw_motion *m, uint8_t id, uint8_t pan, int8_t delta);
uint8_t play_sound_effect_at_xy(uw_motion *m, uint8_t id, int16_t x, int16_t y, int8_t delta);
uint8_t play_sound_effect_at_object(uw_motion *m, uint8_t id, uint16_t obj, int8_t delta);
void sound_effect_stop(uw_motion *m, uint8_t slot);
void sound_effect_stop_all(uw_motion *m);
int  snd_timbre_ready(uw_motion *m, uint8_t bank, uint8_t program);

/* uw_motion.c */
void apply_movement_mode(uw_motion *m, int8_t mode);
void set_movement_state(uw_motion *m, uint16_t flags, int force);
void print_string(uw_motion *m, uint16_t id);
void print_message(uw_motion *m, uint16_t id);
void motion_params_init(uw_motion *m, uint16_t obj, uint16_t blk);
uint16_t object_terrain_test(uw_motion *m, uint16_t obj, uint16_t bp);
uint32_t rd32(const uint8_t *ds, uint16_t at);
void wr32(uint8_t *ds, uint16_t at, uint32_t v);
void movement_set_mode(uw_motion *m, int16_t mode);
void movement_input_update(uw_motion *m);
void level_effects_tick(uw_motion *m, uint8_t ticks, uint16_t bp);
int level_effect_door_seat(uw_motion *m, uw_effects *e, int index, uint16_t bp);
void level_effect_door_move(uw_motion *m, uw_effects *e, int index, int step, uint16_t bp);

/* uw_motion_query.c */
uint16_t tile_ptr(uw_motion *m, uint16_t x, uint16_t y);
uint16_t tile_slope_height(uw_motion *m, int16_t x, int16_t y);
void sq_terrain(uw_motion *m, uint8_t tol);
void sq_corner_walk(uw_motion *m);
void sq_gather(uw_motion *m, int props_filter, int use_filter);
void sq_sort(uw_motion *m);
uint16_t collision_check(uw_motion *m, int slot, uint16_t mover_index, uint16_t bp);
void query_sync(uw_motion *m);
void pick_support(uw_motion *m, int arg);

/* uw_motion_run.c */
uint8_t collision_face(uw_motion *m, uint16_t f);
int8_t motion_substep(uw_motion *m, int16_t di, uint16_t bp);
uint16_t obj_index_of(uw_motion *m, uint16_t o);
/* The object list's own check: true when the level's
 * chains and free lists account for every index exactly once. */
int  objcheck_run(uw_motion *m);
void objcheck_quiet(uw_motion *m);
void motion_run(uw_motion *m, uint16_t block, uint16_t filter, uint16_t bp);

/* uw_motion_object.c */
void pool_from_ds(uw_motion *m, uw_objpool *pool);
void pool_to_ds(uw_motion *m, uw_objpool *pool);
uint16_t object_remove(uw_motion *m, uint16_t link, uint16_t obj, int force);
int object_cull_test(uw_motion *m, int16_t importance, uint16_t obj);
void obj_reclaim_distant(uw_motion *m, int16_t margin, int16_t max);
uint16_t object_hits_floor(uw_motion *m, uint16_t obj);
uint16_t deref_link(uw_motion *m, uint16_t at);
void object_chain_clear(uw_motion *m, uint16_t link);
void object_chain_clear_local(uw_motion *m, uint16_t o);
uint16_t placed_object_collision(uw_motion *m, uint16_t obj, uint16_t tx, uint16_t ty, int toss, uint16_t bp);
int item_fits_in_tile(uw_motion *m, uint16_t id, uint16_t index, int16_t x, int16_t y, int16_t z, int slope, uint8_t radius, uint16_t bp);
int object_place_near(uw_motion *m, uint16_t obj, int16_t fx, int16_t fy, int16_t z, int16_t spread, uint16_t bp);
int object_move_to_coords(uw_motion *m, int16_t fx, int16_t fy, int16_t z, uint16_t obj, int16_t spread, int keep, uint16_t bp);
uint16_t create_object(uw_motion *m, uint16_t id, int mobile);
void spawn_npc_loot(uw_motion *m, uint16_t npc);
void drop_npc_remains(uw_motion *m, uint16_t npc, uint8_t remains, uint8_t fluid, uint16_t bp);
int spill_inventory(uw_motion *m, uint16_t obj, uint16_t owner, uint16_t bp);
int projectile_motion_apply(uw_motion *m, uint16_t obj, uint16_t si, uint16_t bp);
void object_chain_weight(uw_motion *m, uint16_t link, uint16_t *acc);
uint16_t object_weight(uw_motion *m, uint16_t obj);

/* uw_motion_npc.c */
uint16_t npc(uw_motion *m);
uint8_t nb(uw_motion *m, int off);
void nbset(uw_motion *m, int off, uint8_t v);
uint16_t nw(uw_motion *m, int off);
void nwset(uw_motion *m, int off, uint16_t v);
uint8_t crit(uw_motion *m, int off);
void creature_set_goal(uw_motion *m, int goal, int gtarg);
uint8_t vector_to_heading(int8_t dx, int8_t dy);
int tile_no_magic(uw_motion *m, uint16_t x, uint16_t y);
void update_mobile_objects(uw_motion *m, uint8_t substeps);

/* uw_motion_path.c */
int test_between_points(uw_motion *m, int16_t x0, int16_t y0, int16_t z0, int16_t x1, int16_t y1, int16_t z1);
int tile_line_walk(uw_motion *m, uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1);
void npc_pose_from_height(uw_motion *m, uint8_t x, uint8_t y);
int path_step(uw_motion *m, uint16_t rec);
int turn_towards_path(uw_motion *m, uint16_t rec);
int pathfind_between_tiles(uw_motion *m, uint8_t x0, uint8_t y0, uint8_t z0, uint8_t x1, uint8_t y1, uint8_t z1, uint8_t limit);
void path_pack(uw_motion *m, uint16_t rec);
int path_slot_alloc(uw_motion *m, uint8_t *slot);
uint8_t creature_acceptable_danger(uw_motion *m);
void release_path_slot(uw_motion *m);

/* uw_motion_combat.c */
int16_t roll_dice(uw_motion *m, int16_t count, int16_t sides);
uint8_t compute_damage(uw_motion *m, uint16_t obj, uint8_t dmg, uint8_t type);
int use_special_npc(uw_motion *m, uint16_t obj, int removal);
int apply_damage(uw_motion *m, uint16_t obj, uint16_t attacker, int16_t x, int16_t y, uint8_t dmg, uint8_t type);
void view_apply_impairment(uw_motion *m);
void angle_to_offset(uw_motion *m, uint16_t angle, int16_t scale, uint16_t at_x, uint16_t at_y);
void spawn_class7(uw_motion *m, uint16_t src, int kind, int16_t timer, uint8_t seed, int16_t height, uint8_t x, uint8_t y);
int execute_attack(uw_motion *m, uint16_t obj, int16_t swing, uint8_t scale, int16_t attack, uint16_t poison, uint16_t bp_cai);
int  cast_spell_from_object(uw_motion *m, uint8_t x, uint8_t y, uint16_t caster, uint16_t obj, int flag, uint16_t bp);
void magic_charge_update(uw_motion *m, uint16_t obj);
void object_use_dispatch(uw_motion *m, uint16_t a, uint16_t b, int flag);
void motion_knockback(uw_motion *m, uint16_t hit);
uint16_t mouse_sample_buttons(uw_motion *m);
void combat_show_ready_weapon(uw_motion *m);
void input_wait_button_release(uw_motion *m, int keep_running);
int item_enchantment(uw_motion *m, uint16_t o, int16_t *effect, int16_t *magnitude, int *special);
void combat_swing(uw_motion *m, int16_t cell);

/* uw_motion_spell.c */
int is_tracked(uw_motion *m, uint16_t obj);
void motion_state_init(uw_motion *m, uint16_t o, uint16_t tx, uint16_t ty);
uint16_t launch_projectile(uw_motion *m, uint16_t bp);
int level_effect_add(uw_motion *m, uint16_t index, int16_t timer, uint8_t seed, uint8_t x, uint8_t y);
void effect_area_apply(uw_motion *m, uint16_t target, int8_t count, uint32_t callback, uint8_t mode, uint8_t distance, uint8_t radius, uint16_t bp);
void effect_cast_projectile(uw_motion *m, uint16_t obj, int8_t arg, uint16_t bp);
int effect_dispatch_2(uw_motion *m, uint8_t cls, uint8_t arg, uint16_t obj, uint16_t other, uint16_t bp2);
int effect_dispatch(uw_motion *m, uint8_t effect, uint16_t obj, uint16_t other, uint16_t bp);
int spell_cast_from_shelf_wait(uw_motion *m, int16_t from_key);
void spell_cast_from_shelf(uw_motion *m, int16_t from_key);
void spell_cast_from_shelf_rest(uw_motion *m);
void announce_skill_by_index(uw_motion *m, int16_t n);

/* uw_motion_elem.c */
uint16_t elem_alloc(uw_motion *m, uint16_t group, uint16_t w, uint16_t h, int span_variant);
void elem_set_rect(uw_motion *m, uint16_t h, uint16_t x, uint8_t y, uint16_t w, uint8_t hh);
void elem_show(uw_motion *m, uint16_t h, uint16_t art);
void elem_show_shaded(uw_motion *m, uint16_t h, uint16_t art);
void elem_hide(uw_motion *m, uint16_t h);
void elem_move(uw_motion *m, uint16_t h, uint16_t x, uint8_t y);
void elem_set_top_crop(uw_motion *m, uint16_t h, uint8_t rows);
const uint8_t *art_image(uw_motion *m, uint16_t art, int *w, int *h);
uint16_t imgbuf_record(const uw_motion *m, uint16_t handle);
uint16_t imgbuf_alloc(uw_motion *m, uint16_t w, uint16_t h);
uint16_t imgbuf_free(uw_motion *m, uint16_t handle);
void imgbuf_copy(uw_motion *m, uint16_t handle, int x, int y, int w, int h, int restore);
void imgbuf_restore(uw_motion *m, uint16_t handle);
void elem_flush(uw_motion *m);

/* uw_motion_panel.c */
void panel_set_value(uw_motion *m, int8_t e, uint16_t v);
void weapon_stow(uw_motion *m);
void panel_flask_fill(uw_motion *m, int which);
void panel_draw_compass(uw_motion *m);
uint16_t trap_teleport(uw_motion *m, uint16_t who, int16_t x, int16_t y, int16_t level, uint16_t bp);
/* The player's impacts and statuses:
 * player_start_status_effect(bit, ticks); player_daze, thirty ticks of
 * bit 0x40; player_knockback_up(n), the vertical velocity n * 0x2f / 4,
 * the horizontal halved, the gravity mode -2 unless -4;
 * player_apply_impact(mask, n), bit 0 the daze and bit 1 the knockback;
 * player_start_vertical_motion(obj), which sets the player's vertical pair
 * (0x8d, 0) when `obj` is the player and collision face bit 4 is clear;
 * ethereal_void_effect, level 9's, banded by the hit points left. */
void player_start_status_effect(uw_motion *m, uint8_t bit, uint8_t ticks);
/* shade_set_level(level) -- src/uw_motion_inventory.c. */
void shade_set_level(uw_motion *m, uint8_t level);
/* run_function_on_whoami_list(whoami, all, arg, fn): the active
 * mobile roster walked, `fn(obj, arg)` for each creature whose +0x1a is
 * `whoami`; a true return steps the cursor back (the list closed up), and
 * without `all` the walk stops at the first match. */
void run_function_on_whoami_list(uw_motion *m, uint8_t whoami, int all, int16_t arg,
                                 int (*fn)(uw_motion *, uint16_t, int16_t));
/* run_code_on_objects_in_area -- src/uw_motion_spell.c. */
void run_code_on_objects_in_area(uw_motion *m, int8_t count, uint8_t arg, uint32_t callback,
                                 uint8_t mode, int8_t x0, int8_t y0, int8_t w, int8_t h, uint16_t bp);
/* delta_to_direction(dx, dy) -- src/uw_motion_spell.c -- and
 * report_direction_to(name, from x, y, level, to x, y, level,
 * near) -- src/uw_motion_trap.c: the name, "to the <direction>" unless
 * within `near` (a negative near names message 0x23 - near outright),
 * the height clause by the levels' difference, ' and ', 'very near' and
 * the full stop as the original composes them. */
int  delta_to_direction(int8_t dx, int8_t dy);
void report_direction_to(uw_motion *m, const char *name, int16_t fx, int16_t fy, int16_t flevel,
                         int16_t tx, int16_t ty, int16_t tlevel, int16_t near);
/* report_crime(obj, kind), and npc_witness_crime,
 * its callback over the 15 x 15 square; trap_tyball_death;
 * use_tyballs_orb(obj, applied); trap_exploding_book
 * -- src/uw_motion_trap.c. */
void report_crime(uw_motion *m, uint16_t obj, uint8_t kind, uint16_t bp);
int  npc_witness_crime(uw_motion *m, int16_t x, int16_t y, uint16_t obj);
void trap_tyball_death(uw_motion *m);
void use_tyballs_orb(uw_motion *m, uint16_t obj, int applied);
void trap_exploding_book(uw_motion *m);
#define NPC_WITNESS_CRIME_FAR 0x61d50048u   /* its far address as a callback */
void player_daze(uw_motion *m);
void player_knockback_up(uw_motion *m, int16_t n);
void player_apply_impact(uw_motion *m, uint16_t mask, int16_t n);
void player_start_vertical_motion(uw_motion *m, uint16_t obj);
void ethereal_void_effect(uw_motion *m);
/* player_death and what the silver tree does on arrival
 * (player_resurrect) -- src/uw_motion_tick.c. */
void player_death(uw_motion *m);
void player_resurrect(uw_motion *m);
void inventory_discard_all(uw_motion *m);
void palette_cycle(uw_motion *m, uint8_t clock);
void itoa10(int v, char *out);
void print_flask_status(uw_motion *m);
void print_time_status(uw_motion *m);
void weapon_toggle(uw_motion *m);
void panel_button_click(uw_motion *m, int16_t button, int after);

/* uw_motion_tick.c */
void player_gain_experience(uw_motion *m, int16_t delta);
void award_kill_exp(uw_motion *m, uint16_t obj);
void panels_refresh(uw_motion *m);
void player_restore_mana(uw_motion *m, uint16_t obj, int8_t n);
void player_restore_vitality(uw_motion *m, uint16_t obj, int8_t n);
void change_health(uw_motion *m, uint16_t obj, uint8_t n);
int change_hunger(uw_motion *m, int16_t n);
int active_spell_expire(uw_motion *m, int16_t *i);

/* uw_motion_cursor.c */
void cursor_hide(uw_motion *m);
void cursor_show(uw_motion *m);
void cursor_shape_push(uw_motion *m, uint16_t shape);
void cursor_set_shape(uw_motion *m, uint16_t shape);
int16_t cursor_region_add(uw_motion *m, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t shape);
void cursor_reset_bounds(uw_motion *m);     /* uw_motion.c */
void cursor_key_move(uw_motion *m, int16_t key);
void cursor_region_remove(uw_motion *m, int16_t slot);
/* uw_motion_input.c */
int16_t input_bind_hotspot(uw_motion *m, int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                           int16_t param, uint16_t mask, uint16_t off, uint16_t seg);
void input_unbind(uw_motion *m, int16_t id);

/* ---- for the boot (src/uw_boot.c): the routines a new game runs ------ */
void level_transition_effects(uw_motion *m, int level, int phase, uint16_t bp);
void place_player_in_tile(uw_motion *m, int16_t x, int16_t y, uint16_t bp);
void player_recompute_maxima(uw_motion *m, int fill);
/* panel_build_elements: the HUD's elements allocated once --
 * the flasks' four each, the compass rose and needle, element 7 -- and
 * drawn: the flasks filled, the dragons' art, the compass, the runes; the
 * mode panel's image and its drawer are the caller's. */
void panel_build_elements(uw_motion *m);
/* the stack's question and the takes past it (uw_motion_stack_answer) */
void stack_ask(uw_motion *m, uint16_t obj, int path, int16_t slot);
void action_pickup_take(uw_motion *m, uint16_t obj, uint16_t seg);
void inventory_drag_take(uw_motion *m, int16_t slot, uint16_t o, int split, int answered);
/* src/uw_motion_barter.c */
int16_t barter_slot_at(uw_motion *m, int whose, int16_t x, int16_t y);
void barter_click_slot(uw_motion *m, int whose, int slot);
void barter_click_npc_slot(uw_motion *m);
void barter_click_player_slot(uw_motion *m);
void barter_click_player_slot_at_cursor(uw_motion *m);
void barter_lift_take(uw_motion *m, uint16_t obj, uint16_t rest, int answered);
void uw_motion_barter_wait_end(uw_motion *m);
/* scroll_print of a NUL-terminated string in the data segment */
void uw_motion_print_ds_string(uw_motion *m, uint16_t at);
/* panel_button_draw(mode, down): the action button's art. */
void panel_button_draw(uw_motion *m, int16_t mode, int down);
void cursor_shape_pop(uw_motion *m, uint16_t flags);
int cursor_over_view_rect(uw_motion *m);
int projectile_aim_from_cursor(uw_motion *m);

/* uw_motion_inventory.c */
void store_obj_far(uw_motion *m, uint16_t at, uint16_t o);
uint16_t inventory_slot_object(uw_motion *m, int16_t slot);
uint16_t inventory_find_by_kind(uw_motion *m, int16_t cls, int16_t sub, int16_t type, int16_t scope, int16_t *slot);
int inventory_draw_weight_left(uw_motion *m, int report);
void inventory_panel_redraw(uw_motion *m, int16_t from, int16_t to);
void inventory_slot_click(uw_motion *m, int16_t element);
void paperdoll_load_body_art(uw_motion *m);
void imgbuf_capture_rect(uw_motion *m, uint16_t handle, int x, int y, int w, int h);
/* gfx_blit_planar of a saved image: its rows from `srcy` up to
 * `end` (the argument is the end row, not a count), put back at (x, y) --
 * the strip a stats skill row restores. */
void imgbuf_restore_rows(uw_motion *m, uint16_t handle, int x, int y, int w, int end, int srcy);
/* panel_draw_runes, for the rune bag's own clicks. */
void panel_draw_runes_at(uw_motion *m, uint16_t runes);
/* src/uw_motion_panelview.c: the panel's other two views and the flip. */
void stats_panel_draw(uw_motion *m);
void stats_panel_refresh(uw_motion *m);
void panel_set_mode_icon(uw_motion *m);
void stats_skill_scroll_click(uw_motion *m);
void rune_bag_draw(uw_motion *m);
int  rune_bag_add(uw_motion *m, uint16_t obj);
void rune_bag_click(uw_motion *m);
void rune_shelf_clear(uw_motion *m);
void panel_view_switch_begin(uw_motion *m, uint8_t target);
int  panel_view_switch_step(uw_motion *m);
void inventory_panel_container_button(uw_motion *m);
void inventory_click_slot_index(uw_motion *m, int16_t slot);
int16_t inventory_damage_slot(uw_motion *m, int16_t slot, uint8_t dmg, uint8_t type, int16_t mode,
                              int debris);
uint16_t inventory_unlink_object(uw_motion *m, int16_t cls, int16_t sub, int16_t type, int16_t slot, int16_t count);
void player_state_recalc(uw_motion *m);
void player_settle_motion(uw_motion *m);
void player_sleep(uw_motion *m, int16_t mode, uint16_t bp);
void try_sleep(uw_motion *m, uint16_t bp);
char *format_article_plural(char *s, int article, int plural);
int format_object_name(uw_motion *m, char *dest, size_t cap, uint16_t w0, uint8_t who, int article, int plural);
void scroll_print(uw_motion *m, const char *text);
int16_t combat_check_for_ammo(uw_motion *m, uint16_t launcher);
void missile_release(uw_motion *m, uint16_t launcher, uint16_t bp);
void projectile_take_fields(uw_motion *m, uint16_t p, uint16_t t);
void inventory_panel_activate_view(uw_motion *m);
void print_message_parts(uw_motion *m, int16_t a, int16_t b, int16_t c);
void panel_inventory_click(uw_motion *m);

/* uw_motion_action.c */
void key_step_or_turn(uw_motion *m, int16_t dir);
void action_use(uw_motion *m, int after);
void action_pickup(uw_motion *m);
int object_tree_contains_id(uw_motion *m, uint16_t obj, uint16_t id);
void pending_action_run(uw_motion *m, uint16_t obj, int in_inventory);
void view_action_dispatch(uw_motion *m);

/* uw_motion_use.c */
const char *ds_text(uw_motion *m, uint16_t at, char *buf, size_t cap);
void effect_nonlethal_damage(uw_motion *m, uint16_t obj, int16_t dice);
void use_misc_dispatch(uw_motion *m, uint16_t a, uint16_t b, int flag);
void use_object_dispatch_misc(uw_motion *m, uint16_t obj, int flag);
uint16_t spawn_object_in_hand(uw_motion *m, uint16_t obj, uint16_t id);
void use_special_item(uw_motion *m, uint16_t a, uint16_t b, int flag);
void use_readable(uw_motion *m, uint16_t obj, int flag);
int16_t use_object(uw_motion *m, uint16_t a, uint16_t b, int flag);
void inventory_panel_activate(uw_motion *m, int16_t element);
void use_object_with_prompt(uw_motion *m, uint16_t obj, uint16_t handler);
void use_key_or_lockpick(uw_motion *m, uint16_t obj, int flag);
int16_t inventory_find_object(uw_motion *m, uint16_t obj);
void use_light_source(uw_motion *m, uint16_t obj, int flag);

/* uw_motion_look.c */
void look_at_texture(uw_motion *m, uint8_t kind, int16_t surface);
void look_at_scenery(uw_motion *m, uint16_t obj, int16_t mode);

/* uw_motion.c: the free camera */
void debug_camera_goto(uw_motion *m, int16_t which);
void debug_camera_set_target(uw_motion *m, int16_t mode);
void debug_camera_mouse_drag(uw_motion *m);
/* uw_motion_object.c: obj_pool_init and game_world_reset */
void obj_pool_init(uw_motion *m);
void game_world_reset(uw_motion *m);
/* uw_motion_save.c */
void inventory_reset(uw_motion *m);
void inventory_chain_free(uw_motion *m, uint16_t link);
/* uw_motion_panelview.c */
void rune_bag_clear(uw_motion *m);
void panel_redraw(uw_motion *m);
/* uw_motion_tick.c */
void teleport_to_moonstone(uw_motion *m);
/* uw_motion_spell.c */
void damage_objects_in_tile(uw_motion *m, int16_t x, int16_t y, uint8_t kind, uint8_t attacker, uint16_t bp);
void spawn_animo_copies(uw_motion *m, uint16_t src, uint8_t x, uint8_t y);
int  projectile_detonate(uw_motion *m, uint16_t obj, uint8_t x, uint8_t y, uint8_t attacker, uint16_t bp);
/* uw_motion_trap.c */
void trap_bullfrog(uw_motion *m, uint8_t owner);
int  disarm_trap(uw_motion *m, uint16_t obj, uint8_t skill);
uint16_t trap_create(uw_motion *m, int16_t x, int16_t y, uint8_t kind);
/* uw_motion_look.c */
void action_look_rest(uw_motion *m, uint16_t obj);

/* uw_motion_prompt.c */
void scroll_ask_yes_no(uw_motion *m, uint16_t message, int initial, uint8_t whose);
int  skill_gain(uw_motion *m, int skill);
void chant_mantra(uw_motion *m);
void item_repair(uw_motion *m, uint16_t obj, int skill);
void use_anvil(uw_motion *m, uint16_t obj, int in_inventory);
void play_instrument(uw_motion *m, int which);
int action_in_reach(uw_motion *m, int16_t max_dist2, uint16_t obj, uint16_t tile);
void append(char *buf, size_t cap, const char *text);
void look_at(uw_motion *m, uint16_t obj, int16_t lore);
int search_for_trap(uw_motion *m, uint16_t obj, uint8_t skill);
void action_look(uw_motion *m);

/* uw_motion_place.c */
int16_t inventory_panel_hit_test(uw_motion *m, int16_t x, int16_t y);
int inventory_add_object(uw_motion *m, uint16_t obj, int16_t slot);
void inventory_drop_on_slot(uw_motion *m, int16_t slot);
int cursor_wait_for_drag(uw_motion *m, int pump);
uint16_t inventory_click_take(uw_motion *m);
int inventory_remove_quantity(uw_motion *m, uint16_t obj, int16_t count);
int object_clear(uw_motion *m, uint16_t obj, int from_inventory, int force);

/* uw_motion_container.c */
uint8_t *far_bytes(uw_motion *m, uint16_t off, uint16_t seg, uint16_t n);
void container_stack_less_weight(uw_motion *m, uint16_t w);
void container_view_refresh(uw_motion *m);
void container_panel_close(uw_motion *m);
void container_page_fill(uw_motion *m);
void container_panel_up(uw_motion *m);
void container_page_forward(uw_motion *m);
void container_page_back(uw_motion *m);
void open_container(uw_motion *m, uint16_t a, uint16_t b, int flag);
void container_empty(uw_motion *m, uint16_t obj, int verbose);
void object_locked_message(uw_motion *m, uint16_t obj);
void paperdoll_click(uw_motion *m, int16_t param);
void paperdoll_click_rest(uw_motion *m, int16_t param, int16_t region, int flag);
void inventory_pick_up_from_slot(uw_motion *m, int16_t slot, int rest);

/* uw_motion_trap.c */
uint16_t object_find_matching(uw_motion *m, uint16_t *link, int recurse, uint16_t cls, uint16_t sub, uint16_t type);
void trigger_object_link_port(uw_motion *m, uint16_t who, uint16_t obj, int kind);
void trigger_object_link_at(uw_motion *m, uint16_t who, uint16_t obj, int kind, uint16_t x, uint16_t y, uint16_t bp);
void door_open(uw_motion *m, uint16_t actor, uint16_t obj);
void door_close(uw_motion *m, uint16_t obj);
uint16_t object_find_in_tilemap(uw_motion *m, uint16_t cls, uint16_t sub, uint16_t type,
                                int16_t *x, int16_t *y);
void find_and_close_doors(uw_motion *m, int8_t mode);
void trigger_create_object_trap(uw_motion *m, int8_t mode, uint16_t bp);
void creature_ai_load_context(uw_motion *m, uint16_t obj);
int  hostile_nearby_probe(uw_motion *m, uint16_t obj);
int  hostile_creature_nearby(uw_motion *m, uint16_t bp);
int  npc_hunt_sleeper_probe(uw_motion *m, uint16_t obj, uint16_t bp);
int  npc_hunt_sleeper(uw_motion *m, uint16_t bp);
int  tile_standing_spot(uint8_t type, uint8_t *x, uint8_t *y);
void npc_settle_level(uw_motion *m, uint16_t bp);
uint16_t trap_dispatch(uw_motion *m, uint16_t trap, uint16_t x, uint16_t y, uint16_t bp);
int door_unlock_attempt(uw_motion *m, uint16_t actor, uint16_t door, int16_t skill);
int door_lock_remove(uw_motion *m, uint16_t door, int all);
uint16_t trap_fire(uw_motion *m, uint8_t x, uint8_t y, uint16_t trap, uint16_t who, int16_t cls, uint8_t arg, uint16_t bp);
void use_door_furniture_or_switch(uw_motion *m, uint16_t actor, uint16_t obj);
uint16_t object_find_link(uw_motion *m, uint16_t link, uint16_t index);
void object_chain_remove(uw_motion *m, uint16_t link, uint16_t obj);
void trap_after_firing(uw_motion *m, uint16_t link, uint16_t obj);
int find_landing_spot(uw_motion *m, uint16_t obj, int16_t x, int16_t y, uint16_t out_x, uint16_t out_y, uint8_t relax, uint16_t bp);
uint16_t trigger_chain(uw_motion *m, uint16_t who, uint16_t what, uint16_t trig, int16_t kind, uint16_t bp);

/* uw_motion_save.c */
void combat_reset(uw_motion *m);

#endif
