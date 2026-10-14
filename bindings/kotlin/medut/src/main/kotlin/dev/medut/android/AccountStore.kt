package dev.medut.android

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Log
import dev.medut.Account
import dev.medut.AccountConfig
import dev.medut.DtmfMode
import dev.medut.IceServer
import dev.medut.IceTransportPolicy
import dev.medut.MediaEncryption
import dev.medut.Medut
import dev.medut.PushProvider
import dev.medut.Transport
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * Accounts that outlive the process: `MedutAndroid.accountStore`.
 *
 * The reason native code owns the stack is that a push can start the process
 * with no UI, and the stack must then register before the INVITE arrives.
 * That needs the account configs on the native side, so [save] keeps them
 * here, and [MedutAndroid.start] re-creates and registers every saved account
 * (`AndroidOptions.restoreAccounts`).  Apps that keep their own account
 * storage can do the same from `Application.onCreate` with [restore], or
 * never touch this.
 *
 * Storage is one JSON file in the app's private files directory, encrypted
 * with an AES-GCM key in the Android Keystore, so the SIP and TURN passwords
 * are not readable from a backup or a rooted device's file system.  Entries
 * are keyed by the account URI; saving the same URI again replaces it.
 */
class AccountStore(context: Context) {
    private val app = context.applicationContext
    private val file = File(app.filesDir, "medut/accounts.json.enc")
    private val lock = Any()

    /** Every saved account, in save order. */
    fun list(): List<AccountConfig> = synchronized(lock) { load() }

    /** Save (or replace) [config], keyed by its URI. */
    fun save(config: AccountConfig) = synchronized(lock) {
        val key = keyOf(config.uri)
        store(load().filterNot { keyOf(it.uri) == key } + config)
    }

    /**
     * Replace the push token of the saved account with this URI or AOR, or
     * of every saved account when [uri] is null, skipping any without a push
     * provider, so a process a push starts registers with the current token.
     * Returns how many were updated.  [MedutPush.onNewToken] calls this.
     */
    @JvmOverloads
    fun updatePushToken(token: String?, uri: String? = null): Int = synchronized(lock) {
        val key = uri?.let { keyOf(it) }
        var changed = 0
        val updated = load().map {
            if (it.pushProvider == PushProvider.NONE || it.pushToken == token ||
                (key != null && keyOf(it.uri) != key)) it
            else it.copy(pushToken = token).also { changed++ }
        }
        if (changed > 0) store(updated)
        changed
    }

    /** Forget the account with this URI or AOR.  Returns true if one was saved. */
    fun remove(uri: String): Boolean = synchronized(lock) {
        val key = keyOf(uri)
        val before = load()
        val after = before.filterNot { keyOf(it.uri) == key }
        if (after.size == before.size) return false
        store(after)
        true
    }

    fun clear() = synchronized(lock) {
        file.delete()
        Unit
    }

    /**
     * Create every saved account on the running stack, skipping any whose
     * AOR is already live (a reattached stack has them), and start
     * registering them when [register] is set.  Returns the accounts created.
     */
    @JvmOverloads
    fun restore(register: Boolean = true): List<Account> {
        val live = Medut.accounts.map { keyOf(it.aor) }.toSet()
        val created = ArrayList<Account>()
        for (config in list()) {
            if (keyOf(config.uri) in live) continue
            val account = try {
                Medut.createAccount(config)
            } catch (e: Exception) {
                Log.w(TAG, "restore ${config.uri}: $e")
                continue
            }
            created.add(account)
            if (register) runCatching { account.register() }
                .onFailure { Log.w(TAG, "register ${config.uri}: $it") }
        }
        return created
    }

    // ── Keys ─────────────────────────────────────────────────────────────

    /** "sip:Alice@PBX.example.com:5060" and "alice@pbx.example.com" are the
     *  same account. */
    private fun keyOf(uri: String): String {
        var s = uri.trim().lowercase()
        if (s.startsWith("sip:")) s = s.removePrefix("sip:")
        if (s.startsWith("sips:")) s = s.removePrefix("sips:")
        val at = s.lastIndexOf('@')
        if (at >= 0) {
            val host = s.substring(at + 1).substringBefore(';').substringBefore(':')
            s = s.substring(0, at + 1) + host
        }
        return s
    }

    // ── File ─────────────────────────────────────────────────────────────

    private fun load(): List<AccountConfig> {
        if (!file.exists()) return emptyList()
        return try {
            val json = JSONArray(String(decrypt(file.readBytes()), Charsets.UTF_8))
            (0 until json.length()).map { fromJson(json.getJSONObject(it)) }
        } catch (e: Exception) {
            // A key that no longer exists (keystore wiped) or a corrupt file:
            // there is nothing to recover; start empty.
            Log.w(TAG, "accounts file unreadable, starting empty: $e")
            emptyList()
        }
    }

    private fun store(accounts: List<AccountConfig>) {
        val json = JSONArray().apply { accounts.forEach { put(toJson(it)) } }
        file.parentFile?.mkdirs()
        val tmp = File(file.path + ".tmp")
        tmp.writeBytes(encrypt(json.toString().toByteArray(Charsets.UTF_8)))
        if (!tmp.renameTo(file)) {
            file.delete()
            tmp.renameTo(file)
        }
    }

