package fr.wozt.capture2cloud

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Opus in, speakers out.
 *
 * The host sends bare Opus packets -- no Ogg, no container, just what the
 * encoder produced. MediaCodec's Opus decoder will take those, but only
 * after being told what they are, and that description is not in the
 * stream: it has to be supplied as codec-specific data. So the OpusHead
 * below is written by hand from what the handshake reported, which is the
 * one piece of this file that is not obvious and the one worth reading
 * twice.
 *
 * Volume is applied to the samples rather than to the AudioTrack. The
 * track's own gain stops at 1.0, and the scale here runs to eight times
 * the stream's level -- the same scale as the page and the console
 * client, so a given number sounds the same wherever it is played. A
 * stream that arrives quiet is the normal case, not the exception.
 */
class AudioPlayer(
    private val sampleRate: Int,
    private val channels: Int,
) {
    private var codec: MediaCodec? = null
    private var track: AudioTrack? = null
    private val info = MediaCodec.BufferInfo()
    private var presentationUs = 0L

    /* Counted so "there is no sound" can be told apart from "nothing is
     * arriving", "nothing is being queued" and "nothing comes back out"
     * -- three different faults that look identical from a chair. */
    @Volatile var packetsIn = 0L
        private set
    @Volatile var buffersOut = 0L
        private set
    @Volatile var lastError: String = ""
        private set

    /** 0..100, where 100 is [MAX_GAIN] times the source. */
    @Volatile var volume: Int = 13
    @Volatile var muted: Boolean = false

    companion object {
        /** Matches LOCAL_VOLUME_MAX_GAIN on the host and the page's. */
        const val MAX_GAIN = 8

        /** Opus always decodes at 48 kHz whatever it was captured at. */
        private const val OPUS_RATE = 48000

        /**
         * The 3840 samples Opus reserves at the head of a stream, and the
         * 80 ms of pre-roll it wants before a seek. Both are given in
         * nanoseconds, which is what MediaCodec asks for, and both are
         * the values the specification fixes rather than choices.
         */
        private const val PRE_SKIP_SAMPLES = 3840L
        private const val SEEK_PREROLL_NS = 80_000_000L
    }

    fun start(): Boolean = try {
        val out = if (channels >= 2) AudioFormat.CHANNEL_OUT_STEREO
                  else AudioFormat.CHANNEL_OUT_MONO
        val minBuffer = AudioTrack.getMinBufferSize(
            OPUS_RATE, out, AudioFormat.ENCODING_PCM_16BIT)
        val t = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_GAME)
                .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                .build())
            .setAudioFormat(AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(OPUS_RATE)
                .setChannelMask(out)
                .build())
            /* Twice the minimum: enough that a scheduling hiccup costs
             * latency instead of a hole, and little enough that the sound
             * stays with the picture. */
            .setBufferSizeInBytes(minBuffer * 2)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        t.play()
        track = t

        val format = MediaFormat.createAudioFormat(
            MediaFormat.MIMETYPE_AUDIO_OPUS, OPUS_RATE, channels)
        format.setByteBuffer("csd-0", ByteBuffer.wrap(opusHead()))
        format.setByteBuffer("csd-1", nanos(PRE_SKIP_SAMPLES * 1_000_000_000L / OPUS_RATE))
        format.setByteBuffer("csd-2", nanos(SEEK_PREROLL_NS))
        val c = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS)
        c.configure(format, null, null, 0)
        c.start()
        codec = c
        true
    } catch (e: Exception) {
        /* Logged, because a silent failure here means silence, which is
         * indistinguishable from a stream that carries no sound. */
        lastError = e.message ?: e.javaClass.simpleName
        Log.w("Capture2Cloud", "no Opus decoder on this device", e)
        stop()
        false
    }

    fun stop() {
        codec?.let {
            try { it.stop() } catch (_: Exception) {}
            try { it.release() } catch (_: Exception) {}
        }
        codec = null
        presentationUs = 0L
        track?.let {
            try { it.stop() } catch (_: Exception) {}
            try { it.release() } catch (_: Exception) {}
        }
        track = null
    }

    fun decode(packet: ByteBuffer) {
        val c = codec ?: return
        try {
            /* A short wait rather than none. Asking with no timeout at
             * all returns nothing most of the time, and audio arrives
             * far too steadily to skip a packet whenever the decoder
             * happens to be busy for a millisecond. */
            /* Output first, for the same reason as the video: a
             * decoder that is not drained runs out of input buffers and
             * then nothing can be queued at all, which is how this
             * shipped silent. */
            drain(c)
            val index = c.dequeueInputBuffer(2000)
            if (index >= 0) {
                val buffer = c.getInputBuffer(index)
                /* Once an input buffer has been taken it is ALWAYS given
                 * back, even empty. Abandoning one on a failed check
                 * leaks it: after a handful the decoder has none left,
                 * dequeue returns nothing for ever, and the sound stops
                 * with no error anywhere -- which is exactly how this
                 * shipped silent. */
                /* Whole packets only, for the same reason as the
                 * video: half an Opus packet is not a shorter packet,
                 * it is a corrupt one. An Opus packet is a few hundred
                 * bytes, so this should never fire -- but silently
                 * feeding rubbish if it ever did is not a risk worth
                 * carrying for the sake of one branch. */
                val size = if (buffer != null && buffer.capacity() >= packet.remaining()) {
                    buffer.clear()
                    buffer.put(packet.duplicate())
                    packet.remaining()
                } else 0
                /* Opus at 48 kHz in 20 ms packets. The timestamps have
                 * to advance: given zero for every packet the decoder
                 * has no idea how the stream is paced. */
                c.queueInputBuffer(index, 0, size, presentationUs, 0)
                presentationUs += 20_000
                packetsIn++
            }
            drain(c)
        } catch (e: IllegalStateException) {
            Log.w("Capture2Cloud", "audio decoder gave up", e)
            stop()
        }
    }

    private fun drain(c: MediaCodec) {
        val t = track ?: return
        while (true) {
            val out = c.dequeueOutputBuffer(info, 0)
            if (out < 0) break
            buffersOut++
            val buffer = c.getOutputBuffer(out)
            if (buffer != null && info.size > 0) {
                val pcm = ByteArray(info.size)
                buffer.position(info.offset)
                buffer.get(pcm, 0, info.size)
                applyGain(pcm)
                t.write(pcm, 0, pcm.size)
            }
            c.releaseOutputBuffer(out, false)
        }
    }

    /**
     * Scales in place, clipping rather than wrapping: above the source's
     * own level a loud passage has nowhere to go, and wrapping would turn
     * it into noise instead of merely flattening it.
     */
    private fun applyGain(pcm: ByteArray) {
        val gain = if (muted) 0 else volume * MAX_GAIN
        if (gain == 100) return
        val b = ByteBuffer.wrap(pcm).order(ByteOrder.LITTLE_ENDIAN)
        val shorts = b.asShortBuffer()
        for (i in 0 until shorts.limit()) {
            var v = shorts.get(i).toInt() * gain / 100
            if (v > 32767) v = 32767
            if (v < -32768) v = -32768
            shorts.put(i, v.toShort())
        }
    }

    /** The 19-byte OpusHead the decoder needs and the stream does not carry. */
    private fun opusHead(): ByteArray {
        val b = ByteBuffer.allocate(19).order(ByteOrder.LITTLE_ENDIAN)
        b.put("OpusHead".toByteArray(Charsets.US_ASCII))
        b.put(1)                              // version
        b.put(channels.toByte())
        b.putShort(PRE_SKIP_SAMPLES.toShort())
        b.putInt(sampleRate)                  // the rate it was captured at
        b.putShort(0)                         // output gain, none
        b.put(0)                              // mapping family: plain mono/stereo
        return b.array()
    }

    private fun nanos(value: Long): ByteBuffer =
        ByteBuffer.allocate(8).order(ByteOrder.nativeOrder()).putLong(value).apply { flip() }
}
