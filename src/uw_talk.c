/* SPDX-License-Identifier: MIT */
/* See uw_talk.h. */
#include "uw_talk.h"
#include "uw_motion_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TALKER         = 0x4a14,   /* far: the object talked to */
    CONV_SLOT      = 0x0ebc,   /* conv_slot_wanted */
    MENU_OPEN      = 0x4948,
    MENU_RUNNING   = 0x4954,
    MENU_ROWS      = 0x495e,   /* the option each row of the menu window shows */
    MENU_COUNT     = 0x4a2c,   /* one past the options listed */
    BARTER_RUNNING = 0x494e,
    PANEL_SAVED    = 0x4902,
    SPOKE          = 0x0ea6,   /* who spoke last: 1 the player */
    EMPTY_STRING   = 0x485a,
    STRBLOCK       = 0x3644,
    HOLD_TICKS     = 500
};

/* ---- the host callbacks ------------------------------------------------ */

static const char *import_name(const uw_talk *t, uint16_t id) {
    size_t k;
    for (k = 0; k < t->nimps; k++)
        if (t->imps[k].id == id) return t->imps[k].name;
    return "";
}

static int waits(const char *name) {
    return !strcmp(name, "babl_menu") || !strcmp(name, "babl_fmenu") || !strcmp(name, "babl_ask");
}

static int16_t calli(uw_convvm *vm, uint16_t id, uint16_t *top, int *pause) {
    uw_talk *t = vm->host.user;
    const char *name = import_name(t, id);
    int ported;
    if (waits(name)) {
        if (strcmp(name, "babl_ask")) uw_convbi_menu_prints(&t->bi, vm, top, !strcmp(name, "babl_fmenu"));
        *pause = 1;
        return 0;
    }
    return uw_convbi_call(&t->bi, vm, name, top, &ported);
}

/* SAY_OP and RESPOND_OP: the print stops where the original waits -- a
 * pause or a [MORE] -- and the machine pauses on the operator until the
 * shell's passes end the wait (uw_talk_tick); the speaker byte, which the
 * builtin marks after its print, is put back until then. */
static void say_or_respond(uw_convvm *vm, uint16_t string_id, int *pause, int respond) {
    uw_talk *t = vm->host.user;
    uw_scroll *s = t->m->scroll;
    uint8_t spoke = t->m->ds[SPOKE];
    if (s) s->stop_at_wait = 1;
    uw_convbi_say(&t->bi, vm, string_id, respond);
    if (s) s->stop_at_wait = 0;
    if (s && s->stopped) {
        *pause = 1;
        t->say_respond = respond;
        t->say_spoke = spoke;
        t->m->ds[SPOKE] = spoke;
    }
}

static void say(uw_convvm *vm, uint16_t string_id, int *pause) { say_or_respond(vm, string_id, pause, 0); }
static void respond(uw_convvm *vm, uint16_t string_id, int *pause) { say_or_respond(vm, string_id, pause, 1); }

/* A stopped say's wait as the scroll's kind names it: a pause of 200 or
 * 600 ticks from `clock`, a [MORE] until a click (0). */
static uint32_t wait_end(const uw_talk *t, uint32_t clock) {
    const uw_scroll *s = t->m->scroll;
    if (!s) return clock;
    return s->stop_kind == 1 ? clock + 200 : s->stop_kind == 2 ? clock + 600 : 0;
}

static int strcmp_ids(uw_convvm *vm, uint16_t below, uint16_t top) {
    uw_talk *t = vm->host.user;
    return uw_convbi_strcmp_ids(&t->bi, vm, below, top);
}

/* ---- opening and closing ------------------------------------------------- */

/* bglobals_create (init_babl): SAVE0's BGLOBALS.DAT from BABGLOBS.DAT,
 * every conversation's variables zero */
