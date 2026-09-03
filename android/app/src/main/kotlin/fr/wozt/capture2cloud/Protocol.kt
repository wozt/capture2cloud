package fr.wozt.capture2cloud

/**
 * The host's own protocol, as declared in c2s_protocol.h.
 *
 * Transcribed rather than invented: the C header is the single definition
 * and this file has to follow it, so the constants below carry the same
 * names and the same numbers. Anything added there has to be added here,
 * and the sizes are pinned by _Static_assert on the C side -- if a struct
 * below is the wrong length, the host's handshake will not line up and
 * that is the symptom to look for.
 *
 * Everything is little-endian, because both ends have always been.
 */
object Protocol {
    const val MAGIC = 0x57533243   // "C2SW"
    const val VERSION = 1

    /** How many slots a controller state carries. */
    const val PAD_SLOTS = 21

    // Message types, client -> server unless said otherwise.
    const val MSG_VIDEO = 1        // server -> client, one encoded frame
    const val MSG_AUDIO = 2        // server -> client, one encoded packet
    const val MSG_INPUT = 16       // PAD_SLOTS bytes
    const val MSG_PING = 17
    const val MSG_HOME = 18
    const val MSG_PROFILE = 19     // what this client wants encoded for it
    const val MSG_CODEC = 20       // which video codec to send
    const val MSG_STREAM_INFO = 21 // server -> client, the stream changed shape
    const val MSG_KEYFRAME = 22
    const val MSG_WAKE = 23
    const val MSG_RESET_DONGLE = 24
    const val MSG_RESTART = 25
    /* server -> client: the settings this client does not own alone --
     * the stream's shape, its codec, its bitrate, the capture format.
     * Told rather than asked for, so a client that joins adopts what
     * everyone else is already watching instead of changing it. */
    const val MSG_SHARED = 26

    const val FLAG_KEYFRAME = 0x01

    const val CODEC_VP8 = 1
    const val CODEC_OPUS = 2
    const val CODEC_H264 = 3

    /** Header size: type, flags, reserved, size. */
    const val HEADER_SIZE = 8

    /**
     * The slot each button and axis lives in.
     *
     * These are the host's GAMEPAD_XB360_* indices, in its order, with the
     * same -100..100 range. Naming them for an Xbox pad is historical and
     * not a claim about the console: the adapter translates, and the
     * positions are what carry across. Slot 19 is the button under your
     * thumb wherever you are.
     */
    const val GUIDE = 0
    const val BACK = 1
    const val START = 2
    const val RB = 3
    const val RT = 4
    const val RS = 5
    const val LB = 6
    const val LT = 7
    const val LS = 8
    const val RX = 9
    const val RY = 10
    const val LX = 11
    const val LY = 12
    const val UP = 13
    const val DOWN = 14
    const val LEFT = 15
    const val RIGHT = 16
    const val Y = 17
    const val B = 18
    const val A = 19
    const val X = 20
}
