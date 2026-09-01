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
    /* The port the console client connects to. Its own, because the two
     * streams are two servers: the browser's is HTTP and this one is a
     * small binary protocol, and moving one has no reason to move the
     * other. */
    int switch_port;
    int browser_height;     /* 1080, 720 or 480 -- what the browser gets */
    int bitrate_mbps;       /* the browser encoder's target */
    int capture_mjpeg;      /* 1 = MJPEG from the card, 0 = raw YUYV */

    /* --- this machine's controller, driving the console --- */
    int gamepad_enabled;    /* off in headless, where there is nobody here */
    int gamepad_index;      /* which SDL controller, -1 for none */
    int invert_ry;
    int lt_threshold;       /* percent of the trigger's travel */
    int rt_threshold;
    int stick_deadzone[2];  /* left, right; percent */
    int stick_range[2];     /* percent that counts as fully pushed */
    int stick_diagonal[2];  /* the same, for the corners */

    /* --- this window only --- */
    int local_muted;        /* the speakers here; the stream is unaffected */
    int local_volume;       /* 0..100, where 25 is the source's own level */
    int brightness;         /* 50..150 */
    int contrast;           /* 50..150 */
    int vsync;              /* wait for the local display before drawing */
} AppSettings;

/* What everything starts as. The stick limits mirror the page's and the
 * console client's, so a stick behaves the same wherever it is plugged
 * in. */
#define APP_SETTINGS_DEFAULTS                                                  \
    {                                                                          \
        .stream_enabled = 0, .web_port = 5080, .switch_port = 5081,             \
        .browser_height = 1080,         \
        .bitrate_mbps = 12, .capture_mjpeg = 0, .gamepad_enabled = 1,          \
        .gamepad_index = -1, .invert_ry = 0, .lt_threshold = 30,               \
        .rt_threshold = 30, .stick_deadzone = {5, 5}, .stick_range = {100, 100},\
        .stick_diagonal = {100, 100}, .local_muted = 0, .local_volume = 25,    \
        .brightness = 100, .contrast = 100, .vsync = 1,                        \
    }

#endif