void uw_talk_new_globals(uw_talk *t) {
    char path[768];
    uw_babglobs bg;
    free(t->bglobals);
    t->bglobals = NULL;
    t->bglobals_size = 0;
    snprintf(path, sizeof path, "%s/DATA/BABGLOBS.DAT", t->dir);
    if (uw_babglobs_open(&bg, path)) {
        size_t cap = uw_babglobs_live_size(&bg) + 0x1000;
        t->bglobals = calloc(1, cap);
        if (t->bglobals) t->bglobals_size = uw_bglobals_create(&bg, t->bglobals, cap);
        uw_babglobs_close(&bg);
    }
}

bool uw_talk_open(uw_talk *t, uw_motion *m, const char *dir) {
    char path[768];
    memset(t, 0, sizeof *t);
    t->m = m;
    snprintf(t->dir, sizeof t->dir, "%s", dir);
    snprintf(path, sizeof path, "%s/DATA/CNV.ARK", dir);
    t->have_cnv = uw_ark_open(&t->cnv, path);
    if (!t->have_cnv) return false;
    uw_talk_new_globals(t);
    t->sc.pak = m->strings;
    t->sc.ring_seg = 0x4f4b;
    return m->strings != NULL;
}

void uw_talk_close(uw_talk *t) {
    if (t->have_cnv) uw_ark_close(&t->cnv);
    free(t->bglobals);
    t->bglobals = NULL;
}

/* The run on to its next wait, or its end. */
static void run(uw_talk *t, uint32_t clock) {
    uw_motion *m = t->m;
    uint8_t *ds = m->ds;
    int r = uw_convvm_run(&t->vm, 1000000);
    if (r < 0) {
        const char *name;
        if (t->code[t->vm.ip & 0x7fff] != 0x14) {
            /* paused on a SAY_OP or RESPOND_OP: the print's wait */
            t->wait = UW_TALK_MORE;
            t->hold_until = wait_end(t, clock);
            return;
        }
        name = import_name(t, t->code[(t->vm.ip + 1) & 0x7fff]);
        if (!strcmp(name, "babl_ask")) {
            /* scroll_text_input's field, in the menu window */
            t->wait = UW_TALK_ASK;
            if (m->scroll) {
                uw_scroll_select_conv_menu(m->scroll);
                uw_scroll_clear(m->scroll, 1);
                uw_scroll_print(m->scroll, ">");     /* the default prompt */
                uw_scroll_edit_begin(m->scroll, &t->edit, "", 1, 0x32);
            }
        } else {
            t->wait = UW_TALK_MENU;
            t->menu_options = (int16_t)rw(ds, MENU_COUNT) - 1;
        }
        return;
    }
    if (r > 0) UW_NOT_CARRIED(t->not_carried);
    {
        /* conv_start's tail: the variables written back; conv_unload's
         * bglobals_save_conv and the run-time strings emptied; then
         * scroll_wait: any input ends it, or 500 ticks when the player
         * spoke last (the speaker byte set) and nothing changed --
         * otherwise it has no timeout */
        int changed = uw_convbi_export_npc_vars(&t->bi, &t->vm, t->talker);
        if (t->bglobals)
            uw_bglobals_save_conv(t->bglobals, t->bglobals_size, t->slot, t->mem, t->vm.memslots);
        uw_strcache_reset(&t->sc, 0x7c);
        t->wait = UW_TALK_HOLD;
        t->hold_until = !changed && ds[SPOKE] ? clock + HOLD_TICKS : 0;
    }
}

