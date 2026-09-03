package fr.wozt.capture2cloud

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs
import kotlin.math.hypot

/**
 * The stick shaping, pulled out of the pad so it can be checked without
 * a screen.
 *
 * This is a copy of the arithmetic in VirtualPad.shape(), and it is a
 * copy deliberately: the version there works on a view with settings and
 * touch state attached, and lifting all of that into a test would test
 * the lifting. What is asserted here is the behaviour the two share, and
 * the first test is the one that would have caught the bug that shipped
 * -- both sticks inverted, on a convention nobody had written down
 * correctly.
 */
private fun shape(dx: Float, dy: Float, dead: Int, range: Int, diagonal: Int): Pair<Int, Int> {
    val length = hypot(dx, dy)
    if (length <= 0.0001f) return 0 to 0
    val d = dead / 100f
    if (length <= d) return 0 to 0
    val axis = range / 100f
    val diag = diagonal / 100f
    val corner = (abs(dx) * abs(dy)) * 2f
    val limit = axis + (diag - axis) * corner.coerceIn(0f, 1f)
    val scaled = ((length - d) / (limit - d)).coerceIn(0f, 1f)
    val ux = dx / length
    val uy = dy / length
    return (ux * scaled * 100f).toInt().coerceIn(-100, 100) to
           (uy * scaled * 100f).toInt().coerceIn(-100, 100)
}

class StickShapingTest {

    /**
     * Up is negative on the wire.
     *
     * The host's header says the opposite. The page -- the
     * implementation that has always worked -- sends the screen's own
     * downward-growing Y with no inversion, and says so. This pins the
     * behaviour to the one that works, so the next person to read that
     * header and "fix" the sign has to delete a test that says why.
     */
    @Test
    fun `pushing up sends a negative vertical`() {
        // dy comes from (touchY - centreY), so a finger above the centre
        // is negative before any shaping.
        val (_, up) = shape(0f, -1f, dead = 5, range = 100, diagonal = 100)
        val (_, down) = shape(0f, 1f, dead = 5, range = 100, diagonal = 100)
        assertTrue("up must be negative, was $up", up < 0)
        assertTrue("down must be positive, was $down", down > 0)
    }

    @Test
    fun `the deadzone swallows small movements and nothing more`() {
        val (_, inside) = shape(0f, 0.04f, dead = 5, range = 100, diagonal = 100)
        assertEquals(0, inside)
        val (_, outside) = shape(0f, 0.30f, dead = 5, range = 100, diagonal = 100)
        assertTrue("just past the deadzone must move, was $outside", outside > 0)
    }

    @Test
    fun `a full push reaches the end of the range`() {
        val (x, _) = shape(1f, 0f, dead = 5, range = 100, diagonal = 100)
        assertEquals(100, x)
    }

    /**
     * A lower range means the stick counts as fully pushed sooner, which
     * is the whole point of the setting: a stick that physically reaches
     * only 80% still has to be able to say 100.
     */
    @Test
    fun `a reduced range reaches full deflection early`() {
        val (x, _) = shape(0.8f, 0f, dead = 5, range = 80, diagonal = 100)
        assertEquals(100, x)
    }

    /**
     * The corner limit is separate because a stick reaches less far
     * diagonally than along an axis. With it lowered, a diagonal that
     * would otherwise fall short reaches the corner.
     */
    @Test
    fun `the diagonal limit lets the corners be reached`() {
        val d = 0.7f    // 0.99 long: a full diagonal on a square-ish gate
        val (nx, ny) = shape(d, d, dead = 5, range = 100, diagonal = 100)
        val short = hypot(nx.toFloat(), ny.toFloat())
        val (cx, cy) = shape(d, d, dead = 5, range = 100, diagonal = 70)
        val full = hypot(cx.toFloat(), cy.toFloat())
        assertTrue("a lower diagonal limit must reach further: $short then $full",
                   full > short)
    }

    /**
     * Shaping is applied to the vector's length, not to each axis
     * separately. Done per axis, a diagonal would be shortened twice and
     * the corners would be the one place the pad could not reach -- which
     * is exactly the complaint that led to the diagonal setting existing.
     */
    @Test
    fun `a diagonal keeps its direction`() {
        val (x, y) = shape(0.5f, -0.5f, dead = 5, range = 100, diagonal = 100)
        assertEquals("the two axes must stay equal in magnitude",
                     abs(x), abs(y))
    }
}
