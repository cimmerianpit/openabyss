/* SPDX-License-Identifier: MIT */
/* THE CUTSCENE PLAYER: cutscene_process_data, the 2,969-byte
 * interpreter that is the whole of how a cutscene runs.
 *
 * The pieces it drives are ported separately: the script
 * (uw_cuts.h), the animation files and their RLE (uw_cuts.h again), the
 * palette fades (uw_screen.h) and the strings.
 *
 * THE SHAPE. A cutscene is a numbered SET of files. `.n00` is a script of
 * timed records and `.n01` upwards are animation segments played in turn;
 * nothing about a cutscene is hard-coded except its number. The
 * interpreter runs one frame at a time, and around each frame it runs the
 * script records that are due:
 *
 *     open the script, read 0x800 bytes of it
 *     save the working palette; fade it out, two steps, full screen only
 *     run the records due at frame 0
 *     for each animation file, until op6 ends the cutscene or an open fails:
 *         read the header; palette_to_dac its palette; sort the pages by
 *         their order word; the frame counter starts at 1 AGAIN
 *         for each page in that order, for each frame in the page:
 *             decode the frame into the screen page and present it
 *             run every record whose frame word is this frame counter,
 *             and the chain of records due immediately after it
 *             wait out the frame period, 0x100 / header[0x44] ticks
 *             frame counter += 1
 *
 * THE FRAME WORD IS A TIMELINE TICK, NOT AN INDEX. 96 of the 634 records
 * in UW1's scripts name a frame past the frame count of the file playing
 * at that point, because a hold (op11) or a play-to (op4) keeps the
 * counter running while the last picture stays up. A player that treats
 * the word as an index into the file desynchronises on the first hold.
 *
 * WHAT A PORT MUST DO DIFFERENTLY. The original blocks: its frame loop
 * spins on the clock, polls input, runs its palette fades and services the
 * voice inside itself. Here the host owns the loop, and calls in as often
 * as it likes -- every pass, or once with a clock far ahead -- and this
 * works through what the interpreter would have done by then IN ORDER AND
 * AT THE TICK IT WOULD HAVE DONE IT: a frame goes up when its time comes
 * and the next is timed from that, not from whenever the host happened to
 * call, or a host that looks every four ticks falls a frame behind in a
 * hundred. The fades are the interpreter's too, on the same clock (a frame
 * every UW_FADE_FRAME_TICKS), and so is the DAC they ramp (`dac`); the
 * host presents `screen` through `dac` and sounds what it is asked to.
 *
 * THE VOICE AND THE KLANG are the host's to sound; what the interpreter
 * decides about them is here. With a digital driver (begin's `digital`,
 * state+0x49 bit 5 as digital_is_available leaves it) op13 records its
 * voice at +0x43 INSTEAD of putting up its text -- a voiced cutscene has
 * no subtitles -- and after the frame the records ran on is presented, a
 * voice recorded while none is sounding starts (`voice_start`, for the
 * host to open SOUND\NN.VOC; bit 6 set). cutscene_service_audio, which the
 * original polls through every wait, clears bit 6 and the voice together
 * once playback has finished -- so a voice op13 recorded while another was
 * still sounding is dropped with it, as the introduction's 29.VOC is. An
 * op14 pause with the voice sounding (bit 7) waits for it to end and then
 * its limit in seconds more, counted from the whole seconds already
 * waited; and while a voice is recorded no key or click advances. op15 is
 * sound effect 0x11 at the centre, owed to the host in `effects`.
 *
 * NOT CARRIED, and not counted: the EMS paging, which is a DOS memory
 * strategy rather than behaviour -- this holds the whole file -- and the
 * texture cache the voice's staging pages come from.
 */
#ifndef UW_CUTPLAY_H
#define UW_CUTPLAY_H

#include "uw.h"
#include "uw_cuts.h"
#include "uw_screen.h"
#include "uw_strings.h"

#define UW_CUT_LINES   6       /* op0 wraps into at most six */
#define UW_CUT_LINE    128
#define UW_CUT_SCREEN  (320 * 200)
#define UW_CUT_MAX_PAGES 64

