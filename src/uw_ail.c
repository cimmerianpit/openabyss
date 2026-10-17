/* SPDX-License-Identifier: MIT */
/* The Miles AIL 2 XMIDI driver, as ADLIB.ADV runs it: see uw_ail.h. */
#include "uw_ail.h"
#include <string.h>

/* the nine controllers the driver caches per channel -- the
 * table the driver builds from them at install, controller to slot * 16 */
static const uint8_t cached_controllers[9] = { 0x07, 0x01, 0x0a, 0x0b, 0x40, 0x72, 0x6e, 0x6f, 0x70 };

static int cache_slot(uint8_t controller) {
    int i;
    for (i = 0; i < 9; i++)
        if (cached_controllers[i] == controller) return i;
    return -1;
}

static void emit(uw_ail *a, uint8_t status, uint8_t d1, uint8_t d2) {
    if (a->synth.message) a->synth.message(a->synth.user, status, d1, d2);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

void uw_ail_init(uw_ail *a, const uw_ail_synth *synth) {
    /* each cached controller's value at reset */
    static const uint8_t defaults[9] = { 0x7f, 0, 0x40, 0x7f, 0, 0, 0, 0, 0 };
    /* the program each channel is sent after its bend, 0xff none: the
     * MT-32's own instruments, which every driver's build carries */
    static const uint8_t programs[10] = { 0xff, 0x44, 0x30, 0x5f, 0x4e, 0x29, 0x03, 0x6e, 0x7a, 0xff };
    int i, ch;
    memset(a, 0, sizeof *a);
    if (synth) a->synth = *synth;
    memset(a->cache, 0xff, sizeof a->cache);
    memset(a->cache_program, 0xff, sizeof a->cache_program);
    memset(a->cache_bend_lsb, 0xff, sizeof a->cache_bend_lsb);
    memset(a->cache_bend_msb, 0xff, sizeof a->cache_bend_msb);
    for (i = 0; i < 9; i++)
        for (ch = 1; ch <= 9; ch++) {
            a->cache[i][ch] = defaults[i];
            emit(a, (uint8_t)(0xb0 | ch), cached_controllers[i], defaults[i]);
        }
    for (ch = 1; ch <= 9; ch++) {
        a->cache_bend_lsb[ch] = 0;
        a->cache_bend_msb[ch] = 0x40;
        emit(a, (uint8_t)(0xe0 | ch), 0, 0x40);
        if (programs[ch] != 0xff) {
            a->cache_program[ch] = programs[ch];
            emit(a, (uint8_t)(0xc0 | ch), programs[ch], 0);
        }
    }
}

static uw_ail_sequence *seq_of(uw_ail *a, int h) {
    return h >= 0 && h < UW_AIL_SEQUENCES && a->seq[h].used ? &a->seq[h] : NULL;
}

/* 0x2be6: the loop stack empty, the channel map the identity, the caches
 * and the note table cleared, volume and tempo 100 */
static void seq_reset(uw_ail_sequence *s) {
    int i;
    for (i = 0; i < UW_AIL_LOOPS; i++) s->loop[i].count = -1;
    for (i = 0; i < UW_AIL_CHANNELS; i++) {
        s->map[i] = (uint8_t)i;
        s->program[i] = s->bend_lsb[i] = s->bend_msb[i] = s->indirect[i] = 0xff;
        s->volume_cache[i] = s->sustain[i] = s->locked[i] = s->protect[i] = s->voice_protect[i] = 0xff;
    }
    for (i = 0; i < UW_AIL_NOTES; i++) s->note[i].channel = 0xff;
    s->delay = 0;
    s->active = 0;
    s->volume = s->volume_target = 100;
    s->tempo = s->tempo_target = 100;
    s->tempo_acc = 0;
}

/* The IFF walk service 0x97 makes: FORM XDIR, then the CAT XMID's FORM
 * XMIDs, and in sequence `index` its TIMB and EVNT chunks. */
int uw_ail_register_sequence(uw_ail *a, const uint8_t *d, size_t len, int index) {
    size_t at = 0, end, cat_end;
    int h, n = 0;
    uw_ail_sequence *s;
    for (h = 0; h < UW_AIL_SEQUENCES && a->seq[h].used; h++) { }
    if (h == UW_AIL_SEQUENCES || len < 12) return -1;
    if (memcmp(d, "FORM", 4) || memcmp(d + 8, "XDIR", 4)) return -1;
    at = 8 + be32(d + 4);
    at += at & 1;
    if (at + 12 > len || memcmp(d + at, "CAT ", 4) || memcmp(d + at + 8, "XMID", 4)) return -1;
    cat_end = at + 8 + be32(d + at + 4);
    if (cat_end > len) cat_end = len;
    at += 12;
    s = &a->seq[h];
    memset(s, 0, sizeof *s);
    while (at + 12 <= cat_end) {
        size_t form_end = at + 8 + be32(d + at + 4);
        if (memcmp(d + at, "FORM", 4) || memcmp(d + at + 8, "XMID", 4) || form_end > cat_end) return -1;
        if (n++ == index) {
            size_t c = at + 12;
            while (c + 8 <= form_end) {
                size_t clen = be32(d + c + 4);
                end = c + 8 + clen;
                if (end > form_end) return -1;
                if (!memcmp(d + c, "TIMB", 4) && clen >= 2) {
                    s->timb = d + c + 10;
                    s->timbres = d[c + 8] | d[c + 9] << 8;
                    if (10 + (size_t)s->timbres * 2 > clen + 8) s->timbres = (int)((clen - 2) / 2);
                } else if (!memcmp(d + c, "EVNT", 4)) {
                    s->evnt = d + c + 8;
                    s->evnt_len = clen;
                }
                c = end + (end & 1);
            }
            if (!s->evnt) return -1;
            s->used = true;
            s->status = 0;
            seq_reset(s);
            return h;
        }
        at = form_end + (form_end & 1);
    }
    return -1;
}

static void seq_notes_off(uw_ail *a, uw_ail_sequence *s);
static void seq_end(uw_ail *a, uw_ail_sequence *s);

void uw_ail_release_sequence(uw_ail *a, int h) {
    uw_ail_sequence *s = seq_of(a, h);
    if (!s) return;
    if (s->status == 1) { seq_notes_off(a, s); seq_end(a, s); }
    s->used = false;
}

static bool installed(const uw_ail *a, int bank, int program) {
    return a->installed[bank & 0xff][(program & 0x7f) >> 3] & (1 << (program & 7));
}

int uw_ail_timbre_request(uw_ail *a, int h) {
    uw_ail_sequence *s = seq_of(a, h);
    int i;
    if (!s || !s->timb) return 0xffff;
    for (i = 0; i < s->timbres; i++) {
        uint8_t program = s->timb[i * 2], bank = s->timb[i * 2 + 1];
        if (!installed(a, bank, program)) return bank << 8 | program;
    }
    return 0xffff;
}

bool uw_ail_timbre_installed(const uw_ail *a, int bank, int program) {
    return installed(a, bank, program);
}

void uw_ail_install_timbre(uw_ail *a, int bank, int program, const uint8_t *patch, size_t len) {
    a->installed[bank & 0xff][(program & 0x7f) >> 3] |= (uint8_t)(1 << (program & 7));
    if (a->synth.timbre) a->synth.timbre(a->synth.user, (uint8_t)bank, (uint8_t)program, patch, len);
}

/* 0x2d12: every sounding note of the sequence off, the channel counts down */
static void seq_notes_off(uw_ail *a, uw_ail_sequence *s) {
    int i;
    for (i = 0; i < UW_AIL_NOTES; i++) {
        uint8_t ch = s->note[i].channel;
        if (ch == 0xff) continue;
        s->note[i].channel = 0xff;
        a->notes[s->map[ch]]--;
        emit(a, (uint8_t)(0x80 | s->map[ch]), s->note[i].note, 0);
    }
    s->active = 0;
}

/* 0x2c90: every sequence's notes on logical channel `ch` off */
static void channel_notes_off(uw_ail *a, int ch) {
    int h, i;
    for (h = 0; h < UW_AIL_SEQUENCES; h++) {
        uw_ail_sequence *s = &a->seq[h];
        if (!s->used || !s->active) continue;
        for (i = 0; i < UW_AIL_NOTES; i++) {
            if (s->note[i].channel != ch) continue;
            s->note[i].channel = 0xff;
            a->notes[s->map[ch]]--;
            emit(a, (uint8_t)(0x80 | s->map[ch]), s->note[i].note, 0);
            s->active--;
        }
    }
}

/* 0x2d66: the sequence's hold on its channels given back -- sustain off, a
 * channel it locked released and mapped home, its protection cleared, its
 * voice protection off */
static void seq_end(uw_ail *a, uw_ail_sequence *s) {
    int ch;
    for (ch = 0; ch < UW_AIL_CHANNELS; ch++) {
        if (s->sustain[ch] != 0xff && s->sustain[ch] >= 0x40) {
            a->cache[4][ch] = 0;
            emit(a, (uint8_t)(0xb0 | ch), 0x40, 0);
        }
        if (s->locked[ch] != 0xff && s->locked[ch] >= 0x40) {
            channel_notes_off(a, ch);
            uw_ail_release_channel(a, s->map[ch] + 1);
            s->map[ch] = (uint8_t)ch;
        }
        if (s->protect[ch] != 0xff && s->protect[ch] >= 0x40) a->flags[ch] &= 0xbf;
        if (s->voice_protect[ch] != 0xff && s->voice_protect[ch] >= 0x40)
            emit(a, (uint8_t)(0xb0 | ch), 0x70, 0);
    }
}

void uw_ail_start_sequence(uw_ail *a, int h) {
    uw_ail_sequence *s = seq_of(a, h);
    if (!s) return;
    if (s->status == 1) uw_ail_stop_sequence(a, h);
    seq_reset(s);
    s->pos = 0;
    s->status = 1;
    s->started = true;
}

void uw_ail_stop_sequence(uw_ail *a, int h) {
    uw_ail_sequence *s = seq_of(a, h);
    if (!s || s->status != 1) return;
    seq_notes_off(a, s);
    seq_end(a, s);
    s->status = 0;
}

int uw_ail_sequence_status(const uw_ail *a, int h) {
    return h >= 0 && h < UW_AIL_SEQUENCES && a->seq[h].used ? a->seq[h].status : 0xffff;
}

/* 0x2efc: every channel volume the score set, sent again at the sequence's
 * volume -- unless the channel is locked */
static void seq_resend_volumes(uw_ail *a, uw_ail_sequence *s) {
    int ch;
    for (ch = 0; ch < UW_AIL_CHANNELS; ch++) {
        unsigned v;
        if (s->volume_cache[ch] == 0xff) continue;
        v = (unsigned)s->volume_cache[ch] * (unsigned)s->volume / 100u;
        if (v > 0x7f) v = 0x7f;
        a->cache[0][ch] = (uint8_t)v;
        if (!(a->flags[ch] & 0x80)) emit(a, (uint8_t)(0xb0 | s->map[ch]), 7, (uint8_t)v);
    }
}

void uw_ail_set_sequence_volume(uw_ail *a, int h, int volume, int ms) {
    uw_ail_sequence *s = seq_of(a, h);
    int delta;
    if (!s) return;
    s->volume_target = volume;
    if (!ms) {
        s->volume = volume;
        seq_resend_volumes(a, s);
        return;
    }
    delta = s->volume_target - s->volume;
    if (!delta) return;
    if (delta < 0) delta = -delta;
    s->volume_period = (int32_t)((long)ms * 10 / delta);
    if (!s->volume_period) s->volume_period = 1;
    s->volume_acc = 0;
}

void uw_ail_set_sequence_tempo(uw_ail *a, int h, int tempo, int ms) {
    uw_ail_sequence *s = seq_of(a, h);
    int delta;
    if (!s) return;
    s->tempo_target = tempo;
    if (!ms) { s->tempo = tempo; return; }
    delta = s->tempo_target - s->tempo;
    if (!delta) return;
    if (delta < 0) delta = -delta;
    s->tempo_period = (int32_t)((long)ms * 10 / delta);
    if (!s->tempo_period) s->tempo_period = 1;
    s->tempo_time = 0;
}

void uw_ail_send_voice(uw_ail *a, uint8_t status, uint8_t d1, uint8_t d2) {
    emit(a, status, d1, d2);
}

int uw_ail_lock_channel(uw_ail *a) {
    int pass, ch, best = -1;
    uint8_t mask = 0xc0, fewest;
    for (pass = 0; pass < 2 && best < 0; pass++, mask = 0x80) {
        fewest = 0xff;
        for (ch = 8; ch >= 1; ch--) {
            if (a->flags[ch] & mask) continue;
            if (a->notes[ch] < fewest) { fewest = a->notes[ch]; best = ch; }
        }
    }
    if (best < 0) return 0;
    emit(a, (uint8_t)(0xb0 | best), 0x40, 0);
    channel_notes_off(a, best);
    a->notes[best] = 0;
    a->flags[best] |= 0x80;
    return best + 1;
}

void uw_ail_release_channel(uw_ail *a, int channel) {
    int ch = channel - 1, i;
    if (ch < 0 || ch >= UW_AIL_CHANNELS || !(a->flags[ch] & 0x80)) return;
    a->flags[ch] &= 0x7f;
    a->notes[ch] = 0;
    emit(a, (uint8_t)(0xb0 | ch), 0x40, 0);
    emit(a, (uint8_t)(0xb0 | ch), 0x7b, 0);
    for (i = 0; i < 9; i++)
        if (a->cache[i][ch] != 0xff) emit(a, (uint8_t)(0xb0 | ch), cached_controllers[i], a->cache[i][ch]);
    if (a->cache_program[ch] != 0xff) emit(a, (uint8_t)(0xc0 | ch), a->cache_program[ch], 0);
    if (a->cache_bend_lsb[ch] != 0xff && a->cache_bend_msb[ch] != 0xff)
        emit(a, (uint8_t)(0xe0 | ch), a->cache_bend_lsb[ch], a->cache_bend_msb[ch]);
}

/* ---- the event stream -------------------------------------------------- */

static uint32_t vlq(const uint8_t *d, size_t len, size_t *at) {
    uint32_t v = 0;
    while (*at < len) {
        uint8_t b = d[(*at)++];
        v = v << 7 | (b & 0x7f);
        if (!(b & 0x80)) break;
    }
    return v;
}

/* 0x3131: a note on, its duration in one of the 32 slots */
static void seq_note_on(uw_ail *a, uw_ail_sequence *s, uint8_t ch, uint8_t note, uint8_t velocity, uint32_t duration) {
    int i;
    if (a->flags[ch] & 0x80) return;
    for (i = 0; i < UW_AIL_NOTES && s->note[i].channel != 0xff; i++) { }
    if (i < UW_AIL_NOTES) s->active++;
    else i = 0;                                 /* the table full: slot 0 taken over, uncounted */
    s->note[i].channel = ch;
    s->note[i].note = note;
    s->note[i].left = (int32_t)duration - 1;
    a->notes[s->map[ch]]++;
    emit(a, (uint8_t)(0x90 | s->map[ch]), note, velocity);
}

/* 0x2f5b: a controller */
static void seq_controller(uw_ail *a, uw_ail_sequence *s, uint8_t ch, uint8_t c, uint8_t v) {
    int slot, i;
    if (s->indirect[ch] != 0xff) s->indirect[ch] = 0xff;   /* the indirect array is the game's, which UW gives none */
    slot = cache_slot(c);
    if (slot >= 0) a->cache[slot][ch] = v;
    switch (c) {
    case 0x07:
        s->volume_cache[ch] = v;
        if (s->volume != 100) {
            unsigned w = (unsigned)v * (unsigned)s->volume / 100u;
            v = (uint8_t)(w > 0x7f ? 0x7f : w);
            a->cache[0][ch] = v;
        }
        break;
    case 0x40: s->sustain[ch] = v; break;
    case 0x74:                                  /* FOR: the loop's count and where it starts */
        for (i = 0; i < UW_AIL_LOOPS && s->loop[i].count != -1; i++) { }
        if (i < UW_AIL_LOOPS) { s->loop[i].count = v; s->loop[i].at = s->pos; }
        return;
    case 0x75:                                  /* NEXT, from 0x40 up: the innermost loop again */
        if (v < 0x40) return;
        for (i = UW_AIL_LOOPS - 1; i >= 0 && s->loop[i].count == -1; i--) { }
        if (i < 0) return;
        if (s->loop[i].count && --s->loop[i].count == 0) { s->loop[i].count = -1; return; }
        s->pos = s->loop[i].at;
        return;
    case 0x76: case 0x77:                       /* the beat counters, the game's callback: none in UW */
        return;
    case 0x6f:                                  /* the score's lock protect */
        s->protect[ch] = v;
        if (v >= 0x40) a->flags[ch] |= 0x40; else a->flags[ch] &= 0xbf;
        return;
    case 0x6e:                                  /* a lock of the score's own */
        s->locked[ch] = v;
        if (v >= 0x40) {
            /* ADLIB.ADV 0x30f3: the channel got, or the score's own when
             * none is free */
            int got = uw_ail_lock_channel(a);
            s->map[ch] = (uint8_t)(got ? got - 1 : ch);
        } else {
            /* 0x3109, whatever the mapping: the channel's notes off, its
             * mapped channel released, and it maps to itself */
            channel_notes_off(a, ch);
            uw_ail_release_channel(a, s->map[ch] + 1);
            s->map[ch] = (uint8_t)ch;
        }
        return;
    case 0x70: s->voice_protect[ch] = v; break;
    case 0x73: s->indirect[ch] = v; return;
    default: break;
    }
    if (!(a->flags[ch] & 0x80)) emit(a, (uint8_t)(0xb0 | s->map[ch]), c, v);
}

/* Events from the stream's position to the next interval, whose value is
 * the new delay; false when the sequence stopped (end of track). */
static void seq_events(uw_ail *a, uw_ail_sequence *s) {
    const uint8_t *d = s->evnt;
    size_t len = s->evnt_len;
    while (s->status == 1) {
        uint8_t b, hi, ch;
        if (s->pos >= len) { seq_end(a, s); s->status = 2; return; }
        b = d[s->pos];
        if (b < 0x80) {                         /* an interval: one byte, the delay */
            s->pos++;
            s->delay = b;
            return;
        }
        hi = b & 0xf0;
        ch = b & 0x0f;
        if (hi == 0x80 || hi == 0x90) {         /* below 0xa0 the note-on path, a duration after it */
            size_t at = s->pos + 3;
            uint32_t dur;
            if (at > len) { s->pos = len; continue; }
            dur = vlq(d, len, &at);
            seq_note_on(a, s, ch, d[s->pos + 1], d[s->pos + 2], dur);
            s->pos = at;
        } else if (hi == 0xb0) {
            uint8_t c = s->pos + 1 < len ? d[s->pos + 1] : 0, v = s->pos + 2 < len ? d[s->pos + 2] : 0;
            s->pos += 3;                        /* the loop's start is the event after the FOR */
            seq_controller(a, s, ch, c, v);
        } else if (hi == 0xc0) {
            s->program[ch] = d[s->pos + 1];
            a->cache_program[ch] = d[s->pos + 1];
            if (!(a->flags[ch] & 0x80)) emit(a, (uint8_t)(0xc0 | s->map[ch]), d[s->pos + 1], 0);
            s->pos += 2;
        } else if (hi == 0xd0) {
            if (!(a->flags[ch] & 0x80)) emit(a, (uint8_t)(0xd0 | s->map[ch]), d[s->pos + 1], 0);
            s->pos += 2;
        } else if (hi == 0xe0) {
            s->bend_lsb[ch] = d[s->pos + 1];
            s->bend_msb[ch] = d[s->pos + 2];
            a->cache_bend_lsb[ch] = d[s->pos + 1];
            a->cache_bend_msb[ch] = d[s->pos + 2];
            if (!(a->flags[ch] & 0x80)) emit(a, (uint8_t)(0xe0 | s->map[ch]), d[s->pos + 1], d[s->pos + 2]);
            s->pos += 3;
        } else if (hi == 0xa0) {
            if (!(a->flags[ch] & 0x80)) emit(a, (uint8_t)(0xa0 | s->map[ch]), d[s->pos + 1], d[s->pos + 2]);
            s->pos += 3;
        } else if (b == 0xff) {                 /* a meta: 0x2f ends the sequence */
            size_t at = s->pos + 2;
            uint8_t kind = s->pos + 1 < len ? d[s->pos + 1] : 0x2f;
            uint32_t n = vlq(d, len, &at);
            s->pos = at + n;
            /* 0x3246: the channels given back and status 2 -- the notes
             * still sounding are not turned off, and with the sequence
             * done nothing counts them down; every shipped track's last
             * note has ended before its end meta */
            if (kind == 0x2f) { seq_end(a, s); s->status = 2; return; }
        } else {                                /* sysex: skipped */
            size_t at = s->pos + 1;
            uint32_t n = vlq(d, len, &at);
            s->pos = at + n;
        }
    }
}

/* The timer service for one sequence (0x33b5..0x3612) */
static void seq_tick(uw_ail *a, uw_ail_sequence *s) {
    int i;
    s->tempo_acc += s->tempo;
    while (s->tempo_acc >= 100 && s->status == 1) {
        s->tempo_acc -= 100;
        if (s->active) {
            for (i = 0; i < UW_AIL_NOTES; i++) {
                uint8_t ch = s->note[i].channel;
                if (ch == 0xff || --s->note[i].left >= 0) continue;
                s->note[i].channel = 0xff;
                a->notes[s->map[ch]]--;
                emit(a, (uint8_t)(0x80 | s->map[ch]), s->note[i].note, 0);
                if (--s->active == 0) break;
            }
        }
        if (--s->delay <= 0) seq_events(a, s);
    }
    if (s->status != 1) return;
    if (s->tempo != s->tempo_target) {       /* 0x3580: a step each period, 83 tenths of a ms a call */
        int steps = 0;
        s->tempo_time += 83;
        while (s->tempo_time >= s->tempo_period) { s->tempo_time -= s->tempo_period; steps++; }
        if (steps) {
            if (s->tempo < s->tempo_target) s->tempo = s->tempo + steps > s->tempo_target ? s->tempo_target : s->tempo + steps;
            else s->tempo = s->tempo - steps < s->tempo_target ? s->tempo_target : s->tempo - steps;
        }
    }
    if (s->volume != s->volume_target) {     /* 0x35c6 */
        int steps = 0;
        s->volume_acc += 83;
        while (s->volume_acc >= s->volume_period) { s->volume_acc -= s->volume_period; steps++; }
        if (steps) {
            if (s->volume < s->volume_target) s->volume = s->volume + steps > s->volume_target ? s->volume_target : s->volume + steps;
            else s->volume = s->volume - steps < s->volume_target ? s->volume_target : s->volume - steps;
            seq_resend_volumes(a, s);
        }
    }
}

void uw_ail_tick(uw_ail *a) {
    int h;
    a->ticks++;
    for (h = 0; h < UW_AIL_SEQUENCES; h++)
        if (a->seq[h].used && a->seq[h].status == 1) seq_tick(a, &a->seq[h]);
    if (a->synth.tick) a->synth.tick(a->synth.user);
}
