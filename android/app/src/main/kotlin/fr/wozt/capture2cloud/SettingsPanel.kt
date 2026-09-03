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
 * The menu, mirroring the page's and the console client's.
 *
 * Categories that open one at a time, the way the console client does
 * it, rather than one long list. The reason is the same on both: this
 * sits over a running picture, so it should cover as little of it as it
 * can and for as long as it takes to change one thing.
 *
 * Every row is a label, a control and a value on one line. Built in code
 * rather than XML because thirty rows of the same three shapes in XML is
 * thirty chances for one to differ from the rest by a margin nobody
 * meant.
 *
 * It knows nothing about the stream: everything it changes goes through
 * [Settings], everything it asks for goes out through [actions]. So it
 * opens and is useful with no connection at all.
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
        fun onHostChanged()
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
        setPadding(24, 12, 24, 40)
    }

    /** Which category is open, by title. Only ever one. */
    private var open: String? = null

    init {
        setBackgroundColor(0xF00B1A26.toInt())
        addView(body)
        build()
    }

    fun rebuild() {
        body.removeAllViews()
        build()
    }

    private fun build() {
        /* Pinned, because back now leaves the app rather than closing
         * this, and a menu with no way back to the picture is a trap. */
        val top = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
        top.addView(Button(context).apply {
            text = "back to the stream"
            textSize = 13f
            layoutParams = LinearLayout.LayoutParams(0, WRAP, 1f)
            setOnClickListener { actions.onClose() }
        })
        top.addView(TextView(context).apply {
            text = if (actions.mayControl()) "player" else "viewer"
            setTextColor(if (actions.mayControl()) ACCENT else DIM)
            textSize = 13f
            gravity = Gravity.CENTER
            setPadding(18, 0, 6, 0)
        })
        body.addView(top)

        category("connection") {
            val host = EditText(context).apply {
                setText(settings.host)
                textSize = 14f
                setTextColor(Color.WHITE)
                inputType = InputType.TYPE_CLASS_TEXT
            }
            val port = EditText(context).apply {
                setText(settings.directPort.toString())
                textSize = 14f
                setTextColor(Color.WHITE)
                inputType = InputType.TYPE_CLASS_NUMBER
            }
            row("host", host)
            row("port", port)
            /* Applied on a button rather than on every keystroke: the
             * address is retyped a character at a time and reconnecting
             * to each half-finished one would be absurd. */
            button("use this address") {
                settings.host = host.text.toString().trim()
                settings.directPort = port.text.toString().trim().toIntOrNull() ?: 5081
                actions.onHostChanged()
            }
            val password = EditText(context).apply {
                hint = "player password"
                textSize = 14f
                inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
                setTextColor(Color.WHITE)
                setHintTextColor(DIM)
            }
            body.addView(password)
            button(if (actions.mayControl()) "logged in" else "log in as player") {
                actions.onLogin(password.text.toString())
                password.setText("")
            }
            button("disconnect") { actions.onDisconnect() }
        }

        category("stream") {
            /* No 1080 in H.264: the host has no encoder for it at that
             * size, so offering the button would be offering a setting
             * that quietly does nothing. */
            val heights = if (settings.codec == Protocol.CODEC_H264)
                listOf("720" to 720, "480" to 480)
            else
                listOf("1080" to 1080, "720" to 720, "480" to 480)
            choice("resolution", heights, settings.height) {
                settings.height = it; actions.onStreamShapeChanged()
            }
            choice("fps", listOf("60" to 60, "30" to 30),
                   settings.fps) { settings.fps = it; actions.onStreamShapeChanged() }
            /* H.264 first: it is the one every phone decodes in silicon.
             * VP8 is here for a host with no H.264 encoder. */
            choice("codec", listOf("H.264" to Protocol.CODEC_H264, "VP8" to Protocol.CODEC_VP8),
                   settings.codec) {
                settings.codec = it
                /* Coming back to H.264 from 1080 would leave a height it
                 * cannot serve, so it comes down with the codec. */
                if (it == Protocol.CODEC_H264 && settings.height > 720) settings.height = 720
                actions.onStreamShapeChanged()
            }
            slider("bitrate", 1, 50, settings.bitrateMbps, "M") {
                settings.bitrateMbps = it; actions.onStreamShapeChanged()
            }
        }

        category("picture") {
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
            button("reset") {
                settings.brightness = 100; settings.contrast = 100
                settings.saturation = 100; settings.hue = 0
                actions.onPictureChanged(); rebuild()
            }
        }

        category("sound") {
            check("mute", settings.muted) { settings.muted = it; actions.onSoundChanged() }
            /* 0 to 100, where 100 is eight times the stream's own level:
             * the same scale as the page and the console client, so a
             * number sounds the same wherever it is played. */
            slider("volume", 0, 100, settings.volume, "%") {
                settings.volume = it; actions.onSoundChanged()
            }
        }

        category("controls") {
            check("invert right stick", settings.invertRy) { settings.invertRy = it }
            slider("LT", 0, 100, settings.ltThreshold, "%") { settings.ltThreshold = it }
            slider("RT", 0, 100, settings.rtThreshold, "%") { settings.rtThreshold = it }
            for (stick in 0..1) {
                val side = if (stick == 0) "L" else "R"
                slider("$side deadzone", 0, 40, settings.deadzone(stick), "%") {
                    settings.setDeadzone(stick, it)
                }
                slider("$side range", 45, 100, settings.range(stick), "%") {
                    settings.setRange(stick, it)
                }
                /* The corners get their own limit: a stick reaches less
                 * far diagonally than along an axis, by an amount that
                 * differs from one stick to the next. */
                slider("$side diagonals", 45, 100, settings.diagonal(stick), "%") {
                    settings.setDiagonal(stick, it)
                }
            }
        }

        category("touch pad") {
            check("show it", settings.padEnabled) { settings.padEnabled = it; actions.onPictureChanged() }
            /* Letters only. The slots underneath stay positional, so the
             * button under your thumb is the same button whichever of
             * these you read it as. */
            choice("letters", listOf("Nintendo" to 0, "Xbox" to 1, "PlayStation" to 2),
                   settings.padLabels) { settings.padLabels = it; actions.onPictureChanged() }
            slider("opacity", 10, 100, settings.padOpacity, "%") {
                settings.padOpacity = it; actions.onPictureChanged()
            }
        }

        category("console") {
            val gated = actions.mayControl()
            button("HOME", gated) { actions.onHome() }
            button("wake the console", gated) { actions.onWake() }
            button("reset the adapter", gated) { actions.onResetDongle() }
            button("restart the host", gated) { actions.onRestartHost() }
            if (!gated) {
                body.addView(TextView(context).apply {
                    text = "these need a player login"
                    setTextColor(DIM); textSize = 12f
                })
            }
        }
    }

    // --- the shapes every row is made of ------------------------------

    /** A heading that opens its contents and closes whatever else was. */
    private fun category(title: String, contents: () -> Unit) {
        val isOpen = open == title
        body.addView(TextView(context).apply {
            text = (if (isOpen) "▾  " else "▸  ") + title
            setTextColor(if (isOpen) ACCENT else Color.WHITE)
            textSize = 14f
            setPadding(0, 14, 0, 8)
            setOnClickListener {
                open = if (isOpen) null else title
                rebuild()
            }
        })
        if (isOpen) contents()
    }

    private fun row(label: String, control: View) {
        val line = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        line.addView(TextView(context).apply {
            text = label
            textSize = 13f
            setTextColor(DIM)
            layoutParams = LinearLayout.LayoutParams(0, WRAP, 0.35f)
        })
        control.layoutParams = LinearLayout.LayoutParams(0, WRAP, 0.65f)
        line.addView(control)
        body.addView(line)
    }

    private fun button(label: String, enabled: Boolean = true, onClick: () -> Unit) {
        body.addView(Button(context).apply {
            text = label
            textSize = 13f
            isEnabled = enabled
            minHeight = 0
            minimumHeight = 0
            setPadding(20, 6, 20, 6)
            setOnClickListener { onClick() }
        })
    }

    private fun check(label: String, value: Boolean, onChange: (Boolean) -> Unit) {
        body.addView(CheckBox(context).apply {
            text = label
            textSize = 13f
            setTextColor(Color.WHITE)
            isChecked = value
            setOnCheckedChangeListener { _, checked -> onChange(checked) }
        })
    }

    /**
     * Label, slider and value on one line. SeekBar only counts from
     * zero, so ranges that start elsewhere -- hue runs to either side of
     * nothing -- are offset here rather than in every caller.
     */
    private fun slider(label: String, min: Int, max: Int, value: Int, unit: String,
                       onChange: (Int) -> Unit) {
        val readout = TextView(context).apply {
            text = "$value$unit"
            textSize = 12f
            setTextColor(DIM)
            minWidth = 110
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
        val line = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        line.addView(TextView(context).apply {
            text = label
            textSize = 13f
            setTextColor(Color.WHITE)
            layoutParams = LinearLayout.LayoutParams(0, WRAP, 0.3f)
        })
        line.addView(bar)
        line.addView(readout)
        body.addView(line)
    }

    private fun <T> choice(label: String, options: List<Pair<String, T>>, current: T,
                           onPick: (T) -> Unit) {
        val line = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        line.addView(TextView(context).apply {
            text = label
            textSize = 13f
            setTextColor(Color.WHITE)
            layoutParams = LinearLayout.LayoutParams(0, WRAP, 0.3f)
        })
        for ((text, value) in options) {
            line.addView(Button(context).apply {
                this.text = text
                textSize = 12f
                alpha = if (value == current) 1f else 0.4f
                minHeight = 0; minimumHeight = 0
                setPadding(8, 4, 8, 4)
                layoutParams = LinearLayout.LayoutParams(0, WRAP, 0.7f / options.size)
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
