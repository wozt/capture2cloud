package fr.wozt.capture2cloud

import android.annotation.SuppressLint
import android.graphics.ColorMatrix
import android.graphics.ColorMatrixColorFilter
import android.graphics.Paint
import android.view.GestureDetector
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.graphics.SurfaceTexture
import android.view.Surface
import android.view.TextureView
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import java.nio.ByteBuffer
import kotlin.concurrent.thread

/**
 * The stream, and the little that has to sit on top of it.
 *
 * Two ways in, chosen in the connection screen and not negotiated: the
 * direct path, which is the host's own protocol over a socket, and the
 * HTTP path, which is what the page does. Direct is the default on a
 * phone -- on the same wifi as the host, WebRTC's jitter buffer and
 * congestion control are latency spent solving a problem that is not
 * there.
 */
private const val BITRATE_FLOOR = 1500
private const val BITRATE_MAX = 20000

class MainActivity : AppCompatActivity(), TextureView.SurfaceTextureListener {

    private lateinit var settings: Settings
    private lateinit var root: FrameLayout
    private lateinit var view: TextureView
    private var surface: Surface? = null
    private lateinit var status: TextView
    private var connectPanel: View? = null
    private var menu: SettingsPanel? = null
    private lateinit var pad: VirtualPad
    private var videoWidth = 0
    private var videoHeight = 0
    private var immersive = false
    private lateinit var diagnostics: Diagnostics
    private lateinit var statsLine: TextView
    private var lastStats: Diagnostics.Snapshot? = null

    private var client: DirectClient? = null
    private var video: VideoDecoder? = null
    private var audio: AudioPlayer? = null
    private val main = Handler(Looper.getMainLooper())

