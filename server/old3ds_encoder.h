#ifndef CAPTURE2CLOUD_OLD3DS_ENCODER_H
#define CAPTURE2CLOUD_OLD3DS_ENCODER_H

#include <stddef.h>
#include <stdint.h>

typedef struct Old3dsEncoder Old3dsEncoder;

#define OLD3DS_VIDEO_WIDTH 400
#define OLD3DS_VIDEO_HEIGHT 240
#define OLD3DS_VIDEO_FPS 30
#define OLD3DS_VIDEO_BITRATE_KBPS 1200

/* A completely independent 400x240 JPEG / MPEG-1/2/4 encoder for the
 * Old 3DS/2DS client.  The returned buffer belongs to the encoder and
 * stays valid until the next encode call. */
Old3dsEncoder *old3ds_encoder_create(void);
void old3ds_encoder_destroy(Old3dsEncoder *encoder);
void old3ds_encoder_set_bitrate(Old3dsEncoder *encoder, int bitrate_kbps);
void old3ds_encoder_set_codec(Old3dsEncoder *encoder, uint8_t codec);
void old3ds_encoder_request_keyframe(Old3dsEncoder *encoder);

int old3ds_encoder_encode(Old3dsEncoder *encoder,
                          const uint8_t *const planes[3],
                          const int strides[3],
                          int av_pixel_format,
                          int width,
                          int height,
                          const uint8_t **output,
                          uint32_t *output_size,
                          int *keyframe);

#endif