bool uw_talk_begin(uw_talk *t, uint16_t obj, uint32_t clock) {
    uw_motion *m = t->m;
    uint8_t *ds = m->ds, *ls = m->lseg;
    uint16_t id = (uint16_t)(rw(ls, obj) & 0x1ff), ev = rw(ds, 0x00e2);
    uint8_t whoami;
    size_t k, n;
    if (!t->have_cnv || !m->strings) return false;
    /* conv_begin_with_object's guard: a
     * shrine chants (chant_mantra, whose field the host then answers); a
     * special tmap object whose texture -- word 3's low six bits -- has
     * terrain kind 8 in the low byte of its 0x720c word is "There is no
     * reaction from the princess." (0x110), any other says nothing; anything
     * else not a creature is 0xe00 */
    if ((id & 0x1c0) != 0x40) {
        if (id == 0x157) uw_motion_chant_mantra(m);
        else if (id == 0x16e) {
            if ((rw(ds, (uint16_t)(0x720c + 2 * (ls[(uint16_t)(obj + 6)] & 0x3f))) & 0xff) == 8) print_message(m, 0x110);
        } else print_message(m, 0xe00);
        return false;
    }
    whoami = ls[obj + 0x1a];
    /* the talker's far pointer, before the guard: a creature
     * that will not talk is still left there, as is its slot below */
    ww(ds, TALKER, obj);
    ww(ds, (uint16_t)(TALKER + 2), rw(ds, (uint16_t)(MOBILE_BASE + 2)));
    {
        /* a creature talks when its whoami is 0x16, 0x8e or 0xe7; or, its
         * whoami not 0xff, when it is an ally (+0x19 bit 6) or neither
         * attacking the player (goal 5, 6 or 9 with gtarg 1) nor hostile
         * (attitude, +0x0d bits 14..15, zero); or, whatever its whoami, when
         * its goal is 10 */
        int goal = ls[obj + 0xb] & 0xf, gtarg = (rw(ls, (uint16_t)(obj + 0xb)) >> 4) & 0xff;
        int attacking = (goal == 5 || goal == 6 || goal == 9) && gtarg == 1;
        int hostile = (rw(ls, (uint16_t)(obj + 0xd)) >> 14) == 0, ally = (ls[obj + 0x19] & 0x40) != 0;
        int talks = whoami == 0x16 || whoami == 0x8e || whoami == 0xe7
                    || (whoami != 0xff && ((!attacking && !hostile) || ally)) || goal == 10;
        if (!talks) { print_message(m, 0xe01); return false; }
    }
    t->slot = whoami ? whoami : (uint16_t)(0x100 + (id & 0x3f));
    ww(ds, CONV_SLOT, t->slot);                 /* before ark_slot_exists */
    if (!uw_conv_open(&t->conv, &t->cnv, t->slot)) { print_message(m, 0xe01); return false; }
    t->talker = obj;
    t->talker_temp = m->talk_object_temp;
    m->talk_object_temp = 0;
    /* game_change_mode(4): the dungeon left, mode 4, its index 2 */
    ww(ds, (uint16_t)(ev + 8), 4);
    ww(ds, 0x565e, 4);
    ww(ds, 0x5664, 2);
    ww(ds, PENDING_EVENTS, 0);

    /* converse_draw_screen */
    memset(&t->bi, 0, sizeof t->bi);
    t->bi.ds = ds;
    t->bi.level = m;
    t->bi.vars = t->vars;
    t->bi.strings = &t->sc;
    /* time(0)'s low word, which barter_setup reseeds rt_rand from: the
     * 256 Hz clock's seconds, an input like the player's answers (the
     * wall clock would make a scripted run differ from itself) */
    t->bi.clock_low = (uint16_t)(clock >> 8);
    t->bi.scroll = m->scroll;
    t->bi.vm = &t->vm;
    {
        char data[768];
        snprintf(data, sizeof data, "%s/DATA", t->dir);
        uw_convbi_draw_screen(&t->bi, data);
        if (m->scroll) {
            uw_scroll_select_conv_menu(m->scroll);
            uw_scroll_clear(m->scroll, 0);
            uw_scroll_select_conv_npc(m->scroll);
            uw_scroll_clear(m->scroll, 0);
        }
        uw_convbi_draw_heads(&t->bi, data);
    }
    uw_convbi_barter_setup(&t->bi);
    if (m->scroll) uw_scroll_select_scroll(m->scroll);
    uw_motion_cursor_show(m);

    /* conv_load: the code, the imports, the memory with the globals'
     * record, the string variables' slots cleared, the empty string in
     * block 0x7c */
    n = t->conv.code_words < 0x8000 ? t->conv.code_words : 0x8000;
    for (k = 0; k < n; k++) t->code[k] = uw_conv_word(&t->conv, (int)k);
    t->nimps = t->nvars = 0;
    for (k = 0; k < t->conv.nimports && k < 256; k++) {
        uw_conv_import imp;
        char name[24];
        int len;
        if (!uw_conv_import_at(&t->conv, (int)k, &imp)) break;
        len = imp.name_len < 23 ? imp.name_len : 23;
        memcpy(name, imp.name, (size_t)len);
        name[len] = '\0';
        if (imp.type == UW_CONV_TYPE_FUNCTION) {
            t->imps[t->nimps].id = imp.id;
            memcpy(t->imps[t->nimps].name, name, sizeof name);
            t->nimps++;
        } else {
            t->vars[t->nvars].addr = imp.id;
            memcpy(t->vars[t->nvars].name, name, sizeof name);
            t->nvars++;
        }
    }
    t->bi.nvars = t->nvars;
    memset(&t->vm, 0, sizeof t->vm);
    t->vm.code = (const uint8_t *)t->code;
    t->vm.code_words = n;
    t->vm.mem = t->mem;
    t->vm.mem_words = 0x10000;
    t->vm.memslots = t->conv.memslots;
    t->vm.host.calli = calli;
    t->vm.host.say = say;
    t->vm.host.respond = respond;
    t->vm.host.strcmp_ids = strcmp_ids;
    t->vm.host.user = t;
    memset(t->mem, 0, sizeof t->mem);
    if (t->bglobals) uw_bglobals_load_conv(t->bglobals, t->bglobals_size, t->slot, t->mem, t->vm.memslots);
    memset(&t->sc.slot, 0, sizeof t->sc.slot);
    t->sc.nslots = 0;
    t->sc.default_block = t->conv.strblock;
    ww(ds, STRBLOCK, t->conv.strblock);
    {
        static char empty[1] = "";
        ww(ds, EMPTY_STRING, uw_strcache_add(&t->sc, empty, 0x7c));
    }
    for (k = 0; k < t->nvars; k++)
        if (!strcmp(t->vars[k].name, "npc_name") || !strcmp(t->vars[k].name, "play_name"))
            t->mem[t->vars[k].addr] = 0;
    uw_convbi_bind_npc_vars(&t->bi, &t->vm, obj);
    uw_motion_spawn_npc_loot(m, obj);
    if (!uw_convvm_start(&t->vm)) {
        t->wait = UW_TALK_DONE;
        return true;
    }
    t->wait = UW_TALK_IDLE;
    run(t, clock);
    return true;
}

