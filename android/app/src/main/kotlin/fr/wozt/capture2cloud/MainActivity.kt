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
        pad.onState = { padState ->
            val merged = physicalPad.copyOf()
            pad.mergeInto(merged)
            merged.copyInto(this.padState)
            pushPad()
        }
        root.addView(pad, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))

        status = TextView(this).apply {
            setPadding(24, 24, 24, 24)
            setTextColor(0xFFCCDDEE.toInt())
            textSize = 13f
        }
        root.addView(status, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.TOP or Gravity.START))

        /* A double tap in the middle is the whole gesture: one tap does
         * nothing, so a stray touch during a game costs nothing either.
         * The bars come back the same way they went. */
        val taps = GestureDetector(this, object : GestureDetector.SimpleOnGestureListener() {
            override fun onDoubleTap(e: MotionEvent): Boolean {
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

        /* Small, monospaced and fluorescent green, at the top where a
         * game has least going on. Discreet on purpose: it is meant to
         * be readable at a glance without becoming part of the picture. */
        statsLine = TextView(this).apply {
            typeface = android.graphics.Typeface.MONOSPACE
            textSize = 10f
            setTextColor(0xFF39FF14.toInt())
            setPadding(24, 6, 24, 6)
            visibility = if (settings.showStats) View.VISIBLE else View.GONE
        }
        root.addView(statsLine, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.TOP or Gravity.END))
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
                val snapshot = diagnostics.sample(client)
                lastStats = snapshot
                val text = diagnostics.line(snapshot)
                statsLine.text = text
                menu?.refreshStats(text)
                main.postDelayed(this, 1000)
            }
        }
        main.postDelayed(tick, 1000)
    }

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
            main.postDelayed({ hideConnectPanel(); connect() }, 400)
        }
        override fun onStatsChanged() {
            statsLine.visibility = if (settings.showStats) View.VISIBLE else View.GONE
        }
        override fun statsText() =
            lastStats?.let { diagnostics.line(it) } ?: "sampling..."
        override fun onClose() = closeMenu()
        override fun mayControl() = mayControl
        override fun onLogin(password: String) = login(password)
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
                                    settings.fps, settings.bitrateMbps * 1000)
            },
            onVideo = { buf, key ->
                val d = video
                if (d != null) {
                    d.decode(buf, key)
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
            onClosed = { reason -> main.post { onDisconnected(reason) } },
        )
        client = c
        c.connect()
    }

    /** Starts the decoder if there is a stream to decode and nowhere yet
     *  to put it. Cheap and idempotent: called on every frame that
     *  arrives while there is no decoder. */
    private fun retryVideoIfNeeded() {
        if (video != null || !surfaceReady) return
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
        if (video != null) say("$w x $h, playing")
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
                    /* close() lands onClosed, which puts the connection
                     * screen back up; connecting from here would race it.
                     * A moment's wait costs nothing and is honest about
                     * what is happening. */
                    main.postDelayed({ hideConnectPanel(); connect() }, 600)
                } else {
                    say("login refused")
                }
            }
        }
    }

    private fun say(text: String) {
        status.text = text
    }

    override fun onDestroy() {
        super.onDestroy()
        client?.close()
        video?.stop()
        audio?.stop()
    }
}
