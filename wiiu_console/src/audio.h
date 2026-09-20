#ifndef CAPTURE2WIIU_AUDIO_H
#define CAPTURE2WIIU_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int audio_init(int rate, int channels,
               char *why, size_t why_size);

void audio_exit(void);

/* One Opus packet from C2S. */
void audio_decode(const uint8_t *data, uint32_t size);

void audio_stats(unsigned long *decoded,
                 unsigned long *failed,
                 unsigned long *dropped);

unsigned audio_queue_ms(void);

#ifdef __cplusplus
}
#endif

#endif