void uw_talk_choose(uw_talk *t, int n, uint32_t clock) {
    uint16_t *top;
    const char *name;
    if (t->wait != UW_TALK_MENU || n < 1 || n > t->menu_options) return;
    name = import_name(t, t->code[(t->vm.ip + 1) & 0x7fff]);
    top = &t->mem[(uint16_t)(t->vm.memslots + t->vm.sp)];
    if (!strcmp(name, "babl_menu")) {
        uint16_t id = t->mem[(uint16_t)(top[-1] + n - 1)];
        uw_convbi_menu_echo(&t->bi, &t->vm, id);
        uw_convvm_resume_calli(&t->vm, (int16_t)n);
    } else {
        /* babl_fmenu answers the string id of the choice, from the id
         * table the listing filled */
        uint16_t id = rw(t->m->ds, (uint16_t)(0x4a18 + n * 2));
        uw_convbi_menu_echo(&t->bi, &t->vm, id);
        uw_convvm_resume_calli(&t->vm, (int16_t)id);
    }
    t->wait = UW_TALK_IDLE;
    run(t, clock);
}

void uw_talk_key(uw_talk *t, int code, uint32_t clock) {
    int end;
    if (t->wait != UW_TALK_ASK || !t->m->scroll) return;
    end = uw_scroll_edit_key(t->m->scroll, &t->edit, code);
    if (end == 0x0d || end == 0x1b) uw_talk_answer(t, t->edit.text, clock);
}

void uw_talk_caret(uw_talk *t, int counts) {
    if (t->wait == UW_TALK_ASK && t->m->scroll) uw_scroll_edit_caret(t->m->scroll, &t->edit, counts);
}

