package fr.wozt.capture2cloud

import android.media.MediaCodec
import android.media.MediaFormat
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit

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

    private class Pending(val bytes: ByteArray, val key: Boolean)

    /**
     * A short queue, and a thread of its own to empty it.
     *
     * This exists because of one measurement. Handing frames straight to
     * the decoder from the socket reader looks simple and costs nothing
     * -- until releaseOutputBuffer blocks, which it does whenever the
     * TextureView is not yet consuming what it is given, as happens for
     * a moment every time the window comes back. The reader was then
     * held for hundreds of milliseconds at a stretch, a hundred and
     * fifty kilobytes piled up in the socket behind it, and the backlog
     * that piled up was read as "the link is behind" -- so frames were
     * thrown away, the bitrate was wound down, and none of it touched
     * the actual cause, which was the display taking its time.
     *
     * Nine frames is about a seventh of a second at sixty. Deep enough
     * to ride out a stall in rendering, shallow enough that riding one
     * out never becomes latency worth watching.
     */
    private val queue = ArrayBlockingQueue<Pending>(9)
    @Volatile private var pumping = false
    private var pump: Thread? = null

    /**
     * What is actually decoding, and whether it is silicon.
     *
     * Worth surfacing rather than assuming: asking for a decoder by mime
     * type gets whatever the device offers first, which for VP8 is
     * frequently the software one. A stream that struggles at a size the
     * hardware would have taken in its stride is not a mystery once this
     * is on screen.
     */
    var codecName: String = ""
        private set
    var codecIsHardware: Boolean = false
        private set

    companion object {
        /**
         * Whether this device can decode `mime` in silicon.
         *
         * Worth asking before offering a codec rather than after
         * watching it fail: a phone with no hardware VP8 decoder gets
         * Google's software one, which on this hardware shows artefacts
         * at 720p and gives up entirely at 1080p60, reporting err -14
         * and releasing itself. That is a property of the device and it
         * can be read, rather than discovered the hard way.
         */
        fun hasHardwareDecoder(mime: String): Boolean {
            val list = android.media.MediaCodecList(
                android.media.MediaCodecList.REGULAR_CODECS)
            for (info in list.codecInfos) {
                if (info.isEncoder) continue
                if (!info.supportedTypes.any { it.equals(mime, ignoreCase = true) }) continue
                val hardware =
                    if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.Q)
                        info.isHardwareAccelerated
                    else !info.name.startsWith("OMX.google.") &&
                         !info.name.startsWith("c2.android.")
                if (hardware) return true
            }
            return false
        }
    }

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
            /* VP8 needs no parameter sets, so the decoder is built now
             * -- but it still must not be fed until a keyframe arrives.
             * Handing a software VP8 decoder an inter-frame that refers
             * to a picture it never received is not a glitch it rides
             * out: it errors and releases itself, which is why VP8 went
             * black here while the same phone's browser played it. */
            if (!configure(null, null)) return false
            awaitingKeyframe = true
            return true
        }
        return true   // built at the first keyframe, once its SPS is in hand
    }

    private fun configure(sps: ByteArray?, pps: ByteArray?): Boolean {
        val mime = pendingMime ?: return false
        if (!surface.isValid) return false
        var candidate: MediaCodec? = null
        return try {
            val format = MediaFormat.createVideoFormat(mime, pendingWidth, pendingHeight)
            /* Ask for input buffers big enough for the largest frame the
             * stream can produce.
             *
             * Left to itself a decoder sizes these for what it expects,
             * and the software VP8 decoder expects modest ones. A 1080p
             * keyframe at 12 Mb/s does not fit, and a frame that does not
             * fit was being truncated -- which hands the decoder a
             * corrupt frame and gets err -14 back. That is the whole
             * reason VP8 "did not work here" while the same phone's
             * browser played it perfectly: the browser does not truncate
             * anything, and neither should this. Two megabytes covers a
             * 1080p keyframe with room to spare and costs a buffer that
             * is mostly unused, which is a trade worth making. */
            format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 2 * 1024 * 1024)
            if (sps != null) format.setByteBuffer("csd-0", java.nio.ByteBuffer.wrap(sps))
            if (pps != null) format.setByteBuffer("csd-1", java.nio.ByteBuffer.wrap(pps))
            val c = MediaCodec.createDecoderByType(mime)
            candidate = c
            c.configure(format, surface, null, 0)
            c.start()
            codec = c
            candidate = null
            codecName = c.codecInfo.name
            codecIsHardware =
                if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.Q)
                    c.codecInfo.isHardwareAccelerated
                else !codecName.startsWith("OMX.google.") && !codecName.startsWith("c2.android.")
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
        pumping = false
        pump?.interrupt()
        pump = null
        queue.clear()
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
    /**
     * Asks for a keyframe, at most once a second.
     *
     * Every dropped frame wants one, and a decoder that is behind drops
     * several a second: asking each time floods the host with requests
     * and fills the stream with keyframes, which are the largest frames
     * there are, which makes it further behind.
     */
    private var lastKeyframeRequest = 0L

    /** Frames the decoder had no room for. The signal auto bitrate uses. */
    @Volatile var dropped = 0L
        private set

    /**
     * Frames actually handed to the surface.
     *
     * The rate that matters, and not the one that was being reported:
     * counting frames as they ARRIVE says the network is fine while the
     * screen shows a slideshow, which is precisely the state that needed
     * noticing.
     */
    @Volatile var shown = 0L
        private set

    /** Public, so a caller that skipped a frame can ask for a fresh
     *  start without reaching into the rate limiter itself. */
    fun requestKeyframe() = requestKeyframeSparingly()

    /**
     * Takes a frame from the network thread and returns immediately.
     *
     * The bytes are copied because the caller reuses its buffer for the
     * next frame the moment this returns -- which is the whole point.
     */
    fun decode(frame: ByteBuffer, isKeyframe: Boolean): Boolean {
        val bytes = ByteArray(frame.remaining())
        frame.duplicate().get(bytes)
        startPump()
        if (!queue.offer(Pending(bytes, isKeyframe))) {
            /* Full: the decoder is genuinely behind. A keyframe is worth
             * making room for, since everything after it depends on it;
             * anything else is worth less than the delay it would add. */
            if (isKeyframe) {
                queue.poll()
                queue.offer(Pending(bytes, true))
            } else {
                dropped++
                requestKeyframeSparingly()
                return false
            }
        }
        return true
    }

    private fun startPump() {
        if (pumping) return
        pumping = true
        pump = Thread({
            while (pumping) {
                val next = try {
                    queue.poll(50, TimeUnit.MILLISECONDS)
                } catch (_: InterruptedException) {
                    null
                } ?: continue
                try {
                    decodeNow(ByteBuffer.wrap(next.bytes), next.key)
                } catch (e: Exception) {
                    Log.w("VideoDecoder", "decode failed: ${'$'}e")
                }
            }
        }, "video-decode").apply { isDaemon = true; start() }
    }

    private fun requestKeyframeSparingly() {
        val now = System.currentTimeMillis()
        if (now - lastKeyframeRequest < 1000) return
        lastKeyframeRequest = now
        onNeedKeyframe()
    }

    private fun decodeNow(frame: ByteBuffer, isKeyframe: Boolean): Boolean {
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
            /* Output first, then input.
             *
             * A decoder that is not drained runs out of input buffers,
             * and then nothing can be queued at all: the picture stops
             * with no error anywhere. Draining first also means the
             * buffer asked for below is one this call just freed, which
             * is why the wait after it can be short.
             */
            drain(c)

            val index = c.dequeueInputBuffer(4000)
            if (index < 0) {
                /* Busy for the moment. The frame is lost, and a
                 * predictive codec will show that until the next
                 * keyframe -- so ask for one, but do not stop decoding
                 * in the meantime. Refusing everything until a keyframe
                 * arrives turns a few artefacts into five seconds of
                 * nothing, which is how the picture disappeared. */
                dropped++
                requestKeyframeSparingly()
                return false
            }
            val buffer = c.getInputBuffer(index)
            val remaining = frame.remaining()
            /* Once taken, always given back -- an abandoned input buffer
             * is gone for good -- but never given back HALF a frame. A
             * truncated frame is not a smaller frame, it is a corrupt
             * one, and a decoder handed one of those does not degrade
             * gracefully: it errors out and releases itself. So a frame
             * that will not fit is dropped whole, with an empty buffer
             * returned in its place and a keyframe asked for. */
            if (buffer == null || buffer.capacity() < remaining) {
                c.queueInputBuffer(index, 0, 0, 0, 0)
                Log.w("Capture2Cloud",
                      "frame of $remaining bytes exceeds the input buffer" +
                      " (${buffer?.capacity() ?: 0}); dropped whole")
                requestKeyframeSparingly()
                return false
            }
            buffer.clear()
            buffer.put(frame.duplicate())
            val us = (System.nanoTime() - startedAt) / 1000
            c.queueInputBuffer(index, 0, remaining, us,
                               if (isKeyframe) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0)
            return true
        } catch (e: IllegalStateException) {
            /* The decoder gave up. That is not always a broken stream:
             * the software VP8 decoder on some phones simply fails at
             * 720p and above, reports err -14 and releases itself. The
             * codec is dropped here and the caller builds another,
             * because staying black for ever is the one response that
             * helps nobody. */
            Log.w("Capture2Cloud", "decoder stopped, will rebuild", e)
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
            shown++
        }
    }
}
