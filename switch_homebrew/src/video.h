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

/* H.264 only: the console decodes it on its video engine, and the VP8
 * path this used to offer never produced a picture. */
int  video_init(SDL_Renderer *renderer, int width, int height);
void video_exit(void);

/* Decodes one encoded frame and updates the texture. Returns 1 when a
 * new picture is ready to draw. */
int  video_decode(const uint8_t *data, uint32_t size);

/* Draws the last decoded picture, scaled to fill `dst`. */
void video_draw(SDL_Renderer *renderer, const SDL_Rect *dst);

/* The four adjustments the browser page offers, and they cost very
 * different things.
 *
 * Brightness and contrast are per-channel arithmetic, so the renderer
 * does them and they are free. Saturation and hue mix colour channels
 * into each other, which no blend mode can express -- but the picture is
 * YUV, where both are one 2x2 matrix on the two chroma bytes. That is a
 * quarter of the frame, both settings collapse into a single matrix so
 * using both costs what using one costs, and the pass is skipped
 * entirely while both are at their defaults.
 *
 * brightness/contrast 50..150, saturation 0..200, hue -180..180. */
void video_set_adjust(int brightness, int contrast, int saturation, int hue);
void video_get_adjust(int *brightness, int *contrast, int *saturation, int *hue);

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