void uw_talk_answer(uw_talk *t, const char *typed, uint32_t clock) {
    if (t->wait != UW_TALK_ASK) return;
    if (t->m->scroll) {
        uw_scroll_print(t->m->scroll, "\n");          /* the terminator printed */
        uw_scroll_select_scroll(t->m->scroll);
    }
    uw_convvm_resume_calli(&t->vm, (int16_t)uw_convbi_babl_ask(&t->bi, typed));
    t->wait = UW_TALK_IDLE;
    run(t, clock);
}

int uw_talk_option_at_cursor(const uw_talk *t) {
    const uint8_t *ds = t->m->ds;
    int fh = t->m->font && t->m->font_size >= 8 ? (int16_t)(t->m->font[6] | t->m->font[7] << 8) : 6, row, opt;
    if (fh <= 0) fh = 6;
    row = (0x1e - ((int16_t)rw(ds, CURSOR_Y) + 1)) / fh;
    if (row < 0 || row >= 10) return 0;
    opt = (int16_t)rw(ds, (uint16_t)(MENU_ROWS + row * 2));
    if (opt < 1 || opt >= (int16_t)rw(ds, MENU_COUNT)) return 0;
    return opt;
}

int uw_talk_tick(uw_talk *t, uint32_t clock, int click) {
    if (t->wait == UW_TALK_MENU && !t->m->ds[0x0a8e]) {
        /* conv_menu_run: menus_active cleared by a print -- scroll_wait(500,
         * 0), the ticks or a click, then the options listed again */
        if (!t->relist_at) t->relist_at = clock + 0x1f4;
        if (click || (int32_t)(clock - t->relist_at) > 0) {       /* scroll_wait's `ja`: past the end */
            t->relist_at = 0;
            uw_convbi_menu_relist(&t->bi);
        }
        return t->wait;
    }
    if (t->wait == UW_TALK_HOLD && (click || (t->hold_until && (int32_t)(clock - t->hold_until) > 0)))
        t->wait = UW_TALK_DONE;
    if (t->wait == UW_TALK_MORE && (click || (t->hold_until && (int32_t)(clock - t->hold_until) > 0))) {
        uw_scroll *s = t->m->scroll;
        int resumed = 1;
        if (s) {
            s->stop_at_wait = 1;
            resumed = uw_scroll_resume(s);
            s->stop_at_wait = 0;
        }
        if (!resumed) {
            /* stopped at the next wait of the same print */
            t->hold_until = wait_end(t, clock);
            return t->wait;
        }
        /* the print done: the builtin's mark of who spoke, the scroll
         * selected, and the machine past the operator */
        uw_convbi_spoke(&t->bi, t->say_respond);
        if (s) uw_scroll_select_scroll(s);
        uw_convvm_resume_say(&t->vm);
        t->wait = UW_TALK_IDLE;
        run(t, clock);
    }
    return t->wait;
}

void uw_talk_finish(uw_talk *t) {
    uw_motion *m = t->m;
    uint8_t *ds = m->ds;
    uint16_t ev = rw(ds, 0x00e2);
    /* converse_refresh: with the page frame claimed
     * (converse_draw_screen's) ems_frame_release and
     * end_barter */
    if (rw(ds, BARTER_RUNNING)) {
        ds[0x0a4a] = 0;                     /* ems_frame_release: ems_frame_claimed */
        uw_convbi_end_barter(&t->bi);
    }
    ds[PANEL_MODE] = ds[PANEL_SAVED];
    if (m->scroll) uw_scroll_select_scroll(m->scroll);
    /* game_change_mode(-1): back to mode 1, event 0x7ffe posted */
    ww(ds, (uint16_t)(ev + 8), 1);
    ww(ds, 0x565e, 1);
    ww(ds, 0x5664, 0);
    ww(ds, PENDING_EVENTS, (uint16_t)(rw(ds, PENDING_EVENTS) | 0x7ffe));
    if (t->talker_temp) {
        /* conv_begin_with_door's obj_free of its throwaway partner */
        uw_objpool pool;
        pool_from_ds(m, &pool);
        uw_obj_free(&pool, t->talker);
        pool_to_ds(m, &pool);
        t->talker_temp = 0;
    }
    t->wait = UW_TALK_IDLE;
}
