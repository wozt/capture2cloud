#ifndef CAPTURE2SWITCH_VIDEO_H
#define CAPTURE2SWITCH_VIDEO_H

#include <stdint.h>

#include <SDL2/SDL.h>

/*
 * VP8 decoding, straight into an SDL texture.
 *
 * Decoded on NVDEC, the Tegra X1's video engine, through the nvtegra
 * backend that devkitPro's ffmpeg carries. An earlier note here claimed
 * that engine was unreachable and that software decoding was the only
 * option; it was wrong, and it was the reason this could not hold 30 fps
 * at 360p while three CPU cores did work the hardware does for free.
 *
 * libvpx remains as a fallback, because a client that shows nothing is
 * worse than one that shows a slow picture.
 *
 * Either way the planes reach the texture untouched -- NV12 from the
 * engine, I420 from libvpx -- so the colour conversion happens on the
 * GPU during the draw rather than on the CPU for every pixel.
 */

typedef enum { VIDEO_CODEC_VP8 = 1, VIDEO_CODEC_H264 = 3 } VideoCodec;

int  video_init(SDL_Renderer *renderer, int width, int height, int codec);
void video_exit(void);

/* Decodes one encoded frame and updates the texture. Returns 1 when a
 * new picture is ready to draw. */
int  video_decode(const uint8_t *data, uint32_t size);

/* Draws the last decoded picture, scaled to fill `dst`. */
void video_draw(SDL_Renderer *renderer, const SDL_Rect *dst);

/* Frames decoded and frames the decoder refused, for the menu. A stream
 * that connects but shows nothing is otherwise indistinguishable from
 * one that never arrived. */
void video_stats(unsigned long *decoded, unsigned long *failed);

/* Which decoder actually opened. Worth showing: the difference between
 * the hardware engine and the software fallback is the difference
 * between a smooth picture and a slideshow, and nothing else on screen
 * would say which one is running. */
const char *video_decoder_name(void);

#endif
