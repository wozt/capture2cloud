package fr.wozt.capture2cloud

import java.io.DataInputStream
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import kotlin.concurrent.thread

/**
 * The direct path: the host's own binary protocol over TCP, the same one
 * the Switch client speaks.
 *
 * Worth having alongside the browser path rather than instead of it. The
 * browser path goes through WebRTC, which brings its own jitter buffer,
 * its own congestion control and its own idea of when to drop a frame --
 * excellent over the open internet and pure overhead on a phone sitting
 * on the same wifi as the host. This one is a socket with frames on it,
 * and what arrives is handed straight to the hardware decoder.
 *
 * Everything here runs on one reader thread. Callbacks are invoked on it,
 * so whatever they touch has to be ready for that: the decoders are, the
 * interface is not, and that is why [onState] hops to the main thread at
 * the call site rather than here.
 */
/** The settings the host says everyone connected is sharing. */
data class Shared(
    val width: Int,
    val height: Int,
    val fps: Int,
    val bitrateKbps: Int,
    val codec: Int,
    val captureMjpeg: Boolean,
)

class DirectClient(
    private val host: String,
    private val port: Int,
    private val token: String?,
    private val onAck: (Ack) -> Unit,
    private val onVideo: (ByteBuffer, Boolean, Boolean) -> Unit,
    private val onAudio: (ByteBuffer) -> Unit,
    private val onStreamInfo: (width: Int, height: Int, codec: Int) -> Unit,
    private val onShared: (Shared) -> Unit = {},
    private val onClosed: (String) -> Unit,
) {
    /** What the host answered the handshake with. */
    data class Ack(
        val accepted: Boolean,
        val mayControl: Boolean,
        val width: Int,
        val height: Int,
        val videoCodec: Int,
        val audioCodec: Int,
        val audioRate: Int,
        val audioChannels: Int,
    )

    private companion object {
        /* How much of a backlog is worth throwing away, in milliseconds
         * of video.
         *
         * Measured in time and not in bytes, which was the mistake: a
         * socket carrying 12 Mb/s routinely holds a hundred kilobytes or
         * more, so a byte threshold was true almost every frame and
         * nearly everything was being discarded -- the stream looked
         * like keyframes only, which is exactly the "never quite becomes
         * smooth" it was meant to cure. Two hundred milliseconds is
         * about twelve frames: comfortably more than any normal buffer
         * and comfortably less than a delay anyone would tolerate.
         */
        const val BACKLOG_MS = 200
    }

    /* Set when the reader should throw video away until it is caught up.
     *
     * Asked for on coming back from the background, where the system
     * slows the app's threads and the socket fills with pictures of a
     * moment that has passed. Decoding those is what "it lags, then it
     * artefacts, then it is fine" is -- the decoder faithfully working
     * through a backlog nobody wants to see. */
    @Volatile private var flushing = false

    @Volatile private var socket: Socket? = null
    @Volatile private var running = false

    /**
     * Everything sent goes through here, and nothing is written on the
     * thread that asked.
     *
     * Android kills a process that touches a socket on the main thread,
     * and almost everything this client sends is asked for from there: a
     * button press arrives as an input event, a keyframe request comes
     * from the decoder being built, a menu sends a wake. One thread, so
     * order is kept, and the caller never blocks on the network.
     */
    private val tx: ExecutorService =
        Executors.newSingleThreadExecutor { r -> Thread(r, "c2c-send").apply { isDaemon = true } }

    /* Counters for the diagnostics line. Written on the reader thread
     * and read from the interface without a lock: a torn long here costs
     * one wrong number on a display that refreshes every second, and a
     * lock on the receive path to protect a statistic would be a poor
     * trade. */
    @Volatile var videoBytes = 0L; private set
    @Volatile var audioBytes = 0L; private set
    @Volatile var videoFrames = 0L; private set

    /** The last state sent, so an unchanged pad is not resent. */
    private val lastPad = ByteArray(Protocol.PAD_SLOTS)
    @Volatile private var lastPadSentAt = 0L

    fun connect() {
        running = true
        thread(name = "c2c-direct", isDaemon = true) { run() }
    }

    fun close() {
        running = false
        tx.shutdownNow()
        try { socket?.close() } catch (_: IOException) {}
        socket = null
    }

    private fun run() {
        try {
            val s = Socket()
            /* Nagle would hold a 21-byte pad state back waiting for
             * company. On a control channel that is exactly the wrong
             * trade. */
            s.tcpNoDelay = true
            s.connect(InetSocketAddress(host, port), 5000)
            socket = s

            val tokenBytes = (token ?: "").toByteArray(Charsets.US_ASCII)
            val hello = ByteBuffer.allocate(8 + tokenBytes.size).order(ByteOrder.LITTLE_ENDIAN)
            hello.putInt(Protocol.MAGIC)
            hello.put(Protocol.VERSION.toByte())
            hello.put(tokenBytes.size.toByte())
            hello.putShort(0)
            hello.put(tokenBytes)
            s.getOutputStream().write(hello.array())
            s.getOutputStream().flush()

            val input = DataInputStream(s.getInputStream())
            val ack = readAck(input)
            onAck(ack)
            if (!ack.accepted) {
                onClosed("the host refused the connection")
                return
            }

            readFrames(input)
        } catch (e: Exception) {
            if (running) onClosed(e.message ?: e.javaClass.simpleName)
        } finally {
            close()
        }
    }

    /**
     * C2sHelloAck, including its trailing padding.
     *
     * The three reserved bytes at the end are not decoration: the C
     * struct is packed and pinned by a _Static_assert, so they are on
     * the wire and have to be consumed or every frame after this is
     * read three bytes out of step.
     */
    private fun readAck(input: DataInputStream): Ack {
        val raw = ByteArray(20)
        input.readFully(raw)
        val b = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN)
        val magic = b.int
        val version = b.get().toInt() and 0xff
        val accepted = (b.get().toInt() and 0xff) != 0
        val mayControl = (b.get().toInt() and 0xff) != 0
        b.get() // reserved
        val width = b.short.toInt() and 0xffff
        val height = b.short.toInt() and 0xffff
        val videoCodec = b.get().toInt() and 0xff
        val audioCodec = b.get().toInt() and 0xff
        val audioRate = b.short.toInt() and 0xffff
        val audioChannels = b.get().toInt() and 0xff
        if (magic != Protocol.MAGIC || version != Protocol.VERSION) {
            throw IOException("not a Capture2Cloud host, or a different protocol version")
        }
        return Ack(accepted, mayControl, width, height, videoCodec, audioCodec,
                   audioRate, audioChannels)
    }

    private fun readFrames(input: DataInputStream) {
        val header = ByteArray(Protocol.HEADER_SIZE)
        var payload = ByteArray(256 * 1024)
        while (running) {
            input.readFully(header)
            val h = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN)
            val type = h.get().toInt() and 0xff
            val flags = h.get().toInt() and 0xff
            h.short // reserved
            val size = h.int
            if (size < 0 || size > 16 * 1024 * 1024) {
                throw IOException("frame of $size bytes: the stream is out of step")
            }
            if (size > payload.size) payload = ByteArray(size)
            if (size > 0) input.readFully(payload, 0, size)

            when (type) {
                Protocol.MSG_VIDEO -> {
                    videoBytes += size
                    videoFrames++
                    /* How far behind we are, measured where it actually
                     * shows: bytes already delivered by the network and
                     * still unread. Decoding every one of those means
                     * displaying a picture that is however old the
                     * backlog is, which is the "lag that builds up" --
                     * it is not the network being slow, it is us being
                     * conscientious about frames that stopped mattering. */
                    val key = (flags and Protocol.FLAG_KEYFRAME) != 0
                    /* Bytes waiting, converted to time using the rate
                     * they are arriving at. Below a floor the estimate
                     * is not worth trusting and nothing is thrown away. */
                    val waiting = input.available()
                    val perSecond = bytesPerSecond()
                    val behind = perSecond > 50_000 &&
                                 waiting * 1000L / perSecond > BACKLOG_MS
                    if (flushing) {
                        /* Caught up once the socket is drained AND a
                         * keyframe has come round: stopping at the first
                         * quiet moment would resume mid-prediction and
                         * show a smear instead of a picture. */
                        if (!behind && key) flushing = false else continue
                    }
                    onVideo(ByteBuffer.wrap(payload, 0, size), key, behind)
                }
                Protocol.MSG_AUDIO -> {
                    audioBytes += size
                    onAudio(ByteBuffer.wrap(payload, 0, size))
                }
                Protocol.MSG_SHARED -> if (size >= 10) {
                    val i = ByteBuffer.wrap(payload, 0, size).order(ByteOrder.LITTLE_ENDIAN)
                    onShared(Shared(
                        width = i.short.toInt() and 0xffff,
                        height = i.short.toInt() and 0xffff,
                        fps = i.short.toInt() and 0xffff,
                        bitrateKbps = i.short.toInt() and 0xffff,
                        codec = i.get().toInt() and 0xff,
                        captureMjpeg = (i.get().toInt() and 0xff) != 0,
                    ))
                }
                Protocol.MSG_STREAM_INFO -> if (size >= 6) {
                    val i = ByteBuffer.wrap(payload, 0, size).order(ByteOrder.LITTLE_ENDIAN)
                    onStreamInfo(i.short.toInt() and 0xffff,
                                 i.short.toInt() and 0xffff,
                                 i.get().toInt() and 0xff)
                }
                else -> { /* anything else is the host talking about
                             something this client does not use yet */ }
            }
        }
    }

    // --- sending -----------------------------------------------------

    private fun send(type: Int, body: ByteArray? = null) {
        val s = socket ?: return
        val size = body?.size ?: 0
        val frame = ByteBuffer.allocate(Protocol.HEADER_SIZE + size).order(ByteOrder.LITTLE_ENDIAN)
        frame.put(type.toByte())
        frame.put(0)
        frame.putShort(0)
        frame.putInt(size)
        if (body != null) frame.put(body)
        val bytes = frame.array()
        try {
            tx.execute {
                try {
                    s.getOutputStream().write(bytes)
                    s.getOutputStream().flush()
                } catch (e: IOException) {
                    if (running) onClosed(e.message ?: "the connection went away")
                }
            }
        } catch (_: java.util.concurrent.RejectedExecutionException) {
            /* Closed while something was still asking. Nothing to do and
             * nothing wrong: the connection is going away by request. */
        }
    }

    /**
     * Sends a pad state, skipping one that says what the last one said.
     *
     * The host de-duplicates too, but not sending is cheaper than sending
     * and being ignored, and this runs at the frame rate. A keepalive
     * still goes out regularly, because a client that says nothing for
     * ten seconds is dropped -- and being dropped for holding still would
     * be a poor way to lose a game.
     */
    fun sendPad(state: ByteArray) {
        require(state.size == Protocol.PAD_SLOTS)
        val now = System.currentTimeMillis()
        if (state.contentEquals(lastPad) && now - lastPadSentAt < 500) return
        state.copyInto(lastPad)
        lastPadSentAt = now
        send(Protocol.MSG_INPUT, state)
    }

    /**
     * A rolling estimate of how fast video is arriving, in bytes per
     * second. Used to read a byte backlog as a delay.
     */
    private var rateWindowStart = 0L
    private var rateBaseBytes = 0L
    private var rateEstimate = 0L

    /**
     * How fast video is arriving, in bytes per second, over the last
     * second or so. Used to read a byte backlog as a delay.
     */
    private fun bytesPerSecond(): Long {
        val now = System.currentTimeMillis()
        if (rateWindowStart == 0L) {
            rateWindowStart = now
            rateBaseBytes = videoBytes
            return 0
        }
        val elapsed = now - rateWindowStart
        if (elapsed >= 1000) {
            rateEstimate = (videoBytes - rateBaseBytes) * 1000 / elapsed
            rateWindowStart = now
            rateBaseBytes = videoBytes
        }
        return rateEstimate
    }

    /** Throw away buffered video until caught up at a keyframe. */
    fun flushBacklog() {
        flushing = true
    }

    fun sendPing() = send(Protocol.MSG_PING)
    fun sendHome() = send(Protocol.MSG_HOME)
    fun requestKeyframe() = send(Protocol.MSG_KEYFRAME)
    fun sendWake() = send(Protocol.MSG_WAKE)
    fun sendResetDongle() = send(Protocol.MSG_RESET_DONGLE)
    fun sendRestart() = send(Protocol.MSG_RESTART)

    fun sendCodec(codec: Int) = send(Protocol.MSG_CODEC, byteArrayOf(codec.toByte()))

    /** C2sProfile: what this client asks the host to encode for it. */
    fun sendProfile(width: Int, height: Int, fps: Int, bitrateKbps: Int) {
        val b = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
        b.putShort(width.toShort()); b.putShort(height.toShort())
        b.putShort(fps.toShort()); b.putShort(bitrateKbps.toShort())
        send(Protocol.MSG_PROFILE, b.array())
    }
}