/* state+0x49's bits, as the handlers and the loop use them. */
#define UW_CUT_ACCEPT   0x01   /* bit 0: frames show, and a handler may start a pause */
#define UW_CUT_WAITING  0x02   /* bit 1: a key asked to skip (or op11's hold runs) */
#define UW_CUT_SEGMENT  0x04   /* bit 2: this animation file is still playing */
#define UW_CUT_RUNNING  0x08   /* bit 3: the cutscene itself is still running */
#define UW_CUT_SKIPPY   0x10   /* bit 4: ESC may abort (op12) */
#define UW_CUT_DIGITAL  0x20   /* bit 5: a digital driver is present */
#define UW_CUT_SPEAKING 0x40   /* bit 6: a voice started, its end not yet seen */
#define UW_CUT_VOICED   0x80   /* bit 7: waiting for a voice to finish (op14) */

typedef struct {
    /* ---- what the caller gave ---- */
    char       dir[256];            /* the game directory: CUTS/ and DATA/ */
    char       cuts[300];
    int        number;              /* the cutscene number; the name is octal */
    uw_strings *strings;            /* op0 and op13 resolve through this */
    int        full_screen;         /* cutscene_play: n < 0x100 */
    int        win_x, win_y, win_w, win_h;   /* the window, in the game's
                                              * bottom-up screen coordinates:
                                              * (0, 0xc7, 0x140, 0xc8) full
                                              * screen, (0x34, 0xb4, 0xac,
                                              * 0x70) for the in-game visions */
    uw_blob    font;                /* FONTBIG.SYS, which cutscene_play opens
                                     * for the duration and swaps back to
                                     * font5x6p.sys after */

    /* ---- the script ---- */
    uw_blob    script;
    size_t     at;                  /* the record cursor */

    /* ---- the animation file ---- */
    uw_anim    anim;
    int        have_anim;
    int        file_no;             /* the .nNN counter, octal in the name */
    int        cut_no;              /* op8 can change the cutscene as well */
    int        order[UW_CUT_MAX_PAGES];
    int        page, frame_in_page;

    /* ---- the picture ---- */
    uint8_t    image[UW_CUT_SCREEN];   /* the animation alone, where the delta
                                        * frames accumulate: the buffer that
                                        * rle_expand writes and gfx_blit
                                        * copies out */
    uint8_t    screen[UW_CUT_SCREEN];  /* what the host presents: the image
                                        * blitted over, then the text drawn on
                                        * top, as the original draws on the
                                        * page it flips to */
    uint8_t    palette[768];        /* six-bit, as palette_to_dac leaves it */
    uint8_t    dac[768];            /* the DAC as the interpreter drives it: the
                                     * working palette it found, faded out, and
                                     * faded in to `palette` (full screen) or
                                     * back to `game_pal` (in the view) */
    uint8_t    game_pal[768];
    /* The file's sixteen colour cycles: when it last moved, its rate --
     * the header word's (w >> 8) & 0x3f, read_anim_hdr's normalisation, 0
     * for none -- and its first and last DAC entries */
    struct { uint16_t started, rate; uint8_t first, last; } cycles[16];
    uint16_t   frame;               /* the frame counter, 1 at each file */
    uint16_t   rate;                /* header +0x44, frames a second */

    /* ---- the text ---- */
    char       line[UW_CUT_LINES][UW_CUT_LINE];
    int        lines;               /* state+0x39 */
    uint8_t    colour;              /* state+0x38 */

    /* ---- the waits, at the original's own offsets ---- */
    int        target;              /* +0x3b: op4 and op11's target frame */
    int        pause_frame;         /* +0x3d */
    int        pause_limit;         /* +0x3f, in SECONDS; 999 is forever */
    int        repeat;              /* +0x41: op7's count */
    int        fade_in, fade_out;   /* +0x45, +0x47: op10 and op9's speeds */
    unsigned   flags;               /* +0x49 */

    /* ---- the voice and the klang, the host's to sound ---- */
    int        voice;               /* +0x43: the voice op13 recorded, -1 none */
    int        voice_start;         /* a voice to open (UW_CUT_VOICE), -1 none */
    int        playing;             /* the voice sounds (voc_playback_finished false) */
    uint32_t   voice_end;           /* when it will have played out */
    int        voiced_limit;        /* an op14 pause waiting on the voice: its limit */
    int        pausing;             /* the frame up holds a pause, which a key ends */
    int        effects;             /* op15's sound effect 0x11s owed: the host clears it */

    /* ---- the fades the loop runs ---- */
    uw_palette_fade fader;
    int        fading;              /* 0, 1 in, 2 out */
    double     fade_t0;
    int        fade_done;           /* frames uploaded */
    int        closing;             /* the closing fade: the end after it */
    int        stepped;             /* a frame has been run, shown or not */
    int        over;                /* the end reported */

    /* ---- the time ---- */
    uint32_t   now;                 /* how far the interpreter's loop has got */
    uint32_t   due;                 /* when the next frame goes up; ~0 on a key or the voice */
    uint32_t   frame_start;         /* when the frame on screen was presented */
    uint32_t   advanced;            /* the clock of the last key or click,
                                     * restarted when a file opens */
    int        skipped;             /* ESC on a skippable one: over now */
    int        forever;             /* a 999 pause: only a key ends it */
    long       frames;              /* frames presented */
    long       records;             /* script records run */
    long       files;               /* animation files opened */
    long       not_carried;         /* a missing file or string */
} uw_cutscene;

