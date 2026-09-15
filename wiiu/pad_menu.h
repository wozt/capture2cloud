#pragma once

#include <drc/input.h>

#include <mutex>
#include <string>
#include <vector>

/*
 * A settings menu drawn onto the GamePad's own screen.
 *
 * Everything this changes is something you only notice while holding
 * the pad -- a stick that drifts, a picture that froze, a bar in the
 * wrong corner -- and walking back to the machine to change it is the
 * difference between a setting that gets used and one that does not.
 *
 * It is composited into the video on its way out, so it exists only on
 * the two-pass path, where this program has pixels to draw on. With
 * --single-encode the host sends chunks and there is nothing here to
 * draw into; the menu reports that rather than silently doing nothing.
 *
 * Touch does NOT reach the console. A ConsoleTuner adapter has no touch
 * input and there is nowhere sensible to send it, so the panel's own
 * menu is the one thing it drives -- which is also the only use for it
 * that cannot be done better with a stick.
 */

struct PadSettings {
    /* Sticks. The right one starts with a deadzone because this kind of
     * pad drifts there; the left starts at nothing, because applying a
     * deadzone to a stick nobody complained about silently changes how
     * a game plays. */
    float deadzone[2] = {0.00f, 0.12f};
    /*
     * Where the stick counts as fully pushed. Everything past it reads
     * as maximum, so a stick that no longer quite reaches its corner --
     * or one whose last tenth you would rather not have to find -- can
     * be made to.
     *
     * The companion to the deadzone: one trims the middle, this one
     * trims the edge, and between them the travel that is left is
     * rescaled across the whole range.
     */
    float range[2] = {1.00f, 1.00f};
    /* How far a stick has to travel before anything mapped to a trigger
     * from it reads as pressed. The pad's own ZL/ZR are switches. */
    float threshold = 0.50f;
    /* Up is positive on the wire. Measured wrong on this bench, so it is
     * a setting rather than an argument: see fill_slots(). */
    bool invert_y = true;
    /* The pad is a Nintendo one and the adapter pretends to be an Xbox,
     * whose face letters sit in different places. On by default because
     * matching letters puts every on-screen prompt on the wrong button. */
    bool swap_face = true;

    /* Recovery. Both were fixed numbers that turned out to want tuning
     * on the sofa rather than in a rebuild. */
    int  freeze_reinit_s = 3;   /* stuck this long -> tell it to restart */
    int  freeze_reauth_s = 15;  /* still stuck this long -> deauthenticate */
    /*
     * After a pad associates, before anything is pushed to it.
     *
     * In seconds, and it may not be zero. Zero was allowed for one
     * afternoon and the picture never came back: the association
     * completes well before the pad is ready to be handed a video
     * transport, and starting into that gap loses the stream with
     * nothing saying why. Half a second is the floor -- short enough
     * not to be a wait, long enough to be a gap.
     */
    float settle_s = 2.0f;

    /*
     * The picture, and the only rate control this protocol allows.
     *
     * QP is pinned at 32 and cannot move -- the pad has no slice header
     * to learn it from -- so an encoder here cannot answer a bit budget
     * the way any other would. What it CAN do is spend its bits
     * differently, and these are the three knobs that do that. All are
     * libdrc's own environment variables, applied when the encoder is
     * built.
     */
    /*
     * How many frames an intra refresh sweep takes, and the only
     * setting on this page that was measured to do anything.
     *
     * Longer means fewer intra macroblocks in each frame. Measured on
     * three seconds of this bench's own picture, through the real
     * encoder: 30 frames costs 88542 bytes, 60 costs 53080, 120 costs
     * 31443. Halving the bitrate by doubling this is not a subtlety.
     *
     * What it costs is repair time: a sweep is how loss is healed here,
     * and at 120 frames that is two seconds rather than half of one.
     *
     * Two knobs that used to sit beside this are gone, both because
     * they were measured rather than assumed:
     *
     *   Adaptive quantisation does NOTHING at a fixed quantiser. Not
     *   the strength, not the mode -- six combinations, byte-identical
     *   output. libdrc's note that drc-x264 "keeps aq_mode alive under
     *   DRH" does not survive contact with the encoder.
     *
     *   Trellis costs 2 percent MORE bits here, not fewer. The saving
     *   everyone quotes is at constant QUALITY, where it buys a higher
     *   quantiser; at a quantiser that cannot move it simply keeps more
     *   coefficients. It is off, and not offered.
     */
    int   refresh = 30;

    /*
     * How much detail to take out before encoding. The other half of
     * the same idea: fine detail -- a capture's grain, its dithering,
     * the ringing a sharp downscale adds -- is what a fixed quantiser
     * spends its bits on, and none of it survives this panel anyway.
     *
     * 0 leaves the reduction alone, which since it became an averaging
     * filter rather than a sharpening one is already most of the gain.
     * Each step scales through a slightly smaller picture first, and
     * the steps are small: 4, 8 and 12 percent, because 12 for the
     * first one was visibly soft.
     */
    int   smoothing = 0;        /* 0..3 */

    int  corner = 2;            /* 0 TL, 1 TR, 2 BL, 3 BR */
    bool show_stats = false;    /* frame rate and requests, on the panel */
};

class PadMenu {
public:
    PadMenu();

    /* Where the settings live. Beside the .env, so everything this
     * project remembers is in one directory. */
    static std::string DefaultPath();

    /* Takes the pad's input, consumes what belongs to the menu, and
     * leaves the rest -- with the stick corrections applied -- for the
     * console. Returns true while the menu is open, which is when the
     * caller should send nothing at all: a thumb aiming at a slider is
     * not aiming at a game. */
    bool Filter(drc::InputData &in);

    /* Draws onto an RGBA frame of the panel's size. Cheap when closed:
     * eight pixels in a corner, so there is something to aim at. */
    void Draw(std::vector<unsigned char> &rgba);

    /* A copy, for the loop that acts on them. */
    PadSettings Get();

    /* What the panel shows when the stats row is on. */
    void SetStats(long fps, long asking, long kbps, bool streaming);

    /*
     * True once, when the "reconnect the pad" button has been pressed.
     *
     * It used to be a "send a black keyframe" button, and that is gone
     * rather than renamed: a recovery point is an intra frame, an intra
     * frame does not fit DRH's five 1400-byte packets, and the pad
     * cannot read a split one -- so the button offered to do the thing
     * that causes the freeze. Deauthenticating is the one escape
     * measured to work, so it is the one on offer.
     */
    bool TakeReconnect();

    /* True once, when something was changed that only takes effect in a
     * new encoder -- the three picture knobs. The loop answers by
     * re-exporting them and restarting the stream. */
    bool TakeEncoderChange();

private:
    void Save();
    void Load();
    void Tap(float x, float y);
    void Correct(float &x, float &y, int stick) const;

    std::mutex mutex_;
    std::string path_;
    PadSettings s_;

    bool open_ = false;
    bool touched_ = false;      /* the previous sample had a finger down */
    int  page_ = 0;             /* 0 sticks, 1 picture, 2 recovery, 3 layout */
    bool reconnect_ = false;
    bool encoder_changed_ = false;

    /* The last raw sample, for the stick drawing. */
    float raw_lx_ = 0, raw_ly_ = 0, raw_rx_ = 0, raw_ry_ = 0;

    long fps_ = 0, asking_ = 0, kbps_ = 0;
    bool streaming_ = false;
};