    /** What a physical controller is doing, in the host's slot order. */
    private val physicalPad = ByteArray(Protocol.PAD_SLOTS)
    /** That merged with the on-screen pad: what actually goes out. */
    private val padState = ByteArray(Protocol.PAD_SLOTS)
    @Volatile private var mayControl = false
    @Volatile private var surfaceReady = false
    @Volatile private var pendingStream: Triple<Int, Int, Int>? = null
    /* Which connection attempt is current.
     *
     * Closing a client makes its onClosed fire some time later, on its
     * own thread. Reconnecting immediately -- which is what changing the
     * host or logging in does -- meant the OLD connection's closure
     * arrived after the NEW one had already put the picture back, and
     * put the connection screen up over it. The pad went with it, which
     * is why the controls "took a while to come back": they had been
     * hidden by a message about a connection that no longer existed. */
    private var generation = 0
    private var lastLate = 0L
    private var autoCeilingKbps = BITRATE_MAX
    @Volatile private var foreground = true
    private var bitrateBeforePause = 0
    private var lastVideoAttempt = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = Settings(this)
        diagnostics = Diagnostics(this)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        root = FrameLayout(this)
        /* A TextureView rather than a SurfaceView.
         *
         * A SurfaceView is the cheaper of the two -- its own layer, no
         * copy through the view hierarchy -- and it is what this used.
         * But on this hardware the OMX H.264 decoder refuses that
         * surface outright: configure() throws an IllegalArgumentException
         * with no message, which reads exactly like a device that cannot
         * decode H.264, on a device that decodes it in silicon. The same
         * format with no surface at all is accepted, which is how the
         * surface was identified as the half at fault.
         *
         * A TextureView's surface comes from a SurfaceTexture, which is
         * the path every decoder accepts. It costs one GPU copy per
         * frame; a picture that costs a copy beats no picture. */
        view = TextureView(this).also { it.surfaceTextureListener = this }
        root.addView(view, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))

        /* Over the picture and under everything else. Its own view, so
         * the touches it wants do not fight the double tap that toggles
         * the bars: it consumes what lands on a control and lets the
         * rest through. */
        pad = VirtualPad(this, settings)
        pad.layoutFromString(settings.padLayout)
        pad.onLayoutChanged = { settings.padLayout = pad.layoutToString() }
        pad.onState = { padState ->
            val merged = physicalPad.copyOf()
            pad.mergeInto(merged)
            merged.copyInto(this.padState)
            pushPad()
        }
        root.addView(pad, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))

        /* Both in the top-left corner, one under the other: that is
         * where the eye already goes for this, and a second corner for
         * a second line of the same kind of information would only make
         * it two places to look. */
        val corner = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 16, 24, 16)
        }
        statsLine = TextView(this).apply {
            typeface = android.graphics.Typeface.MONOSPACE
            textSize = 10f
            setTextColor(0xFF39FF14.toInt())
            visibility = if (settings.showStats) View.VISIBLE else View.GONE
        }
        /* One line, not two. The resolution belonged with the rest of
         * the numbers rather than beside them, and it should switch off
         * with them: something permanently drawn over a game is part of
         * the game's picture whether it is wanted or not. */
        status = TextView(this).apply {
            setTextColor(0xFFCCDDEE.toInt())
            textSize = 12f
        }
        corner.addView(statsLine)
        corner.addView(status)
        root.addView(corner, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.TOP or Gravity.START))

        /* A double tap in the middle is the whole gesture: one tap does
         * nothing, so a stray touch during a game costs nothing either.
         * The bars come back the same way they went. */
        val taps = GestureDetector(this, object : GestureDetector.SimpleOnGestureListener() {
            override fun onDoubleTap(e: MotionEvent): Boolean {
                /* Not near a control. A thumb that misses a button lands
                 * right beside it, and hiding the system bars is a poor
                 * thing to get for a missed press. */
                if (pad.isNearControl(e.x, e.y)) return false
                immersive = !immersive
                applyImmersive()
                return true
            }
            override fun onDown(e: MotionEvent) = true
        })
        root.setOnTouchListener { _, e -> taps.onTouchEvent(e) }

        /* The first fit happens before the root has a size, so it does
         * nothing; this runs it again once there is one, and after every
         * rotation. */
        root.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> fitVideo() }

        startStatsTicker()

        setContentView(root)
        showConnectPanel()
        startPinger()
        applyPicture()
    }

    /**
     * Samples once a second and feeds both places the numbers appear.
     *
     * One timer for the overlay and the menu together: two would drift
     * apart and show different figures for the same second, which is the
     * kind of thing that makes someone doubt both.
     */
    private fun startStatsTicker() {
        val tick = object : Runnable {
            override fun run() {
                val link = when {
                    client == null -> "offline"
                    mayControl -> "player"
                    else -> "viewer"
                }
                val d = video
                val stream = when {
                    d == null || !d.isReady -> ""
                    else -> "${videoWidth}x$videoHeight ${codecShortName()} " +
                            (if (d.codecIsHardware) "hw" else "sw") +
                            (if (settings.bitrateIsAuto) " auto ${autoBitrateKbps / 1000}M" else "")
                }
                /* Says why there is no sound when there is none: a
                 * missing decoder, a decoder that refused, or one that
                 * is being fed and returns nothing are three different
                 * faults that look identical from a chair. */
                val a = audio
                val audioState = when {
                    a == null -> " (no decoder)"
                    a.lastError.isNotEmpty() -> " (${a.lastError})"
                    a.buffersOut > 0 -> ""
                    a.packetsIn > 0 -> " (fed, none out)"
                    else -> " (nothing queued)"
                }
                val snapshot = diagnostics.sample(client, link, stream, audioState, skipped, video?.shown ?: 0)
                lastStats = snapshot
                val text = diagnostics.line(snapshot)
                statsLine.text = text
                /* Also to the log, always: the line on screen shows the
                 * present moment, and questions about how something
                 * recovers are questions about a sequence of moments. */
                /* The transient line clears itself once there is a
                 * picture: it is for saying what went wrong, not for
                 * sitting on the screen afterwards. */
                if (video?.isReady == true && status.text.isNotEmpty()) status.text = ""
                steerBitrate(snapshot)
                menu?.refreshStats(text)
                main.postDelayed(this, 1000)
            }
        }
        main.postDelayed(tick, 1000)
    }

    /* Automatic bitrate: what is being asked for right now, and what
     * the last second looked like. */
    private var autoBitrateKbps = 8000
    private var lastDropped = 0L
    /** Frames thrown away to catch up, for the diagnostics line. */
    private var skipped = 0L
    private var goodSeconds = 0

    private fun requestedBitrateKbps(): Int =
        if (settings.bitrateIsAuto) autoBitrateKbps else settings.bitrateMbps * 1000

    /**
     * Moves the requested bitrate towards what this link and this
     * decoder can actually carry.
     *
     * Down fast, up slowly, which is the only sensible asymmetry: the
     * cost of asking for too much is a picture that breaks up now, and
     * the cost of asking for too little is a picture that is softer than
     * it could be. Dropped frames are the signal rather than the arrival
     * rate, because a link that cannot carry the stream shows up first
     * as a decoder that cannot keep up with what does arrive.
     *
     * A change is only sent when it is worth sending: nudging the host's
     * encoder every second would cost more in restarts than it gains.
     */
    private fun steerBitrate(snapshot: Diagnostics.Snapshot) {
        if (!settings.bitrateIsAuto || client == null) return
        /* Not while there is no window. Nothing is being displayed, so
         * every reading says the stream is failing -- and the loop, left
         * running, spent a minute in the background winding a perfectly
         * good eight megabits down to its floor, which is what the
         * picture then came back at. */
        if (!foreground) return

        /* Two signals, and they mean different things.
         *
         * Frames the decoder refused are the loud one: the phone could
         * not keep up at all. Frames thrown away as late are the quiet
         * one, and for a long time the only one that ever fired -- the
         * decoder never runs short of buffers, it simply falls behind,
         * so a controller watching refusals alone saw a clean run at
         * every rate and climbed until the picture broke. That is what
         * made a stream that was smooth at eight megabits come back from
         * the background at twelve and stutter.
         *
         * Lateness alone is no better as a trigger: used that way it
         * collapsed to the floor, because the lateness left over from a
         * resume does not answer to the bitrate and the loop kept
         * paying for it. So lateness does not lower the rate directly --
         * it lowers the CEILING, and the rate is simply not allowed
         * above what has been seen to work. */
        val dropped = video?.dropped ?: 0
        val newlyDropped = (dropped - lastDropped).coerceAtLeast(0)
        lastDropped = dropped
        val late = skipped
        val newlyLate = (late - lastLate).coerceAtLeast(0)
        lastLate = late

        val before = autoBitrateKbps
        if (newlyDropped > 2) {
            autoBitrateKbps = (autoBitrateKbps * 3 / 4).coerceAtLeast(BITRATE_FLOOR)
            autoCeilingKbps = autoBitrateKbps
            goodSeconds = 0
        } else if (newlyLate > 5) {
            /* Behind at this rate. Remember it as too much and settle
             * just under it, rather than chasing the rate downwards. */
            autoCeilingKbps = (autoBitrateKbps * 9 / 10).coerceAtLeast(BITRATE_FLOOR)
            autoBitrateKbps = autoBitrateKbps.coerceAtMost(autoCeilingKbps)
            goodSeconds = 0
        } else if (newlyLate == 0L && snapshot.fps > 0) {
            goodSeconds++
            if (goodSeconds >= 5) {
                goodSeconds = 0
                /* Clean for a while: try a little more, and let the
                 * ceiling itself drift up slowly so a link that really
                 * did get better is not held down by one bad minute. */
                autoCeilingKbps = (autoCeilingKbps * 21 / 20).coerceAtMost(BITRATE_MAX)
                autoBitrateKbps = (autoBitrateKbps * 11 / 10)
                    .coerceAtMost(minOf(autoCeilingKbps, BITRATE_MAX))
            }
        }
        if (kotlin.math.abs(autoBitrateKbps - before) * 100 / before >= 10) {
            client?.sendProfile(settings.height * 16 / 9, settings.height,
                                settings.fps, autoBitrateKbps)
        }
    }

    private fun codecShortName() =
        if (settings.codec == Protocol.CODEC_H264) "h264" else "vp8"

    private fun applyImmersive() {
        val controller = window.insetsController
        if (immersive) {
            controller?.hide(android.view.WindowInsets.Type.systemBars())
            controller?.systemBarsBehavior =
                android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        } else {
            controller?.show(android.view.WindowInsets.Type.systemBars())
        }
    }

    // --- the menu, and what the back button now means ------------------

    /**
     * Back opens the menu, and back again leaves.
     *
     * Leaving on the first press was too easy to do by accident with a
     * gesture bar during a game, and there was nowhere else for a menu
     * to live on a screen that is meant to be all picture.
     */
    override fun onBackPressed() {
        if (menu != null) {
            /* Asked for explicitly: the second press leaves. The menu
             * has its own way back to the picture, at the top where a
             * thumb already is, so this is not the only exit -- it is
             * the deliberate one. */
            @Suppress("DEPRECATION")
            super.onBackPressed()
        } else if (client != null) {
            openMenu()
        } else {
            @Suppress("DEPRECATION")
            super.onBackPressed()
        }
    }

    private fun openMenu() {
        val panel = SettingsPanel(this, settings, menuActions)
        menu = panel
        pad.visibility = View.GONE
        root.addView(panel, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
    }

    private fun closeMenu() {
        menu?.let { root.removeView(it) }
        menu = null
        if (connectPanel == null) pad.visibility = View.VISIBLE
        /* Opacity or visibility may have changed while it was open. */
        pad.invalidate()
    }

    private val menuActions = object : SettingsPanel.Actions {
        override fun onStreamShapeChanged() {
            client?.sendCodec(settings.codec)
            client?.sendProfile(settings.height * 16 / 9, settings.height,
                                settings.fps, settings.bitrateMbps * 1000)
        }
        override fun onPictureChanged() = applyPicture()
        override fun onPadChanged() {
            /* The pad reads its own settings when it lays out, so a
             * change of letters, shape or opacity has to make it do
             * that again. */
            pad.requestLayout()
            pad.invalidate()
        }
        override fun onSoundChanged() {
            audio?.volume = settings.volume
            audio?.muted = settings.muted
        }
        override fun onHome() { client?.sendHome() }
        override fun onWake() { client?.sendWake() }
        override fun onResetDongle() { client?.sendResetDongle() }
        override fun onRestartHost() { client?.sendRestart() }
        override fun onDisconnect() {
            closeMenu()
            client?.close()
        }
        override fun onHostChanged() {
            /* A new address is a new connection: the old one is to the
             * old host, and nothing about it can be redirected. */
            closeMenu()
            client?.close()
            hideConnectPanel()
            connect()
        }
        override fun onStatsChanged() {
            statsLine.visibility = if (settings.showStats) View.VISIBLE else View.GONE
        }
        override fun statsText() =
            lastStats?.let { diagnostics.line(it) } ?: "sampling..."
        override fun onEditPad(on: Boolean) { pad.editing = on }
        override fun onResetPadLayout() {
            pad.resetLayout()
            settings.padLayout = ""
        }
        override fun isEditingPad() = pad.editing
        override fun onClose() = closeMenu()
        override fun mayControl() = mayControl
        override fun onLogin(password: String) = login(password)
        override fun onBackToViewer() {
            /* The token is dropped, not just ignored: what makes a
             * client a player is what it presents in the handshake, so
             * going back to watching means reconnecting without it. */
            settings.token = ""
            closeMenu()
            client?.close()
            hideConnectPanel()
            connect()
        }
    }

    // --- the connection screen ---------------------------------------

    @SuppressLint("SetTextI18n")
    private fun showConnectPanel() {
        val panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(0xE00B1A26.toInt())
            setPadding(48, 48, 48, 48)
        }
        val host = EditText(this).apply {
            hint = "host, e.g. 192.168.2.100"
            setText(settings.host)
            setTextColor(0xFFFFFFFF.toInt())
        }
        val port = EditText(this).apply {
            hint = "port"
            setText(settings.directPort.toString())
            setTextColor(0xFFFFFFFF.toInt())
        }
        val pathButton = Button(this).apply {
            text = pathLabel()
            setOnClickListener {
                settings.path = if (settings.path == Settings.Path.DIRECT)
                    Settings.Path.HTTP else Settings.Path.DIRECT
                text = pathLabel()
                port.setText(if (settings.path == Settings.Path.DIRECT)
                    settings.directPort.toString() else settings.webPort.toString())
            }
        }
        val go = Button(this).apply {
            text = "connect"
            setOnClickListener {
                settings.host = host.text.toString().trim()
                val p = port.text.toString().trim().toIntOrNull() ?: 0
                if (settings.path == Settings.Path.DIRECT) settings.directPort = p
                else settings.webPort = p
                if (settings.host.isEmpty()) {
                    Toast.makeText(this@MainActivity, "a host is needed", Toast.LENGTH_SHORT).show()
                } else {
                    hideConnectPanel()
                    connect()
                }
            }
        }
        panel.addView(TextView(this).apply {
            text = "Capture2Cloud"
            textSize = 22f
            setTextColor(0xFF5CE8FF.toInt())
        })
        panel.addView(host); panel.addView(port); panel.addView(pathButton); panel.addView(go)
        connectPanel = panel
        pad.visibility = View.GONE
        root.addView(panel, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
    }

    private fun pathLabel() =
        if (settings.path == Settings.Path.DIRECT) "path: direct (host's own protocol)"
        else "path: http (WebRTC, like the page)"

    private fun hideConnectPanel() {
        connectPanel?.let { root.removeView(it) }
        connectPanel = null
        pad.visibility = View.VISIBLE
    }

    // --- the surface --------------------------------------------------

    /* surfaceCreated is deliberately not where the decoder starts.
     *
     * It only promises the Surface object exists. MediaCodec wants one
     * that has been through a layout and has a size and a format, and
     * given anything less it refuses the whole configuration with an
     * IllegalArgumentException carrying no message -- which reads
     * exactly like "this device cannot decode H.264", on a device that
     * decodes H.264 in hardware. Proven by configuring the same format
     * with no surface at all, which the same device accepts happily.
     *
     * surfaceChanged is the callback that promises a real size, so that
     * is where this waits. */
    override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
        /* Coming back from the background with a socket full of stale
         * pictures. Throw them away rather than decode them: what is
         * wanted is now, not the last thirty frames of then. */
        client?.flushBacklog()
        client?.requestKeyframe()
        surface = Surface(texture)
        surfaceReady = true
        lastVideoAttempt = 0L
        pendingStream?.let { (w, h, codec) -> if (video == null) startVideo(w, h, codec) }
    }

    override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) {}

    override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
        surfaceReady = false
        video?.stop(); video = null
        surface?.release(); surface = null
        return true
    }

    override fun onSurfaceTextureUpdated(texture: SurfaceTexture) {}

    // --- connecting ---------------------------------------------------

    private fun connect() {
        generation++
        val mine = generation
        if (settings.path == Settings.Path.HTTP) {
            say("the http path is not built yet -- use direct")
            showConnectPanel()
            return
        }
        say("connecting to ${settings.host}:${settings.directPort}...")
        val c = DirectClient(
            host = settings.host,
            port = settings.directPort,
            token = settings.token.ifEmpty { null },
            onAck = { ack ->
                if (mine != generation) return@DirectClient
                mayControl = ack.mayControl
                main.post {
                    say("${ack.width}x${ack.height}, " +
                        (if (ack.videoCodec == Protocol.CODEC_H264) "h264" else "vp8") +
                        (if (ack.mayControl) " -- player" else " -- viewer"))
                    startAudio(ack.audioRate, ack.audioChannels)
                    pendingStream = Triple(ack.width, ack.height, ack.videoCodec)
                    if (surfaceReady) startVideo(ack.width, ack.height, ack.videoCodec)
                }
                /* Ask for what this device wants rather than taking what
                 * the last client asked for: the host encodes one native
                 * stream and the previous client may have wanted
                 * something this one cannot decode well. */
                client?.sendCodec(settings.codec)
                client?.sendProfile(settings.height * 16 / 9, settings.height,
                                    settings.fps, requestedBitrateKbps())
            },
            onVideo = { buf, key, behind ->
                val d = video
                if (d != null) {
                    /* Behind, and this is not a keyframe: throw it away
                     * rather than decode a picture that is already old.
                     * Keyframes are always taken, so catching up ends at
                     * the next one rather than at a black screen. */
                    if (behind && !key) {
                        skipped++
                        d.requestKeyframe()
                    } else {
                        d.decode(buf, key)
                    }
                } else {
                    /* No decoder yet, or the last attempt was refused
                     * because the surface was between two lives -- a
                     * rotation does exactly that, and this app starts in
                     * portrait and turns. Retried here rather than once
                     * at connection time: frames are arriving, so there
                     * is no better moment to notice the surface is ready
                     * than when one does. */
                    main.post { retryVideoIfNeeded() }
                }
            },
            onAudio = { buf -> audio?.decode(buf) },
            onStreamInfo = { w, h, codec ->
                main.post {
                    pendingStream = Triple(w, h, codec)
                    if (surfaceReady) startVideo(w, h, codec)
                }
            },
            onClosed = { reason ->
                main.post { if (mine == generation) onDisconnected(reason) }
            },
        )
        client = c
        c.connect()
    }

    /** Starts the decoder if there is a stream to decode and nowhere yet
     *  to put it. Cheap and idempotent: called on every frame that
     *  arrives while there is no decoder. */
    private fun retryVideoIfNeeded() {
        /* Also retried when the decoder died on its own: the software
         * VP8 one does that at 720p and above, and a decoder that has
         * released itself leaves an object that is no longer ready. */
        if (video?.isReady == true || !surfaceReady) return
        video?.stop()
        /* Once a second at most. Retrying on every frame meant building
         * and tearing down a decoder sixty times a second, which is a
         * way of turning one failure into a resource shortage. */
        val now = System.currentTimeMillis()
        if (now - lastVideoAttempt < 1000) return
        lastVideoAttempt = now
        val (w, h, codec) = pendingStream ?: return
        startVideo(w, h, codec)
    }

    /**
     * Sizes the picture to the largest box of its own shape that fits.
     *
     * The view filled the screen before, which stretched a 16:9 stream
     * onto a 20:9 phone -- everything a little too wide, which is the
     * kind of wrong that is easier to feel than to see. Letterboxed
     * instead: black at the sides rather than a distorted picture.
     */
    private fun fitVideo() {
        if (videoWidth <= 0 || videoHeight <= 0) return
        val availableW = root.width
        val availableH = root.height
        if (availableW <= 0 || availableH <= 0) return
        val scale = minOf(availableW.toFloat() / videoWidth, availableH.toFloat() / videoHeight)
        val w = (videoWidth * scale).toInt()
        val h = (videoHeight * scale).toInt()
        view.layoutParams = FrameLayout.LayoutParams(w, h, Gravity.CENTER)
        view.requestLayout()
    }

    /**
     * Brightness, contrast, saturation and hue, as one colour matrix on
     * the view's own layer.
     *
     * The same four the page offers, and applied the same way it applies
     * them: to what is drawn, not to what is sent, so a picture adjusted
     * here stays adjusted here and nobody else's changes.
     */
    private fun applyPicture() {
        val b = settings.brightness / 100f
        val c = settings.contrast / 100f
        val sat = settings.saturation / 100f
        val hue = Math.toRadians(settings.hue.toDouble())

        val m = ColorMatrix()
        m.setSaturation(sat)

        /* Hue is a rotation of the colour plane. Written out rather than
         * pulled from a library so the three coefficients are visible:
         * they are the standard luminance weights, which is why a
         * rotated grey stays grey. */
        val cosv = Math.cos(hue).toFloat()
        val sinv = Math.sin(hue).toFloat()
        val lr = 0.213f; val lg = 0.715f; val lb = 0.072f
        val hueMatrix = ColorMatrix(floatArrayOf(
            lr + cosv * (1 - lr) - sinv * lr, lg - cosv * lg - sinv * lg, lb - cosv * lb + sinv * (1 - lb), 0f, 0f,
            lr - cosv * lr + sinv * 0.143f, lg + cosv * (1 - lg) + sinv * 0.140f, lb - cosv * lb - sinv * 0.283f, 0f, 0f,
            lr - cosv * lr - sinv * (1 - lr), lg - cosv * lg + sinv * lg, lb + cosv * (1 - lb) + sinv * lb, 0f, 0f,
            0f, 0f, 0f, 1f, 0f))
        m.postConcat(hueMatrix)

        /* Contrast pivots around mid grey, then brightness scales: the
         * order matters, and this is the order the page's filters use. */
        val t = (1f - c) * 127.5f
        m.postConcat(ColorMatrix(floatArrayOf(
            c * b, 0f, 0f, 0f, t,
            0f, c * b, 0f, 0f, t,
            0f, 0f, c * b, 0f, t,
            0f, 0f, 0f, 1f, 0f)))

        val identity = settings.brightness == 100 && settings.contrast == 100 &&
                       settings.saturation == 100 && settings.hue == 0
        if (identity) {
            /* Nothing to apply, so nothing is applied: no layer, no
             * filter, no per-frame cost for a picture nobody adjusted. */
            view.setLayerType(View.LAYER_TYPE_NONE, null)
        } else {
            view.setLayerType(View.LAYER_TYPE_HARDWARE, Paint().apply {
                colorFilter = ColorMatrixColorFilter(m)
            })
        }
    }

    private fun startVideo(w: Int, h: Int, codec: Int) {
        videoWidth = w
        videoHeight = h
        fitVideo()
        video?.stop()
        val target = surface ?: return
        val d = VideoDecoder(target) { client?.requestKeyframe() }
        video = if (d.start(w, h, codec)) d else null
        /* Nothing said on success: the green line carries the resolution
         * now, and a second line saying the same thing was the point of
         * the complaint. */
        if (video == null) say("no decoder for that stream")
    }

    private fun startAudio(rate: Int, channels: Int) {
        audio?.stop()
        val a = AudioPlayer(rate, channels)
        a.volume = settings.volume
        a.muted = settings.muted
        audio = if (a.start()) a else null
    }

    private fun onDisconnected(reason: String) {
        say("disconnected: $reason")
        client = null
        video?.stop(); video = null
        audio?.stop(); audio = null
        if (connectPanel == null) showConnectPanel()
    }

    /**
     * A ping every couple of seconds, whatever else is happening.
     *
     * The host drops a client that has said nothing for ten seconds, and
     * a client watching quietly says nothing at all -- being dropped for
     * holding still would be a poor way to lose a game.
     */
    private fun startPinger() {
        thread(isDaemon = true, name = "c2c-ping") {
            while (true) {
                Thread.sleep(2000)
                client?.sendPing()
            }
        }
    }

    // --- a physical controller, if there is one -----------------------

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean =
        padKey(keyCode, 100) || super.onKeyDown(keyCode, event)

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean =
        padKey(keyCode, 0) || super.onKeyUp(keyCode, event)

    private fun padKey(keyCode: Int, value: Int): Boolean {
        val slot = when (keyCode) {
            KeyEvent.KEYCODE_BUTTON_A -> Protocol.A
            KeyEvent.KEYCODE_BUTTON_B -> Protocol.B
            KeyEvent.KEYCODE_BUTTON_X -> Protocol.X
            KeyEvent.KEYCODE_BUTTON_Y -> Protocol.Y
            KeyEvent.KEYCODE_BUTTON_L1 -> Protocol.LB
            KeyEvent.KEYCODE_BUTTON_R1 -> Protocol.RB
            KeyEvent.KEYCODE_BUTTON_THUMBL -> Protocol.LS
            KeyEvent.KEYCODE_BUTTON_THUMBR -> Protocol.RS
            KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_MENU -> Protocol.START
            KeyEvent.KEYCODE_BUTTON_SELECT -> Protocol.BACK
            KeyEvent.KEYCODE_DPAD_UP -> Protocol.UP
            KeyEvent.KEYCODE_DPAD_DOWN -> Protocol.DOWN
            KeyEvent.KEYCODE_DPAD_LEFT -> Protocol.LEFT
            KeyEvent.KEYCODE_DPAD_RIGHT -> Protocol.RIGHT
            else -> return false
        }
        physicalPad[slot] = value.toByte()
        mergeAndPush()
        return true
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.source and android.view.InputDevice.SOURCE_JOYSTICK == 0) {
            return super.onGenericMotionEvent(event)
        }
        physicalPad[Protocol.LX] = axis(event, MotionEvent.AXIS_X, 0)
        /* No inversion: Android's AXIS_Y is already negative when the
         * stick is pushed up, which is the sign the host wants -- the
         * page sends exactly that. Inverting here made a physical stick
         * disagree with the on-screen one, and both disagree with the
         * page. */
        physicalPad[Protocol.LY] = axis(event, MotionEvent.AXIS_Y, 0)
        physicalPad[Protocol.RX] = axis(event, MotionEvent.AXIS_Z, 1)
        physicalPad[Protocol.RY] = axis(event, MotionEvent.AXIS_RZ, 1,
                                        invert = settings.invertRy)
        physicalPad[Protocol.LT] = trigger(event, MotionEvent.AXIS_LTRIGGER, settings.ltThreshold)
        physicalPad[Protocol.RT] = trigger(event, MotionEvent.AXIS_RTRIGGER, settings.rtThreshold)
        mergeAndPush()
        return true
    }

    /**
     * One stick axis, put through the same shaping the page and the
     * console client use: a dead zone at the centre, a range that says
     * how far counts as fully pushed, and a separate range for the
     * corners because a stick reaches less far diagonally than along an
     * axis, by an amount that differs from one stick to the next.
     */
    private fun axis(event: MotionEvent, which: Int, stick: Int, invert: Boolean = false): Byte {
        var v = event.getAxisValue(which)
        if (invert) v = -v
        val dead = settings.deadzone(stick) / 100f
        val range = settings.range(stick) / 100f
        val magnitude = kotlin.math.abs(v)
        if (magnitude <= dead) return 0
        val scaled = ((magnitude - dead) / (range - dead)).coerceIn(0f, 1f)
        val out = (scaled * 100f * if (v < 0) -1 else 1).toInt()
        return out.coerceIn(-100, 100).toByte()
    }

    private fun trigger(event: MotionEvent, which: Int, thresholdPercent: Int): Byte {
        val v = event.getAxisValue(which)
        val threshold = thresholdPercent / 100f
        if (v <= threshold) return 0
        return (((v - threshold) / (1f - threshold)) * 100f).toInt().coerceIn(0, 100).toByte()
    }

    /** Both sources, combined the way the host combines its own. */
    private fun mergeAndPush() {
        physicalPad.copyInto(padState)
        pad.mergeInto(padState)
        /* The overlay shows what the real pad is doing too, so with a
         * controller in hand it becomes a picture of that controller. */
        pad.setMirror(physicalPad)
        pushPad()
    }

    private fun pushPad() {
        if (!mayControl) return
        client?.sendPad(padState)
    }

    /**
     * Asks the host for a session token, then reconnects with it.
     *
     * The password is used here and nowhere else -- not stored, not
     * logged. What is kept is the token, which the host expires on its
     * own terms; that is the same bargain the page makes, and it is why
     * losing this file costs a login rather than a password.
     *
     * The reconnect is not ceremony: whether a client may control is
     * decided in the handshake, so a token that arrives afterwards does
     * nothing until the next one.
     */
    private fun login(password: String) {
        if (password.isEmpty()) { say("a password is needed"); return }
        say("logging in...")
        thread(isDaemon = true, name = "c2c-login") {
            val token = try {
                val url = java.net.URL("http://${settings.host}:${settings.webPort}/login")
                (url.openConnection() as java.net.HttpURLConnection).run {
                    requestMethod = "POST"
                    doOutput = true
                    connectTimeout = 5000
                    readTimeout = 5000
                    outputStream.use { it.write(password.toByteArray()) }
                    if (responseCode == 200) inputStream.bufferedReader().readText().trim() else ""
                }
            } catch (e: Exception) {
                ""
            }
            main.post {
                if (token.length == 64) {
                    settings.token = token
                    say("logged in, reconnecting as player...")
                    closeMenu()
                    client?.close()
                    /* Straight into the new connection. The old one's
                     * closure is ignored when it lands, because it
                     * belongs to a previous generation. */
                    hideConnectPanel()
                    connect()
                } else {
                    say("login refused")
                }
            }
        }
    }

    private fun say(text: String) {
        status.text = text
    }

    /**
     * Silent in the background, and audible again on return.
     *
     * Muted rather than stopped: the decoder keeps draining, which keeps
     * the socket moving and means there is no audio backlog to work
     * through on the way back -- the same problem the video had, solved
     * by never letting it build. Restarting the decoder instead would
     * cost a rebuild for something that only needs to be quiet.
     *
     * The stream itself is untouched: this is one client going quiet,
     * not the host, so nobody else's sound changes.
     */
    override fun onPause() {
        super.onPause()
        audio?.muted = true
        foreground = false
        if (settings.bitrateIsAuto) bitrateBeforePause = autoBitrateKbps
        /* Only while there is something to keep alive: a notification
         * for an app that is not connected to anything would be a lie. */
        if (client != null) StreamService.start(this)
    }

    override fun onResume() {
        super.onResume()
        StreamService.stop(this)
        audio?.muted = settings.muted
        /* A ceiling learned while the window was gone describes a
         * situation that no longer exists. Coming back with it still in
         * force is how the picture returned at one megabit and stayed
         * there. */
        autoCeilingKbps = BITRATE_MAX
        goodSeconds = 0
        lastLate = skipped
        lastDropped = video?.dropped ?: 0
        /* Back to what was working before, rather than climbing there
         * ten percent at a time from the floor. */
        if (settings.bitrateIsAuto && bitrateBeforePause > 0) {
            autoBitrateKbps = bitrateBeforePause
            client?.sendProfile(settings.height * 16 / 9, settings.height,
                                settings.fps, autoBitrateKbps)
        }
        foreground = true
        /* Whatever piled up while away is not worth watching. */
        client?.flushBacklog()
        client?.requestKeyframe()
    }

    override fun onDestroy() {
        super.onDestroy()
        StreamService.stop(this)
        client?.close()
        video?.stop()
        audio?.stop()
    }
}
