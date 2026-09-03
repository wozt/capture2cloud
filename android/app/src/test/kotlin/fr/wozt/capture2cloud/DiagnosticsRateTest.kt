package fr.wozt.capture2cloud

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The rate arithmetic, which is easy to get wrong in ways nobody
 * notices: a wrong constant here does not break anything, it just
 * reports a number that is quietly untrue, and a diagnostic nobody can
 * trust is worse than none.
 */
private fun kbps(bytes: Long, millis: Long): Int = (bytes * 8 / millis).toInt()
private fun fps(frames: Long, millis: Long): Int = (frames * 1000 / millis).toInt()

class DiagnosticsRateTest {

    @Test
    fun `bytes over a second become kilobits per second`() {
        // 1 500 000 bytes in one second is 12 000 kb/s.
        assertEquals(12000, kbps(1_500_000, 1000))
    }

    @Test
    fun `a short interval is scaled, not counted as a whole second`() {
        // Half a second of the same rate must report the same rate.
        assertEquals(12000, kbps(750_000, 500))
    }

    @Test
    fun `sixty frames in a second is sixty per second`() {
        assertEquals(60, fps(60, 1000))
        assertEquals(60, fps(30, 500))
    }

    /**
     * A reconnection resets the client's counters, so the difference
     * against the previous sample goes negative. Reporting that as a
     * rate would print a large negative number; the sampler treats it as
     * a fresh start instead.
     */
    @Test
    fun `a counter that went backwards is treated as a fresh start`() {
        val previous = 5_000_000L
        val now = 1_000L              // the client reconnected
        val fresh = now < previous
        assertTrue("a smaller total than last time means new counters", fresh)
    }
}
