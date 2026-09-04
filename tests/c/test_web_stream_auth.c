/* Unit tests for web_stream.c's authentication logic -- the part that
 * decides whether a connecting client may drive the console.
 *
 * This is security-relevant code, so it's worth testing directly rather
 * than only through the HTTP surface: a constant-time comparison that
 * silently stops being constant-time, or a token generator that repeats
 * itself, would both be invisible from the outside until abused.
 *
 * web_stream.c is #included so its `static` functions are reachable. It
 * calls two functions from gst_webrtc.c; those are stubbed below rather
 * than linking the real GStreamer pipeline in, since none of the code
 * under test here touches WebRTC at all. */
#include "../../app_config.c"
#include "../../web_stream.c"

#include "test_util.h"

#include <sys/stat.h>
#include <sys/un.h>
#include <sys/types.h>

/* --- stubs for the gst_webrtc.c side (never actually exercised here) --- */
char *gst_webrtc_stream_handle_offer(GstWebrtcStream *g, const char *offer_sdp, int may_control,
                                     const struct sockaddr *peer, socklen_t peer_len) {
    (void)g;
    (void)offer_sdp;
    (void)may_control;
    (void)peer;
    (void)peer_len;
    return NULL;
}
void gamepad_bridge_reset(void) {}
/* /gamepad-rate reports this; nothing here drives a real adapter. */
double gamepad_bridge_report_rate(void) { return 0.0; }
/* The resolution endpoint only needs to be seen refusing a viewer here;
 * the pipeline itself is not linked into this test. */
static int stub_res_w = 1920, stub_res_h = 1080;
void gst_webrtc_stream_set_browser_resolution(GstWebrtcStream *g, int w, int h) {
    (void)g; stub_res_w = w; stub_res_h = h;
}
void gst_webrtc_stream_get_browser_resolution(GstWebrtcStream *g, int *w, int *h) {
    (void)g; if (w) *w = stub_res_w; if (h) *h = stub_res_h;
}
/* video_capture.c is not linked here; the format endpoint only needs to
 * be seen refusing a viewer. */
static VideoFormat stub_requested = VIDEO_FORMAT_YUYV;
void video_capture_request_format(VideoFormat f) { stub_requested = f; }
VideoFormat video_capture_active_format(void) { return stub_requested; }
void video_capture_watch_for_change(void) {}
const char *video_format_name(VideoFormat f) {
    return f == VIDEO_FORMAT_YUYV ? "yuyv" : "mjpeg";
}
static int stub_bitrate_kbps = 12000;
void gst_webrtc_stream_set_video_bitrate(GstWebrtcStream *g, int bitrate_kbps) {
    (void)g;
    stub_bitrate_kbps = bitrate_kbps;
}
/* /shared reports it, so the stub has to have one to report. */
int gst_webrtc_stream_get_video_bitrate(GstWebrtcStream *g) {
    (void)g;
    return stub_bitrate_kbps;
}
int gst_webrtc_stream_get_client_count(GstWebrtcStream *g, int *max_clients) {
    (void)g;
    if (max_clients) *max_clients = 0;
    return 0;
}

/* These tests rewrite the .env repeatedly, so it matters a great deal
 * WHICH .env they touch. Since config paths now resolve relative to the
 * running executable, and the test binary is built into a temporary
 * directory (see tests/run_all.sh), that is a throwaway .env -- the real
 * one, holding live credentials, is never opened at all. All this needs
 * is for the `scripts/` subdirectory to exist next to the binary, since
 * fopen() won't create it.
 *
 * This is a nice side effect of making paths executable-relative: the
 * tests became incapable of clobbering real configuration, instead of
 * merely careful about it. */
static char env_path[PATH_MAX];

static void env_setup(void) {
    char dir[PATH_MAX];
    app_path(dir, sizeof(dir), "scripts");
    mkdir(dir, 0700); /* fine if it already exists */
    app_config_path(env_path, sizeof(env_path));

    /* Guard against ever running against the real configuration: if the
     * binary somehow sits in the project directory, refuse rather than
     * overwrite it. */
    if (strstr(env_path, "/capture2cloud/scripts/.env") != NULL) {
        fprintf(stderr,
                "REFUSING TO RUN: this would overwrite the real config at %s.\n"
                "Build the test binary outside the project directory (see tests/run_all.sh).\n",
                env_path);
        exit(2);
    }
}
static void env_cleanup(void) {
    remove(env_path);
}

/* Writes a whole .env body (so tests can control the surrounding lines,
 * not just the password entry). */
