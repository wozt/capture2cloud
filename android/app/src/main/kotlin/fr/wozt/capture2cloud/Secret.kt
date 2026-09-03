package fr.wozt.capture2cloud

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * Encrypts the saved password with a key the app never sees.
 *
 * A stored password is a stored password, and no amount of cryptography
 * changes that: anyone holding an unlocked phone with this app on it can
 * connect as a player. What this does buy is worth having anyway -- the
 * key lives in the Android keystore and, on hardware that has one, in a
 * secure element, so it cannot be read out of a backup, out of the app's
 * data directory, or by anything else on the device. The password is
 * never on disk in a form anything can read.
 *
 * AES-GCM rather than a cipher chosen for speed: it authenticates as
 * well as encrypts, so a tampered blob fails to decrypt rather than
 * yielding a different password. Each save gets a fresh initialisation
 * vector, stored beside the ciphertext, because reusing one with GCM
 * would be the one mistake that undoes the whole thing.
 */
object Secret {
    private const val KEY_NAME = "capture2cloud.password"
    private const val TRANSFORM = "AES/GCM/NoPadding"
    private const val TAG_BITS = 128

    fun encrypt(plain: String): String = try {
        val cipher = Cipher.getInstance(TRANSFORM)
        cipher.init(Cipher.ENCRYPT_MODE, key())
        val body = cipher.doFinal(plain.toByteArray(Charsets.UTF_8))
        val iv = cipher.iv
        /* iv length, iv, ciphertext -- one blob, so there is nothing to
         * keep in step with anything else. */
        val out = ByteArray(1 + iv.size + body.size)
        out[0] = iv.size.toByte()
        iv.copyInto(out, 1)
        body.copyInto(out, 1 + iv.size)
        Base64.encodeToString(out, Base64.NO_WRAP)
    } catch (e: Exception) {
        ""
    }

    fun decrypt(stored: String): String = try {
        if (stored.isEmpty()) "" else {
            val blob = Base64.decode(stored, Base64.NO_WRAP)
            val ivLength = blob[0].toInt()
            val iv = blob.copyOfRange(1, 1 + ivLength)
            val body = blob.copyOfRange(1 + ivLength, blob.size)
            val cipher = Cipher.getInstance(TRANSFORM)
            cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(TAG_BITS, iv))
            String(cipher.doFinal(body), Charsets.UTF_8)
        }
    } catch (e: Exception) {
        /* A key that has gone -- the app was reinstalled, or the screen
         * lock changed -- makes this fail, and the right answer is an
         * empty password rather than a crash: it costs one typing. */
        ""
    }

    private fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getEntry(KEY_NAME, null) as? KeyStore.SecretKeyEntry)?.let { return it.secretKey }
        val generator = KeyGenerator.getInstance(
            KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        generator.init(
            KeyGenParameterSpec.Builder(
                KEY_NAME,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                /* Deliberately not requiring authentication for each use:
                 * the point is to reconnect without typing anything, and
                 * a key that asks for a fingerprint every time would put
                 * the typing back. */
                .setUserAuthenticationRequired(false)
                .build())
        return generator.generateKey()
    }
}
