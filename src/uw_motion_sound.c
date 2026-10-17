/* SPDX-License-Identifier: MIT */
/* The sound: the engine's own state machine over the AIL driver -- the music's choice of track and its loads, the four
 * sound-effect slots and their ageing, and the two settings.
 *
 * None of it makes a sound. The driver (src/uw_ail.c, m->ail) sequences the
 * score and hands channel messages to the host's synthesiser; what lives
 * here is what the engine keeps in its own data segment, which the pairs
 * see: music_track_playing and _wanted, the sequence handle, the combat
 * music's clock, and the effect slots -- active, note, program, channel,
 * countdown -- with the channels they hold in snd_channel_alloc_mask.
 * snd2's footstep is one of these: slot 0, note 0x1d on channel 9 for four
 * sixteenths of a second.
 *
 * Two things run from the driver's timer rather than from a pass, and the
 * host calls them: uw_ail_tick at 120 Hz and uw_motion_sound_age_slots at
 * 16 Hz (sfx_timer_install's ail_register_timer). With no driver (m->ail
 * NULL) the host leaves music_available and sfx_available clear, as
 * sound_init does with none configured, and every function here returns at
 * its first line.
 *
 * One of the src/uw_motion*.c files; see uw_motion.h and uw_motion_int.h. */
#include "uw_motion_int.h"
#include "uw_ail.h"
#include "uw_sound.h"

enum {
    MUSIC_DRIVER_ID    = 0x013a,   /* snd_music_driver_id: 1 the PC speaker, 2 AdLib, 6 MT-32 */
    AIL_DRIVER         = 0x013c,   /* ail_driver_handle, -1 with none */
    CHANNEL_MASK       = 0x014c,   /* snd_channel_alloc_mask: the channels the effects hold */
    SFX_ACTIVE         = 0x25f6,   /* sfx_slot_active, a bit a slot */
    SFX_NOTE           = 0x25f7,   /* sfx_slot_note[4] */
    SFX_PROGRAM        = 0x2606,   /* sfx_slot_program[4] */
    SFX_CHANNEL        = 0x2612,   /* sfx_slot_channel[4], 1-based */
    SFX_COUNTDOWN      = 0x262c,   /* sfx_slot_countdown[4], words, sixteenths of a second, -1 held */
    SOUND_UPDATE_CLOCK = 0x2634,   /* 32 bits: when the combat music last changed */
    MUSIC_SEQUENCE     = 0x2646,   /* music_sequence_handle, -1 with none */
    GAME_MODE          = 0x565e
};

/* a byte a track: whether a finished track is followed by the
 * track the game wants rather than a dungeon theme drawn afresh */
static const uint8_t track_keeps_its_place[16] = { 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0 };

static uint32_t clock_now(uw_motion *m) { return m->clock; }

/* ---- the music ----------------------------------------------------------- */

/* music_track_finished: 1 with no sequence, or when the driver
 * says the loaded one is not playing */
int music_track_finished(uw_motion *m) {
    uint16_t h = rw(m->ds, MUSIC_SEQUENCE);
    if (h == 0xffff || !m->ail) return 1;
    return uw_ail_sequence_status(m->ail, (int16_t)h) != 1;
}

/* snd_install_timbre(bank, program): the patch from the bank
 * (snd_bank_find_patch, length-prefixed) into the driver's cache; 0 when
 * the bank has none */
static int snd_install_timbre(uw_motion *m, uint8_t bank, uint8_t program) {
    size_t len = 0;
    const uint8_t *patch = m->timbre_bank ? uw_bank_patch(m->timbre_bank, program, bank, &len) : NULL;
    if (!patch || !m->ail) return 0;
    uw_ail_install_timbre(m->ail, bank, program, patch, len);
    return 1;
}

/* ail_timbre_installed, else snd_install_timbre: what play_instrument and
 * sound_effect_start ask before a channel is taken */
int snd_timbre_ready(uw_motion *m, uint8_t bank, uint8_t program) {
    if (!m->ail) return 0;
    return uw_ail_timbre_installed(m->ail, bank, program) || snd_install_timbre(m, bank, program);
}

/* load_xmi(track, start), from the instructions: a track not
 * the one playing is read (music_read_xmi_file, "SOUND\" + the template
 * the driver's letter and the track's two octal digits patch), the old
 * sequence stopped and released, the new one registered and every timbre
 * it asks for installed; then, `start` set, started at tempo 100 and
 * volume 0x60. The track playing, nothing wanted, 1. A file that does not
 * read, a sequence the driver refuses or a timbre the bank lacks leaves no
 * sequence and turns the music off for good (music_available 0). */
