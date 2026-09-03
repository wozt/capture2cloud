package fr.wozt.capture2cloud

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.view.MotionEvent
import android.view.View
import kotlin.math.hypot
import kotlin.math.roundToInt

/**
 * The on-screen pad, laid out like the page's and the console client's.
 *
 * Multi-touch throughout: two thumbs on two sticks and a finger on a
 * face button is the ordinary case, not a corner one, and a pad that
 * tracks a single pointer is useless for anything but menus.
 *
 * Three things learned on the console client are built in here rather
 * than found again:
 *
 * A finger that slides off a button releases it. Without that, dragging
 * a thumb from the stick onto a button and away again left the button
 * held down forever.
 *
 * Pointer ids get reused. When one finger goes up and another comes
 * down, the system may hand out the id that was just freed, and a naive
 * map from id to control then attributes the new touch to the old
 * control. Each pointer therefore remembers which control it grabbed
 * when it went down, and only that one.
 *
 * The sticks are shaped exactly as the page shapes a real one --
 * deadzone, range, and a separate limit for the corners -- so a game
 * tuned with a physical pad feels the same here.
 */
class VirtualPad(context: Context, private val settings: Settings) : View(context) {

    private companion object {
        /** North, west, east, south -- in that order, for each layout. */
        val FACE_LABELS = arrayOf(
            arrayOf("X", "Y", "A", "B"),      // Nintendo
            arrayOf("Y", "X", "B", "A"),      // Microsoft
            arrayOf("△", "□", "○", "✕"),      // PlayStation
        )
    }

    /** Called whenever the pad state changes. Slots are the host's. */
    var onState: ((ByteArray) -> Unit)? = null

