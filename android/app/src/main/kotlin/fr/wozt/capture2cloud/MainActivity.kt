package fr.wozt.capture2cloud

import android.annotation.SuppressLint
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
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
class MainActivity : AppCompatActivity(), SurfaceHolder.Callback {

    private lateinit var settings: Settings
    private lateinit var root: FrameLayout
    private lateinit var surface: SurfaceView
    private lateinit var status: TextView
    private var connectPanel: View? = null

    private var client: DirectClient? = null
    private var video: VideoDecoder? = null
    private var audio: AudioPlayer? = null
    private val main = Handler(Looper.getMainLooper())

    /** The merged pad state, in the host's slot order. */
    private val pad = ByteArray(Protocol.PAD_SLOTS)
    @Volatile private var mayControl = false
    @Volatile private var surfaceReady = false
    @Volatile private var pendingStream: Triple<Int, Int, Int>? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = Settings(this)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        root = FrameLayout(this)
        surface = SurfaceView(this).also { it.holder.addCallback(this) }
        root.addView(surface, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))

        status = TextView(this).apply {
            setPadding(24, 24, 24, 24)
            setTextColor(0xFFCCDDEE.toInt())
            textSize = 13f
        }
        root.addView(status, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.TOP or Gravity.START))

        setContentView(root)
        showConnectPanel()
        startPinger()
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
        root.addView(panel, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
    }

    private fun pathLabel() =
        if (settings.path == Settings.Path.DIRECT) "path: direct (host's own protocol)"
        else "path: http (WebRTC, like the page)"

    private fun hideConnectPanel() {
        connectPanel?.let { root.removeView(it) }
        connectPanel = null
    }

    // --- the surface --------------------------------------------------

    override fun surfaceCreated(holder: SurfaceHolder) {
        surfaceReady = true
        /* A stream announced before there was anywhere to draw it: start
         * the decoder now rather than losing the announcement. */
        pendingStream?.let { (w, h, codec) -> startVideo(w, h, codec) }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        surfaceReady = false
        video?.stop()
        video = null
    }

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
            onVideo = { buf, key -> video?.decode(buf, key) },
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

    private fun startVideo(w: Int, h: Int, codec: Int) {
        video?.stop()
        val d = VideoDecoder(surface.holder.surface) { client?.requestKeyframe() }
        video = if (d.start(w, h, codec)) d else null
        if (video == null) say("no decoder for that stream on this device")
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
        pad[slot] = value.toByte()
        pushPad()
        return true
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.source and android.view.InputDevice.SOURCE_JOYSTICK == 0) {
            return super.onGenericMotionEvent(event)
        }
        pad[Protocol.LX] = axis(event, MotionEvent.AXIS_X, 0)
        pad[Protocol.LY] = axis(event, MotionEvent.AXIS_Y, 0, invert = true)
        pad[Protocol.RX] = axis(event, MotionEvent.AXIS_Z, 1)
        pad[Protocol.RY] = axis(event, MotionEvent.AXIS_RZ, 1,
                                invert = !settings.invertRy)
        pad[Protocol.LT] = trigger(event, MotionEvent.AXIS_LTRIGGER, settings.ltThreshold)
        pad[Protocol.RT] = trigger(event, MotionEvent.AXIS_RTRIGGER, settings.rtThreshold)
        pushPad()
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

    private fun pushPad() {
        if (!mayControl) return
        client?.sendPad(pad)
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
