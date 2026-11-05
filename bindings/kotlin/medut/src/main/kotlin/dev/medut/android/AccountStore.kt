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
import java.security.GeneralSecurityException
import java.security.KeyStore
import java.security.ProviderException
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
 * Storage is one JSON file in the app's no-backup directory
 * (`Context.noBackupFilesDir`), encrypted with an AES-GCM key in the Android
 * Keystore, so the SIP and TURN passwords are not readable from a rooted
 * device's file system.  The no-backup directory keeps the file out of Auto
 * Backup and device-to-device transfer, where it would be useless anyway:
 * the Keystore key that decrypts it never leaves the device.  Entries are
 * keyed by the account URI; saving the same URI again replaces it.
 *
 * Keystore and crypto failures (a key invalidated or wiped, a vendor
 * Keystore throwing ProviderException) are contained: they are logged, a
 * read returns nothing, a write recreates the key and retries once, and
 * nothing throws out of [MedutAndroid.start].
 */
class AccountStore(context: Context) {
    private val app = context.applicationContext
    private val file = File(app.noBackupFilesDir, "medut/accounts.json.enc")
    private val lock = Any()

    init {
        // Earlier builds kept the file in filesDir, which Auto Backup copies.
        val legacy = File(app.filesDir, "medut/accounts.json.enc")
        if (legacy.exists()) {
            runCatching {
                if (!file.exists()) {
                    file.parentFile?.mkdirs()
                    if (!legacy.renameTo(file)) {
                        legacy.copyTo(file, overwrite = false)
                        legacy.delete()
                    }
                } else {
                    legacy.delete()
                }
            }.onFailure { Log.w(TAG, "moving the accounts file out of backup: $it") }
        }
    }

    /** Every saved account, in save order.  Empty when the file cannot be
     *  read (logged). */
    fun list(): List<AccountConfig> = synchronized(lock) { load() }

    /** Save (or replace) [config], keyed by its URI.  A failure to write
     *  (storage full, Keystore unusable) is logged, not thrown: the account
     *  works for this process, it just will not be restored by a push. */
    fun save(config: AccountConfig) = synchronized(lock) {
        val key = keyOf(config.uri)
        tryStore("save ${config.uri}", load().filterNot { keyOf(it.uri) == key } + config)
        Unit
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
        if (changed > 0 && !tryStore("updatePushToken", updated)) return@synchronized 0
        changed
    }

    /** Forget the account with this URI or AOR.  Returns true if one was saved. */
    fun remove(uri: String): Boolean = synchronized(lock) {
        val key = keyOf(uri)
        val before = load()
        val after = before.filterNot { keyOf(it.uri) == key }
        if (after.size == before.size) return false
        tryStore("remove $uri", after)
    }

    fun clear() = synchronized(lock) {
        file.delete()
        Unit
    }

    /** [store], logging instead of throwing.  Returns whether it was written. */
    private fun tryStore(what: String, accounts: List<AccountConfig>): Boolean =
        try {
            store(accounts)
            true
        } catch (e: Exception) {
            Log.e(TAG, "$what: the accounts file could not be written: $e")
            false
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
            // A key that no longer exists (keystore wiped, key invalidated)
            // or a corrupt file: there is nothing to recover; start empty.
            // The next write replaces the file (and the key, if it is the
            // key that broke).
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

    private fun keyStore(): KeyStore = KeyStore.getInstance(KEYSTORE).apply { load(null) }

    private fun key(): SecretKey {
        val ks = keyStore()
        try {
            (ks.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }
        } catch (e: GeneralSecurityException) {
            // UnrecoverableKeyException and friends: the entry is unusable.
            Log.w(TAG, "keystore key unusable, recreating it: $e")
            runCatching { ks.deleteEntry(KEY_ALIAS) }
        } catch (e: ProviderException) {
            Log.w(TAG, "keystore key unusable, recreating it: $e")
            runCatching { ks.deleteEntry(KEY_ALIAS) }
        }
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE)
        generator.init(KeyGenParameterSpec.Builder(KEY_ALIAS,
            KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setKeySize(256)
            .build())
        return generator.generateKey()
    }

    /** On a key the Keystore will not use any more (KeyPermanentlyInvalidated-
     *  Exception, a ProviderException from a vendor Keystore), the key is
     *  deleted and recreated once: the whole file is rewritten from
     *  [plain], so nothing encrypted under the old key is still needed. */
    private fun encrypt(plain: ByteArray): ByteArray =
        try {
            encryptWith(key(), plain)
        } catch (e: GeneralSecurityException) {
            Log.w(TAG, "encrypt failed, recreating the keystore key: $e")
            deleteKey()
            encryptWith(key(), plain)
        } catch (e: ProviderException) {
            Log.w(TAG, "encrypt failed, recreating the keystore key: $e")
            deleteKey()
            encryptWith(key(), plain)
        }

    private fun encryptWith(key: SecretKey, plain: ByteArray): ByteArray {
        val cipher = Cipher.getInstance(TRANSFORM)
        cipher.init(Cipher.ENCRYPT_MODE, key)
        val iv = cipher.iv
        return byteArrayOf(iv.size.toByte()) + iv + cipher.doFinal(plain)
    }

    private fun deleteKey() {
        runCatching { keyStore().deleteEntry(KEY_ALIAS) }
            .onFailure { Log.w(TAG, "deleting the keystore key: $it") }
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
        // Not "verifyTls": that key held a non-null flag the core ignored,
        // written as true for every account, so reading it now would turn
        // every saved account into an explicit override.
        c.verifyTls?.let { put("verifyTlsOverride", it) }
        put("pushProvider", c.pushProvider.value)
        putOpt("pushToken", c.pushToken)
        putOpt("pushParam", c.pushParam)
        put("audioCodecs", JSONArray(c.audioCodecs))
        put("dtmfMode", c.dtmfMode.value)
        put("headers", JSONObject(c.headers))
        put("registerHeaders", JSONObject(c.registerHeaders))
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
            verifyTls = if (j.has("verifyTlsOverride")) j.getBoolean("verifyTlsOverride") else null,
            pushProvider = PushProvider.entries.firstOrNull { it.value == j.optInt("pushProvider", 0) }
                ?: PushProvider.NONE,
            pushToken = j.optStringOrNull("pushToken"),
            pushParam = j.optStringOrNull("pushParam"),
            audioCodecs = codecs,
            dtmfMode = DtmfMode.of(j.optInt("dtmfMode", d.dtmfMode.value)),
            headers = j.optJSONObject("headers").toStringMap(),
            registerHeaders = j.optJSONObject("registerHeaders").toStringMap(),
        )
    }

    /** In the order saved: header order can matter to a server. */
    private fun JSONObject?.toStringMap(): Map<String, String> {
        if (this == null) return emptyMap()
        val map = LinkedHashMap<String, String>()
        for (key in keys()) map[key] = getString(key)
        return map
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