    private val state = ByteArray(Protocol.PAD_SLOTS)
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 3f
    }
    private val label = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        isFakeBoldText = true
    }

    /**
     * One control. Circles and pills are buttons; the d-pad is a circle
     * that reads the direction from its centre instead.
     */
    private class Control(
        val x: Float, val y: Float,
        val w: Float, val h: Float,      // half-extents; equal for a circle
        val slot: Int,                   // -1 for the d-pad zone
        val text: String,
        val round: Boolean,
    )

    private class Stick(val x: Float, val y: Float, val r: Float,
                        val axisX: Int, val axisY: Int, val index: Int)

    private val controls = mutableListOf<Control>()
    private val sticks = mutableListOf<Stick>()
    private var dpad: Control? = null

    private val grabbedControl = HashMap<Int, Control>()
    private val grabbedStick = HashMap<Int, Stick>()
    private val grabbedDpad = HashSet<Int>()

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        layout(w.toFloat(), h.toFloat())
    }

    /**
     * The console client's layout, transposed.
     *
     * Not a new arrangement: those positions were tuned against the page
     * and then moved by hand on the console until they fell under the
     * thumbs, and doing that a third time from scratch would only
     * produce a third answer. The unit is the same one -- a sixteenth of
     * the height -- so the proportions hold on any screen.
     *
     * The face buttons carry Nintendo letters on positional slots, as
     * the console client does: the button under your thumb is B, and the
     * adapter is what decides what the console makes of it.
     */
    private fun layout(w: Float, h: Float) {
        controls.clear(); sticks.clear()
        val u = h / 16.36f
        fun bottom(units: Float) = h - units * u

        sticks += Stick(4.800f * u, bottom(6.000f), 1.8f * u,
                        Protocol.LX, Protocol.LY, 0)
        sticks += Stick(w - 4.800f * u, bottom(2.200f), 1.8f * u,
                        Protocol.RX, Protocol.RY, 1)

        dpad = Control(4.800f * u, bottom(2.050f), 1.65f * u, 1.65f * u,
                       -1, "", round = true)

        /* The face cluster. The slots are positional -- north, west,
         * east, south -- and only the letters follow the chosen layout,
         * because what is pressed is decided by where your thumb is and
         * what the adapter makes of it, not by what is printed on it. */
        val fx = w - 4.800f * u
        val fy = bottom(6.200f)
        val fr = 0.54f * u
        val letters = FACE_LABELS[settings.padLabels.coerceIn(0, 2)]
        controls += Control(fx, fy - 1.30f * u, fr, fr, Protocol.Y, letters[0], true)
        controls += Control(fx - 1.30f * u, fy, fr, fr, Protocol.X, letters[1], true)
        controls += Control(fx + 1.30f * u, fy, fr, fr, Protocol.B, letters[2], true)
        controls += Control(fx, fy + 1.30f * u, fr, fr, Protocol.A, letters[3], true)

        // Stick clicks, offset from their sticks exactly as on the console.
        controls += Control(7.375f * u, bottom(5.275f), 0.50f * u, 0.50f * u,
                            Protocol.LS, "L3", true)
        controls += Control(w - 7.375f * u, bottom(1.475f), 0.50f * u, 0.50f * u,
                            Protocol.RS, "R3", true)

        // Shoulders and triggers: pills along the edges.
        val pw = 0.78f * u; val ph = 0.48f * u
        controls += Control(1.400f * u, bottom(0.950f), pw, ph, Protocol.LT, "ZL", false)
        controls += Control(1.400f * u, bottom(2.250f), pw, ph, Protocol.LB, "L", false)
        controls += Control(w - 1.400f * u, bottom(0.950f), pw, ph, Protocol.RT, "ZR", false)
        controls += Control(w - 1.400f * u, bottom(2.250f), pw, ph, Protocol.RB, "R", false)
        controls += Control(1.400f * u, bottom(3.450f), pw, 0.40f * u, Protocol.BACK, "-", false)
        controls += Control(w - 1.400f * u, bottom(3.450f), pw, 0.40f * u, Protocol.START, "+", false)
        controls += Control(w / 2f, bottom(0.900f), 0.44f * u, 0.44f * u, Protocol.GUIDE, "H", true)

        label.textSize = 0.40f * u
    }

    override fun onDraw(canvas: Canvas) {
        if (!settings.padEnabled) return
        val a = (settings.padOpacity * 255 / 100).coerceIn(0, 255)

        dpad?.let { d -> drawDpad(canvas, d, a) }

        for (c in controls) {
            val held = state[c.slot].toInt() != 0
            fill.color = Color.argb(if (held) a else a / 4, 0x5C, 0xE8, 0xFF)
            stroke.color = Color.argb(a, 0x5C, 0xE8, 0xFF)
            if (c.round) {
                canvas.drawCircle(c.x, c.y, c.w, fill)
                canvas.drawCircle(c.x, c.y, c.w, stroke)
            } else {
                val r = c.h
                canvas.drawRoundRect(c.x - c.w, c.y - c.h, c.x + c.w, c.y + c.h, r, r, fill)
                canvas.drawRoundRect(c.x - c.w, c.y - c.h, c.x + c.w, c.y + c.h, r, r, stroke)
            }
            label.color = Color.argb(a, 0xFF, 0xFF, 0xFF)
            canvas.drawText(c.text, c.x, c.y + label.textSize * 0.35f, label)
        }

        for (s in sticks) {
            stroke.color = Color.argb(a, 0x2E, 0xC4, 0xB6)
            canvas.drawCircle(s.x, s.y, s.r, stroke)
            /* The knob sits where the stick is pushed: the only feedback
             * glass can give that a physical stick gives for free. */
            val kx = s.x + state[s.axisX] / 100f * s.r * 0.6f
            val ky = s.y - state[s.axisY] / 100f * s.r * 0.6f
            fill.color = Color.argb(a, 0x2E, 0xC4, 0xB6)
            canvas.drawCircle(kx, ky, s.r * 0.38f, fill)
        }
    }

    /**
     * Three ways of drawing the same eight directions.
     *
     * The difficulty is honest: the four directions are independent, a
     * thumb in a corner sends two of them, and any shape that reads as
     * four separate buttons says the opposite of that. None of these is
     * obviously right, so all three are here and the thumb decides.
     */
    private fun drawDpad(canvas: Canvas, d: Control, a: Int) {
        val up = state[Protocol.UP].toInt() != 0
        val down = state[Protocol.DOWN].toInt() != 0
        val left = state[Protocol.LEFT].toInt() != 0
        val right = state[Protocol.RIGHT].toInt() != 0
        stroke.color = Color.argb(a, 0x5C, 0xE8, 0xFF)

        when (settings.dpadStyle) {
            // --- 0: a cross whose inner corners are filled in ----------
            //
            // The arms say "four directions" and the corner wedges say
            // "and the pairs between them". A wedge lights only when
            // both of its neighbours do, which is exactly the condition
            // it stands for.
            0 -> {
                val arm = d.w
                val t = d.w * 0.40f
                val r = t * 0.4f
                fun bar(l: Float, top: Float, rr: Float, b: Float, lit: Boolean) {
                    fill.color = Color.argb(if (lit) a else a / 5, 0x5C, 0xE8, 0xFF)
                    canvas.drawRoundRect(l, top, rr, b, r, r, fill)
                }
                bar(d.x - t, d.y - arm, d.x + t, d.y + t, up)
                bar(d.x - t, d.y - t, d.x + arm, d.y + t, right)
                bar(d.x - t, d.y - t, d.x + t, d.y + arm, down)
                bar(d.x - arm, d.y - t, d.x + t, d.y + t, left)

                val c = t * 1.55f
                val corners = listOf(
                    Triple(d.x + t, d.y - c, up && right),
                    Triple(d.x + t, d.y + t, down && right),
                    Triple(d.x - c, d.y + t, down && left),
                    Triple(d.x - c, d.y - c, up && left),
                )
                for ((cx, cy, lit) in corners) {
                    fill.color = Color.argb(if (lit) a else a / 8, 0x2E, 0xC4, 0xB6)
                    canvas.drawRoundRect(cx, cy, cx + c - t + t * 0.9f, cy + c - t + t * 0.9f,
                                         r, r, fill)
                }
                canvas.drawRoundRect(d.x - t, d.y - arm, d.x + t, d.y + arm, r, r, stroke)
                canvas.drawRoundRect(d.x - arm, d.y - t, d.x + arm, d.y + t, r, r, stroke)
            }

            // --- 1: an eight-way rose ---------------------------------
            //
            // Eight wedges around a hub, each lighting on its own. There
            // is no arguing with it: the diagonals are drawn, so they
            // exist.
            1 -> {
                val lit = booleanArrayOf(
                    up, up && right, right, down && right,
                    down, down && left, left, up && left)
                val box = android.graphics.RectF(d.x - d.w, d.y - d.w, d.x + d.w, d.y + d.w)
                for (i in 0..7) {
                    fill.color = Color.argb(
                        if (lit[i]) a else a / 6,
                        if (i % 2 == 0) 0x5C else 0x2E,
                        if (i % 2 == 0) 0xE8 else 0xC4,
                        if (i % 2 == 0) 0xFF else 0xB6)
                    /* Starting at -112.5 so a wedge is centred on north
                     * rather than straddling it. */
                    canvas.drawArc(box, -112.5f + i * 45f, 41f, true, fill)
                }
                canvas.drawCircle(d.x, d.y, d.w, stroke)
                fill.color = Color.argb(a, 0x0B, 0x1A, 0x26)
                canvas.drawCircle(d.x, d.y, d.w * 0.3f, fill)
                canvas.drawCircle(d.x, d.y, d.w * 0.3f, stroke)
            }

            // --- 2: a pad with corner marks ---------------------------
            //
            // The quietest of the three: one rounded square, four
            // arrows, and four corner dots that light on the pairs.
            else -> {
                val r = d.w * 0.42f
                fill.color = Color.argb(a / 6, 0x5C, 0xE8, 0xFF)
                canvas.drawRoundRect(d.x - d.w, d.y - d.w, d.x + d.w, d.y + d.w, r, r, fill)
                canvas.drawRoundRect(d.x - d.w, d.y - d.w, d.x + d.w, d.y + d.w, r, r, stroke)

                val marks = arrayOf("▲", "▶", "▼", "◀")
                val on = booleanArrayOf(up, right, down, left)
                val mx = floatArrayOf(0f, 0.62f, 0f, -0.62f)
                val my = floatArrayOf(-0.62f, 0f, 0.62f, 0f)
                for (i in 0..3) {
                    label.color = Color.argb(if (on[i]) a else a / 3, 0xFF, 0xFF, 0xFF)
                    canvas.drawText(marks[i], d.x + mx[i] * d.w,
                                    d.y + my[i] * d.w + label.textSize * 0.35f, label)
                }
                val dots = listOf(
                    Triple(0.52f, -0.52f, up && right),
                    Triple(0.52f, 0.52f, down && right),
                    Triple(-0.52f, 0.52f, down && left),
                    Triple(-0.52f, -0.52f, up && left),
                )
                for ((ox, oy, litc) in dots) {
                    fill.color = Color.argb(if (litc) a else a / 8, 0x2E, 0xC4, 0xB6)
                    canvas.drawCircle(d.x + ox * d.w, d.y + oy * d.w, d.w * 0.16f, fill)
                }
            }
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (!settings.padEnabled) return false
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                if (!grab(event.getPointerId(i), event.getX(i), event.getY(i))) {
                    /* Nothing here. Let it through, so a double tap on
                     * the picture still reaches the view underneath. */
                    return false
                }
            }
            MotionEvent.ACTION_MOVE ->
                for (i in 0 until event.pointerCount) {
                    move(event.getPointerId(i), event.getX(i), event.getY(i))
                }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP,
            MotionEvent.ACTION_CANCEL -> release(event.getPointerId(event.actionIndex))
        }
        invalidate()
        onState?.invoke(state)
        return true
    }

    private fun grab(id: Int, x: Float, y: Float): Boolean {
        for (s in sticks) {
            if (hypot(x - s.x, y - s.y) <= s.r * 1.2f) {
                grabbedStick[id] = s
                move(id, x, y)
                return true
            }
        }
        dpad?.let { d ->
            if (hypot(x - d.x, y - d.y) <= d.w) {
                grabbedDpad += id
                move(id, x, y)
                return true
            }
        }
        for (c in controls) {
            if (inside(c, x, y)) {
                grabbedControl[id] = c
                state[c.slot] = 100
                return true
            }
        }
        return false
    }

    private fun inside(c: Control, x: Float, y: Float): Boolean =
        if (c.round) hypot(x - c.x, y - c.y) <= c.w
        else kotlin.math.abs(x - c.x) <= c.w && kotlin.math.abs(y - c.y) <= c.h

    private fun move(id: Int, x: Float, y: Float) {
        grabbedStick[id]?.let { s ->
            val dx = ((x - s.x) / s.r).coerceIn(-1f, 1f)
            val dy = ((s.y - y) / s.r).coerceIn(-1f, 1f)
            val (vx, vy) = shape(dx, dy, s.index)
            state[s.axisX] = vx
            state[s.axisY] = vy
            return
        }
        if (id in grabbedDpad) {
            val d = dpad ?: return
            val dx = x - d.x
            val dy = y - d.y
            /* A quarter of the radius of slack in the middle, so resting
             * a thumb there is not four directions at once. */
            val slack = d.w * 0.25f
            state[Protocol.LEFT] = if (dx < -slack) 100 else 0
            state[Protocol.RIGHT] = if (dx > slack) 100 else 0
            state[Protocol.UP] = if (dy < -slack) 100 else 0
            state[Protocol.DOWN] = if (dy > slack) 100 else 0
            return
        }
        grabbedControl[id]?.let { c ->
            /* Slid off: released. Without this, dragging a thumb from a
             * stick across a button and away left it held for good --
             * found the hard way on the console client. */
            if (!inside(c, x, y)) {
                state[c.slot] = 0
                grabbedControl.remove(id)
            }
        }
    }

    private fun release(id: Int) {
        grabbedStick.remove(id)?.let { s ->
            state[s.axisX] = 0
            state[s.axisY] = 0
        }
        if (grabbedDpad.remove(id)) {
            state[Protocol.UP] = 0; state[Protocol.DOWN] = 0
            state[Protocol.LEFT] = 0; state[Protocol.RIGHT] = 0
        }
        grabbedControl.remove(id)?.let { c -> state[c.slot] = 0 }
    }

    /**
     * Shapes a stick the way the page and the console client shape one:
     * a dead zone at the centre, a range that says how far counts as
     * fully pushed, and a separate range for the corners.
     *
     * Applied to the vector's length rather than to each axis, or a
     * diagonal would be shortened twice and the corners would be the one
     * place the pad could not reach.
     */
    private fun shape(dx: Float, dy: Float, stick: Int): Pair<Byte, Byte> {
        val length = hypot(dx, dy)
        if (length <= 0.0001f) return 0.toByte() to 0.toByte()
        val dead = settings.deadzone(stick) / 100f
        if (length <= dead) return 0.toByte() to 0.toByte()

        /* How far this direction can physically go: the axis limit near
         * the axes, the diagonal limit near the corners, interpolated
         * between. */
        val axis = settings.range(stick) / 100f
        val diag = settings.diagonal(stick) / 100f
        val corner = (kotlin.math.abs(dx) * kotlin.math.abs(dy)) * 2f   // 0 on an axis, 1 in a corner
        val limit = axis + (diag - axis) * corner.coerceIn(0f, 1f)

        val scaled = ((length - dead) / (limit - dead)).coerceIn(0f, 1f)
        val ux = dx / length
        val uy = dy / length
        val vx = (ux * scaled * 100f).roundToInt().coerceIn(-100, 100)
        val vy = (uy * scaled * 100f).roundToInt().coerceIn(-100, 100)
        return vx.toByte() to vy.toByte()
    }

    /** Merges what a physical controller is doing, so both can be used. */
    fun mergeInto(out: ByteArray) {
        for (i in 0 until Protocol.PAD_SLOTS) {
            val mine = state[i].toInt()
            val theirs = out[i].toInt()
            out[i] = when (i) {
                Protocol.LX, Protocol.LY, Protocol.RX, Protocol.RY ->
                    if (kotlin.math.abs(mine) > kotlin.math.abs(theirs)) mine.toByte() else theirs.toByte()
                else -> if (mine > theirs) mine.toByte() else theirs.toByte()
            }
        }
    }
}