static void write_env(const char *contents) {
    FILE *f = fopen(env_path, "w");
    if (f) {
        fputs(contents, f);
        fclose(f);
    }
    /* The config is cached and notices edits by their timestamp. These
     * rewrites happen microseconds apart, well inside one tick of the
     * filesystem's clock, so two of the same length would otherwise look
     * unchanged -- a hazard only a test creates. */
    app_config_invalidate();
}
/* Convenience: a realistic .env with other keys around the password, to
 * make sure the parser picks the right line rather than the first one. */
static void write_env_with_password(const char *line) {
    char body[1024];
    snprintf(body, sizeof(body),
             "# comment line\n"
             "HA_URL=http://192.168.2.2:8123\n"
             "HA_TOKEN=sometoken\n"
             "\n"
             "%s\n"
             "HA_PLUG_ENTITY=switch.test\n",
             line);
    write_env(body);
}
static void write_no_password(void) {
    write_env("HA_URL=http://192.168.2.2:8123\nHA_TOKEN=sometoken\n");
}

static void test_secure_equals(void) {
    t_begin("secure_equals");

    t_ok("identical strings match", secure_equals("hunter2", "hunter2"));
    t_ok("different strings don't match", !secure_equals("hunter2", "hunter3"));
    t_ok("empty strings match", secure_equals("", ""));
    t_ok("empty vs non-empty", !secure_equals("", "x"));

    /* Length differences must not match even when one is a prefix of the
     * other -- a comparison that stopped at the shorter length would
     * accept "hunter" for "hunter2". */
    t_ok("prefix is not accepted", !secure_equals("hunter", "hunter2"));
    t_ok("longer is not accepted", !secure_equals("hunter22", "hunter2"));

    /* Differences at either end must be caught: an implementation that
     * only compared the first or last bytes would pass one of these and
     * fail the other. */
    t_ok("differs in first char", !secure_equals("aunter2", "hunter2"));
    t_ok("differs in last char", !secure_equals("hunter1", "hunter2"));
    t_ok("differs in middle", !secure_equals("hunXer2", "hunter2"));
}

static void test_token_generation(void) {
    t_begin("generate_token");

    char a[WS_TOKEN_HEX_LEN + 1];
    char b[WS_TOKEN_HEX_LEN + 1];

    t_ok("generation succeeds", generate_token(a) == 1);
    t_ok("second generation succeeds", generate_token(b) == 1);

    t_eq_int("token length", (long long)strlen(a), WS_TOKEN_HEX_LEN);
    t_ok("two tokens differ", strcmp(a, b) != 0);

    /* Must be pure lowercase hex: anything else would mean a formatting
     * bug, and the token travels in an HTTP header where stray bytes
     * would be trouble. */
    int all_hex = 1;
    for (size_t i = 0; i < strlen(a); i++) {
        char c = a[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) all_hex = 0;
    }
    t_ok("token is lowercase hex only", all_hex);

    /* A token full of zeros would suggest /dev/urandom wasn't actually
     * read (or was read into the wrong buffer). */
    int all_zeros = 1;
    for (size_t i = 0; i < strlen(a); i++) {
        if (a[i] != '0') all_zeros = 0;
    }
    t_ok("token is not all zeros", !all_zeros);
}

