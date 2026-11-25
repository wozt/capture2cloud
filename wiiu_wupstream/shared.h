#ifndef C2C_WIIU_WUPSTREAM_SHARED_H
#define C2C_WIIU_WUPSTREAM_SHARED_H
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>

#define C2C_WUP_W 854
#define C2C_WUP_H 480
#define C2C_WUP_VIDEO_SIZE ((size_t)C2C_WUP_W * C2C_WUP_H * 4u)
#define C2C_WUP_VIDEO_SLOTS 3
#define C2C_WUP_AUDIO_FRAMES 8192u

typedef struct {
    uint32_t buttons;
    int lx, ly, rx, ry;
    int touched, tx, ty;
} C2cWupInput;

typedef struct {
    _Atomic int running;
    _Atomic int connected;
    _Atomic unsigned frame_generation;
    /* One-second snapshot, written by the WUP helper, read by main thread.
     * All rates are in tenths; duration is microseconds. No GUI locks. */
    _Atomic unsigned stats_tx_fps10;
    _Atomic unsigned stats_new_fps10;
    _Atomic unsigned stats_capture_fps10;
    _Atomic unsigned stats_repeat;
    _Atomic unsigned stats_drop;
    _Atomic unsigned stats_error;
    _Atomic unsigned stats_frame_us;
    _Atomic unsigned stats_scale_us;
    _Atomic int video_state[C2C_WUP_VIDEO_SLOTS]; /* 0 free; 1 writing; 2 ready; 3 reading */
    unsigned video_sequence[C2C_WUP_VIDEO_SLOTS];
    unsigned char video[C2C_WUP_VIDEO_SLOTS][C2C_WUP_VIDEO_SIZE];
    _Atomic unsigned audio_write;
    _Atomic unsigned audio_read;
    int16_t audio[C2C_WUP_AUDIO_FRAMES * 2];
    _Atomic unsigned input_seq; /* even=stable; odd=writer active */
    C2cWupInput input;
} C2cWupShared;
#endif
