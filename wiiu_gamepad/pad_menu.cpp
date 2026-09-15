#include "pad_menu.h"

#include <cairo/cairo.h>
#include <drc/screen.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

constexpr int W = drc::kScreenWidth;    /* 864 */
constexpr int H = drc::kScreenHeight;   /* 480 */

/* The corner marker: small enough not to be in the way, big enough to
 * hit with a thumb on a moving picture. Eight pixels is what the sibling
 * project settled on and it has not been a complaint. */
constexpr int kMark = 8;
/* But the area that ANSWERS a tap is larger than the mark that shows
 * where it is. A thumb aiming at eight pixels misses; a thumb aiming at
 * eight pixels and hitting forty has still aimed deliberately, and
 * nothing else lives in a corner. */
constexpr int kHit = 40;

struct Row {
    float x, y, w, h;
    int   id;
};

/* Where the rows are, laid out once so the drawing and the hit testing
 * cannot disagree -- which is the usual way a menu ends up with a button
 * that does the thing above it. */
std::vector<Row> g_rows;

void reset_rows() { g_rows.clear(); }

void add_row(float x, float y, float w, float h, int id) {
    g_rows.push_back({x, y, w, h, id});
}

int row_at(float px, float py) {
    for (const Row &r : g_rows) {
        if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h) {
            return r.id;
        }
    }
    return -1;
}

/* Identifiers, grouped so a page's rows are contiguous. */
enum {
    kClose = 1,
    kPageSticks, kPagePicture, kPageRecovery, kPageLayout,

    kLeftDeadLess, kLeftDeadMore,
    kRightDeadLess, kRightDeadMore,
    kThresholdLess, kThresholdMore,
    kLeftRangeLess, kLeftRangeMore,
    kRightRangeLess, kRightRangeMore,
    kInvertY, kSwapFace,

    kReinitLess, kReinitMore,
    kReauthLess, kReauthMore,
    kSettleLess, kSettleMore,
    kReconnect,

    kRefreshLess, kRefreshMore,
    kSmoothLess, kSmoothMore,

    kCornerTL, kCornerTR, kCornerBL, kCornerBR,
    kStats,
};

}  // namespace

PadMenu::PadMenu() : path_(DefaultPath()) { Load(); }

std::string PadMenu::DefaultPath() {
    /* ~/.config, not beside the .env: this file belongs to whoever is
     * holding the pad rather than to the machine, and it holds nothing
     * secret -- unlike the .env, which is git-ignored for good reason
     * and should not grow a neighbour that wants committing. */
    if (const char *home = getenv("HOME")) {
        return std::string(home) + "/.config/capture2cloud-wiiu.conf";
    }
    return "capture2cloud-wiiu.conf";
}

void PadMenu::Load() {
    std::ifstream f(path_);
    if (!f) {
        return;
    }
    std::string key;
    double v = 0;
    while (f >> key >> v) {
        if (key == "left_deadzone")       s_.deadzone[0] = (float)v;
        else if (key == "right_deadzone") s_.deadzone[1] = (float)v;
        else if (key == "threshold")      s_.threshold = (float)v;
        else if (key == "left_range")     s_.range[0] = (float)v;
        else if (key == "right_range")    s_.range[1] = (float)v;
        else if (key == "invert_y")       s_.invert_y = v != 0;
        else if (key == "swap_face")      s_.swap_face = v != 0;
        else if (key == "reinit_s")       s_.freeze_reinit_s = (int)v;
        else if (key == "reauth_s")       s_.freeze_reauth_s = (int)v;
        else if (key == "settle_s") {
            /* Clamped on the way in, not just on the way up: a file
             * written before this had a floor -- or edited by hand --
             * must not be able to reintroduce the setting that loses
             * the stream. */
            s_.settle_s = (float)v;
            if (s_.settle_s < 0.5f) s_.settle_s = 0.5f;
            if (s_.settle_s > 20.f) s_.settle_s = 20.f;
        }
        else if (key == "refresh")        s_.refresh = (int)v;
        else if (key == "smoothing")      s_.smoothing = (int)v;
        else if (key == "corner")         s_.corner = (int)v;
        else if (key == "show_stats")     s_.show_stats = v != 0;
    }
}