static void test_password_from_env(void) {
    t_begin("load_player_password (.env parsing)");

    char pw[WS_MAX_PASSWORD_LEN];

    remove(env_path);
    t_ok("no .env -> no password set", load_player_password(pw, sizeof(pw)) == 0);
    t_ok("no .env -> password not required", !password_required());

    write_no_password();
    t_ok("key absent -> no password set", load_player_password(pw, sizeof(pw)) == 0);

    /* The key must be found among other lines, not just at the top. */
    write_env_with_password("PLAYER_PASSWORD=secret123");
    t_ok("key present -> password set", load_player_password(pw, sizeof(pw)) == 1);
    t_eq_str("value read correctly", pw, "secret123");
    t_ok("key present -> password required", password_required());

    /* Written by an editor on Windows, or pasted with CRLF. */
    write_env("PLAYER_PASSWORD=secret123\r\n");
    load_player_password(pw, sizeof(pw));
    t_eq_str("CRLF stripped", pw, "secret123");

    /* No trailing newline at all (e.g. `printf` rather than `echo`). */
    write_env("PLAYER_PASSWORD=secret123");
    load_player_password(pw, sizeof(pw));
    t_eq_str("no trailing newline works", pw, "secret123");

    /* The same file is `source`d by the shell scripts, where quoting is
     * normal -- the quotes must not end up part of the password. */
    write_env_with_password("PLAYER_PASSWORD=\"secret123\"");
    load_player_password(pw, sizeof(pw));
    t_eq_str("double quotes stripped", pw, "secret123");
    write_env_with_password("PLAYER_PASSWORD='secret123'");
    load_player_password(pw, sizeof(pw));
    t_eq_str("single quotes stripped", pw, "secret123");

    /* Passwords legitimately contain punctuation -- none of it should
     * confuse the parser, including '=' and '#'. */
    write_env_with_password("PLAYER_PASSWORD=p@ss=w0rd#1");
    load_player_password(pw, sizeof(pw));
    t_eq_str("value keeps '=' and '#'", pw, "p@ss=w0rd#1");

    /* Leading whitespace before the key is tolerated. */
    write_env("   PLAYER_PASSWORD=indented\n");
    load_player_password(pw, sizeof(pw));
    t_eq_str("indented key found", pw, "indented");

    /* A commented-out entry must NOT be picked up -- otherwise disabling
     * the password with a '#' would silently keep it active. */
    write_env("#PLAYER_PASSWORD=commented\n");
    t_ok("commented key ignored", load_player_password(pw, sizeof(pw)) == 0);
    write_env("  # PLAYER_PASSWORD=commented\n");
    t_ok("indented comment ignored", load_player_password(pw, sizeof(pw)) == 0);

    /* An empty value means "no password", not "the empty password" --
     * otherwise clearing it would silently accept every login. */
    write_env_with_password("PLAYER_PASSWORD=");
    t_ok("empty value -> treated as no password", load_player_password(pw, sizeof(pw)) == 0);
    write_env_with_password("PLAYER_PASSWORD=\"\"");
    t_ok("empty quoted value -> no password", load_player_password(pw, sizeof(pw)) == 0);

    /* A key that merely starts the same must not match. */
    write_env("PLAYER_PASSWORD_OLD=secret123\n");
    t_ok("similarly-named key ignored", load_player_password(pw, sizeof(pw)) == 0);

    /* Duplicated key: last one wins, matching shell `source` semantics. */
    write_env("PLAYER_PASSWORD=first\nPLAYER_PASSWORD=second\n");
    load_player_password(pw, sizeof(pw));
    t_eq_str("last duplicate wins", pw, "second");

    remove(env_path);
}

static void test_sessions(void) {
    t_begin("session table");

    WebStream *ws = web_stream_create(NULL);
    t_ok("web_stream_create succeeded", ws != NULL);
    if (!ws) return;

    t_ok("unknown token rejected", !session_is_valid(ws, "deadbeef"));
    t_ok("empty token rejected", !session_is_valid(ws, ""));
    t_ok("NULL token rejected", !session_is_valid(ws, NULL));

    char token[WS_TOKEN_HEX_LEN + 1];
    generate_token(token);
    session_add(ws, token);
    t_ok("added token is valid", session_is_valid(ws, token));

    /* A token that shares a prefix with a valid one must still be
     * rejected (guards against a partial/prefix comparison creeping in). */
    char almost[WS_TOKEN_HEX_LEN + 1];
    snprintf(almost, sizeof(almost), "%s", token);
    almost[WS_TOKEN_HEX_LEN - 1] = (almost[WS_TOKEN_HEX_LEN - 1] == 'a') ? 'b' : 'a';
    t_ok("near-miss token rejected", !session_is_valid(ws, almost));

    /* Several players at once. */
    char second[WS_TOKEN_HEX_LEN + 1];
    generate_token(second);
    session_add(ws, second);
    t_ok("first token still valid", session_is_valid(ws, token));
    t_ok("second token valid too", session_is_valid(ws, second));

    web_stream_destroy(ws);
}

