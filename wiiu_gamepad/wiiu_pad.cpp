/*
 * Capture2Cloud, on a real Wii U GamePad.
 *
 * A native client like the Switch homebrew: it connects to the same
 * port, decodes the same H.264, and sends the same twenty-one bytes of
 * controller state back. What is different is where the picture goes --
 * over the air to a GamePad, through libdrc and a Realtek adapter
 * pretending to be a Wii U.
 *
 * Almost everything surprising in here was learned the hard way in the
 * sibling project and is written up in ../WIIU_GAMEPAD_HANDOVER.md.
 * Where a line exists because of one of those, it says so rather than
 * looking like a preference.
 *
 * Two lossy passes sit between the capture card and the panel: the
 * host's encoder, and libdrc's at a quantiser the protocol pins to 32.
 * Section 9 of that document describes how to remove the first one. This
 * is the version that works today and that the better one falls back to.
 */

#include <drc/streamer.h>
#include <drc/input.h>
#include <drc/screen.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <opus/opus.h>
}

#include "../c2s_protocol.h"
#include "pad_menu.h"
#include "../gamepad_bridge.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <netdb.h>
#include <netinet/tcp.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

/* The panel, which is not any shape a capture card produces. */
constexpr int kPanelW = drc::kScreenWidth;      /* 864 */
constexpr int kPanelH = drc::kScreenHeight;     /* 480 */

/* What libdrc consumes per call: 384 stereo samples, 8 ms at 48 kHz. */
constexpr int kAudioChunkFrames = 384;

std::atomic<bool> g_stop{false};

/* ------------------------------------------------------------ network */

