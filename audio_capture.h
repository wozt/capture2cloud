#ifndef CAPTURE2CLOUD_AUDIO_CAPTURE_H
#define CAPTURE2CLOUD_AUDIO_CAPTURE_H

#include <signal.h>

#include "gst_webrtc.h"
#include "web_stream.h"

/*
 * Audio path: reads the capture card's audio from PulseAudio, cleans it
 * up, plays it back locally, and feeds the WebRTC stream.
 *
 * The cleanup exists because these HDMI-to-USB capture sticks inject a
 * strong mains hum plus a DC offset into their audio: a high-pass, a
 * bank of notch filters on the hum's harmonics, and a noise gate. See
 * audio_capture.c for why each is there.
 *
 * Runs on its own thread: PulseAudio reads block, and neither the video
 * loop nor the GTK interface can afford to wait on them.
 */

typedef struct AudioCapture AudioCapture;

/* Starts the capture thread. `source` is a PulseAudio source name, or
 * NULL for the server default. `running` is the shared flag the thread
 * polls to know when to stop -- the caller owns it and clears it at
 * shutdown. `web`/`gst` are used to decide whether to forward audio to
 * the stream (only while it's actually running) and where to push it;
 * both may be NULL to capture and play back locally only.
 *
 * Returns NULL if the thread could not be started. Failing to open the
 * audio device is NOT an error here: the thread reports it and exits,
 * leaving video working -- audio is not worth aborting the app for. */
AudioCapture *audio_capture_start(const char *source, volatile sig_atomic_t *running, WebStream *web,
                                  GstWebrtcStream *gst);

/* Waits for the thread to finish. The caller must have cleared the
 * `running` flag first, otherwise this blocks forever. */
void audio_capture_stop(AudioCapture *ac);

#endif