static void test_session_table_wraparound(void) {
    t_begin("session table capacity");

    WebStream *ws = web_stream_create(NULL);
    if (!ws) {
        t_ok("web_stream_create succeeded", 0);
        return;
    }

    /* Fill the table completely. */
    char tokens[WS_MAX_SESSIONS][WS_TOKEN_HEX_LEN + 1];
    for (int i = 0; i < WS_MAX_SESSIONS; i++) {
        generate_token(tokens[i]);
        session_add(ws, tokens[i]);
    }
    int all_valid = 1;
    for (int i = 0; i < WS_MAX_SESSIONS; i++) {
        if (!session_is_valid(ws, tokens[i])) all_valid = 0;
    }
    t_ok("a full table keeps every token valid", all_valid);

    /* Overflowing must reuse a slot rather than corrupt memory or drop
     * the new token on the floor: the newest login has to work. */
    char extra[WS_TOKEN_HEX_LEN + 1];
    generate_token(extra);
    session_add(ws, extra);
    t_ok("token added past capacity is valid", session_is_valid(ws, extra));

    /* Exactly one of the originals should have been evicted. */
    int still_valid = 0;
    for (int i = 0; i < WS_MAX_SESSIONS; i++) {
        if (session_is_valid(ws, tokens[i])) still_valid++;
    }
    t_eq_int("exactly one old session evicted", still_valid, WS_MAX_SESSIONS - 1);

    web_stream_destroy(ws);
}

/* Drives one endpoint over a socketpair and returns its first response
 * line, so the endpoint's own access check is exercised -- not just the
 * predicate it calls. */
static void endpoint_response(void (*fn)(WebStream *, int, long, const char *),
                              WebStream *ws, const char *body, const char *token,
                              char *out, size_t out_size) {
    int sv[2];
    out[0] = '\0';
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return;
    }
    /* The handler reads the body off the same socket it replies on. */
    if (write(sv[1], body, strlen(body)) < 0) {
        close(sv[0]);
        close(sv[1]);
        return;
    }
    fn(ws, sv[0], (long)strlen(body), token);
    close(sv[0]);
    ssize_t n = read(sv[1], out, out_size - 1);
    out[n > 0 ? (size_t)n : 0] = '\0';
    close(sv[1]);
}

/* There is ONE shared encoder feeding every client, so the bitrate is
 * global. A viewer lowering it would degrade the picture for whoever is
 * actually playing, which is why this is refused server-side and not
 * merely by hiding the slider -- anyone can POST to the endpoint. */
/* handle_reset_dongle takes no body length; adapt it to the driver. */
static void handle_reset_dongle_for_test(WebStream *ws, int fd, long content_length, const char *token) {
    (void)content_length;
    handle_reset_dongle(ws, fd, token);
}

/* handle_login takes no token; this adapts it to the shared driver. */
static void handle_login_for_test(WebStream *ws, int fd, long content_length, const char *token) {
    (void)token;
    handle_login(ws, fd, content_length);
}

static void test_quality_is_player_only(void) {
    t_begin("/quality is refused to viewers");

    WebStream *ws = web_stream_create(NULL);
    if (!ws) {
        t_ok("web_stream_create succeeded", 0);
        return;
    }
    char resp[256];

    write_env_with_password("PLAYER_PASSWORD=secret123");
    endpoint_response(handle_quality, ws, "8000", NULL, resp, sizeof(resp));
    t_ok("no token -> 403", strncmp(resp, "HTTP/1.1 403", 12) == 0);

    endpoint_response(handle_quality, ws, "8000", "deadbeef", resp, sizeof(resp));
    t_ok("bogus token -> 403", strncmp(resp, "HTTP/1.1 403", 12) == 0);

    char token[WS_TOKEN_HEX_LEN + 1];
    generate_token(token);
    session_add(ws, token);
    endpoint_response(handle_quality, ws, "8000", token, resp, sizeof(resp));
    t_ok("valid token -> accepted", strncmp(resp, "HTTP/1.1 403", 12) != 0);

    /* With no password configured at all, everyone is a player -- the
     * pre-login behaviour has to keep working. */
    write_no_password();
    endpoint_response(handle_quality, ws, "8000", NULL, resp, sizeof(resp));
    t_ok("no password set -> open to all", strncmp(resp, "HTTP/1.1 403", 12) != 0);

    remove(env_path);
    web_stream_destroy(ws);
}

/* The half-second penalty on a wrong password only slowed the connection
 * that guessed: 30 parallel guesses against the live server finished in
 * 505 ms instead of 15 s. The counter has to be shared for parallelism
 * to stop helping. */
