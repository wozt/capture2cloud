package fr.wozt.capture2cloud

import android.media.MediaCodec
import android.media.MediaFormat
import android.util.Log
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
    /** So a retry every frame does not fill the log with the same line. */
    private var loggedFailure = false
    private var pendingMime: String? = null
    private var pendingWidth = 0
    private var pendingHeight = 0

    /** True once there is somewhere for frames to go. */
    val isReady: Boolean get() = codec != null

    /**
     * Notes what the host is sending. For VP8 the decoder is built right
     * away; for H.264 it waits for the first keyframe.
     *
     * That wait is not caution, it is a requirement of this hardware.
     * Configured with a surface, the OMX H.264 decoders that many phones
     * still use refuse a format that does not carry the stream's
     * parameter sets -- configure() throws an IllegalArgumentException
     * with no message, which reads exactly like a device that cannot
     * decode H.264. The same format with no surface is accepted, because
     * buffer mode is lenient and picks the parameters up in flight. So
     * the sets are taken out of the first keyframe and handed over up
     * front, which is what a container would have done.
     */
    fun start(width: Int, height: Int, protocolCodec: Int): Boolean {
        stop()
        pendingMime = when (protocolCodec) {
            Protocol.CODEC_H264 -> MediaFormat.MIMETYPE_VIDEO_AVC
            Protocol.CODEC_VP8 -> MediaFormat.MIMETYPE_VIDEO_VP8
            else -> return false
        }
        pendingWidth = width
        pendingHeight = height
        onNeedKeyframe()
        if (pendingMime == MediaFormat.MIMETYPE_VIDEO_VP8) {
            return configure(null, null)
        }
        return true   // built at the first keyframe, once its SPS is in hand
    }

    private fun configure(sps: ByteArray?, pps: ByteArray?): Boolean {
        val mime = pendingMime ?: return false
        if (!surface.isValid) return false
        var candidate: MediaCodec? = null
        return try {
            val format = MediaFormat.createVideoFormat(mime, pendingWidth, pendingHeight)
            if (sps != null) format.setByteBuffer("csd-0", java.nio.ByteBuffer.wrap(sps))
            if (pps != null) format.setByteBuffer("csd-1", java.nio.ByteBuffer.wrap(pps))
            val c = MediaCodec.createDecoderByType(mime)
            candidate = c
            c.configure(format, surface, null, 0)
            c.start()
            codec = c
            candidate = null
            awaitingKeyframe = false
            startedAt = System.nanoTime()
            true
        } catch (e: Exception) {
            /* Release what was built before the failure: a MediaCodec
             * refused at configure() still holds the surface, and every
             * later attempt is then refused for a different reason than
             * the first. */
            candidate?.let { try { it.release() } catch (_: Exception) {} }
            if (!loggedFailure) {
                loggedFailure = true
                Log.w("Capture2Cloud", "decoder for $mime ${pendingWidth}x$pendingHeight refused", e)
            }
            codec = null
            false
        }
    }

    /**
     * Finds the SPS and PPS in an Annex B frame.
     *
     * The stream carries them in band, before each keyframe, separated
     * by start codes. Both are handed over including their start code,
     * because that is the form a container would have produced and the
     * form the decoder expects.
     */
    private fun parameterSets(frame: ByteArray, size: Int): Pair<ByteArray, ByteArray>? {
        var sps: ByteArray? = null
        var pps: ByteArray? = null
        var i = 0
        var nalStart = -1
        var nalType = 0
        while (i + 3 < size) {
            val isStart4 = frame[i].toInt() == 0 && frame[i + 1].toInt() == 0 &&
                           frame[i + 2].toInt() == 0 && frame[i + 3].toInt() == 1
            val isStart3 = frame[i].toInt() == 0 && frame[i + 1].toInt() == 0 &&
                           frame[i + 2].toInt() == 1
            if (isStart4 || isStart3) {
                val headerLen = if (isStart4) 4 else 3
                if (nalStart >= 0) {
                    val slice = frame.copyOfRange(nalStart, i)
                    if (nalType == 7 && sps == null) sps = slice
                    if (nalType == 8 && pps == null) pps = slice
                }
                nalStart = i
                nalType = frame[i + headerLen].toInt() and 0x1f
                i += headerLen
                if (sps != null && pps != null) break
            } else {
                i++
            }
        }
        if (nalStart >= 0 && (sps == null || pps == null)) {
            val slice = frame.copyOfRange(nalStart, size)
            if (nalType == 7 && sps == null) sps = slice
            if (nalType == 8 && pps == null) pps = slice
        }
        val a = sps ?: return null
        val b = pps ?: return null
        return a to b
    }

    fun stop() {
        loggedFailure = false
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
        /* No decoder yet means H.264 waiting for its parameter sets. The
         * first keyframe carries them; anything before it is of no use
         * to a decoder that does not exist. */
        if (codec == null) {
            if (!isKeyframe) return false
            val bytes = ByteArray(frame.remaining())
            frame.duplicate().get(bytes)
            val sets = parameterSets(bytes, bytes.size) ?: return false
            if (!configure(sets.first, sets.second)) return false
        }
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