    // ── Crypto ───────────────────────────────────────────────────────────

    private fun key(): SecretKey {
        val ks = KeyStore.getInstance(KEYSTORE).apply { load(null) }
        (ks.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE)
        generator.init(KeyGenParameterSpec.Builder(KEY_ALIAS,
            KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setKeySize(256)
            .build())
        return generator.generateKey()
    }

    private fun encrypt(plain: ByteArray): ByteArray {
        val cipher = Cipher.getInstance(TRANSFORM)
        cipher.init(Cipher.ENCRYPT_MODE, key())
        val iv = cipher.iv
        return byteArrayOf(iv.size.toByte()) + iv + cipher.doFinal(plain)
    }

    private fun decrypt(blob: ByteArray): ByteArray {
        val ivLen = blob[0].toInt()
        val iv = blob.copyOfRange(1, 1 + ivLen)
        val cipher = Cipher.getInstance(TRANSFORM)
        cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, iv))
        return cipher.doFinal(blob, 1 + ivLen, blob.size - 1 - ivLen)
    }

    // ── JSON ─────────────────────────────────────────────────────────────

    private fun toJson(c: AccountConfig) = JSONObject().apply {
        put("uri", c.uri)
        put("password", c.password)
        put("transport", c.transport.value)
        putOpt("serverUrl", c.serverUrl)
        putOpt("serverHost", c.serverHost)
        put("serverPort", c.serverPort)
        putOpt("outboundProxy", c.outboundProxy)
        putOpt("authUser", c.authUser)
        putOpt("displayName", c.displayName)
        put("mediaEncryption", c.mediaEncryption.value)
        put("iceEnabled", c.iceEnabled)
        c.rtcpMux?.let { put("rtcpMux", it) }
        putOpt("stunServer", c.stunServer)
        putOpt("turnServer", c.turnServer)
        putOpt("turnUser", c.turnUser)
        putOpt("turnPass", c.turnPass)
        put("iceServers", JSONArray().apply {
            c.iceServers.forEach { s ->
                put(JSONObject().apply {
                    put("url", s.url)
                    putOpt("username", s.username)
                    putOpt("password", s.password)
                })
            }
        })
        put("iceTransportPolicy", c.iceTransportPolicy.value)
        put("verifyTls", c.verifyTls)
        put("pushProvider", c.pushProvider.value)
        putOpt("pushToken", c.pushToken)
        putOpt("pushParam", c.pushParam)
        put("audioCodecs", JSONArray(c.audioCodecs))
        put("dtmfMode", c.dtmfMode.value)
    }

    private fun fromJson(j: JSONObject): AccountConfig {
        val d = AccountConfig(j.getString("uri"), j.optString("password", ""))
        val servers = j.optJSONArray("iceServers")?.let { arr ->
            (0 until arr.length()).map { i ->
                val s = arr.getJSONObject(i)
                IceServer(s.getString("url"), s.optStringOrNull("username"), s.optStringOrNull("password"))
            }
        } ?: emptyList()
        val codecs = j.optJSONArray("audioCodecs")?.let { arr ->
            (0 until arr.length()).map { arr.getString(it) }
        } ?: emptyList()
        return d.copy(
            transport = Transport.of(j.optInt("transport", d.transport.value)),
            serverUrl = j.optStringOrNull("serverUrl"),
            serverHost = j.optStringOrNull("serverHost"),
            serverPort = j.optInt("serverPort", 0),
            outboundProxy = j.optStringOrNull("outboundProxy"),
            authUser = j.optStringOrNull("authUser"),
            displayName = j.optStringOrNull("displayName"),
            mediaEncryption = MediaEncryption.of(j.optInt("mediaEncryption", d.mediaEncryption.value)),
            iceEnabled = j.optBoolean("iceEnabled", false),
            rtcpMux = if (j.has("rtcpMux")) j.getBoolean("rtcpMux") else null,
            stunServer = j.optStringOrNull("stunServer"),
            turnServer = j.optStringOrNull("turnServer"),
            turnUser = j.optStringOrNull("turnUser"),
            turnPass = j.optStringOrNull("turnPass"),
            iceServers = servers,
            iceTransportPolicy = IceTransportPolicy.of(j.optInt("iceTransportPolicy", 0)),
            verifyTls = j.optBoolean("verifyTls", true),
            pushProvider = PushProvider.entries.firstOrNull { it.value == j.optInt("pushProvider", 0) }
                ?: PushProvider.NONE,
            pushToken = j.optStringOrNull("pushToken"),
            pushParam = j.optStringOrNull("pushParam"),
            audioCodecs = codecs,
            dtmfMode = DtmfMode.of(j.optInt("dtmfMode", d.dtmfMode.value)),
        )
    }

    private fun JSONObject.optStringOrNull(name: String): String? =
        if (has(name) && !isNull(name)) getString(name) else null

    private companion object {
        const val TAG = "MedutAccounts"
        const val KEYSTORE = "AndroidKeyStore"
        const val KEY_ALIAS = "dev.medut.accounts"
        const val TRANSFORM = "AES/GCM/NoPadding"
    }
}