static void test_login_lockout(void) {
    t_begin("failed logins are throttled server-wide");

    WebStream *ws = web_stream_create(NULL);
    if (!ws) {
        t_ok("web_stream_create succeeded", 0);
        return;
    }
    write_env_with_password("PLAYER_PASSWORD=secret123");
    char resp[256];

    t_ok("not locked to begin with", !login_is_locked(ws));

    /* Failures from different connections must accumulate, which is the
     * whole point -- otherwise opening more sockets defeats it. */
    for (int i = 0; i < WS_LOGIN_MAX_FAILS - 1; i++) {
        endpoint_response(handle_login_for_test, ws, "wrong", NULL, resp, sizeof(resp));
        t_ok("wrong password -> 401", strncmp(resp, "HTTP/1.1 401", 12) == 0);
    }
    t_ok("still open just below the limit", !login_is_locked(ws));

    endpoint_response(handle_login_for_test, ws, "wrong", NULL, resp, sizeof(resp));
    t_ok("the limit locks logins", login_is_locked(ws));

    /* During a lockout even the CORRECT password is refused, or guessing
     * straight through it would still work. */
    endpoint_response(handle_login_for_test, ws, "secret123", NULL, resp, sizeof(resp));
    t_ok("correct password refused while locked", strncmp(resp, "HTTP/1.1 429", 12) == 0);
    t_ok("no token handed out while locked", strstr(resp, "\r\n\r\n") != NULL &&
                                             strlen(strstr(resp, "\r\n\r\n") + 4) == 0);

    /* A successful login clears the counter so a user who mistyped a few
     * times is not left one slip from a lockout. */
    ws->login_locked_until = 0;
    ws->login_fails = WS_LOGIN_MAX_FAILS - 1;
    endpoint_response(handle_login_for_test, ws, "secret123", NULL, resp, sizeof(resp));
    t_ok("correct password accepted once unlocked", strncmp(resp, "HTTP/1.1 200", 12) == 0);
    t_eq_int("success resets the failure count", ws->login_fails, 0);

    remove(env_path);
    web_stream_destroy(ws);
}

/* One capture device for everyone, so switching its format is not a
 * viewer's to do -- and refusing it has to happen server-side, since
 * anyone can POST to the endpoint whatever the page shows. */
static void test_capture_format_is_player_only(void) {
    t_begin("/capture-format is refused to viewers");

    WebStream *ws = web_stream_create(NULL);
    if (!ws) {
        t_ok("web_stream_create succeeded", 0);
        return;
    }
    char resp[256];
    write_env_with_password("PLAYER_PASSWORD=secret123");

    endpoint_response(handle_capture_format, ws, "mjpeg", NULL, resp, sizeof(resp));
    t_ok("no token -> 403", strncmp(resp, "HTTP/1.1 403", 12) == 0);
    t_eq_int("and the format was left alone", (int)stub_requested, (int)VIDEO_FORMAT_YUYV);

    char token[WS_TOKEN_HEX_LEN + 1];
    generate_token(token);
    session_add(ws, token);

    endpoint_response(handle_capture_format, ws, "mjpeg", token, resp, sizeof(resp));
    t_ok("valid token -> accepted", strncmp(resp, "HTTP/1.1 204", 12) == 0);
    t_eq_int("the switch was requested", (int)stub_requested, (int)VIDEO_FORMAT_MJPEG);

    endpoint_response(handle_capture_format, ws, "yuyv", token, resp, sizeof(resp));
    t_eq_int("and back again", (int)stub_requested, (int)VIDEO_FORMAT_YUYV);

    /* An unknown value must be rejected outright rather than silently
     * picking one of the two. */
    endpoint_response(handle_capture_format, ws, "nawak", token, resp, sizeof(resp));
    t_ok("unknown format -> 400", strncmp(resp, "HTTP/1.1 400", 12) == 0);
    t_eq_int("and changed nothing", (int)stub_requested, (int)VIDEO_FORMAT_YUYV);

    remove(env_path);
    web_stream_destroy(ws);
}

/* The reset takes the adapter away from the console for three seconds;
 * that is not a viewer's to do. */
static void test_reset_dongle_is_player_only(void) {
    t_begin("/reset-dongle is refused to viewers");

    WebStream *ws = web_stream_create(NULL);
    if (!ws) { t_ok("web_stream_create succeeded", 0); return; }
    char resp[256];
    write_env_with_password("PLAYER_PASSWORD=secret123");

    endpoint_response(handle_reset_dongle_for_test, ws, "", NULL, resp, sizeof(resp));
    t_ok("no token -> 403", strncmp(resp, "HTTP/1.1 403", 12) == 0);

    char token[WS_TOKEN_HEX_LEN + 1];
    generate_token(token);
    session_add(ws, token);
    endpoint_response(handle_reset_dongle_for_test, ws, "", token, resp, sizeof(resp));
    t_ok("valid token -> accepted", strncmp(resp, "HTTP/1.1 204", 12) == 0);

    remove(env_path);
    web_stream_destroy(ws);
}