void PadMenu::Save() {
    std::ofstream f(path_, std::ios::trunc);
    if (!f) {
        return;
    }
    f << "left_deadzone "  << s_.deadzone[0]      << "\n"
      << "right_deadzone " << s_.deadzone[1]      << "\n"
      << "threshold "      << s_.threshold        << "\n"
      << "left_range "     << s_.range[0]         << "\n"
      << "right_range "    << s_.range[1]         << "\n"
      << "invert_y "       << (s_.invert_y ? 1 : 0) << "\n"
      << "swap_face "      << (s_.swap_face ? 1 : 0) << "\n"
      << "reinit_s "       << s_.freeze_reinit_s  << "\n"
      << "reauth_s "       << s_.freeze_reauth_s  << "\n"
      << "settle_s "       << s_.settle_s         << "\n"
      << "refresh "        << s_.refresh          << "\n"
      << "smoothing "      << s_.smoothing        << "\n"
      << "corner "         << s_.corner           << "\n"
      << "show_stats "     << (s_.show_stats ? 1 : 0) << "\n";
}

PadSettings PadMenu::Get() {
    std::lock_guard<std::mutex> lock(mutex_);
    return s_;
}

void PadMenu::SetStats(long fps, long asking, long kbps, bool streaming) {
    std::lock_guard<std::mutex> lock(mutex_);
    fps_ = fps;
    asking_ = asking;
    kbps_ = kbps;
    streaming_ = streaming;
}

bool PadMenu::TakeEncoderChange() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool was = encoder_changed_;
    encoder_changed_ = false;
    return was;
}

bool PadMenu::TakeReconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool was = reconnect_;
    reconnect_ = false;
    return was;
}

/*
 * A radial deadzone with the travel outside it rescaled, so the stick
 * still reaches its corners.
 *
 * Clamping the low values to zero and leaving the rest alone would lose
 * the first part of the stick's travel: a game would see nothing, then
 * suddenly twelve percent.
 */
void PadMenu::Correct(float &x, float &y, int stick) const {
    const float dz = s_.deadzone[stick];
    if (dz <= 0.f && s_.range[stick] >= 1.f) {
        return;
    }
    const float m = std::sqrt(x * x + y * y);
    if (m <= dz) {
        x = y = 0.f;
        return;
    }
    /* Rescaled across what is left between the deadzone and the range,
     * so the stick still reaches its corner however much has been
     * trimmed off each end. */
    const float top = std::max(dz + 0.05f, s_.range[stick]);
    float scaled = (m - dz) / (top - dz);
    if (scaled > 1.f) scaled = 1.f;
    const float k = scaled / m;
    x *= k;
    y *= k;
}