bool read_exactly(int fd, void *into, size_t len, bool *timed_out)
{
    uint8_t *p = static_cast<uint8_t *>(into);
    const size_t want = len;
    if (timed_out)
        *timed_out = false;
    while (len > 0) {
        const ssize_t n = recv(fd, p, len, 0);
        if (n == 0)
            return false;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            /*
             * Only before the first byte, and only then.
             *
             * A timeout that can fire part way through a message leaves
             * the stream one header out of step and everything after it
             * is noise -- worse than the silence it was meant to
             * detect. So the socket's timeout is cleared as soon as a
             * message has begun, and restored by the caller.
             */
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && len == want) {
                if (timed_out)
                    *timed_out = true;
                return false;
            }
            return false;
        }
        if (len == want) {
            struct timeval none = { 0, 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool write_all(int fd, const void *from, size_t len)
{
    const uint8_t *p = static_cast<const uint8_t *>(from);
    while (len > 0) {
        const ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

/*
 * One message at a time, because two threads send on this socket.
 *
 * The input thread sends pad state; the receive loop sends keyframe and
 * codec requests, and now a keepalive. A message is a header followed by
 * a body, so without this a header from one thread could land between
 * the header and the body of the other -- and the host, which reads a
 * length and then that many bytes, would be desynchronised from that
 * moment on. It had not bitten yet because the two threads rarely spoke
 * at once; recovery is exactly when they both do.
 */
static std::mutex g_send_mutex;

bool send_message(int fd, uint8_t type, const void *body, uint32_t len)
{
    C2sFrameHeader h;
    memset(&h, 0, sizeof(h));
    h.type = type;
    h.size = len;
    std::lock_guard<std::mutex> hold(g_send_mutex);
    if (!write_all(fd, &h, sizeof(h)))
        return false;
    return len == 0 || write_all(fd, body, len);
}

/* -------------------------------------------------------------- video */

/*
 * Decodes what the host sends and lays it out on the panel.
 *
 * The scaling happens here rather than by asking the host for 864x480,
 * and that is deliberate: `C2sShared` says the size, the frame rate and
 * the bitrate belong to every native client at once, so a pad asking for
 * its own size would drag the browser and the Switch down with it. One
 * extra resample here costs nobody else anything.
 */
class Video {
public:
    ~Video()
    {
        if (sws_) sws_freeContext(sws_);
        if (frame_) av_frame_free(&frame_);
        if (packet_) av_packet_free(&packet_);
        if (ctx_) avcodec_free_context(&ctx_);
    }

    bool Open(std::string *err)
    {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) { *err = "no H.264 decoder in this ffmpeg"; return false; }
        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) { *err = "could not allocate a decoder"; return false; }
        ctx_->thread_count = 2;
        ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        if (avcodec_open2(ctx_, codec, nullptr) < 0) {
            *err = "could not open the decoder";
            return false;
        }
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        return frame_ && packet_;
    }

    /*
     * Rebuilt rather than told the new size.
     *
     * Writing the number down and carrying on is what made the Switch
     * client freeze whenever the host changed shape: the request left,
     * the frames arrived, and nothing decoded.
     */
    void Reset(std::string *err)
    {
        if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
        src_w_ = src_h_ = 0;
        if (ctx_) avcodec_free_context(&ctx_);
        ctx_ = nullptr;
        Open(err);
    }

    void SetSmoothing(int level) { smoothing_ = level; }

    bool Decode(const uint8_t *data, size_t len, std::vector<drc::byte> *rgba)
    {
        if (av_new_packet(packet_, static_cast<int>(len)) < 0)
            return false;
        memcpy(packet_->data, data, len);
        const int sent = avcodec_send_packet(ctx_, packet_);
        av_packet_unref(packet_);
        if (sent < 0)
            return false;
        if (avcodec_receive_frame(ctx_, frame_) < 0)
            return false;

        /* built_smoothing_ too: the smoothing stage is built here, so a
         * level changed from the menu takes effect only if this notices
         * it. Without it the setting moves and the picture does not. */
        if (frame_->width != src_w_ || frame_->height != src_h_ || !sws_ ||
            built_smoothing_ != smoothing_) {
            built_smoothing_ = smoothing_;
            if (sws_) sws_freeContext(sws_);
            src_w_ = frame_->width;
            src_h_ = frame_->height;

            /*
             * The largest rectangle of the source's shape that fits the
             * panel, centred, with the rest left black.
             *
             * 16:9 into 864x480 is 848x480 and eight pixels of black
             * each side. Even numbers because the conversion is through
             * 4:2:0 and an odd dimension has half a chroma sample at the
             * edge.
             */
            int w = kPanelW, h = kPanelH;
            if (static_cast<long long>(src_w_) * kPanelH !=
                static_cast<long long>(src_h_) * kPanelW) {
                if (static_cast<long long>(src_w_) * kPanelH >
                    static_cast<long long>(src_h_) * kPanelW) {
                    w = kPanelW;
                    h = static_cast<int>(
                        static_cast<long long>(kPanelW) * src_h_ / src_w_);
                } else {
                    h = kPanelH;
                    w = static_cast<int>(
                        static_cast<long long>(kPanelH) * src_w_ / src_h_);
                }
                w &= ~1;
                h &= ~1;
            }
            fit_w_ = w;
            fit_h_ = h;
            fit_x_ = (kPanelW - w) / 2 & ~1;
            fit_y_ = (kPanelH - h) / 2 & ~1;

            /*
             * Scaled through a smaller picture first, when asked.
             *
             * Averaging down and back up is a low-pass with a cutoff
             * the caller chooses: the detail that does not survive the
             * smaller size is gone, and it is exactly the detail a
             * quantiser pinned at 32 would otherwise pay full price
             * for. Two scales of an already-small frame, which costs
             * far less than the bits it saves.
             */
            if (soft_) { sws_freeContext(soft_); soft_ = nullptr; }
            if (soft_up_) { sws_freeContext(soft_up_); soft_up_ = nullptr; }
            if (smoothing_ > 0) {
                soft_w_ = (fit_w_ * (100 - 4 * smoothing_) / 100) & ~1;
                soft_h_ = (fit_h_ * (100 - 4 * smoothing_) / 100) & ~1;
                if (soft_w_ < 16) soft_w_ = 16;
                if (soft_h_ < 16) soft_h_ = 16;
                soft_ = sws_getContext(src_w_, src_h_,
                                       static_cast<AVPixelFormat>(frame_->format),
                                       soft_w_, soft_h_, AV_PIX_FMT_RGBA,
                                       SWS_AREA, nullptr, nullptr, nullptr);
                soft_up_ = sws_getContext(soft_w_, soft_h_, AV_PIX_FMT_RGBA,
                                          fit_w_, fit_h_, AV_PIX_FMT_RGBA,
                                          SWS_BILINEAR, nullptr, nullptr, nullptr);
                soft_buf_.assign(static_cast<size_t>(soft_w_) * soft_h_ * 4, 0);
                fprintf(stderr, "smoothing through %dx%d\n", soft_w_, soft_h_);
            }

            sws_ = sws_getContext(src_w_, src_h_,
                                  static_cast<AVPixelFormat>(frame_->format),
                                  fit_w_, fit_h_, AV_PIX_FMT_RGBA,
                                  /*
                                   * SWS_AREA, not SWS_LANCZOS.
                                   *
                                   * Lanczos is a sharpening filter: it
                                   * rings, and ringing is high-frequency
                                   * detail that a quantiser pinned at 32
                                   * pays full price for. It was making
                                   * the picture harder to encode than
                                   * the picture actually is. AREA
                                   * averages every source pixel landing
                                   * in a destination one -- the correct
                                   * filter for a reduction this large,
                                   * cheaper, and a genuine low-pass.
                                   */
                                  SWS_AREA, nullptr, nullptr, nullptr);
            if (!sws_)
                return false;
            fprintf(stderr, "%dx%d drawn as %dx%d at %d,%d\n",
                    src_w_, src_h_, fit_w_, fit_h_, fit_x_, fit_y_);
        }

        /* Cleared first, so the bars stay black when the shape changes. */
        rgba->assign(static_cast<size_t>(kPanelW) * kPanelH * 4, 0);
        uint8_t *base = reinterpret_cast<uint8_t *>(rgba->data()) +
                        (static_cast<size_t>(fit_y_) * kPanelW + fit_x_) * 4;
        uint8_t *dst[4] = { base, nullptr, nullptr, nullptr };
        int dst_stride[4] = { kPanelW * 4, 0, 0, 0 };
        if (soft_ && soft_up_) {
            uint8_t *mid[4] = { soft_buf_.data(), nullptr, nullptr, nullptr };
            int mid_stride[4] = { soft_w_ * 4, 0, 0, 0 };
            sws_scale(soft_, frame_->data, frame_->linesize, 0, src_h_,
                      mid, mid_stride);
            const uint8_t *mid_src[4] = { soft_buf_.data(), nullptr, nullptr, nullptr };
            sws_scale(soft_up_, mid_src, mid_stride, 0, soft_h_, dst, dst_stride);
        } else {
            sws_scale(sws_, frame_->data, frame_->linesize, 0, src_h_,
                      dst, dst_stride);
        }
        return true;
    }

    /* Where the picture sits, for anything that has to aim at it. */
    int fit_x() const { return fit_x_; }
    int fit_y() const { return fit_y_; }
    int fit_w() const { return fit_w_; }
    int fit_h() const { return fit_h_; }

private:
    AVCodecContext *ctx_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVPacket *packet_ = nullptr;
    SwsContext *sws_ = nullptr;
    SwsContext *soft_ = nullptr, *soft_up_ = nullptr;
    std::vector<uint8_t> soft_buf_;
    int soft_w_ = 0, soft_h_ = 0;
    int smoothing_ = 0, built_smoothing_ = -1;
    int src_w_ = 0, src_h_ = 0;
    int fit_x_ = 0, fit_y_ = 0, fit_w_ = kPanelW, fit_h_ = kPanelH;
};

/* -------------------------------------------------------------- audio */

class Audio {
public:
    ~Audio() { if (dec_) opus_decoder_destroy(dec_); }

    bool Start(int channels, std::string *err)
    {
        int e = 0;
        channels_ = channels > 0 ? channels : 2;
        dec_ = opus_decoder_create(48000, channels_, &e);
        if (e != OPUS_OK) { *err = opus_strerror(e); return false; }
        return true;
    }

    /*
     * Paced at the rate libdrc consumes, in the chunks it wants.
     *
     * Handing it an unbounded amount on every packet fills its queue
     * with samples that are already old, and the sound then runs
     * steadily further behind the picture with nothing to catch it up.
     */
    void Decode(const uint8_t *data, size_t len, drc::Streamer *out)
    {
        if (!dec_ || !out)
            return;
        int16_t pcm[5760 * 2];
        const int frames = opus_decode(dec_, data, static_cast<opus_int32>(len),
                                       pcm, 5760, 0);
        if (frames <= 0)
            return;

        for (int i = 0; i < frames; i++) {
            if (channels_ == 1) {
                /* Untested: nothing here sends mono. */
                pending_.push_back(pcm[i]);
                pending_.push_back(pcm[i]);
            } else {
                pending_.push_back(pcm[i * 2]);
                pending_.push_back(pcm[i * 2 + 1]);
            }
        }
        while (pending_.size() >= kAudioChunkFrames * 2) {
            std::vector<int16_t> chunk(pending_.begin(),
                                       pending_.begin() + kAudioChunkFrames * 2);
            pending_.erase(pending_.begin(),
                           pending_.begin() + kAudioChunkFrames * 2);
            out->PushAudSamples(chunk);
        }
    }

private:
    OpusDecoder *dec_ = nullptr;
    int channels_ = 2;
    std::vector<int16_t> pending_;
};

/* -------------------------------------------------------------- input */

/*
 * The pad's state as Capture2Cloud's twenty-one slots.
 *
 * Sticks are NOT negated. libdrc already reports them the way this
 * protocol wants -- up is positive on both sides, which is what
 * `gamepad_bridge.h` says about GAMEPAD_XB360_LY and RY. This was got
 * wrong three separate times in the sibling project by reasoning from
 * the screen's Y axis growing downward. That is a rule about screens,
 * not about sticks.
 */
int8_t to_slot(float v)
{
    if (!std::isfinite(v)) return 0;
    int n = static_cast<int>(v * 100.0f);
    if (n > 100) n = 100;
    if (n < -100) n = -100;
    return static_cast<int8_t>(n);
}

void fill_slots(const drc::InputData &in, int8_t *slots, const PadSettings &set)
{
    memset(slots, 0, C2S_PAD_SLOTS);
    const auto down = [&](drc::InputData::ButtonMask m) {
        return (in.buttons & m) != 0;
    };

    /*
     * Mapped by POSITION, not by letter, and the two are not the same
     * thing.
     *
     * This pad is a Nintendo one and the adapter pretends to be an Xbox
     * 360, whose face buttons carry the same four letters in different
     * places: Nintendo's A is on the right where Xbox's B is, and
     * Nintendo's X is on top where Xbox's Y is. Matching the letters
     * puts every prompt in every game on the wrong button -- which is
     * exactly what it did.
     *
     * So the thumb decides: the button on the right sends the button on
     * the right.
     */
    const bool swap = set.swap_face;
    slots[GAMEPAD_XB360_A]     = down(swap ? drc::InputData::kBtnB : drc::InputData::kBtnA) ? 100 : 0;
    slots[GAMEPAD_XB360_B]     = down(swap ? drc::InputData::kBtnA : drc::InputData::kBtnB) ? 100 : 0;
    slots[GAMEPAD_XB360_X]     = down(swap ? drc::InputData::kBtnY : drc::InputData::kBtnX) ? 100 : 0;
    slots[GAMEPAD_XB360_Y]     = down(swap ? drc::InputData::kBtnX : drc::InputData::kBtnY) ? 100 : 0;
    slots[GAMEPAD_XB360_UP]    = down(drc::InputData::kBtnUp)    ? 100 : 0;
    slots[GAMEPAD_XB360_DOWN]  = down(drc::InputData::kBtnDown)  ? 100 : 0;
    slots[GAMEPAD_XB360_LEFT]  = down(drc::InputData::kBtnLeft)  ? 100 : 0;
    slots[GAMEPAD_XB360_RIGHT] = down(drc::InputData::kBtnRight) ? 100 : 0;
    slots[GAMEPAD_XB360_LB]    = down(drc::InputData::kBtnL)     ? 100 : 0;
    slots[GAMEPAD_XB360_RB]    = down(drc::InputData::kBtnR)     ? 100 : 0;
    /* The pad's ZL/ZR are switches, not travel: the triggers can only
     * be all the way in or all the way out. */
    slots[GAMEPAD_XB360_LT]    = down(drc::InputData::kBtnZL)    ? 100 : 0;
    slots[GAMEPAD_XB360_RT]    = down(drc::InputData::kBtnZR)    ? 100 : 0;
    slots[GAMEPAD_XB360_LS]    = down(drc::InputData::kBtnL3)    ? 100 : 0;
    slots[GAMEPAD_XB360_RS]    = down(drc::InputData::kBtnR3)    ? 100 : 0;
    slots[GAMEPAD_XB360_START] = down(drc::InputData::kBtnPlus)  ? 100 : 0;
    slots[GAMEPAD_XB360_BACK]  = down(drc::InputData::kBtnMinus) ? 100 : 0;
    slots[GAMEPAD_XB360_GUIDE] = down(drc::InputData::kBtnHome)  ? 100 : 0;

    /*
     * The vertical axes ARE negated here, and that contradicts the
     * handover.
     *
     * Section 4 of WIIU_GAMEPAD_HANDOVER.md says not to: libdrc reports
     * a stick with up positive, gamepad_bridge.h wants up positive, so
     * a negation would be someone reasoning from a screen's Y axis
     * rather than from a stick. It says this was got wrong three times
     * in three clients.
     *
     * On this pad, on this bench, pushing up moved the character down
     * until this was added. Both sticks. The note is a good warning and
     * it loses to the hardware; leaving it un-negated to respect a
     * document would be preferring a claim to a measurement. Whoever
     * reads this next: check before you "fix" it back, and if it is
     * inverted for you, this line is why.
     */
    const float sign = set.invert_y ? -1.f : 1.f;
    slots[GAMEPAD_XB360_LX] = to_slot(in.left_stick_x);
    slots[GAMEPAD_XB360_LY] = to_slot(sign * in.left_stick_y);
    slots[GAMEPAD_XB360_RX] = to_slot(in.right_stick_x);
    slots[GAMEPAD_XB360_RY] = to_slot(sign * in.right_stick_y);
}

void input_loop(drc::Streamer *streamer, int fd, PadMenu *menu)
{
    int8_t slots[C2S_PAD_SLOTS];
    int8_t last[C2S_PAD_SLOTS];
    memset(last, 0, sizeof(last));
    bool ever = false;
    auto last_sent = std::chrono::steady_clock::now();

    while (!g_stop) {
        drc::InputData in;
        streamer->PollInput(&in);
        if (in.valid) {
            /* The menu takes what belongs to it and corrects the sticks.
             * While it is open nothing goes to the console at all: a
             * thumb aiming at a slider is not aiming at a game. */
            const bool menu_open = menu->Filter(in);
            fill_slots(in, slots, menu->Get());
            if (menu_open) {
                memset(slots, 0, sizeof(slots));
            }
            /*
             * Only when something moved. The host applies a state, not
             * a change, so repeating an identical one says nothing and
             * costs a packet 200 times a second.
             */
            if (!ever || memcmp(slots, last, sizeof(slots)) != 0) {
                ever = true;
                memcpy(last, slots, sizeof(slots));
                if (!send_message(fd, C2S_MSG_INPUT, slots, sizeof(slots)))
                    return;
                last_sent = std::chrono::steady_clock::now();
            }
        }

        /*
         * Something to say when the pad has nothing to say.
         *
         * The host drops a native client that has sent nothing for ten
         * seconds, and the only thing this client sends unprompted is
         * the block above -- which sends only when something MOVED. So
         * a pad resting on the table for ten seconds was killing the
         * session, and a pad away being reauthenticated was killing it
         * every single time: in the log, every "still stuck" was
         * followed by "silent too long", twelve whole session restarts
         * in twelve minutes.
         *
         * The host has learnt to ask before reaping, but it is the
         * client that goes quiet, so the client is where this belongs:
         * it costs five bytes every three seconds and it does not
         * depend on the host being new enough to probe.
         */
        const auto now = std::chrono::steady_clock::now();
        if (now - last_sent >= std::chrono::seconds(3)) {
            if (!send_message(fd, C2S_MSG_PING, nullptr, 0))
                return;
            last_sent = now;
        }
        /* 200 Hz, comfortably above the pad's own 60. What PollInput
         * actually updates at was never measured. */
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

/* ---------------------------------------------------- the radio's turn */

/*
 * A pad that is already associated will not accept a new video
 * transport. It has to be deauthenticated and allowed back.
 *
 * The streamer is started BEFORE this runs. Starting it afterwards
 * leaves audio silent -- measured twice, and not negotiable.
 */
/* Defined below, beside the interface detection it belongs with. */
std::string find_hostapd_cli();

class PadLink {
public:
    PadLink(const char *cli, const char *iface)
        : cli_(cli ? cli : ""), iface_(iface ? iface : "") {}

    /* Whether a pad is on the access point at all. */
    bool HasStation() { return !FirstStation().empty(); }

    std::string Station() { return FirstStation(); }

    /*
     * Waits until one is, or until asked to stop.
     *
     * This is the whole shape of the program now: the pad's own
     * association is what starts a session, so nothing is encoded,
     * sent, or even connected to the host until somebody turns a pad
     * on. A host that streams to nobody is an encoder running for an
     * empty room, and on this path it is also a radio talking to
     * itself.
     */
    std::string WaitForStation()
    {
        bool said = false;
        while (!g_stop) {
            std::string mac = FirstStation();
            if (!mac.empty()) {
                return mac;
            }
            if (!said) {
                fprintf(stderr, "access point up, waiting for a pad...\n");
                said = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return "";
    }

    /* Kicks one off so it comes back fresh. A pad that was already
     * associated before this program started will not accept a new
     * video transport; letting it reassociate is what makes it take
     * one. */
    void Kick(const std::string &mac)
    {
        if (mac.empty()) {
            return;
        }
        fprintf(stderr, "a pad was already associated; asking it to come back\n");
        Run("deauthenticate " + mac);
    }

    bool Cycle(int wait_seconds)
    {
        std::string mac = FirstStation();
        if (mac.empty()) {
            /*
             * Two very different faults used to share one message, and
             * the wrong one was believed: hostapd_cli is not installed
             * by the fork that builds it, so every call went to a
             * program that was not on the PATH, popen() returned
             * nothing, and that reads exactly like a pad which is not
             * there. Say which file was actually tried.
             */
            fprintf(stderr, "no station on the access point. asked %s -- if that "
                            "is the wrong hostapd_cli, pass --hostapd-cli or set "
                            "WIIU_HOSTAPD_CLI\n",
                    cli_.empty() ? find_hostapd_cli().c_str() : cli_.c_str());
            return false;
        }
        fprintf(stderr, "deauthenticating %s\n", mac.c_str());
        const long long before = ConnectedTime(mac);
        Run("deauthenticate " + mac);

        /*
         * Watched through connected_time resetting to zero, not through
         * the station disappearing: it comes back in under half a
         * second and the gap is usually missed entirely.
         */
        for (int i = 0; i < wait_seconds * 5; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const long long now = ConnectedTime(mac);
            if (now >= 0 && now < before) {
                fprintf(stderr, "pad reauthorized after %.1fs\n", (i + 1) * .2);
                return true;
            }
        }
        fprintf(stderr, "the pad did not come back\n");
        return false;
    }

private:
    std::string Run(const std::string &command) const
    {
        std::string cli = cli_;
        if (cli.empty()) {
            cli = find_hostapd_cli();
        }
        std::string line = cli + " -p /var/run/hostapd";
        if (!iface_.empty())
            line += " -i " + iface_;
        line += " " + command + " 2>/dev/null";

        FILE *pipe = popen(line.c_str(), "r");
        if (!pipe)
            return "";
        std::string out;
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe))
            out += buf;
        pclose(pipe);
        return out;
    }

    std::string FirstStation() const
    {
        /* list_sta, not all_sta: it answers with the MAC addresses and
         * nothing else, where all_sta follows each with a dozen lines
         * of detail nobody here reads. Less output is less that can be
         * misread, and this runs once a second for hours. */
        const std::string all = Run("list_sta");
        const size_t end = all.find('\n');
        std::string first = all.substr(0, end == std::string::npos ? 0 : end);
        return first.size() == 17 ? first : std::string();
    }

    long long ConnectedTime(const std::string &mac) const
    {
        const std::string info = Run("sta " + mac);
        const size_t at = info.find("connected_time=");
        if (at == std::string::npos)
            return -1;
        return atoll(info.c_str() + at + strlen("connected_time="));
    }

    std::string cli_, iface_;
};

/*
 * Where the forked hostapd_cli is.
 *
 * Not "hostapd_cli" from the PATH, which is how this failed silently
 * for a whole session: the fork is built in its own tree and never
 * installed, and every call went to a program that was not there.
 * popen() reports that as empty output, which reads exactly like a pad
 * that is not associated -- so the client blamed the radio for a
 * missing file.
 *
 * WIIU_HOSTAPD_CLI in the environment wins, then the usual place the
 * fork is built, then the PATH for a machine that did install it.
 */
std::string find_hostapd_cli()
{
    if (const char *env = getenv("WIIU_HOSTAPD_CLI"))
        if (*env) return env;
    static const char *const candidates[] = {
        "/home/wozt/rtw88_TSF/drc-hostap/hostapd/hostapd_cli",
        "/usr/local/bin/hostapd_cli",
    };
    for (const char *c : candidates)
        if (access(c, X_OK) == 0) return c;
    return "hostapd_cli";
}

std::string detect_interface()
{
    /* One socket in hostapd's control directory means no ambiguity. */
    FILE *pipe = popen("ls /var/run/hostapd 2>/dev/null", "r");
    if (!pipe)
        return "";
    std::string only;
    char buf[128];
    int count = 0;
    while (fgets(buf, sizeof(buf), pipe)) {
        count++;
        only = buf;
        while (!only.empty() && (only.back() == '\n' || only.back() == ' '))
            only.pop_back();
    }
    pclose(pipe);
    return count == 1 ? only : std::string();
}

}  // namespace

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    const char *token = getenv("C2S_TOKEN");
    const char *iface = nullptr;
    const char *cli = nullptr;
    uint16_t port = C2S_DRC_PORT;
    bool no_pad = false;
    bool single_encode = false;
    bool deauth = true;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : nullptr;
        if (!strcmp(a, "--host") && next)            { host = next; i++; }
        else if (!strcmp(a, "--port") && next)       { port = (uint16_t)atoi(next); i++; }
        else if (!strcmp(a, "--token") && next)      { token = next; i++; }
        else if (!strcmp(a, "--iface") && next)      { iface = next; i++; }
        else if (!strcmp(a, "--hostapd-cli") && next){ cli = next; i++; }
        else if (!strcmp(a, "--no-pad"))             { no_pad = true; }
        else if (!strcmp(a, "--single-encode"))      { single_encode = true; }
        else if (!strcmp(a, "--no-deauth"))          { deauth = false; }
        else if (!strcmp(a, "--stats"))              { setenv("DRC_STATS", "1", 1); }
        else if (!strcmp(a, "--preset") && next)     { setenv("DRC_PRESET", next, 1); i++; }
        else {
            fprintf(stderr,
                "wiiu_pad -- Capture2Cloud on a real Wii U GamePad\n"
                "\n"
                "  --host ADDR      where the host is (default 127.0.0.1)\n"
                "  --port N         the gamepad's own port (default %d)\n"
                "  --token T        session token, or C2S_TOKEN\n"
                "  --no-pad         decode and scale, send nothing to a pad\n"
                "  --single-encode  take the host's drc-x264 chunks instead of\n"
                "                   decoding and encoding again (see the source)\n"
                "  --no-deauth      do not drop the pad's association first\n"
                "  --iface NAME     the access point's interface\n"
                "  --hostapd-cli P  where hostapd_cli is\n"
                "  --preset P       libdrc's x264 preset (default fast)\n"
                "  --stats          libdrc's own counters\n"
                "\n"
                "Everything it needs from the radio is in\n"
                "../WIIU_GAMEPAD_HANDOVER.md.\n",
                C2S_DRC_PORT);
            return a[0] == '-' && a[1] == '-' ? 1 : 1;
        }
    }

    /*
     * `fast`, not libdrc's own suggestion of medium.
     *
     * Measured against a running game: medium produces 60 keyframe
     * requests a second -- the picture is about to freeze -- and fast
     * produces none. libdrc's note was measured on a flat test pattern.
     * The 0 leaves anything exported by hand winning.
     */
    setenv("DRC_PRESET", "fast", 0);

    /*
     * The three picture knobs, exported before libdrc builds anything:
     * it reads them once, when it creates an encoder.
     *
     * They are the only rate control this protocol leaves. QP is pinned
     * at 32 -- the pad has no slice header to learn it from -- so the
     * encoder cannot answer a bit budget by quantising harder. It can
     * only spend the bits it does produce differently, and mb_qp_delta
     * IS transmitted, which is what makes adaptive quantisation work
     * here at all. drc-x264 keeps it alive under DRH for exactly this.
     *
     * setenv with overwrite=1, unlike the preset above: these come from
     * the pad's own menu, which is a deliberate choice made while
     * looking at the picture, and it should win over a stale export.
     */
    const auto export_encoder_settings = [](const PadSettings &set) {
        char v[32];
        snprintf(v, sizeof(v), "%d", set.refresh);
        setenv("DRC_REFRESH", v, 1);
        /* Off, measured: at a quantiser that cannot move, trellis keeps
         * more coefficients and costs 2 percent more bits for 0.0005 of
         * SSIM. The saving it is famous for is at constant quality. */
        setenv("DRC_TRELLIS", "0", 1);
    };

    /*
     * libdrc's own recovery is left ON, which is the opposite of what
     * this file said for one afternoon.
     *
     * Turning it off was reasoned: the pad asks, libdrc restarts its
     * encoder, the intra frame does not fit DRH's packets, the pad
     * cannot read it and asks again. All true -- and with it off the
     * stream did not recover at all, restarting the whole session six
     * times in fifty seconds. Stock libdrc is the only configuration
     * measured to hold 0 resync and exactly 5 packets an image, so it
     * is the one that ships. DRC_RESYNC_RESTART=0 still turns it off
     * for whoever wants to try again.
     */

    /*
     * --- wait for a pad -----------------------------------------------
     *
     * Nothing happens before one is there. Not the connection to the
     * host, not libdrc, not a single encoded frame -- the host's fourth
     * chain is fed only while a client holds that stream, so staying
     * away IS switching the encoder off.
     *
     * A pad already associated when this starts is kicked once rather
     * than streamed to: it would not accept a new video transport, and
     * the failure looks exactly like a pad that is simply dark.
     *
     * Then four seconds. The association completes well before the pad
     * is ready to be given a transport, and starting into that gap is
     * the difference between a picture and a black screen.
     */
    /*
     * Which interface, before anything asks hostapd anything.
     *
     * This used to happen further down, with the rest of the pad setup,
     * and the wait below therefore ran hostapd_cli with no -i. That is
     * not an error: it selects one and says so, on the first line --
     * "Selected interface '...'" where a station's MAC was expected. So
     * the wait saw no pad, for ever, with one sitting right there.
     */
    std::string detected;
    if (!iface) {
        detected = detect_interface();
        if (!detected.empty())
            iface = detected.c_str();
    }

    /* Read before anything uses them, and again whenever the menu has
     * changed one: everything below that used to be a constant is a row
     * on the pad's own screen now. */
    PadMenu menu;
    PadSettings set = menu.Get();
    export_encoder_settings(set);

    const float kSettleSeconds = set.settle_s;
    if (!no_pad) {
        PadLink radio(cli, iface);
        /*
         * A pad that is already associated is served as it is, not
         * kicked first.
         *
         * Kicking it was a mistake that cost a bench session: the pad
         * was deauthenticated, went to sleep rather than coming back,
         * and the wait below then sat there for ever with the radio
         * perfectly healthy. The old reason for it -- that a pad will
         * not accept a NEW video transport while associated -- belonged
         * to the old order, where the streamer was already running
         * before the pad was asked to return. Nothing is running here
         * until a pad is seen, so there is nothing for it to refuse.
         */
        const std::string mac = radio.WaitForStation();
        if (mac.empty()) {
            return 0;               /* asked to stop while waiting */
        }
        fprintf(stderr, "pad %s associated; settling for %.1fs\n",
                mac.c_str(), kSettleSeconds);
        const int settle_ticks = (int)(kSettleSeconds * 10.f);
        for (int i = 0; i < settle_ticks && !g_stop; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (g_stop) {
            return 0;
        }
        if (!radio.HasStation()) {
            fprintf(stderr, "the pad went away while settling; starting over\n");
            return 0;
        }
    }

    /* --- connect ------------------------------------------------------ */
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *found = nullptr;
    if (getaddrinfo(host, port_text, &hints, &found) != 0 || !found) {
        fprintf(stderr, "cannot resolve %s\n", host);
        return 1;
    }
    const int fd = socket(found->ai_family, found->ai_socktype, 0);
    if (fd < 0 || connect(fd, found->ai_addr, found->ai_addrlen) != 0) {
        fprintf(stderr, "cannot reach %s:%u -- is the host running?\n",
                host, (unsigned)port);
        freeaddrinfo(found);
        return 1;
    }
    freeaddrinfo(found);
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    C2sHello hello{};
    hello.magic = C2S_MAGIC;
    hello.version = C2S_VERSION;
    const size_t token_len = token ? strnlen(token, C2S_MAX_TOKEN_LEN) : 0;
    hello.token_len = static_cast<uint8_t>(token_len);
    if (!write_all(fd, &hello, sizeof(hello)) ||
        (token_len && !write_all(fd, token, token_len))) {
        fprintf(stderr, "the host closed the connection during the handshake\n");
        return 1;
    }

    C2sHelloAck ack{};
    if (!read_exactly(fd, &ack, sizeof(ack), nullptr) || !ack.accepted) {
        fprintf(stderr, "the host refused the connection -- wrong token?\n");
        return 1;
    }
    fprintf(stderr, "connected: %ux%u, audio %u Hz x%u\n",
            ack.width, ack.height, ack.audio_rate, ack.audio_channels);
    if (!ack.may_control) {
        /*
         * Said out loud. Without the password the stream plays and every
         * button is dropped, which looks exactly like broken input.
         */
        fprintf(stderr, "watching only: no token, so buttons will be ignored\n");
    }

    /* H.264, whatever the host would otherwise have picked: the decoder
     * here is built for it and VP8 would simply produce nothing. */
    /*
     * The pad's own format, if the host can make it.
     *
     * C2S_CODEC_DRC_H264 is H.264 sliced the way the pad's decoder
     * needs -- macroblock rows, no slice header -- which the host
     * produces with drc-x264 at 864x480 and hands over as five chunks.
     * Taking it means this program never decodes and never re-encodes:
     * the chunks go straight to libdrc's packetiser, and the second
     * lossy pass at a quantiser pinned to 32 disappears.
     *
     * A host without drc-x264 refuses, says so in its STREAM_INFO, and
     * this falls back to ordinary H.264 and the two-pass path below --
     * which is why that path is still here and still tested.
     */
    /*
     * Ordinary H.264, decoded here and encoded again by libdrc.
     *
     * The single-encode path -- the host producing DRH chunks directly
     * with drc-x264 -- is built, correct, and measurably WORSE on this
     * bench, so it is not the default. DRH gives each of five chunks
     * one 1400-byte packet, about 7 KB for a whole frame, and an encode
     * of the raw capture at the pinned quantiser of 32 does not fit:
     * measured at 5.4 packets an image against the two-pass path's
     * exact 5.0, with the pad asking for a keyframe sixty times a
     * second against zero.
     *
     * What makes the two passes fit is the first one: the picture has
     * already been through an encoder by the time libdrc sees it, so
     * the fine detail that costs the most is gone. Paying a second
     * lossy pass to fit inside the protocol's ceiling is a real cost,
     * and it is smaller than a picture that does not arrive.
     *
     * --single-encode asks for the other path. It is the one to finish
     * when there is a way to spend fewer bits at a quantiser that
     * cannot move: a softer downscale was not enough on its own.
     */
    uint8_t want_codec = single_encode ? C2S_CODEC_DRC_H264 : C2S_CODEC_H264;
    /*
     * When to stop waiting for the GamePad's own encode.
     *
     * A host that cannot make it says so in its log and simply carries
     * on sending ordinary H.264; there is no refusal message to wait
     * for. So the test is the honest one -- have any chunks turned up?
     * -- and two seconds is far longer than the round trip while still
     * being short enough that a fallback is not something you sit
     * through.
     */
    const auto drc_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool drc_confirmed = false;
    /*
     * Asked for even with --no-pad, deliberately. That flag exists to
     * tell a fault in the half that can be tested from a fault in the
     * radio, and it would do the opposite if it quietly exercised the
     * other codec: the chunks are checked here, they are simply not
     * pushed anywhere.
     */
    /*
     * Only when this is NOT the pad's own port.
     *
     * Arriving on C2S_DRC_PORT already says what this client is: the
     * host puts it on that stream as it accepts the connection, and
     * asking again afterwards only produces a STREAM_INFO describing
     * what was already true. On any other port the request is the only
     * way to say so.
     */
    if (port != C2S_DRC_PORT) {
        send_message(fd, C2S_MSG_CODEC, &want_codec, sizeof(want_codec));
    }

    /* --- the pad ------------------------------------------------------ */
    drc::Streamer streamer;

    if (!no_pad && !streamer.Start()) {
        fprintf(stderr, "libdrc would not start -- is the access point up and "
                        "the pad paired? another copy of this may also be "
                        "holding its ports\n");
        return 1;
    }
    if (no_pad)
        fprintf(stderr, "no pad: decoding only\n");

    /*
     * Installed again here, and unblocked, because libdrc's threads were
     * created by the call above with this thread's mask.
     *
     * A client that stops answering SIGTERM keeps libdrc's three UDP
     * ports, pushes nothing, and needs SIGKILL -- which on a pad looks
     * like the picture simply stopping, with no replacement able to
     * start.
     */
    {
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, SIGTERM);
        sigaddset(&unblock, SIGINT);
        pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
        std::signal(SIGTERM, [](int) { _exit(0); });
        std::signal(SIGINT,  [](int) { _exit(0); });
    }

    std::string err;
    Video video;
    if (!video.Open(&err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    Audio audio;
    if (ack.audio_rate > 0 && !audio.Start(ack.audio_channels, &err))
        fprintf(stderr, "no sound (%s)\n", err.c_str());

    std::thread input;
    std::thread output;
    /* Asked once a second in the loop below, so a pad that goes away
     * takes the session with it. */
    PadLink pad_check(cli, iface);
    int misses = 0;
    std::thread unstick;
    std::atomic<bool> unsticking{false};
    auto last_unstick = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    /* What the status line above reports. Cheap counters, reset every
     * second, so a glance says whether anything is moving. */
    long bits_second = 0;
    long resyncs_sent = 0;
    long resyncs_this_second = 0;
    int  stuck_seconds = 0;
    long frames_dropped = 0;
    bool pad_present = true;

    std::vector<drc::byte> latest;
    std::mutex frame_lock;

    if (!no_pad) {
        input = std::thread(input_loop, &streamer, fd, &menu);
        /*
         * Pushed at a steady 60 Hz from its own copy rather than
         * whenever a frame happens to decode. The pad wants a frame on
         * time more than it wants the newest one, and a stall upstream
         * should leave the last picture standing rather than nothing.
         */
        output = std::thread([&] {
            auto next = std::chrono::steady_clock::now();
            while (!g_stop) {
                std::vector<drc::byte> frame;
                { std::lock_guard<std::mutex> lock(frame_lock); frame = latest; }
                if (!frame.empty())
                    streamer.PushVidFrame(&frame, kPanelW, kPanelH,
                                          drc::PixelFormat::kRGBA);
                next += std::chrono::microseconds(16683);
                const auto now = std::chrono::steady_clock::now();
                if (next < now) next = now;
                std::this_thread::sleep_until(next);
            }
        });
    }

    /* --- the loop ----------------------------------------------------- */
    std::vector<uint8_t> buf(C2S_MAX_PAYLOAD);
    std::vector<drc::byte> rgba;
    long frames = 0;
    /* Seconds of silence before each step of getting the picture back. */
    constexpr int kQuietAsk = 2, kQuietRetry = 4, kQuietGiveUp = 30;
    int quiet = 0;
    auto last_second = std::chrono::steady_clock::now();
    long in_second = 0;

    while (!g_stop) {
        struct timeval one_second = { 1, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &one_second, sizeof(one_second));

        C2sFrameHeader head;
        bool timed_out = false;
        if (!read_exactly(fd, &head, sizeof(head), &timed_out)) {
            if (timed_out) {
                if (++quiet == kQuietAsk) {
                    fprintf(stderr, "nothing for %d s, asking for a keyframe\n",
                            kQuietAsk);
                    send_message(fd, C2S_MSG_KEYFRAME, nullptr, 0);
                } else if (quiet == kQuietRetry) {
                    fprintf(stderr, "still nothing, asking for H.264 again\n");
                    send_message(fd, C2S_MSG_CODEC, &want_codec,
                                 sizeof(want_codec));
                } else if (quiet >= kQuietGiveUp) {
                    fprintf(stderr, "%d s of silence, giving up\n", kQuietGiveUp);
                    break;
                }
                continue;
            }
            fprintf(stderr, "the stream ended\n");
            break;
        }
        if (quiet) {
            if (quiet >= kQuietAsk)
                fprintf(stderr, "the picture is back\n");
            quiet = 0;
        }

        if (head.size > C2S_MAX_PAYLOAD) {
            fprintf(stderr, "a message claimed %u bytes; dropping the "
                            "connection\n", head.size);
            break;
        }
        if (head.size && !read_exactly(fd, buf.data(), head.size, nullptr)) {
            fprintf(stderr, "the stream ended mid-message\n");
            break;
        }

        if (!drc_confirmed && want_codec == C2S_CODEC_DRC_H264 &&
            std::chrono::steady_clock::now() > drc_deadline) {
            fprintf(stderr, "no wii u encode from this host; decoding and "
                            "re-encoding instead\n");
            want_codec = C2S_CODEC_H264;
            send_message(fd, C2S_MSG_CODEC, &want_codec, sizeof(want_codec));
            drc_confirmed = true;   /* decided, either way */
        }

        switch (head.type) {
        case C2S_MSG_VIDEO:
            /*
             * Already in the pad's format: hand it straight over.
             *
             * No decode, no scale, no colour conversion, no re-encode --
             * which is very nearly everything this program used to do
             * per frame. The host letterboxed it into 864x480 and chose
             * whether this frame is a recovery point; neither is this
             * program's business any more.
             */
            if (want_codec == C2S_CODEC_DRC_H264) {
                C2sDrcFrame df{};
                if (head.size < sizeof(df)) {
                    break;
                }
                memcpy(&df, buf.data(), sizeof(df));
                if (df.chunks != C2S_DRC_CHUNKS) {
                    frames_dropped++;
                    break;
                }
                size_t total = sizeof(df), sizes[C2S_DRC_CHUNKS];
                for (int i = 0; i < C2S_DRC_CHUNKS; i++) {
                    sizes[i] = df.size[i];
                    total += sizes[i];
                }
                /* A length is a promise from the other end, and it
                 * arrives before the bytes do. */
                if (total != head.size) {
                    frames_dropped++;
                    break;
                }
                /*
                 * Here, and not at the top of this branch.
                 *
                 * Set before the checks, one ordinary H.264 frame -- a
                 * host that refused this codec is still sending them --
                 * was enough to convince this client the codec had been
                 * granted. The deadline that exists to notice a refusal
                 * then never fired, and every frame after it was parsed
                 * as five chunks it did not contain: nothing decoded,
                 * nothing pushed, zero frames a second, for ever.
                 */
                drc_confirmed = true;
                bits_second += (long)head.size * 8;
                if (!no_pad) {
                    streamer.PushEncodedVidFrame(buf.data() + sizeof(df), sizes,
                                                 (head.flags & 1) != 0);
                }
                if (frames++ == 0) {
                    fprintf(stderr, no_pad ? "chunks arriving (no pad)\n"
                                           : "streaming to the pad\n");
                }
                in_second++;
                break;
            }
            bits_second += (long)head.size * 8;
            if (video.Decode(buf.data(), head.size, &rgba)) {
                /* Drawn onto the frame on its way to the radio, which
                 * is the only surface this program has. */
                if (!no_pad) {
                    menu.Draw(rgba);
                }
                if (!no_pad) {
                    std::lock_guard<std::mutex> lock(frame_lock);
                    latest.swap(rgba);
                }
                if (frames++ == 0)
                    fprintf(stderr, no_pad ? "decoding (no pad)\n"
                                           : "streaming to the pad\n");
                in_second++;

                /*
                 * No deauthentication cycle here any more.
                 *
                 * It existed because the streamer had to be running
                 * before the pad reassociated, so the pad was kicked to
                 * make that happen. The order is the other way round
                 * now -- the pad associates, and THAT is what starts
                 * everything -- so kicking it here would only take down
                 * the session that had just begun.
                 */
            }
            break;

        case C2S_MSG_AUDIO:
            if (!no_pad)
                audio.Decode(buf.data(), head.size, &streamer);
            break;

        case C2S_MSG_STREAM_INFO: {
            C2sStreamInfo info{};
            if (head.size >= sizeof(info)) {
                memcpy(&info, buf.data(), sizeof(info));
                fprintf(stderr, "the stream is now %ux%u\n",
                        info.width, info.height);
                /*
                 * NOT read as a refusal, however tempting.
                 *
                 * The first of these arrives before the host has even
                 * read the codec request -- it describes the stream
                 * this client was put on when it connected. Concluding
                 * from it that the host has no drc-x264 gives up a
                 * moment before the answer, every single time. What
                 * settles it is whether the chunks actually arrive,
                 * which the deadline below watches for.
                 */
                if (info.video_codec != want_codec) {
                    /* Asked again rather than accepted: this client has
                     * one decoder and VP8 would produce nothing at all. */
                    fprintf(stderr, "the stream is not what this client asked "
                                    "for; asking again\n");
                    send_message(fd, C2S_MSG_CODEC, &want_codec,
                                 sizeof(want_codec));
                }
                video.Reset(&err);
            }
            break;
        }

        case C2S_MSG_PING:
            /* Answered so a quiet connection is not reaped. */
            send_message(fd, C2S_MSG_PING, nullptr, 0);
            break;

        default:
            break;
        }

        /*
         * The pad asking for a recovery point, forwarded to whoever can
         * make one.
         *
         * On this path the encoder is in the host, so libdrc cannot
         * answer -- it records the request instead, and this passes it
         * on. Without it the pad asks on every frame it cannot decode,
         * sixty times a second, the host never restarts its encoder,
         * and the panel stays dark while the packets arrive perfectly.
         * That is precisely what it did: 60 resync/s and 0 IDR/s.
         *
         * Checked per message rather than per second, because the first
         * one decides how long a black screen lasts. The host rate-
         * limits the answer; a restart costs a whole intra frame.
         */
        /*
         * The pad asking for a recovery point, which is the only sign
         * it gives that it cannot decode. Counted on both paths: on the
         * single-encode one it is also forwarded, because the encoder
         * that could answer is in the host; on the two-pass one this
         * client's own libdrc answers it and the count is purely a
         * symptom to watch.
         */
        if (!no_pad && streamer.TakeResyncRequest()) {
            if (want_codec == C2S_CODEC_DRC_H264) {
                send_message(fd, C2S_MSG_KEYFRAME, nullptr, 0);
            }
            resyncs_sent++;
            resyncs_this_second++;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_second >= std::chrono::seconds(1)) {
            /* Only when asked. A line a second for as long as it runs
             * buries the two that matter: the freeze and the recovery. */
            if (getenv("DRC_STATS"))
                fprintf(stderr, "[wiiu_pad] %ld frames/s\n", in_second);

            /*
             * One line a second, in a shape the host picks out for its
             * status label. Everything somebody staring at a dark panel
             * would want to know, because the alternative is reading a
             * terminal to tell "no pad has ever connected" from
             * "streaming perfectly to a screen you are not looking at".
             */
            {
                const char *state;
                if (in_second == 0) {
                    state = frames ? "STALLED" : "connected, no frames yet";
                } else {
                    state = "streaming";
                }
                fprintf(stderr,
                        "status: %s | %ld fps | %ld kbit/s | asking %ld/s "
                        "(%ld total) | dropped %ld | input %s | pad %s\n",
                        state, in_second, bits_second / 1000,
                        resyncs_this_second, resyncs_sent,
                        frames_dropped,
                        ack.may_control ? "on" : "IGNORED (viewer)",
                        pad_present ? "associated" : "GONE");
            }
            /*
             * Frozen, and the one thing known to bring it back.
             *
             * A pad that cannot decode asks for a recovery point on
             * every frame, and once it is in that state it does not
             * leave: the stream is fine, the packets arrive, and the
             * panel stays on whatever it last managed to draw. Half the
             * frame rate for three seconds running is not a bad moment,
             * it is that state.
             *
             * Deauthenticating is the sledgehammer and it is also the
             * only thing measured to work: the pad reassociates in
             * about 0.8s and takes the video transport again from
             * nothing. On its own thread because it blocks for about a
             * second, and rate-limited to once every thirty, because a
             * pad kicked in a loop never finishes coming back.
             */
            if (resyncs_this_second > 30) {
                stuck_seconds++;
            } else {
                stuck_seconds = 0;
            }
            /*
             * Two ways out, cheapest first.
             *
             * THREE SECONDS: tell the pad to re-initialise its decoder.
             * A vstrm packet carries an "init" flag which libdrc sets on
             * the first frame of a stream and never again -- so a pad
             * that has lost the sequence is sent nothing but
             * continuations of a stream it can no longer follow, for
             * ever. Re-arming that flag, together with a recovery point,
             * is the protocol's own way of saying "start over", and it
             * costs one frame.
             *
             * TEN SECONDS: deauthenticate. The sledgehammer, and the
             * only thing measured to work before this existed: the pad
             * reassociates in about 0.8s and takes the transport again
             * from nothing. On its own thread because it blocks for
             * about a second, and rarely, because a pad kicked in a loop
             * never finishes coming back.
             */
            set = menu.Get();
            video.SetSmoothing(set.smoothing);
            if (!no_pad && stuck_seconds >= set.freeze_reinit_s &&
                (stuck_seconds - set.freeze_reinit_s) % 2 == 0) {
                /*
                 * Tried again every two seconds, not once.
                 *
                 * One attempt is one frame, and a frame can be lost --
                 * this is a radio. Retrying costs nothing anybody sees
                 * and it is what keeps the sledgehammer below from ever
                 * being reached: six attempts happen before it is.
                 */
                /* A sweep, not a restart. Restarting produces an IDR,
                 * and an IDR is the frame this protocol cannot carry --
                 * measured at 28 packets where five are allowed, and
                 * followed by 48 more requests. The heavy answer is
                 * kept for the escalation below, where the pad has had
                 * several sweeps and not come back. */
                fprintf(stderr, "the pad is stuck; telling it to start the "
                                "picture over (%ds)\n", stuck_seconds);
                streamer.ReinitStream();
                if (want_codec == C2S_CODEC_DRC_H264) {
                    /* And something to start from, or the init flag
                     * arrives on a frame that predicts from one the pad
                     * never had. On the two-pass path libdrc's own
                     * encoder is restarted by ReinitStream itself. */
                    send_message(fd, C2S_MSG_KEYFRAME, nullptr, 0);
                }
            }
            /*
             * Still stuck: deauthenticate, which is the mitigation that
             * was here first and the one measured to work.
             *
             * I replaced it with "exit and let the supervisor start a
             * new session", reasoning that a pad returning to the same
             * transport would still be stuck. Measured, that was worse:
             * eight recoveries and four whole session restarts a
             * minute, against a deauthentication the pad answers in
             * about 0.8s without the stream ever going away.
             *
             * On its own thread because it blocks for roughly a second,
             * and no more often than every thirty, because a pad kicked
             * in a loop never finishes coming back.
             */
            if (!no_pad && stuck_seconds >= set.freeze_reauth_s && !unsticking &&
                now - last_unstick >= std::chrono::seconds(30)) {
                stuck_seconds = 0;
                last_unstick = now;
                unsticking = true;
                if (unstick.joinable()) unstick.join();
                unstick = std::thread([&] {
                    fprintf(stderr, "still stuck; asking the pad to associate "
                                    "again\n");
                    PadLink(cli, iface).Cycle(20);
                    unsticking = false;
                });
            }
            /*
             * The button on the recovery page, doing by hand what the
             * second escalation does on its own. Same thread rule: it
             * blocks for about a second.
             */
            /*
             * A picture knob moved. They live inside the encoder, so
             * the only way to apply one is to build another -- which
             * ReinitStream does, and which costs the same restart the
             * automatic recovery costs.
             */
            if (!no_pad && menu.TakeEncoderChange()) {
                export_encoder_settings(menu.Get());
                fprintf(stderr, "picture settings changed; rebuilding the encoder\n");
                streamer.ReinitStream();
            }

            if (!no_pad && menu.TakeReconnect() && !unsticking) {
                unsticking = true;
                if (unstick.joinable()) unstick.join();
                unstick = std::thread([&] {
                    fprintf(stderr, "reconnecting the pad, asked for by hand\n");
                    PadLink(cli, iface).Cycle(20);
                    unsticking = false;
                });
            }

            /* And what the panel shows, BEFORE the counters are reset --
             * reading them afterwards is how a stats row shows zero
             * while everything is moving. */
            menu.SetStats(in_second, resyncs_this_second, bits_second / 1000,
                          in_second > 0);

            resyncs_this_second = 0;
            bits_second = 0;
            in_second = 0;
            last_second = now;

            /*
             * And the pad itself, once a second.
             *
             * When it goes -- switched off, carried out of range, its
             * battery gone -- everything this session costs has to stop
             * with it: the encode on the host (which stops because this
             * connection is what was holding that stream open), the
             * video and audio going out over the radio, and the input
             * being read back. Leaving it running would be a GPU
             * encoding 864x480 for a dark panel.
             *
             * One second is chosen against the deauthentication window:
             * a pad that reassociates does so in about 0.8s, so a check
             * this slow cannot mistake a reassociation for a departure.
             */
            /*
             * Three readings, not one.
             *
             * A single empty answer is not a pad leaving. hostapd_cli
             * is a separate process spawned through popen every time
             * this asks, and under load -- which is exactly when this
             * runs, sixty frames a second going out over a radio -- a
             * fork can fail, or the control socket can answer late.
             * That ended sessions with the pad sitting right there,
             * associated, for as long as anyone cared to check.
             *
             * Three seconds of silence is still far quicker than
             * anybody notices, and it is well past the ~0.8s a
             * reassociation takes.
             */
            if (!no_pad) {
                if (pad_check.HasStation()) {
                    misses = 0;
                    pad_present = true;
                } else if (++misses >= 3) {
                    pad_present = false;
                    fprintf(stderr, "the pad has been gone for %ds; stopping\n", misses);
                    break;
                }
            }
        }
    }

    g_stop = true;
    if (unstick.joinable()) unstick.join();
    if (input.joinable()) input.join();
    if (output.joinable()) output.join();
    if (!no_pad) streamer.Stop();
    close(fd);
    fprintf(stderr, "stopped\n");
    return 0;
}