int load_xmi(uw_motion *m, uint8_t track, int start) {
    uint8_t *ds = m->ds;
    int h;
    if (!ds[MUSIC_AVAILABLE] || !ds[MUSIC_ENABLED] || !m->ail) return 0;
    if (track != ds[MUSIC_TRACK_PLAYING]) {
        int r;
        if (track >= 16 || !m->xmi[track]) goto fail;
        uw_ail_stop_sequence(m->ail, (int16_t)rw(ds, MUSIC_SEQUENCE));
        uw_ail_release_sequence(m->ail, (int16_t)rw(ds, MUSIC_SEQUENCE));
        h = uw_ail_register_sequence(m->ail, m->xmi[track], m->xmi_size[track], 0);
        ww(ds, MUSIC_SEQUENCE, (uint16_t)h);
        if (h == -1) goto fail;
        while ((r = uw_ail_timbre_request(m->ail, h)) != 0xffff)
            if (!snd_install_timbre(m, (uint8_t)(r >> 8), (uint8_t)r)) goto fail;
    }
    if (start) {
        h = (int16_t)rw(ds, MUSIC_SEQUENCE);
        uw_ail_start_sequence(m->ail, h);
        uw_ail_set_sequence_tempo(m->ail, h, 100, 0);
        uw_ail_set_sequence_volume(m->ail, h, 0x60, 0);
    }
    ds[MUSIC_TRACK_PLAYING] = track;
    ds[MUSIC_TRACK_WANTED] = 0;
    return 1;
fail:
    ww(ds, MUSIC_SEQUENCE, 0xffff);
    ds[MUSIC_AVAILABLE] = 0;
    return 0;
}

/* music_restart_current: the track playing reloaded and
 * started when it has run out, track 1 (the title's) as 4 -- the screens
 * that had the display while sound_update did not run come back to it */
void music_restart_current(uw_motion *m) {
    uint8_t track = m->ds[MUSIC_TRACK_PLAYING];
    if (!music_track_finished(m)) return;
    if (track == 1) track = 4;
    load_xmi(m, track, 1);
}

/* music_resume: the sequence started again unless playing, its
 * volume back to 0x60 at once */
void music_resume(uw_motion *m) {
    uint8_t *ds = m->ds;
    int16_t h = (int16_t)rw(ds, MUSIC_SEQUENCE);
    if (!ds[MUSIC_AVAILABLE] || !ds[MUSIC_ENABLED] || h == -1 || !m->ail) return;
    if (uw_ail_sequence_status(m->ail, h) != 1) uw_ail_start_sequence(m->ail, h);
    uw_ail_set_sequence_volume(m->ail, h, 0x60, 0);
}

/* music_stop: the sequence stopped, the setting left alone */
void music_stop(uw_motion *m) {
    uint8_t *ds = m->ds;
    if (!ds[MUSIC_AVAILABLE] || !ds[MUSIC_ENABLED] || !m->ail) return;
    uw_ail_stop_sequence(m->ail, (int16_t)rw(ds, MUSIC_SEQUENCE));
}

/* music_fade(up): four seconds to 0x60 or to silence */
void music_fade(uw_motion *m, int up) {
    uint8_t *ds = m->ds;
    int16_t h = (int16_t)rw(ds, MUSIC_SEQUENCE);
    if (!ds[MUSIC_AVAILABLE] || !ds[MUSIC_ENABLED] || h == -1 || !m->ail) return;
    uw_ail_set_sequence_volume(m->ail, h, up ? 0x60 : 0, 4000);
}

/* music_set_enabled(on): on, a dungeon theme drawn and loaded
 * when no sequence is, and the sequence started; off, stopped */
void music_set_enabled(uw_motion *m, int on) {
    uint8_t *ds = m->ds;
    if (!ds[MUSIC_AVAILABLE] || !m->ail) return;
    if (on && !ds[MUSIC_ENABLED]) {
        ds[MUSIC_ENABLED] = 1;
        if (rw(ds, MUSIC_SEQUENCE) == 0xffff) {
            ds[MUSIC_TRACK_WANTED] = (uint8_t)(rt_rand(m) % 3 + 2);     /* pick_level_theme_music */
            load_xmi(m, ds[MUSIC_TRACK_WANTED], 1);
        }
        uw_ail_start_sequence(m->ail, (int16_t)rw(ds, MUSIC_SEQUENCE));
    } else if (!on && ds[MUSIC_ENABLED] && rw(ds, MUSIC_SEQUENCE) != 0xffff) {
        ds[MUSIC_ENABLED] = 0;
        uw_ail_stop_sequence(m->ail, (int16_t)rw(ds, MUSIC_SEQUENCE));
    }
}