void PadMenu::Tap(float x, float y) {
    const int id = row_at(x, y);
    const auto nudge = [](float &v, float by, float lo, float hi) {
        v = std::max(lo, std::min(hi, v + by));
    };
    const auto step = [](int &v, int by, int lo, int hi) {
        v = std::max(lo, std::min(hi, v + by));
    };

    switch (id) {
    case kClose:        open_ = false; break;
    case kPageSticks:   page_ = 0; return;
    case kPagePicture:  page_ = 1; return;
    case kPageRecovery: page_ = 2; return;
    case kPageLayout:   page_ = 3; return;

    case kLeftDeadLess:   nudge(s_.deadzone[0], -0.02f, 0.f, 0.5f); break;
    case kLeftDeadMore:   nudge(s_.deadzone[0],  0.02f, 0.f, 0.5f); break;
    case kRightDeadLess:  nudge(s_.deadzone[1], -0.02f, 0.f, 0.5f); break;
    case kRightDeadMore:  nudge(s_.deadzone[1],  0.02f, 0.f, 0.5f); break;
    case kThresholdLess:  nudge(s_.threshold,   -0.05f, 0.05f, 0.95f); break;
    case kThresholdMore:  nudge(s_.threshold,    0.05f, 0.05f, 0.95f); break;
    case kLeftRangeLess:  nudge(s_.range[0], -0.05f, 0.30f, 1.0f); break;
    case kLeftRangeMore:  nudge(s_.range[0],  0.05f, 0.30f, 1.0f); break;
    case kRightRangeLess: nudge(s_.range[1], -0.05f, 0.30f, 1.0f); break;
    case kRightRangeMore: nudge(s_.range[1],  0.05f, 0.30f, 1.0f); break;
    case kInvertY:        s_.invert_y = !s_.invert_y; break;
    case kSwapFace:       s_.swap_face = !s_.swap_face; break;

    /* One second at the bottom, not zero: a recovery that fires the
     * instant anything hiccups fires during every ordinary hiccup. */
    case kReinitLess: step(s_.freeze_reinit_s, -1, 1, 30); break;
    case kReinitMore: step(s_.freeze_reinit_s,  1, 1, 30); break;
    case kReauthLess: step(s_.freeze_reauth_s, -1, 2, 120); break;
    case kReauthMore: step(s_.freeze_reauth_s,  1, 2, 120); break;
    /* Half a second at a time, and never below half a second: the four
     * were a guess, but zero is not a shorter wait, it is a broken
     * stream. */
    case kSettleLess: nudge(s_.settle_s, -0.5f, 0.5f, 20.f); break;
    case kSettleMore: nudge(s_.settle_s,  0.5f, 0.5f, 20.f); break;
    case kReconnect:  reconnect_ = true; break;

    /* These three only exist inside an encoder, so changing one means
     * building another. The loop is told rather than doing it per tap:
     * a thumb on "+" would otherwise restart the stream five times. */
    case kRefreshLess: step(s_.refresh, -10, 10, 120); encoder_changed_ = true; break;
    case kRefreshMore: step(s_.refresh,  10, 10, 120); encoder_changed_ = true; break;
    /* No encoder rebuild: this happens before the encoder, on the way
     * in, so it takes effect on the next frame. */
    case kSmoothLess:  step(s_.smoothing, -1, 0, 3); break;
    case kSmoothMore:  step(s_.smoothing,  1, 0, 3); break;

    case kCornerTL: s_.corner = 0; break;
    case kCornerTR: s_.corner = 1; break;
    case kCornerBL: s_.corner = 2; break;
    case kCornerBR: s_.corner = 3; break;
    case kStats:    s_.show_stats = !s_.show_stats; break;
    default: return;
    }
    Save();
}

bool PadMenu::Filter(drc::InputData &in) {
    std::lock_guard<std::mutex> lock(mutex_);

    const bool down = in.ts_pressed;
    const float tx = in.ts_x, ty = in.ts_y;

    /* Acted on the press, not the release: a slider you hold should not
     * wait for you to let go, and a corner you tapped should open at
     * once. */
    if (down && !touched_) {
        if (open_) {
            Tap(tx * W, ty * H);
        } else {
            const bool right = (s_.corner & 1) != 0;
            const bool bottom = (s_.corner & 2) != 0;
            const float hx = right ? 1.f - (float)kHit / W : 0.f;
            const float hy = bottom ? 1.f - (float)kHit / H : 0.f;
            if (tx >= hx && tx <= hx + (float)kHit / W &&
                ty >= hy && ty <= hy + (float)kHit / H) {
                open_ = true;
                page_ = 0;
            }
        }
    }
    touched_ = down;

    /* The sticks are corrected whether the menu is open or not: the
     * deadzone is a property of the pad, and a value the menu is showing
     * you should be the value the console is being sent. */
    /* Kept before correcting, so the page can show both: the thumb's
     * real position and what the console is actually sent. */
    raw_lx_ = in.left_stick_x;
    raw_ly_ = in.left_stick_y;
    raw_rx_ = in.right_stick_x;
    raw_ry_ = in.right_stick_y;

    Correct(in.left_stick_x, in.left_stick_y, 0);
    Correct(in.right_stick_x, in.right_stick_y, 1);

    return open_;
}

