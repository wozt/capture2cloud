package fr.wozt.capture2cloud

import android.content.Context
import android.content.SharedPreferences

/**
 * Everything the app remembers, mirroring what the page stores per
 * browser.
 *
 * Same names and same ranges as app.js wherever they mean the same thing,
 * so a stick that behaves one way on the page behaves that way here. The
 * ones that have no counterpart are the two this app adds: which host to
 * talk to, and which way to talk to it.
 *
 * The player password is deliberately absent. The page keeps a token in
 * sessionStorage and never the password; the same rule holds here, and
 * for the same reason -- a stored password is a stored password however
 * private the file claims to be.
 */
class Settings(context: Context) {
    private val prefs: SharedPreferences =
        context.getSharedPreferences("capture2cloud", Context.MODE_PRIVATE)

    /** How the picture gets here. */
    enum class Path { DIRECT, HTTP }

    var host: String
        get() = prefs.getString("host", "") ?: ""
        set(v) = put { putString("host", v) }

    /** The console server's port. The web server's is [webPort]. */
    var directPort: Int
        get() = prefs.getInt("directPort", 5081)
        set(v) = put { putInt("directPort", v.coerceIn(1, 65535)) }

    var webPort: Int
        get() = prefs.getInt("webPort", 5080)
        set(v) = put { putInt("webPort", v.coerceIn(1, 65535)) }

    var path: Path
        get() = if (prefs.getString("path", "direct") == "http") Path.HTTP else Path.DIRECT
        set(v) = put { putString("path", if (v == Path.HTTP) "http" else "direct") }

    // --- what the host is asked to encode ----------------------------

    /** 1080, 720 or 480. The host scales; this is what it scales to. */
    var height: Int
        get() = prefs.getInt("height", 720)
        set(v) = put { putInt("height", v) }

    var fps: Int
        get() = prefs.getInt("fps", 60)
        set(v) = put { putInt("fps", v) }

    var bitrateMbps: Int
        get() = prefs.getInt("bitrateMbps", 12)
        set(v) = put { putInt("bitrateMbps", v.coerceIn(1, 50)) }

    /**
     * H.264 by default, unlike the page, which negotiates VP8.
     *
     * Not a preference so much as a fact about phones: every Android
     * device since forever decodes H.264 in hardware, and VP8 hardware
     * decoding is common but not universal. Falling back to software VP8
     * on a phone at 720p60 is how a stream ends up dropping frames.
     */
    var codec: Int
        get() = prefs.getInt("codec", Protocol.CODEC_H264)
        set(v) = put { putInt("codec", v) }

    // --- sound, on the same scale as everywhere else -----------------

    /** 0..100, where 100 is eight times the stream's own level. */
    var volume: Int
        get() = prefs.getInt("volume", 13)
        set(v) = put { putInt("volume", v.coerceIn(0, 100)) }

    var muted: Boolean
        get() = prefs.getBoolean("muted", false)
        set(v) = put { putBoolean("muted", v) }

    // --- picture, applied here and not asked of the host -------------

    var brightness: Int
        get() = prefs.getInt("brightness", 100)
        set(v) = put { putInt("brightness", v.coerceIn(50, 150)) }

    var contrast: Int
        get() = prefs.getInt("contrast", 100)
        set(v) = put { putInt("contrast", v.coerceIn(50, 150)) }

    var saturation: Int
        get() = prefs.getInt("saturation", 100)
        set(v) = put { putInt("saturation", v.coerceIn(0, 200)) }

    var hue: Int
        get() = prefs.getInt("hue", 0)
        set(v) = put { putInt("hue", v.coerceIn(-180, 180)) }

    // --- sticks and triggers, the page's ranges exactly --------------

    var invertRy: Boolean
        get() = prefs.getBoolean("invertRy", false)
        set(v) = put { putBoolean("invertRy", v) }

    var ltThreshold: Int
        get() = prefs.getInt("ltThreshold", 30)
        set(v) = put { putInt("ltThreshold", v.coerceIn(0, 100)) }

    var rtThreshold: Int
        get() = prefs.getInt("rtThreshold", 30)
        set(v) = put { putInt("rtThreshold", v.coerceIn(0, 100)) }

    /** index 0 is the left stick, 1 the right. */
    fun deadzone(stick: Int) = prefs.getInt("deadzone$stick", 5)
    fun setDeadzone(stick: Int, v: Int) = put { putInt("deadzone$stick", v.coerceIn(0, 40)) }

    /** How far the stick has to go to count as fully pushed. */
    fun range(stick: Int) = prefs.getInt("range$stick", 100)
    fun setRange(stick: Int, v: Int) = put { putInt("range$stick", v.coerceIn(45, 100)) }

    /** The same, for the corners: a stick reaches less far diagonally. */
    fun diagonal(stick: Int) = prefs.getInt("diagonal$stick", 100)
    fun setDiagonal(stick: Int, v: Int) = put { putInt("diagonal$stick", v.coerceIn(45, 100)) }

    // --- the on-screen pad -------------------------------------------

    var padEnabled: Boolean
        get() = prefs.getBoolean("padEnabled", true)
        set(v) = put { putBoolean("padEnabled", v) }

    var padOpacity: Int
        get() = prefs.getInt("padOpacity", 55)
        set(v) = put { putInt("padOpacity", v.coerceIn(10, 100)) }

    var padColour: Int
        get() = prefs.getInt("padColour", 0)
        set(v) = put { putInt("padColour", v) }

    /**
     * Which letters the face buttons wear: 0 Nintendo, 1 Microsoft,
     * 2 PlayStation.
     *
     * Only the letters change. The slots underneath stay positional --
     * the button under your thumb is always the southern one -- because
     * that is what the adapter forwards and what the console reads. A
     * layout here is a question of what you are used to reading, not of
     * what gets pressed.
     */
    var padLabels: Int
        get() = prefs.getInt("padLabels", 0)
        set(v) = put { putInt("padLabels", v.coerceIn(0, 2)) }

    /** Button positions, as the homebrew stores them: "name:x:y;..." */
    var padLayout: String
        get() = prefs.getString("padLayout", "") ?: ""
        set(v) = put { putString("padLayout", v) }

    /**
     * The session token from a successful login, if there is one.
     *
     * Kept, unlike the password. It expires on the host's terms, which is
     * the point of it being a token: losing this costs a login, losing a
     * password costs rather more.
     */
    var token: String
        get() = prefs.getString("token", "") ?: ""
        set(v) = put { putString("token", v) }

    private inline fun put(block: SharedPreferences.Editor.() -> Unit) {
        prefs.edit().apply(block).apply()
    }
}