/* cutscene_play(n): plays full screen for n < 0x100 and in the
 * 0xac x 0x70 box at (0x34, 0xb4) otherwise, and sets the default string
 * block to 0xc00 + n. `dir` is the GAME directory -- CUTS/ and DATA/ under
 * it. False when the script will not open. `clock` is the 256 Hz counter's
 * value at the start. */
bool uw_cutscene_begin(uw_cutscene *c, const char *dir, int number,
                       uw_strings *strings, uint32_t clock);
/* cutscene_open_file(number, arg): the script's words at 4, 6
 * and 12 -- CS400's and CS401's hold-to-frame and pause-at-frame records --
 * written as `arg` before the play. The original writes them into the file
 * on disk; the port writes the bytes it read. `arg` 0 patches nothing (the
 * plain begin). `digital` is digital_is_available(): the host can sound a
 * voice (the plain begin has none, and prints every line). `dac` is the
 * working palette the interpreter copies (NULL: black); full screen, the
 * copy is faded out over 2 before anything else. */
bool uw_cutscene_begin_patched(uw_cutscene *c, const char *dir, int number, int arg,
                               uw_strings *strings, uint32_t clock, int digital,
                               const uint8_t *dac);
void uw_cutscene_end(uw_cutscene *c);

/* What the interpreter did next, by `clock`: */
enum {
    UW_CUT_IDLE = 0,    /* nothing more by then: call again later */
    UW_CUT_FRAME,       /* a frame went up at `now`: present `screen` */
    UW_CUT_DAC,         /* a fade frame at `now`: `dac` changed */
    UW_CUT_VOICE,       /* voc_play_file(`voice_start`) at `now`: open it and
                         * answer with uw_cutscene_voice_started */
    UW_CUT_SILENT,      /* the voice played out, at `now` */
    UW_CUT_ENDED        /* over, its closing fade done; stop any voice */
};
/* The next thing, and `effects` may have grown with any of them. A call
 * with nothing due returns UW_CUT_IDLE at once; so does a pause until a
 * key, which uw_cutscene_advance ends. */
int uw_cutscene_run(uw_cutscene *c, uint32_t clock);

/* The host has opened the voice UW_CUT_VOICE asked for, and it plays for
 * `ticks` of the 256 Hz clock -- or -1: it would not open, which leaves
 * voc_playback_finished true, as voc_play_file failing does. */
void uw_cutscene_voice_started(uw_cutscene *c, long ticks);

/* ESC, when op12 has made the cutscene skippable: the file closed and the
 * interpreter left at once -- the
 * next turn ends it, repeats and pages left unplayed. */
void uw_cutscene_skip(uw_cutscene *c);

/* A key (`key` set) or a button during a frame's wait. The interpreter
 * polls input while it waits out a frame and takes it as
 * "advance": it cuts a pause short, which is how the 999 pause -- wait
 * until the player says so -- ever ends. A key always counts; a button
 * only 64 ticks after the last input taken or the file's opening. */
void uw_cutscene_advance(uw_cutscene *c, uint32_t clock, int key);


#define UW_CUT_FOREVER 999     /* op3's and op14's "until a key" limit */

#endif
