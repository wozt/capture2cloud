package fr.wozt.capture2cloud

import android.content.Context
import android.graphics.Color
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.TextView

/**
 * The menu, mirroring the page's.
 *
 * Same groups in the same order and the same ranges, so someone who
 * knows the page knows this. Built in code rather than in XML because
 * every row is the same three shapes -- a label, a control, a value --
 * and thirty of those in XML is thirty chances to have one differ from
 * the rest by a margin nobody meant.
 *
 * It knows nothing about the stream. Everything it changes goes through
 * [Settings], and everything it asks for goes out through [actions], so
 * the panel can be opened with no connection and still be useful.
 */
class SettingsPanel(
    context: Context,
    private val settings: Settings,
    private val actions: Actions,
) : ScrollView(context) {

    interface Actions {
        fun onStreamShapeChanged()      // resolution, fps, bitrate, codec
        fun onPictureChanged()
        fun onSoundChanged()
        fun onHome()
        fun onWake()
        fun onResetDongle()
        fun onRestartHost()
        fun onLogin(password: String)
        fun onDisconnect()
        fun onClose()
        fun mayControl(): Boolean
    }

    private val body = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(36, 28, 36, 60)
    }

    init {
        setBackgroundColor(0xF00B1A26.toInt())
        addView(body)
        build()
    }

    private fun build() {
        /* Pinned at the top, because back now leaves the app rather than
         * closing this, and a menu with no way back to the picture would
         * be a trap. */
        body.addView(Button(context).apply {
            text = "back to the stream"
            setOnClickListener { actions.onClose() }
        })

        group("connection")
        row("host", TextView(context).apply {
            text = "${settings.host}:${settings.directPort}"
            setTextColor(DIM)
        })
        val password = EditText(context).apply {
            hint = "player password"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            setTextColor(Color.WHITE)
            setHintTextColor(DIM)
        }
        body.addView(password)
        /* The password is used and not kept. What is kept is the token
         * the host answers with, which expires on the host's terms --
         * the same rule the page follows. */
        button(if (actions.mayControl()) "logged in as player" else "log in as player") {
            actions.onLogin(password.text.toString())
            password.setText("")
        }
        button("disconnect") { actions.onDisconnect() }

        group("stream")
        choice("resolution", listOf("1080p" to 1080, "720p" to 720, "480p" to 480),
               settings.height) { settings.height = it; actions.onStreamShapeChanged() }
        choice("frame rate", listOf("60 fps" to 60, "30 fps" to 30),
               settings.fps) { settings.fps = it; actions.onStreamShapeChanged() }
        /* H.264 first, because it is the one every phone decodes in
         * hardware. VP8 is offered for a host that has no H.264 encoder. */
        choice("codec", listOf("H.264" to Protocol.CODEC_H264, "VP8" to Protocol.CODEC_VP8),
               settings.codec) { settings.codec = it; actions.onStreamShapeChanged() }
        slider("bitrate", 1, 50, settings.bitrateMbps, " Mb/s") {
            settings.bitrateMbps = it; actions.onStreamShapeChanged()
        }

        group("picture")
        slider("brightness", 50, 150, settings.brightness, "%") {
            settings.brightness = it; actions.onPictureChanged()
        }
        slider("contrast", 50, 150, settings.contrast, "%") {
            settings.contrast = it; actions.onPictureChanged()
        }
        slider("saturation", 0, 200, settings.saturation, "%") {
            settings.saturation = it; actions.onPictureChanged()
        }
        slider("hue", -180, 180, settings.hue, "°") {
            settings.hue = it; actions.onPictureChanged()
        }
        button("reset picture") {
            settings.brightness = 100; settings.contrast = 100
            settings.saturation = 100; settings.hue = 0
            actions.onPictureChanged()
            rebuild()
        }

        group("sound")
        check("mute", settings.muted) { settings.muted = it; actions.onSoundChanged() }
        /* 0 to 100, where 100 is eight times the stream's own level --
         * the same scale as the page and the console client, so a given
         * number sounds the same wherever it is played. */
        slider("volume", 0, 100, settings.volume, "%") {
            settings.volume = it; actions.onSoundChanged()
        }

        group("controls")
        check("invert right stick", settings.invertRy) { settings.invertRy = it }
        slider("LT threshold", 0, 100, settings.ltThreshold, "%") { settings.ltThreshold = it }
        slider("RT threshold", 0, 100, settings.rtThreshold, "%") { settings.rtThreshold = it }
        for (stick in 0..1) {
            val side = if (stick == 0) "left" else "right"
            slider("$side deadzone", 0, 40, settings.deadzone(stick), "%") {
                settings.setDeadzone(stick, it)
            }
            slider("$side range", 45, 100, settings.range(stick), "%") {
                settings.setRange(stick, it)
            }
            /* The corners get their own limit: a stick reaches less far
             * diagonally than along an axis, by an amount that differs
             * from one stick to the next. */
            slider("$side diagonals", 45, 100, settings.diagonal(stick), "%") {
                settings.setDiagonal(stick, it)
            }
        }

        group("touch pad")
        check("show it", settings.padEnabled) { settings.padEnabled = it }
        slider("opacity", 10, 100, settings.padOpacity, "%") { settings.padOpacity = it }

        group("console")
        val gated = actions.mayControl()
        button("HOME", gated) { actions.onHome() }
        button("wake the console", gated) { actions.onWake() }
        button("reset the adapter", gated) { actions.onResetDongle() }
        button("restart the host", gated) { actions.onRestartHost() }
        if (!gated) {
            body.addView(TextView(context).apply {
                text = "these need a player login"
                setTextColor(DIM)
                setPadding(0, 8, 0, 0)
            })
        }
    }

    /** Rebuilds after something changed several controls at once. */
    fun rebuild() {
        body.removeAllViews()
        build()
    }

    // --- the three shapes every row is made of ------------------------

    private fun group(title: String) {
        body.addView(TextView(context).apply {
            text = title
            setTextColor(ACCENT)
            textSize = 15f
            setPadding(0, 34, 0, 10)
        })
    }

    private fun row(label: String, control: View) {
        val line = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        line.addView(TextView(context).apply {
            text = label
            setTextColor(Color.WHITE)
            layoutParams = LinearLayout.LayoutParams(0, WRAP, 1f)
        })
        line.addView(control)
        body.addView(line)
    }

    private fun button(label: String, enabled: Boolean = true, onClick: () -> Unit) {
        body.addView(Button(context).apply {
            text = label
            isEnabled = enabled
            setOnClickListener { onClick() }
        })
    }

    private fun check(label: String, value: Boolean, onChange: (Boolean) -> Unit) {
        body.addView(CheckBox(context).apply {
            text = label
            setTextColor(Color.WHITE)
            isChecked = value
            setOnCheckedChangeListener { _, checked -> onChange(checked) }
        })
    }

    /**
     * A slider over an arbitrary range, including negative ones: SeekBar
     * only counts from zero, so the offset lives here rather than in
     * every caller.
     */
    private fun slider(label: String, min: Int, max: Int, value: Int, unit: String,
                       onChange: (Int) -> Unit) {
        val readout = TextView(context).apply {
            text = "$value$unit"
            setTextColor(DIM)
            minWidth = 130
            gravity = Gravity.END
        }
        val bar = SeekBar(context).apply {
            this.max = max - min
            progress = value - min
            layoutParams = LinearLayout.LayoutParams(0, WRAP, 1f)
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(s: SeekBar?, p: Int, fromUser: Boolean) {
                    val v = p + min
                    readout.text = "$v$unit"
                    if (fromUser) onChange(v)
                }
                override fun onStartTrackingTouch(s: SeekBar?) {}
                override fun onStopTrackingTouch(s: SeekBar?) {}
            })
        }
        body.addView(TextView(context).apply {
            text = label
            setTextColor(Color.WHITE)
            setPadding(0, 12, 0, 0)
        })
        val line = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
        line.addView(bar)
        line.addView(readout)
        body.addView(line)
    }

    private fun <T> choice(label: String, options: List<Pair<String, T>>, current: T,
                           onPick: (T) -> Unit) {
        body.addView(TextView(context).apply {
            text = label
            setTextColor(Color.WHITE)
            setPadding(0, 12, 0, 0)
        })
        val line = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
        for ((text, value) in options) {
            line.addView(Button(context).apply {
                this.text = text
                alpha = if (value == current) 1f else 0.45f
                layoutParams = LinearLayout.LayoutParams(0, WRAP, 1f)
                setOnClickListener { onPick(value); rebuild() }
            })
        }
        body.addView(line)
    }

    private companion object {
        const val WRAP = LinearLayout.LayoutParams.WRAP_CONTENT
        const val ACCENT = 0xFF5CE8FF.toInt()
        const val DIM = 0xFF8FA8B8.toInt()
    }
}