/* sound_update, each tick update, from the instructions: with
 * the music on --
 *   tracks 9 and 11 (a kill's fanfare, one never requested) play out whole
 *     before anything changes;
 *   combat music (5..7) ten seconds after the last strike gives way: to 8
 *     with the weapon drawn, else a dungeon theme drawn afresh;
 *   nothing wanted, or the wanted track playing: when it has run out, the
 *     next -- a dungeon theme drawn afresh after a track whose place-keeping
 *     byte is 0 or after a dungeon theme, in the dungeon (mode 1), or when
 *     nothing is wanted; 8 with the weapon drawn -- loaded and started, and
 *     the combat clock cleared;
 *   another track wanted: loaded at once -- but from one combat track to
 *     another only once the last change is 0x800 ticks old, the wanted one
 *     put back until then -- and a combat track's arrival stamps the clock. */
void sound_update(uw_motion *m) {
    uint8_t *ds = m->ds;
    uint16_t rec = rw(ds, PLAYER_RECORD_PTR);
    uint8_t playing = ds[MUSIC_TRACK_PLAYING];
    if (!ds[MUSIC_AVAILABLE] || !ds[MUSIC_ENABLED]) return;
    if ((playing == 9 || playing == 0xb) && !music_track_finished(m)) return;
    if (playing >= 5 && playing <= 7 && clock_now(m) > rd32(ds, COMBAT_MUSIC_TIME) + 0xa00) {
        if (ds[(uint16_t)(rec + 0x5f)] & 2) ds[MUSIC_TRACK_WANTED] = 8;
        else ds[MUSIC_TRACK_WANTED] = (uint8_t)(rt_rand(m) % 3 + 2);
    }
    if (!ds[MUSIC_TRACK_WANTED] || ds[MUSIC_TRACK_WANTED] == ds[MUSIC_TRACK_PLAYING]) {
        if (!music_track_finished(m)) return;
        playing = ds[MUSIC_TRACK_PLAYING];
        if (((!track_keeps_its_place[playing & 0xf] || (playing >= 2 && playing <= 4)) && rw(ds, GAME_MODE) == 1)
            || !ds[MUSIC_TRACK_WANTED])
            ds[MUSIC_TRACK_WANTED] = (uint8_t)(rt_rand(m) % 3 + 2);
        if (ds[(uint16_t)(rec + 0x5f)] & 2) ds[MUSIC_TRACK_WANTED] = 8;
        load_xmi(m, ds[MUSIC_TRACK_WANTED], 1);
        wr32(ds, SOUND_UPDATE_CLOCK, 0);
        return;
    }
    playing = ds[MUSIC_TRACK_PLAYING];
    if (playing >= 5 && playing <= 7 && ds[MUSIC_TRACK_WANTED] >= 5 && ds[MUSIC_TRACK_WANTED] <= 7) {
        if (clock_now(m) > rd32(ds, SOUND_UPDATE_CLOCK) + 0x800) {
            load_xmi(m, ds[MUSIC_TRACK_WANTED], 1);
            wr32(ds, SOUND_UPDATE_CLOCK, clock_now(m));
        } else {
            ds[MUSIC_TRACK_WANTED] = playing;
        }
    } else {
        load_xmi(m, ds[MUSIC_TRACK_WANTED], 1);
    }
    if (ds[MUSIC_TRACK_WANTED] >= 5 && ds[MUSIC_TRACK_WANTED] <= 7) wr32(ds, SOUND_UPDATE_CLOCK, clock_now(m));
}

/* ---- the effects --------------------------------------------------------- */

/* sound_effect_start(id, program, note, velocity, pan,
 * duration) -> the slot, or 0xff. From the instructions: the first of the
 * four slots free; with the PC speaker (driver 1) a note the id's table
 * entry picks on channel 2 for a fixed count; otherwise the program's
 * bank-1 timbre installed if the driver lacks it, a channel locked
 * (ail_lock_channel) and its bit set, the slot's bytes -- the countdown the
 * duration in sixteenths of a second, -1 held -- and the channel set up:
 * controller 114 to 1, the program, 121 (all controllers off), volume and
 * expression 0x7f, the pan. Then the note on. */