void PadMenu::Draw(std::vector<unsigned char> &rgba) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rgba.size() < (size_t)W * H * 4) {
        return;
    }

    /* Closed: just the marker, so there is something to aim at. Drawn
     * directly rather than through Cairo, because building a surface
     * for sixty-four pixels sixty times a second is most of what this
     * would cost. */
    if (!open_) {
        const int bx = (s_.corner & 1) ? W - kMark : 0;
        const int by = (s_.corner & 2) ? H - kMark : 0;
        for (int y = by; y < by + kMark; y++) {
            unsigned char *row = rgba.data() + ((size_t)y * W + bx) * 4;
            for (int x = 0; x < kMark; x++) {
                row[x * 4 + 0] = 40;
                row[x * 4 + 1] = 90;
                row[x * 4 + 2] = 150;
                row[x * 4 + 3] = 255;
            }
        }
        if (!s_.show_stats) {
            return;
        }
    }

    cairo_surface_t *surface = cairo_image_surface_create_for_data(
        rgba.data(), CAIRO_FORMAT_RGB24, W, H, W * 4);
    cairo_t *cr = cairo_create(surface);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    const auto box = [&](float x, float y, float w, float h, double r, double g, double b) {
        cairo_set_source_rgb(cr, r, g, b);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);
    };
    const auto text = [&](float x, float y, const char *label, double size) {
        cairo_set_source_rgb(cr, .92, .95, 1);
        cairo_set_font_size(cr, size);
        cairo_move_to(cr, x, y);
        cairo_show_text(cr, label);
    };

    if (!open_ && s_.show_stats) {
        /* The one line worth having while playing: is it moving, and is
         * the pad asking for help. */
        char line[128];
        snprintf(line, sizeof(line), "%s  %ld fps  %ld kbit/s  asking %ld/s",
                 streaming_ ? "streaming" : "stalled", fps_, kbps_, asking_);
        box(8, H - 26, 330, 20, .06, .08, .11);
        text(14, H - 11, line, 13);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return;
    }

    reset_rows();

    /* The panel, dimmed rather than hidden: knowing what the picture is
     * doing while you change a setting is half of why the menu is here. */
    cairo_set_source_rgba(cr, 0, 0, 0, .78);
    cairo_rectangle(cr, 0, 0, W, H);
    cairo_fill(cr);

    text(28, 44, "Wii U GamePad", 22);

    const auto tab = [&](float x, const char *label, int id, bool on) {
        box(x, 60, 150, 30, on ? .16 : .09, on ? .22 : .11, on ? .30 : .14);
        text(x + 14, 81, label, 15);
        add_row(x, 60, 150, 30, id);
    };
    tab(28, "Sticks", kPageSticks, page_ == 0);
    tab(186, "Picture", kPagePicture, page_ == 1);
    tab(344, "Recovery", kPageRecovery, page_ == 2);
    tab(502, "Layout", kPageLayout, page_ == 3);

    box(W - 76, 28, 48, 34, .22, .10, .10);
    text(W - 62, 51, "X", 18);
    add_row(W - 76, 28, 48, 34, kClose);

    /* One shape for every adjustable value, so a row is a row. */
    float y = 120;
    /* Where a row starts. The stick page moves it right to leave room
     * for the two dials; every other page uses the whole width. */
    float rows_x = 34;
    const auto slider = [&](const char *label, const char *value, int less, int more) {
        text(rows_x, y + 22, label, 15);
        text(rows_x + 236, y + 22, value, 15);
        box(rows_x + 306, y, 48, 32, .12, .16, .22);
        text(rows_x + 324, y + 22, "-", 18);
        add_row(rows_x + 306, y, 48, 32, less);
        box(rows_x + 362, y, 48, 32, .12, .16, .22);
        text(rows_x + 378, y + 22, "+", 18);
        add_row(rows_x + 362, y, 48, 32, more);
        y += 42;
    };
    const auto toggle = [&](const char *label, bool on, int id) {
        text(rows_x, y + 22, label, 15);
        box(rows_x + 306, y, 104, 32, on ? .12 : .16, on ? .26 : .12, on ? .16 : .12);
        text(rows_x + 332, y + 22, on ? "on" : "off", 15);
        add_row(rows_x + 306, y, 104, 32, id);
        y += 42;
    };

    char buf[64];
    if (page_ == 0) {
        /*
         * The sticks, drawn rather than described.
         *
         * A deadzone in percent is a number you tune by guessing; a ring
         * with your own thumb's position inside it is one you tune by
         * pushing the stick until the dot stops jittering at rest and
         * still reaches the edge. The raw sample is shown faintly and
         * the corrected one solid, so the effect of the ring is the
         * distance between them.
         */
        const auto stick = [&](float cx, float cy, const char *label,
                               float rx, float ry, float ox, float oy,
                               float dz, float range) {
            const float R = 62;
            cairo_set_source_rgb(cr, .10, .13, .17);
            cairo_arc(cr, cx, cy, R, 0, 2 * M_PI);
            cairo_fill(cr);

            /* The deadzone itself. */
            cairo_set_source_rgb(cr, .30, .22, .12);
            cairo_arc(cr, cx, cy, R * dz, 0, 2 * M_PI);
            cairo_fill(cr);

            /* Where the stick counts as fully pushed. */
            if (range < 1.f) {
                cairo_set_source_rgb(cr, .18, .34, .26);
                cairo_set_line_width(cr, 2);
                cairo_arc(cr, cx, cy, R * range, 0, 2 * M_PI);
                cairo_stroke(cr);
            }
            cairo_set_source_rgb(cr, .25, .30, .36);
            cairo_set_line_width(cr, 1.5);
            cairo_arc(cr, cx, cy, R, 0, 2 * M_PI);
            cairo_stroke(cr);

            /* Raw, then corrected. Y is negated for the drawing only:
             * this is a screen, where down is positive, and that is a
             * rule about screens rather than about sticks. */
            cairo_set_source_rgba(cr, .55, .60, .68, .55);
            cairo_arc(cr, cx + rx * R, cy - ry * R, 5, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_set_source_rgb(cr, .35, .78, .95);
            cairo_arc(cr, cx + ox * R, cy - oy * R, 7, 0, 2 * M_PI);
            cairo_fill(cr);

            cairo_set_source_rgb(cr, .82, .87, .93);
            cairo_set_font_size(cr, 14);
            cairo_move_to(cr, cx - R, cy + R + 24);
            cairo_show_text(cr, label);
        };

        /* The two drawings own the left of the panel and the rows own
         * the right, because they were on top of each other: a dial and
         * a label competing for the same pixels is a menu you cannot
         * read while you are using it. */
        float lx = raw_lx_, ly = raw_ly_, rx = raw_rx_, ry = raw_ry_;
        float clx = lx, cly = ly, crx = rx, cry = ry;
        Correct(clx, cly, 0);
        Correct(crx, cry, 1);
        stick(112, 210, "left", lx, ly, clx, cly, s_.deadzone[0], s_.range[0]);
        stick(268, 210, "right", rx, ry, crx, cry, s_.deadzone[1], s_.range[1]);

        rows_x = 366;
        y = 112;
        snprintf(buf, sizeof(buf), "%.0f %%", s_.deadzone[0] * 100);
        slider("Left deadzone", buf, kLeftDeadLess, kLeftDeadMore);
        snprintf(buf, sizeof(buf), "%.0f %%", s_.range[0] * 100);
        slider("Left range", buf, kLeftRangeLess, kLeftRangeMore);
        snprintf(buf, sizeof(buf), "%.0f %%", s_.deadzone[1] * 100);
        slider("Right deadzone", buf, kRightDeadLess, kRightDeadMore);
        snprintf(buf, sizeof(buf), "%.0f %%", s_.range[1] * 100);
        slider("Right range", buf, kRightRangeLess, kRightRangeMore);
        snprintf(buf, sizeof(buf), "%.0f %%", s_.threshold * 100);
        slider("Trigger threshold", buf, kThresholdLess, kThresholdMore);
        toggle("Invert stick Y", s_.invert_y, kInvertY);
        toggle("Faces by position", s_.swap_face, kSwapFace);
    } else if (page_ == 1) {
        cairo_set_source_rgb(cr, .62, .70, .78);
        cairo_set_font_size(cr, 13);
        cairo_move_to(cr, 34, 112);
        cairo_show_text(cr, "The quantiser is fixed at 32, so the only way to spend fewer");
        cairo_move_to(cr, 34, 130);
        cairo_show_text(cr, "bits is to give the encoder less to encode.");

        y = 154;
        snprintf(buf, sizeof(buf), "%d frames", s_.refresh);
        slider("Refresh sweep over", buf, kRefreshLess, kRefreshMore);
        static const char *const kSmooth[] = {"off", "light", "medium", "strong"};
        slider("Smooth before encoding", kSmooth[s_.smoothing < 4 ? s_.smoothing : 0],
               kSmoothLess, kSmoothMore);

        cairo_set_source_rgb(cr, .62, .70, .78);
        cairo_set_font_size(cr, 12);
        cairo_move_to(cr, 34, y + 20);
        cairo_show_text(cr, "Doubling the sweep halves the bitrate, and doubles how long a");
        cairo_move_to(cr, 34, y + 36);
        cairo_show_text(cr, "lost picture takes to repair. Measured: 30 frames costs 88 KB");
        cairo_move_to(cr, 34, y + 52);
        cairo_show_text(cr, "over three seconds, 60 costs 53 KB, 120 costs 31 KB.");
        y += 62;

        cairo_set_source_rgb(cr, .55, .62, .70);
        cairo_set_font_size(cr, 12);
        cairo_move_to(cr, 34, y + 24);
        cairo_show_text(cr, "Changing one of these builds a new encoder and restarts the picture.");
    } else if (page_ == 2) {
        cairo_set_source_rgb(cr, .62, .70, .78);
        cairo_set_font_size(cr, 13);
        cairo_move_to(cr, 34, 114);
        cairo_show_text(cr, "When the picture stops, two things are tried in turn.");

        y = 128;
        snprintf(buf, sizeof(buf), "%d s", s_.freeze_reinit_s);
        slider("1. Restart picture after", buf, kReinitLess, kReinitMore);
        snprintf(buf, sizeof(buf), "%d s", s_.freeze_reauth_s);
        slider("2. Reconnect pad after", buf, kReauthLess, kReauthMore);
        snprintf(buf, sizeof(buf), "%.1f s", s_.settle_s);
        slider("Wait before streaming", buf, kSettleLess, kSettleMore);

        /* Below the rows, with its explanation on its own line: the two
         * used to share one, and a label over a sentence is neither. */
        y += 14;
        box(34, y, 300, 42, .12, .20, .26);
        text(56, y + 27, "Reconnect the pad now", 16);
        add_row(34, y, 300, 42, kReconnect);

        cairo_set_source_rgb(cr, .62, .70, .78);
        cairo_set_font_size(cr, 13);
        cairo_move_to(cr, 34, y + 66);
        cairo_show_text(cr, "The pad drops its association and takes it back in about 0.8s,");
        cairo_move_to(cr, 34, y + 84);
        cairo_show_text(cr, "which is the only thing measured to clear a picture that stopped.");
    } else {
        text(34, 146, "Corner for this menu", 17);
        const auto corner = [&](float x, float cy, const char *label, int id, int which) {
            const bool on = s_.corner == which;
            box(x, cy, 120, 40, on ? .12 : .09, on ? .26 : .11, on ? .34 : .14);
            text(x + 18, cy + 26, label, 15);
            add_row(x, cy, 120, 40, id);
        };
        corner(330, 120, "top left", kCornerTL, 0);
        corner(460, 120, "top right", kCornerTR, 1);
        corner(330, 170, "bottom left", kCornerBL, 2);
        corner(460, 170, "bottom right", kCornerBR, 3);
        y = 236;
        toggle("Show stats while playing", s_.show_stats, kStats);
    }

    cairo_set_source_rgb(cr, .55, .62, .70);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, 28, H - 22);
    cairo_show_text(cr,
        "Touch does not reach the console -- a ConsoleTuner adapter has no touch input.");

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}
