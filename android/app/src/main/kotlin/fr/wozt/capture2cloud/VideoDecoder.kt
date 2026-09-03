package fr.wozt.capture2cloud

import android.media.MediaCodec
import android.media.MediaFormat
import android.view.Surface
import java.nio.ByteBuffer

/**
 * Hardware video decoding, straight onto the surface the picture is shown
 * on.
 *
 * The frames arrive already encoded and are handed over untouched: no
 * container, no reordering, no copy into a bitmap. Every Android device
 * has an H.264 decoder in silicon, which is why that is the default
 * asked of the host -- VP8 in hardware is common but not universal, and
 * software VP8 at 720p60 on a phone is how a stream starts dropping
 * frames it was given in time.
 *
 * Feeding starts at the first keyframe and not before. A predictive codec
 * resumed mid-stream decodes against frames it never received, and what
 * that looks like is not "a few glitches" but a smeared picture that
 * stays wrong until the next keyframe seconds later.
 */
class VideoDecoder(
    private val surface: Surface,
    private val onNeedKeyframe: () -> Unit,
) {
    private var codec: MediaCodec? = null
    private var awaitingKeyframe = true
    private val info = MediaCodec.BufferInfo()
    private var startedAt = 0L

    val isReady: Boolean get() = codec != null

    /** Builds a decoder for what the host says it is now sending. */
    fun start(width: Int, height: Int, protocolCodec: Int): Boolean {
        stop()
        val mime = when (protocolCodec) {
            Protocol.CODEC_H264 -> MediaFormat.MIMETYPE_VIDEO_AVC
            Protocol.CODEC_VP8 -> MediaFormat.MIMETYPE_VIDEO_VP8
            else -> return false
        }
        return try {
            val format = MediaFormat.createVideoFormat(mime, width, height)
            /* Low latency where the device offers it: without this a
             * decoder is free to hold frames back to smooth playback,
             * which is the right call for a film and the wrong one for
             * something being played. */
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
                format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }
            val c = MediaCodec.createDecoderByType(mime)
            c.configure(format, surface, null, 0)
            c.start()
            codec = c
            awaitingKeyframe = true
            startedAt = System.nanoTime()
            onNeedKeyframe()
            true
        } catch (e: Exception) {
            codec = null
            false
        }
    }

    fun stop() {
        codec?.let {
            try { it.stop() } catch (_: Exception) {}
            try { it.release() } catch (_: Exception) {}
        }
        codec = null
    }

    /**
     * Hands one encoded frame to the decoder and shows whatever comes
     * out.
     *
     * Returns false when the frame was not taken, which is either "still
     * waiting for a keyframe" or "the decoder has no input buffer free".
     * Neither is an error worth reporting: the first resolves itself at
     * the next keyframe, and the second is the decoder saying it is busy,
     * which dropping a frame answers better than queueing one does.
     */
    fun decode(frame: ByteBuffer, isKeyframe: Boolean): Boolean {
        val c = codec ?: return false
        if (awaitingKeyframe) {
            if (!isKeyframe) return false
            awaitingKeyframe = false
        }
        try {
            val index = c.dequeueInputBuffer(0)
            if (index < 0) return false
            val buffer = c.getInputBuffer(index) ?: return false
            buffer.clear()
            if (buffer.remaining() < frame.remaining()) {
                /* Bigger than any input buffer the decoder offers. Not
                 * something to squeeze in: ask for a fresh keyframe and
                 * start again from there. */
                c.queueInputBuffer(index, 0, 0, 0, 0)
                awaitingKeyframe = true
                onNeedKeyframe()
                return false
            }
            val size = frame.remaining()
            buffer.put(frame)
            val us = (System.nanoTime() - startedAt) / 1000
            c.queueInputBuffer(index, 0, size, us,
                               if (isKeyframe) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0)
            drain(c)
            return true
        } catch (e: IllegalStateException) {
            /* The decoder gave up -- a malformed stream, or the device
             * taking its hardware back. Rebuilt by the caller on the next
             * stream-info message; until then, nothing is shown. */
            stop()
            return false
        }
    }

    private fun drain(c: MediaCodec) {
        while (true) {
            val out = c.dequeueOutputBuffer(info, 0)
            if (out < 0) break
            /* true: hand it to the surface. The decoder is configured
             * with one, so there is no pixel copy on this path at all. */
            c.releaseOutputBuffer(out, true)
        }
    }
}