static uint8_t sound_effect_start(uw_motion *m, uint8_t id, uint8_t program, uint8_t note,
                                  uint8_t velocity, uint8_t pan, uint16_t duration) {
    uint8_t *ds = m->ds, bit = 1, slot, ch;
    for (slot = 0; slot < 4 && (ds[SFX_ACTIVE] & bit); slot++) bit = (uint8_t)(bit << 1);
    if (slot == 4) return 0xff;
    if (ds[MUSIC_DRIVER_ID] == 1) {
        /* ids 3..22: the speaker's three notes */
        static const uint8_t speaker[20] = { 3, 1, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2, 3 };
        uint8_t k = (uint8_t)(id - 3) < 20 ? speaker[(uint8_t)(id - 3)] : 0;
        if (!k) return 0xff;
        note = k == 1 ? 0x48 : k == 2 ? 0x42 : 0x38;
        ds[SFX_ACTIVE] |= bit;
        ds[(uint16_t)(SFX_CHANNEL + slot)] = 2;
        ww(ds, (uint16_t)(SFX_COUNTDOWN + slot * 2), (uint16_t)(k == 1 ? 0x10 : k == 2 ? 8 : 4));
        ch = 2;
    } else {
        if (!m->ail) return 0xff;
        if (!uw_ail_timbre_installed(m->ail, 1, program) && !snd_install_timbre(m, 1, program)) return 0xff;
        ch = (uint8_t)uw_ail_lock_channel(m->ail);
        if (!ch) return 0xff;
        ww(ds, CHANNEL_MASK, (uint16_t)(rw(ds, CHANNEL_MASK) | (1u << ch)));
        ds[SFX_ACTIVE] |= bit;
        ds[(uint16_t)(SFX_CHANNEL + slot)] = ch;
        ww(ds, (uint16_t)(SFX_COUNTDOWN + slot * 2), duration == 0xffff ? 0xffff : (uint16_t)((int16_t)duration / 16));
        ds[(uint16_t)(SFX_NOTE + slot)] = note;
        ds[(uint16_t)(SFX_PROGRAM + slot)] = program;
        uw_ail_send_voice(m->ail, (uint8_t)(0xaf + ch), 0x72, 1);
        uw_ail_send_voice(m->ail, (uint8_t)(0xbf + ch), program, 0);
        uw_ail_send_voice(m->ail, (uint8_t)(0xaf + ch), 0x79, 0);
        uw_ail_send_voice(m->ail, (uint8_t)(0xaf + ch), 7, 0x7f);
        uw_ail_send_voice(m->ail, (uint8_t)(0xaf + ch), 0xb, 0x7f);
        uw_ail_send_voice(m->ail, (uint8_t)(0xaf + ch), 0xa, pan);
    }
    if (m->ail) uw_ail_send_voice(m->ail, (uint8_t)(0x8f + ch), note, velocity);
    return slot;
}

static int effect_record(uw_motion *m, uint8_t id, uw_sound_effect *e) {
    if (m->sounds && uw_sound_effect_at(m->sounds, id, e)) return 1;
    UW_NOT_CARRIED(m->not_carried);
    return 0;
}

static uint8_t clamp7f(int v) { return (uint8_t)(v < 0 ? 0 : v > 0x7f ? 0x7f : v); }

/* play_sound_effect(id, pan, velocity delta): SOUNDS.DAT's
 * record, its velocity plus the delta clamped to 0..0x7f */
uint8_t play_sound_effect(uw_motion *m, uint8_t id, uint8_t pan, int8_t delta) {
    uw_sound_effect e;
    if (!m->ds[SFX_AVAILABLE] || !m->ds[SFX_ENABLED]) return 0xff;
    if (!effect_record(m, id, &e)) return 0xff;
    return sound_effect_start(m, id, e.program, e.note, clamp7f(e.velocity + delta), pan, e.duration);
}

uint8_t uw_motion_sound_effect(uw_motion *m, int id, int pan, int delta) {
    return play_sound_effect(m, (uint8_t)id, (uint8_t)pan, (int8_t)delta);
}

/* play_sound_effect_at_xy(id, x, y, velocity delta), from the
 * instructions: x and y in eighths of a tile, against the player's (the
 * tile from +0x16, the eighth from +2); the distance by the runtime's
 * 32-bit root. At the player the pan is the centre; past 0x30 nothing
 * plays; otherwise each axis of the direction is (d << 7) / distance --
 * exactly 0x7f or 0x80 along an axis -- and the pan 0x40 less their cross
 * with the player's heading (gfx_sincos, >> 8 each, the products 16-bit)
 * >> 8, clamped; from eight away the velocity falls by (0x30 - distance) /
 * 0x28, its product taken to 16 bits and divided unsigned. */
