package fr.wozt.capture2cloud

import org.junit.Assert.assertEquals
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * The wire format, checked against the C header rather than against
 * itself.
 *
 * The numbers below are transcribed from c2s_protocol.h by hand. That is
 * the point: if someone renumbers a message there and updates Protocol.kt
 * to match, these fail and say so, because a client that agrees with
 * itself and disagrees with the host is the failure mode this whole file
 * exists to catch. The C side pins its own struct sizes with
 * _Static_assert; this is the other half of that.
 */
class ProtocolTest {

    @Test
    fun `the magic is the four letters the host checks for`() {
        // "C2SW" little-endian, as the host writes it.
        assertEquals(0x57533243, Protocol.MAGIC)
    }

    @Test
    fun `message numbers match the header`() {
        assertEquals(1, Protocol.MSG_VIDEO)
        assertEquals(2, Protocol.MSG_AUDIO)
        assertEquals(16, Protocol.MSG_INPUT)
        assertEquals(17, Protocol.MSG_PING)
        assertEquals(18, Protocol.MSG_HOME)
        assertEquals(19, Protocol.MSG_PROFILE)
        assertEquals(20, Protocol.MSG_CODEC)
        assertEquals(21, Protocol.MSG_STREAM_INFO)
        assertEquals(22, Protocol.MSG_KEYFRAME)
        assertEquals(23, Protocol.MSG_WAKE)
        assertEquals(24, Protocol.MSG_RESET_DONGLE)
        assertEquals(25, Protocol.MSG_RESTART)
    }

    @Test
    fun `the pad has the twenty-one slots the host reads`() {
        assertEquals(21, Protocol.PAD_SLOTS)
    }

    /**
     * The slot each control lives in. Getting one of these wrong does not
     * fail loudly: it presses a different button on someone's console,
     * which is the kind of bug that gets blamed on the adapter.
     */
    @Test
    fun `slots are where the host expects them`() {
        assertEquals(0, Protocol.GUIDE)
        assertEquals(1, Protocol.BACK)
        assertEquals(2, Protocol.START)
        assertEquals(4, Protocol.RT)
        assertEquals(7, Protocol.LT)
        assertEquals(9, Protocol.RX)
        assertEquals(10, Protocol.RY)
        assertEquals(11, Protocol.LX)
        assertEquals(12, Protocol.LY)
        assertEquals(13, Protocol.UP)
        assertEquals(16, Protocol.RIGHT)
        assertEquals(17, Protocol.Y)   // north
        assertEquals(18, Protocol.B)   // east
        assertEquals(19, Protocol.A)   // south
        assertEquals(20, Protocol.X)   // west
    }

    /**
     * The handshake the host reads back. Twenty bytes including the three
     * of trailing padding: the C struct is packed and its size is pinned
     * by a _Static_assert, so those bytes are on the wire whether or not
     * they mean anything, and a reader that skips them is three bytes out
     * of step for every frame afterwards.
     */
    @Test
    fun `the acknowledgement is twenty bytes`() {
        val ack = ByteBuffer.allocate(20).order(ByteOrder.LITTLE_ENDIAN)
        ack.putInt(Protocol.MAGIC)
        ack.put(Protocol.VERSION.toByte())
        ack.put(1)                       // accepted
        ack.put(1)                       // may control
        ack.put(0)                       // reserved
        ack.putShort(1280); ack.putShort(720)
        ack.put(Protocol.CODEC_H264.toByte())
        ack.put(Protocol.CODEC_OPUS.toByte())
        ack.putShort(48000.toShort())
        ack.put(2)
        ack.put(ByteArray(3))            // the padding that must be read
        assertEquals(0, ack.remaining())
    }

    @Test
    fun `a frame header is eight bytes, little-endian`() {
        val h = ByteBuffer.allocate(Protocol.HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        h.put(Protocol.MSG_INPUT.toByte())
        h.put(0)
        h.putShort(0)
        h.putInt(Protocol.PAD_SLOTS)
        assertEquals(8, Protocol.HEADER_SIZE)
        assertEquals(0, h.remaining())
        val read = ByteBuffer.wrap(h.array()).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(Protocol.MSG_INPUT, read.get().toInt() and 0xff)
        read.get(); read.short
        assertEquals(Protocol.PAD_SLOTS, read.int)
    }
}
