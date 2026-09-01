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
/* The speakers on THIS machine. Two separate questions, because they
 * have separate answers.
 *
 * `set_local_output` is whether they EXIST: headless opens none, and
 * "show capture" turns them on later. Opening and closing a device is
 * not something to do on every click.
 *
 * `set_local_mute` is a stream that is open and silent. Instant, and it
 * keeps the device alive -- a playback stream starved for any length of
 * time is one the server may tear down.
 *
 * What the browser and the console receive is untouched by either: this
 * is a monitor, not the stream.
 *
 * `volume` is 0 to 100, where 25 is the source's own level and 100 is
 * four times it: the same scale as the page and the console client, so a
 * number means the same thing wherever it is read.
 *
 * In headless mode the local output is never opened at all: nobody is
 * sitting at a machine with no screen, and a PulseAudio stream nobody
 * listens to still costs a thread and a device. */
void audio_capture_set_local_output(AudioCapture *ac, int enabled);
void audio_capture_set_local_mute(AudioCapture *ac, int muted);
void audio_capture_set_local_volume(AudioCapture *ac, int volume);

/* `local_output` opens the speakers on this machine. Pass 0 in headless
 * mode: nobody is sitting at a machine with no screen, and the stream is
 * unaffected either way -- this only ever fed a monitor. Not opening it
 * at all is better than opening it and throwing the samples away, which
 * still costs a thread, a PulseAudio stream and a device that shows up
 * in the mixer. */
AudioCapture *audio_capture_start(const char *source, volatile sig_atomic_t *running, WebStream *web,
                                  GstWebrtcStream *gst, int local_output);

/* Waits for the thread to finish. The caller must have cleared the
 * `running` flag first, otherwise this blocks forever. */
void audio_capture_stop(AudioCapture *ac);

#endif
