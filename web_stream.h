#ifndef WEB_STREAM_H
#define WEB_STREAM_H

#include <stddef.h>

#include "gst_webrtc.h"

/*
 * Small HTTP server: serves the player page and does WebRTC signaling
 * (SDP offer/answer exchange). The audio/video stream itself never goes
 * through this server -- once the WebRTC connection is established,
 * media packets go directly from the GStreamer pipeline (gst_webrtc.c) to
 * the browser.
 *
 *   GET  /       HTML page (WebRTC video player)
 *   POST /offer  body = browser's SDP offer, response = SDP answer
 */

typedef struct WebStream WebStream;

WebStream *web_stream_create(GstWebrtcStream *webrtc);
void web_stream_destroy(WebStream *ws);

/* Starts the server on the given port. Returns 0 on success, -1
 * otherwise (error message written to errbuf). No effect if already
 * started. */
int web_stream_start(WebStream *ws, int port, char *errbuf, size_t errbuf_len);

/* Stops the server and disconnects clients. No effect if already
 * stopped. */
void web_stream_stop(WebStream *ws);

int web_stream_is_running(WebStream *ws);
int web_stream_get_port(WebStream *ws);

/* Whether a session token grants control. Exposed so the native
 * transport asks the SAME question the browser path asks, rather than
 * carrying its own idea of who may drive the console -- two answers that
 * could disagree is exactly how an access check becomes decorative.
 * A NULL or empty token is a viewer unless no password is configured. */
/* Wakes the console: runs scripts/wake_console.sh on its own thread and
 * arms the picture watch that re-enumerates the adapter afterwards. The
 * caller is responsible for deciding the requester may do this. Returns
 * 0 when the work was started. */
int web_stream_wake_console(WebStream *ws);

int web_stream_may_control(WebStream *ws, const char *token);

#endif