uint8_t play_sound_effect_at_xy(uw_motion *m, uint8_t id, int16_t x, int16_t y, int8_t delta) {
    uint8_t *ds = m->ds, *ls = m->lseg;
    uw_sound_effect e;
    uint16_t pl = rw(ds, TRACKED_OBJECT), w16 = rw(ls, (uint16_t)(pl + 0x16)), w2 = rw(ls, (uint16_t)(pl + 2));
    int32_t dx, dy;
    uint16_t dist;
    int16_t pan, velocity;
    if (!ds[SFX_AVAILABLE] || !ds[SFX_ENABLED]) return 0xff;
    if (!effect_record(m, id, &e)) return 0xff;
    dx = (int32_t)(int16_t)(x - (int16_t)((w16 >> 10) * 8 + ((w2 & 0xe000) >> 13)));
    dy = (int32_t)(int16_t)(y - (int16_t)(((w16 & 0x3f0) >> 4) * 8 + ((w2 & 0x1c00) >> 10)));
    dist = uw_isqrt32((uint32_t)(dx * dx + dy * dy));
    velocity = (int16_t)(e.velocity + delta);
    if (dist == 0) {
        pan = 0x40;
    } else {
        int16_t ny, nx, s, c, cross;
        uint16_t angle;
        if (dy == dist) ny = 0x7f;
        else if (-dy == dist) ny = (int16_t)0x80;
        else ny = (int16_t)((dy << 7) / (int32_t)dist);
        if (dx == dist) nx = 0x7f;
        else if (-dx == dist) nx = (int16_t)0x80;
        else nx = (int16_t)((dx << 7) / (int32_t)dist);
        angle = (uint16_t)(0x4000 - (uint16_t)(((((w2 & 0x380) >> 7) << 5) + (ls[(uint16_t)(pl + 0x18)] & 0x1f)) << 8));
        uw_sincos(angle, &s, &c);
        s = (int16_t)(s >> 8);
        c = (int16_t)(c >> 8);
        cross = (int16_t)((int16_t)((int16_t)(ny * c) - (int16_t)(nx * s)) >> 8);
        pan = (int16_t)(0x40 - cross);
        if (pan > 0x7f) pan = 0x7f;
        else if (pan < 0) pan = 0;
        if (dist > 0x30) return 0xff;
        if (dist >= 8) velocity = (int16_t)((uint16_t)(int16_t)(velocity * (int16_t)(0x30 - dist)) / 0x28u);
    }
    return sound_effect_start(m, id, e.program, e.note, clamp7f(velocity), (uint8_t)pan, e.duration);
}

/* play_sound_effect_at_object(id, object, velocity delta): at
 * the object's place, in the same eighths */
uint8_t play_sound_effect_at_object(uw_motion *m, uint8_t id, uint16_t obj, int8_t delta) {
    uint8_t *ls = m->lseg;
    uint16_t w16, w2;
    if (!m->ds[SFX_AVAILABLE] || !m->ds[SFX_ENABLED]) return 0xff;
    w16 = rw(ls, (uint16_t)(obj + 0x16));
    w2 = rw(ls, (uint16_t)(obj + 2));
    return play_sound_effect_at_xy(m, id, (int16_t)((w16 >> 10) * 8 + ((w2 & 0xe000) >> 13)),
                                   (int16_t)(((w16 & 0x3f0) >> 4) * 8 + ((w2 & 0x1c00) >> 10)), delta);
}

/* sound_effect_stop(slot): an active slot's bit cleared and its
 * voice silenced -- All Notes Off with the PC speaker, else its channel
 * released -- and the channel's bit */
void sound_effect_stop(uw_motion *m, uint8_t slot) {
    uint8_t *ds = m->ds, ch;
    if (slot >= 4 || !(ds[SFX_ACTIVE] & (1 << slot))) return;
    ds[SFX_ACTIVE] = (uint8_t)(ds[SFX_ACTIVE] - (1 << slot));
    ch = ds[(uint16_t)(SFX_CHANNEL + slot)];
    if (m->ail) {
        if (ds[MUSIC_DRIVER_ID] == 1) uw_ail_send_voice(m->ail, (uint8_t)(ch + 0xaf), 0x7b, 0);
        else uw_ail_release_channel(m->ail, ch);
    }
    ww(ds, CHANNEL_MASK, (uint16_t)(rw(ds, CHANNEL_MASK) & ~(1u << (ch & 0x1f))));
}