static void test_may_control(void) {
    t_begin("request_may_control");

    WebStream *ws = web_stream_create(NULL);
    if (!ws) {
        t_ok("web_stream_create succeeded", 0);
        return;
    }

    /* No password configured: control is open to everyone. This is the
     * backwards-compatible default -- an existing setup keeps working
     * exactly as it did before login existed. */
    write_no_password();
    t_ok("no password -> anyone may control", request_may_control(ws, NULL));
    t_ok("no password -> a bogus token is fine too", request_may_control(ws, "whatever"));

    /* Password configured: only a valid session token gets through. */
    write_env_with_password("PLAYER_PASSWORD=secret123");
    t_ok("password set -> no token means viewer", !request_may_control(ws, NULL));
    t_ok("password set -> empty token means viewer", !request_may_control(ws, ""));
    t_ok("password set -> bogus token means viewer", !request_may_control(ws, "deadbeef"));

    char token[WS_TOKEN_HEX_LEN + 1];
    generate_token(token);
    session_add(ws, token);
    t_ok("password set -> valid token may control", request_may_control(ws, token));

    /* Clearing the password re-opens control: the .env is the single
     * source of truth, re-read on every request -- no restart needed. */
    write_no_password();
    t_ok("password removed -> open again", request_may_control(ws, NULL));

    remove(env_path);
    web_stream_destroy(ws);
}

static void test_app_paths(void) {
    t_begin("app_path / app_dir");

    char path[PATH_MAX];
    app_path(path, sizeof(path), "web/main.js");

    /* Must be absolute and rooted at the EXECUTABLE's directory: the app
     * is started via setsid from another directory, so a bare relative
     * name would not resolve, and anything derived from __FILE__ or
     * $HOME would break as soon as the project is built or checked out
     * somewhere else. */
    t_ok("path is absolute", path[0] == '/');
    t_ok("path ends with the file name", strstr(path, "web/main.js") != NULL);
    t_ok("path is not just the bare name", strcmp(path, "web/main.js") != 0);
    t_ok("path starts with app_dir()", strncmp(path, app_dir(), strlen(app_dir())) == 0);

    /* The config file sits under that same directory. */
    char cfg[PATH_MAX];
    app_config_path(cfg, sizeof(cfg));
    t_ok("config path is under app_dir()", strncmp(cfg, app_dir(), strlen(app_dir())) == 0);
    t_ok("config path ends with .env", strstr(cfg, ".env") != NULL);
}

static void test_config_types(void) {
    t_begin("config_get_int");

    write_env("SOME_NUMBER=42\nHEX_VALUE=0x2508\nNOT_A_NUMBER=abc\nTOO_BIG=9999\nTOO_SMALL=-5\n");

    t_eq_int("plain decimal", config_get_int("SOME_NUMBER", 7, 0, 100), 42);
    /* USB ids are written in hex in the .env (lsusb prints them that
     * way), so base-0 parsing is required -- base 10 would read 0. */
    t_eq_int("hex value", config_get_int("HEX_VALUE", 0, 0, 0xffff), 0x2508);
    t_eq_int("missing key falls back", config_get_int("NO_SUCH_KEY", 7, 0, 100), 7);
    t_eq_int("unparseable falls back", config_get_int("NOT_A_NUMBER", 7, 0, 100), 7);
    /* Out-of-range values are clamped, not rejected: a typo should
     * degrade to a sane limit rather than break the run. */
    t_eq_int("above range is clamped", config_get_int("TOO_BIG", 7, 0, 32), 32);
    t_eq_int("below range is clamped", config_get_int("TOO_SMALL", 7, 0, 32), 0);

    char buf[64];
    t_eq_str("string fallback when missing", config_get_str("NO_SUCH_KEY", buf, sizeof(buf), "default"), "default");
    t_eq_str("string value when present", config_get_str("SOME_NUMBER", buf, sizeof(buf), "default"), "42");

    remove(env_path);
}

int main(void) {
    /* These tests overwrite scripts/.env, which holds real credentials
     * -- stash it first and put it back before exiting, whatever
     * happens. */
    env_setup();

    test_secure_equals();
    test_token_generation();
    test_password_from_env();
    test_sessions();
    test_session_table_wraparound();
    test_may_control();
    test_quality_is_player_only();
    test_login_lockout();
    test_capture_format_is_player_only();
    test_reset_dongle_is_player_only();
    test_app_paths();
    test_config_types();

    env_cleanup();
    return t_report();
}
