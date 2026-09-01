#ifndef CAPTURE2SWITCH_AUDIO_H
#define CAPTURE2SWITCH_AUDIO_H

#include <stdint.h>

/*
 * Opus decoding and playback.
 *
 * Queued rather than pulled from a callback: packets arrive when the
 * network delivers them, not on the audio device's clock, and a callback
 * would have to invent samples whenever the two disagreed.
 */

int  audio_init(int rate, int channels);
void audio_exit(void);

/* One encoded Opus packet from the host. */
void audio_decode(const uint8_t *data, uint32_t size);

void audio_set_muted(int muted);
int  audio_is_muted(void);
/* 0 to 100, where 100 is four times the stream's own level and 25 is
 * that level itself. The scale reads 0-100 because nobody thinks of
 * volume as a number above a hundred; what it can reach is unchanged
 * from a scale that said 400. */
void audio_set_volume(int percent);
int  audio_volume(void);

void audio_stats(unsigned long *decoded, unsigned long *failed);

#endif
