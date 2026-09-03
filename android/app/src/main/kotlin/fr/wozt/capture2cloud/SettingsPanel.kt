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
 * The menu, in either of two shapes.
 *
 * Columns by default: everything at once, spread over three of them. A
 * phone held sideways is wide and short, which is the wrong shape for
 * one long list and the right one for three short ones -- nothing is
 * more than a glance away and nothing has to be opened first.
 *
 * One category at a time is the other shape, kept because it covers less
 * of a running picture, which is the thing this sits on top of. The
 * heading at the top switches between them.
 *
 * It knows nothing about the stream: everything it changes goes through
 * [Settings] and everything it asks for goes out through [actions], so
 * it opens and is useful with no connection at all.
 */
class SettingsPanel(
    context: Context,
    private val settings: Settings,
    private val actions: Actions,
) : ScrollView(context) {

    interface Actions {
        fun onStreamShapeChanged()
        fun onPictureChanged()
        fun onSoundChanged()
        fun onPadChanged()
        fun onStatsChanged()
        fun statsText(): String
        fun onHostChanged()
        fun onHome()
        fun onWake()
        fun onResetDongle()
        fun onRestartHost()
        fun onLogin(password: String)
        fun onBackToViewer()
        fun onDisconnect()
        fun onClose()
        fun mayControl(): Boolean
    }

    /** Where rows are being added right now. */
    private var body = LinearLayout(context)

    /** Which category is open, in one-at-a-time mode. */
    private var open: String? = null

    init {
        setBackgroundColor(0xF00B1A26.toInt())
        build()
    }

    fun rebuild() {
        removeAllViews()
        build()
    }

    private fun build() {
        if (settings.menuColumns) buildColumns() else buildAccordion()
    }

    private fun buildColumns() {
        val outer = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(20, 10, 20, 30)
        }
        body = outer
        header()

        val row = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
        val cols = (0..2).map {
            LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                layoutParams = LinearLayout.LayoutParams(0, WRAP, 1f)
                setPadding(8, 0, 8, 0)
            }
        }
        cols.forEach { row.addView(it) }
        outer.addView(row)

        /* Categories are kept whole -- never split across a boundary --
         * because a heading in one column with half its contents in the
         * next is worse than an uneven column. The long ones lead two
         * different columns so the three come out roughly even. */
        body = cols[0]; connection(); sound(); diagnostics()
        body = cols[1]; stream(); picture()
        body = cols[2]; controls(); touchPad(); console()

        addView(outer)
    }

    private fun buildAccordion() {
        val single = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 12, 24, 40)
        }
        body = single
        header()
        category("connection") { connection() }
        category("stream") { stream() }
        category("picture") { picture() }
        category("sound") { sound() }
        category("controls") { controls() }
        category("touch pad") { touchPad() }
        category("console") { console() }
        addView(single)
    }

    // --- the groups ----------------------------------------------------

    private fun header() {
        val top = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        /* Pinned, because back now leaves the app rather than closing
         * this, and a menu with no way back to the picture is a trap. */
        top.addView(Button(context).apply {
            text = "back to the stream"
            textSize = 13f
            layoutParams = LinearLayout.LayoutParams(0, WRAP, 1f)
            setOnClickListener { actions.onClose() }
        })
        top.addView(Button(context).apply {
            text = if (settings.menuColumns) "columns" else "one at a time"
            textSize = 12f
            setOnClickListener { settings.menuColumns = !settings.menuColumns; rebuild() }
        })
        top.addView(TextView(context).apply {
            text = if (actions.mayControl()) "player" else "viewer"
            setTextColor(if (actions.mayControl()) ACCENT else DIM)
            textSize = 13f
            setPadding(16, 0, 6, 0)
        })
        body.addView(top)
    }

    /**
     * The numbers, under the first column where there was empty space.
     *
     * Live: refreshed by [refreshStats] while the menu is open, so this
     * is what the stream is doing now rather than what it was doing when
     * the menu was opened -- which for a diagnostic is the difference
     * between useful and misleading.
     */
    private var statsView: TextView? = null

    private fun diagnostics() {
        group("diagnostics")
        check("show a line over the stream", settings.showStats) {
            settings.showStats = it; actions.onStatsChanged()
        }
        val v = TextView(context).apply {
            typeface = android.graphics.Typeface.MONOSPACE
            textSize = 11f
            setTextColor(0xFF39FF14.toInt())
            text = actions.statsText()
        }
        statsView = v
        body.addView(v)
    }

    fun refreshStats(text: String) {
        statsView?.text = text
    }

    private fun connection() {
        group("connection")
        val host = field(settings.host, InputType.TYPE_CLASS_TEXT)
        val port = field(settings.directPort.toString(), InputType.TYPE_CLASS_NUMBER)
        row("host", host)
        row("port", port)
        /* Applied on a button, not on every keystroke: an address is
         * retyped a character at a time and reconnecting to each
         * half-finished one would be absurd. */
        button("use this address") {
            settings.host = host.text.toString().trim()
            settings.directPort = port.text.toString().trim().toIntOrNull() ?: 5081
            actions.onHostChanged()
        }
        val password = field("", InputType.TYPE_CLASS_TEXT or
                                 InputType.TYPE_TEXT_VARIATION_PASSWORD, "player password")
        body.addView(password)
        /* Two buttons rather than one that changes its label. "Logged
         * in" said what the state was and left it unclear what pressing
         * it would do; these each say what they do. */
        button("log in as player", !actions.mayControl()) {
            actions.onLogin(password.text.toString())
            password.setText("")
        }
        button("back to viewer", actions.mayControl()) { actions.onBackToViewer() }
        button("disconnect") { actions.onDisconnect() }
    }

    private fun stream() {
        group("stream")
        /* No 1080 in H.264: the host has no encoder for it at that size,
         * and a button that quietly does nothing is worse than no
         * button. */
        val heights = if (settings.codec == Protocol.CODEC_H264)
            listOf("720" to 720, "480" to 480)
        else listOf("1080" to 1080, "720" to 720, "480" to 480)
        choice("size", heights, settings.height) {
            settings.height = it; actions.onStreamShapeChanged()
        }
        choice("fps", listOf("60" to 60, "30" to 30), settings.fps) {
            settings.fps = it; actions.onStreamShapeChanged()
        }
        choice("codec", listOf("H.264" to Protocol.CODEC_H264, "VP8" to Protocol.CODEC_VP8),
               settings.codec) {
            settings.codec = it
            if (it == Protocol.CODEC_H264 && settings.height > 720) settings.height = 720
            actions.onStreamShapeChanged()
        }
        slider("bitrate", 1, 50, settings.bitrateMbps, "M") {
            settings.bitrateMbps = it; actions.onStreamShapeChanged()
        }
    }

    private fun picture() {
        group("picture")
        slider("bright", 50, 150, settings.brightness, "%") {
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

    private fun sound() {
        group("sound")
        check("mute", settings.muted) { settings.muted = it; actions.onSoundChanged() }
        /* 0 to 100, where 100 is eight times the stream's own level --
         * the same scale as the page and the console client. */
        slider("volume", 0, 100, settings.volume, "%") {
            settings.volume = it; actions.onSoundChanged()
        }
    }

    private fun controls() {
        group("controls")
        check("invert right stick", settings.invertRy) { settings.invertRy = it }
        slider("LT", 0, 100, settings.ltThreshold, "%") { settings.ltThreshold = it }
        slider("RT", 0, 100, settings.rtThreshold, "%") { settings.rtThreshold = it }
        for (stick in 0..1) {
            val side = if (stick == 0) "L" else "R"
            slider("$side dead", 0, 40, settings.deadzone(stick), "%") {
                settings.setDeadzone(stick, it)
            }
            slider("$side range", 45, 100, settings.range(stick), "%") {
                settings.setRange(stick, it)
            }
            /* The corners get their own limit: a stick reaches less far
             * diagonally than along an axis, by an amount that differs
             * from one stick to the next. */
            slider("$side diag", 45, 100, settings.diagonal(stick), "%") {
                settings.setDiagonal(stick, it)
            }
        }
    }

    private fun touchPad() {
        group("touch pad")
        check("show it", settings.padEnabled) { settings.padEnabled = it; actions.onPadChanged() }
        /* Letters only: the slots underneath stay positional, so the
         * button under your thumb is the same button whichever of these
         * you read it as. */
        choice("letters", listOf("Nin" to 0, "Xbox" to 1, "PS" to 2), settings.padLabels) {
            settings.padLabels = it; actions.onPadChanged()
        }
        slider("opacity", 10, 100, settings.padOpacity, "%") {
            settings.padOpacity = it; actions.onPadChanged()
        }
    }

    private fun console() {
        group("console")
        val gated = actions.mayControl()
        button("HOME", gated) { actions.onHome() }
        button("wake console", gated) { actions.onWake() }
        button("reset adapter", gated) { actions.onResetDongle() }
        button("restart host", gated) { actions.onRestartHost() }
        if (!gated) {
            body.addView(TextView(context).apply {
                text = "these need a player login"
                setTextColor(DIM); textSize = 12f
            })
        }
    }

    // --- the shapes every row is made of ------------------------------

    private fun category(title: String, contents: () -> Unit) {
        val isOpen = open == title
        body.addView(TextView(context).apply {
            text = (if (isOpen) "▾  " else "▸  ") + title
            setTextColor(if (isOpen) ACCENT else Color.WHITE)
            textSize = 14f
            setPadding(0, 14, 0, 8)
            setOnClickListener { open = if (isOpen) null else title; rebuild() }
        })
        if (isOpen) contents()
    }

    private fun group(title: String) {
        /* In one-at-a-time mode the heading is the category row itself,
         * so this would be the same word twice. */
        if (!settings.menuColumns) return
        body.addView(TextView(context).apply {
            text = title
            setTextColor(ACCENT)
            textSize = 13f
            setPadding(0, 16, 0, 6)
        })
    }

    private fun field(value: String, type: Int, hintText: String? = null) =
        EditText(context).apply {
            setText(value)
            hint = hintText
            inputType = type
            textSize = 13f
            setTextColor(Color.WHITE)
            setHintTextColor(DIM)
        }

    private fun row(label: String, control: View) {
        val line = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        line.addView(TextView(context).apply {
            text = label
            textSize = 12f
            setTextColor(DIM)
            layoutParams = LinearLayout.LayoutParams(0, WRAP, 0.3f)
        })
        control.layoutParams = LinearLayout.LayoutParams(0, WRAP, 0.7f)
        line.addView(control)
        body.addView(line)
    }

    /**
     * Buttons keep their height. They were squeezed flat in the name of
     * compactness, which bought a few pixels and cost the one property a
     * button has to have: looking like something you can hit.
     */
    private fun button(label: String, enabled: Boolean = true, onClick: () -> Unit) {
        body.addView(Button(context).apply {
            text = label
            textSize = 13f
            isEnabled = enabled
            setPadding(24, 18, 24, 18)
            setOnClickListener { onClick() }
            layoutParams = LinearLayout.LayoutParams(MATCH, WRAP).apply {
                topMargin = 6
            }
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
            textSize = 11f
            setTextColor(DIM)
            minWidth = 90
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
            textSize = 12f
            setTextColor(Color.WHITE)
            layoutParams = LinearLayout.LayoutParams(0, WRAP, 0.35f)
        })
        line.addView(bar)
        line.addView(readout)
        body.addView(line)
    }

    private fun <T> choice(label: String, options: List<Pair<String, T>>, current: T,
                           onPick: (T) -> Unit) {
        body.addView(TextView(context).apply {
            text = label
            textSize = 12f
            setTextColor(Color.WHITE)
            setPadding(0, 10, 0, 2)
        })
        val line = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
        for ((text, value) in options) {
            line.addView(Button(context).apply {
                this.text = text
                textSize = 12f
                alpha = if (value == current) 1f else 0.4f
                setPadding(6, 14, 6, 14)
                layoutParams = LinearLayout.LayoutParams(0, WRAP, 1f).apply { rightMargin = 6 }
                setOnClickListener { onPick(value); rebuild() }
            })
        }
        body.addView(line)
    }

    private companion object {
        const val WRAP = LinearLayout.LayoutParams.WRAP_CONTENT
        const val MATCH = LinearLayout.LayoutParams.MATCH_PARENT
        const val ACCENT = 0xFF5CE8FF.toInt()
        const val DIM = 0xFF8FA8B8.toInt()
    }
}
