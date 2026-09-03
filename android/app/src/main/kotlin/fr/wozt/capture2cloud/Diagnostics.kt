package fr.wozt.capture2cloud

import android.content.Context
import android.hardware.input.InputManager
import android.hardware.usb.UsbManager
import android.view.InputDevice

/**
 * What the stream and the controllers are doing, in numbers.
 *
 * Sampled once a second against the client's own counters, so the rates
 * are what actually arrived rather than what was asked for. Everything
 * here is read-only: it observes and reports, and nothing it says
 * changes what the app does.
 */
class Diagnostics(private val context: Context) {

    data class Snapshot(
        val videoKbps: Int,
        val audioKbps: Int,
        val fps: Int,
        val pads: List<Pad>,
        val link: String,
        /** "1280x720 h264 hw", or empty before anything decodes. */
        val stream: String,
    )

    data class Pad(val name: String, val transport: String)

    private var lastVideo = 0L
    private var lastAudio = 0L
    private var lastFrames = 0L
    private var lastAt = 0L

    fun sample(client: DirectClient?, link: String, stream: String): Snapshot {
        val now = System.currentTimeMillis()
        val elapsed = (now - lastAt).coerceAtLeast(1)
        val v = client?.videoBytes ?: 0
        val a = client?.audioBytes ?: 0
        val f = client?.videoFrames ?: 0

        /* First call, or a reconnection which resets the counters: report
         * nothing rather than a number computed from a negative delta. */
        val fresh = lastAt == 0L || v < lastVideo
        val videoKbps = if (fresh) 0 else ((v - lastVideo) * 8 / elapsed).toInt()
        val audioKbps = if (fresh) 0 else ((a - lastAudio) * 8 / elapsed).toInt()
        val fps = if (fresh) 0 else ((f - lastFrames) * 1000 / elapsed).toInt()

        lastVideo = v; lastAudio = a; lastFrames = f; lastAt = now
        return Snapshot(videoKbps, audioKbps, fps, pads(), link, stream)
    }

    /**
     * Every controller the system can see, and how it is attached.
     *
     * Android has no public way to ask an input device what it is
     * plugged into, so USB is established by looking for a matching
     * device in the USB manager's list and anything else external is
     * taken to be Bluetooth. That is a guess, and it is labelled as one
     * rather than dressed up: "usb" is known, "bt?" is inferred.
     */
    fun pads(): List<Pad> {
        val input = context.getSystemService(Context.INPUT_SERVICE) as? InputManager
            ?: return emptyList()
        val usbNames = try {
            val usb = context.getSystemService(Context.USB_SERVICE) as? UsbManager
            usb?.deviceList?.values?.mapNotNull { it.productName }?.map { it.lowercase() }
                ?: emptyList()
        } catch (e: Exception) {
            emptyList()
        }

        val out = mutableListOf<Pad>()
        for (id in input.inputDeviceIds) {
            val device = InputDevice.getDevice(id) ?: continue
            val sources = device.sources
            val isPad = (sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                        (sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
            if (!isPad) continue
            val name = device.name
            val transport = when {
                usbNames.any { it.contains(name.lowercase()) || name.lowercase().contains(it) } -> "usb"
                device.isExternal -> "bt?"
                else -> "built-in"
            }
            out += Pad(name, transport)
        }
        return out
    }

    /** One line, short enough to sit over a game without covering it. */
    fun line(s: Snapshot): String {
        val pad = when {
            s.pads.isEmpty() -> "no pad"
            s.pads.size == 1 -> "${s.pads[0].name} (${s.pads[0].transport})"
            else -> "${s.pads.size} pads"
        }
        val stream = if (s.stream.isEmpty()) "" else "${s.stream}   "
        return "$stream${s.link}   video ${s.videoKbps} kb/s   " +
               "audio ${s.audioKbps} kb/s   ${s.fps} fps   $pad"
    }
}