/* sound_effect_stop_all */
void sound_effect_stop_all(uw_motion *m) {
    uint8_t slot;
    for (slot = 0; slot < 4; slot++) sound_effect_stop(m, slot);
}

/* sfx_set_enabled(on), and the effects silenced when off */
void sfx_set_enabled(uw_motion *m, int on) {
    uint8_t *ds = m->ds;
    if (!ds[SFX_AVAILABLE]) return;
    ds[SFX_ENABLED] = on != 0;
    if (!ds[SFX_ENABLED]) sound_effect_stop_all(m);
}

/* sound_effect_age_slots, AIL's timer at 16 Hz: each active
 * slot's countdown, unless held (-1), one less, and at zero the slot
 * cleared and its voice ended -- All Notes Off with the PC speaker, else
 * the slot's note off and the channel released -- and the channel's bit */
void uw_motion_sound_age_slots(uw_motion *m) {
    uint8_t *ds = m->ds, bit = 1, slot;
    if (!ds[SFX_ACTIVE]) return;
    for (slot = 0; slot < 4; slot++, bit = (uint8_t)(bit << 1)) {
        uint16_t at = (uint16_t)(SFX_COUNTDOWN + slot * 2), n;
        uint8_t ch;
        if (!(ds[SFX_ACTIVE] & bit) || rw(ds, at) == 0xffff) continue;
        n = (uint16_t)(rw(ds, at) - 1);
        ww(ds, at, n);
        if (n) continue;
        ds[SFX_ACTIVE] &= (uint8_t)~bit;
        ch = ds[(uint16_t)(SFX_CHANNEL + slot)];
        if (m->ail) {
            if (ds[MUSIC_DRIVER_ID] == 1) {
                uw_ail_send_voice(m->ail, (uint8_t)(ch + 0xaf), 0x7b, 0);
            } else {
                uw_ail_send_voice(m->ail, (uint8_t)(ch + 0x7f), ds[(uint16_t)(SFX_NOTE + slot)], 0);
                uw_ail_release_channel(m->ail, ch);
            }
        }
        ww(ds, CHANNEL_MASK, (uint16_t)(rw(ds, CHANNEL_MASK) & ~(1u << (ch & 0x1f))));
    }
}

/* What the host sets when it has a driver to give, as sound_init leaves
 * the data segment with one configured: the music driver's id, the handle
 * ail_register_driver gave (the first, 0), and the four flags as the data
 * image holds them -- both available, both enabled -- which a failed
 * sound_init would have cleared and the port's boot clears. The sequence
 * handle keeps the image's 0, which names no sequence until load_xmi
 * registers one. Whether either plays from then on is the player record's
 * +0xb5, which player_load_record applies (music_set_enabled,
 * sfx_set_enabled), or uw_motion_sound_enable's. */
void uw_motion_sound_attach(uw_motion *m, struct uw_ail *ail, int music_driver) {
    uint8_t *ds = m->ds;
    m->ail = ail;
    ds[MUSIC_DRIVER_ID] = ail ? (uint8_t)music_driver : 0;
    ww(ds, AIL_DRIVER, ail ? 0 : 0xffff);
    ds[MUSIC_AVAILABLE] = ds[SFX_AVAILABLE] = ail != NULL;
    ds[MUSIC_ENABLED] = ds[SFX_ENABLED] = ail != NULL;
}

void uw_motion_music_load(uw_motion *m, int track) { load_xmi(m, (uint8_t)track, 1); }
void uw_motion_music_restart(uw_motion *m) { music_restart_current(m); }
void uw_motion_music_stop(uw_motion *m) { music_stop(m); }
void uw_motion_music_resume(uw_motion *m) { music_resume(m); }

void uw_motion_music_theme(uw_motion *m, int track) {
    m->ds[MUSIC_TRACK_WANTED] = (uint8_t)track;     /* set_theme_music */
    sound_update(m);
}

/* pick_level_theme_music: one of the three dungeon themes */
void uw_motion_music_pick_level_theme(uw_motion *m) {
    m->ds[MUSIC_TRACK_WANTED] = (uint8_t)(rt_rand(m) % 3 + 2);
}

void uw_motion_sound_enable(uw_motion *m, int music, int effects) {
    sfx_set_enabled(m, effects);
    music_set_enabled(m, music);
}
