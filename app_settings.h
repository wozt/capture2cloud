#ifndef CAPTURE2CLOUD_APP_SETTINGS_H
#define CAPTURE2CLOUD_APP_SETTINGS_H

/*
 * Everything the local interface can change, in one place.
 *
 * The GTK settings window and the program pass this struct rather than a
 * callback per control: there are twenty-odd settings and a signature
 * for each would be a list nobody could keep in their head, and adding
 * one would mean touching four files instead of one.
 *
 * Some of these are the SERVER's -- the stream's resolution and bitrate,
 * the capture format -- and changing them changes what every browser and
 * the console see. Some are LOCAL to this machine's window: the picture
 * adjustments, the local speakers, whether a controller plugged in here
 * drives the console. The comments say which is which, because "why did
 * my friend's picture change" is otherwise a mystery.
 *
 * Nothing here asks for a password. The person at the keyboard is
 * already at the machine the console is plugged into: a login would
 * guard a door they are standing behind.
 */
typedef struct {
    /* --- the server's, shared by everyone watching --- */
    int stream_enabled;     /* whether the web server is serving at all */
    int web_port;
    /* Whether the console's own server is listening. Separate from the
     * web one because they are separate servers, and because a session
     * with nobody on a Switch has no reason to hold a port open. */
    int switch_enabled;
    /* The port the console client connects to. Its own, because the two
     * streams are two servers: the browser's is HTTP and this one is a
     * small binary protocol, and moving one has no reason to move the
     * other. */
    int switch_port;
    /*
     * Whether a real Wii U GamePad is being fed.
     *
     * It is a separate program (see wiiu_pad.h), started and stopped
     * with this flag. Off by default: it needs a Realtek adapter
     * running an access point and a paired pad, which almost nobody
     * has, and turning it on without them only produces an error.
     */
    int wiiu_pad_enabled;
    int wiiu_pad_bitrate_mbps;   /* what that chain is encoded at */
    /*
     * Whether a homebrew running ON a Wii U console is being fed.
     *
     * Its own flag and not the GamePad's: they are different clients on
     * different ports with nothing in common but a console maker. One
     * needs a Realtek adapter and a paired pad; this one needs a network
     * and nothing else.
     */
    int wiiu_console_enabled;
    int wiiu_console_height;      /* 1080, 720 or 480; 720 is the tested path */
    int wiiu_console_bitrate_mbps;
    int browser_height;     /* 1080, 720 or 480 -- what the browser gets */
    int bitrate_mbps;       /* the browser encoder's target */
    int capture_mjpeg;      /* 1 = MJPEG from the card, 0 = raw YUYV */

    /* --- this machine's controller, driving the console --- */
    int gamepad_enabled;    /* off in headless, where there is nobody here */
    int gamepad_index;      /* which SDL controller, -1 for none */
    /* Console-output backend registry index. Changing this is
     * persisted to GAMEPAD_OUTPUT_BACKEND and requires a restart. */
    int output_backend;
    int invert_ry;
    int lt_threshold;       /* percent of the trigger's travel */
    int rt_threshold;
    int stick_deadzone[2];  /* left, right; percent */
    int stick_range[2];     /* percent that counts as fully pushed */
    int stick_diagonal[2];  /* the same, for the corners */

    /*
     * Final output calibration, applied after ALL sources have been
     * merged. These are intentionally separate from the values above:
     * those calibrate only the physical SDL controller plugged into this
     * host, while these calibrate what the real console finally receives.
     *
     * An output trigger threshold of zero preserves the incoming value.
     */
    int output_invert_ry;
    int output_lt_threshold;
    int output_rt_threshold;
    int output_stick_deadzone[2];
    int output_stick_range[2];
    int output_stick_diagonal[2];

    /* Which controller the adapter pretends to be to the console, in the
     * numbering of gamepad_protocol_name(): 0 auto, 2 xb360, 6 switch...
     * -1 means "leave the adapter as it is". This one is not really the
     * server's nor this window's -- it lives in the adapter's own memory
     * and outlives the program. */
    int output_protocol;

    /* --- this window only --- */
    int local_muted;        /* the speakers here; the stream is unaffected */
    /* Play on the output named by LOCAL_SINK rather than the system
     * default. Local to this machine, like the rest of this group. */
    int local_direct_sink;
    int local_volume;       /* 0..100; the source's own level is near 13 */
    int brightness;         /* 50..150 */
    int contrast;           /* 50..150 */
    int vsync;              /* wait for the local display before drawing */
} AppSettings;

/* What everything starts as. The stick limits mirror the page's and the
 * console client's, so a stick behaves the same wherever it is plugged
 * in. */
#define APP_SETTINGS_DEFAULTS                                                  \
    {                                                                          \
        .stream_enabled = 0, .web_port = 5080, .switch_enabled = 1,             \
        .switch_port = 5081, .wiiu_pad_enabled = 0, .wiiu_pad_bitrate_mbps = 6,     \
        .wiiu_console_enabled = 0, .wiiu_console_height = 720,                 \
        .wiiu_console_bitrate_mbps = 8,                            \
        .browser_height = 1080,         \
        .bitrate_mbps = 12, .capture_mjpeg = 0, .gamepad_enabled = 1,          \
        .gamepad_index = -1, .output_backend = 0, .invert_ry = 0, .lt_threshold = 30,               \
        .rt_threshold = 30, .stick_deadzone = {5, 5}, .stick_range = {100, 100},\
        .stick_diagonal = {100, 100},                                         \
        .output_invert_ry = 0, .output_lt_threshold = 0,                      \
        .output_rt_threshold = 0, .output_stick_deadzone = {0, 0},            \
        .output_stick_range = {100, 100},                                     \
        .output_stick_diagonal = {100, 100}, .output_protocol = -1,           \
        .local_muted = 0, .local_direct_sink = 0, .local_volume = 13,                                  \
        .brightness = 100, .contrast = 100, .vsync = 1,                        \
    }

#endif
