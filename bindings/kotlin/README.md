# Medut for Android: Kotlin binding

Kotlin API for the Medut SIP SDK. It wraps the same C core as the Flutter,
Swift, Python and C++ bindings (`include/medut.h`), and is written for apps
where native code owns the stack for the whole process lifetime: started from
`Application.onCreate` or the FCM service, with no Flutter engine involved in
placing or answering calls.

## Build

The AAR packages the prebuilt core (`libmedut.so`, the copies
`scripts/build-android.sh` stages for the Flutter plugin) and compiles only the
small JNI layer (`medut/src/main/cpp/medut_jni.c`):

```sh
cd bindings/kotlin
./gradlew :medut:assembleRelease     # medut/build/outputs/aar/medut-release.aar
```

Use `-Pmedut.jniLibsDir=../../dist/android` to package an unstripped core
instead.

The core API also runs on a desktop JVM against the Linux build, which is how
it is tested without a device:

```sh
scripts/build-linux.sh
bindings/kotlin/desktop-test/run.sh   # needs kotlinc, or KOTLINC_CP
```

## Use

```kotlin
class App : Application() {
    override fun onCreate() {
        super.onCreate()
        MedutAndroid.start(this, MedutConfig(logLevel = LogLevel.INFO))
    }
}

val account = Medut.createAccount(AccountConfig(
    uri = "alice@pbx.example.com",
    password = "secret",
    serverUrl = "wss://pbx.example.com/ws",
    mediaEncryption = MediaEncryption.DTLS_SRTP,
    iceEnabled = true,
    iceServers = listOf(IceServer("stun:stun.l.google.com:19302")),
    pushProvider = PushProvider.FCM,
    pushToken = fcmToken,
    pushParam = packageName,
))
account.register()

Medut.addListener { event ->            // main thread by default
    when (event) {
        is MedutEvent.Registration -> Log.i(TAG, "registration: ${event.state}")
        is MedutEvent.IncomingCall -> showIncomingCall(event.call, event.fromUri)
        is MedutEvent.CallStateChanged -> if (event.state.isTerminal) dismiss(event.call)
        else -> {}
    }
}

val call = account.call("sip:bob@pbx.example.com")
MedutAndroid.audioRoutes.setSpeakerphone(true)
```

## Design notes

- **One stack per process.** `Medut` is a singleton. `start` takes over a
  stack that something else in the process already started (`reattached` is
  then true) instead of failing.
- **Thread-safe.** Call any method from any thread. Events are decoded on the
  SDK's event thread and delivered on the listener's executor: the main thread
  after `MedutAndroid.start`, or a dedicated thread otherwise.
- **Memory-safe handles.** `Account` and `Call` methods never pass a freed
  pointer to C:
  - **After shutdown or a restart**, handles from an earlier stack throw
    `NOT_RUNNING`.
  - **After `destroy()`**, an account throws `ACCOUNT_DESTROYED`.
  - **After a call ends**, it throws `CALL_ENDED`, and the object stays safe to
    hold. It keeps its native wrapper until it becomes unreachable.
- **Defaults come from C.** `MedutConfig()` reads `medut_config_init()`, so
  this binding never keeps a second copy of the defaults.
- **Structs cross JNI by field name.** The C side reads config fields and
  fills result objects by name, so a mismatch fails loudly instead of shifting
  fields. Strings cross as real UTF-8, so raw SIP messages can't trip JNI's
  modified-UTF-8 checks. The consumer ProGuard rules keep those names in
  minified apps.
- **Audio mode.** `AudioModePolicy` decides who owns `MODE_IN_COMMUNICATION`
  and audio focus:
  - `AUTOMATIC` sets them around calls, for apps without Telecom.
  - `TELECOM` leaves them to a self-managed ConnectionService. Route selection
    then only reports.
  - `MANUAL` leaves them to the app.
- **Network handover.** A default-network callback reports only real changes,
  using the same filter the Flutter plugin used.
- **App-owned audio.** `MedutAndroid.useAppOwnedAudio(true)` hands the
  microphone and speaker to an `AudioRecord`/`AudioTrack` engine that captures
  through `VOICE_COMMUNICATION`. Its streams open only once the audio mode is
  in force. Under `MANUAL`, or `TELECOM` with your own ConnectionService,
  report call audio with `MedutAndroid.setCallAudioActive()`; the SDK's own
  Telecom integration does that itself.
- **ICE multipath.** With `MedutConfig(iceMultipath = true)` the SDK keeps a
  cellular network requested next to Wi-Fi while the stack runs and pins its
  extra ICE sockets to it, so a handover offers candidates that already
  exist. Costs battery and data; needs `CHANGE_NETWORK_STATE` (declared).

## Telecom

```kotlin
MedutAndroid.start(this, MedutConfig(), AndroidOptions(telecom = TelecomOptions()))
```

Every call is then also a self-managed Telecom connection (API 26+): it
shows in the system call UI and on Bluetooth, Android Auto and Wear; a
cellular call and a SIP call never fight for audio; Telecom owns the audio
mode and the route, and `MedutAndroid.audioRoutes` selects routes through
the active connection. Nothing else changes: `account.call()` places a call,
`call.answer()` answers one, and the SDK tells Telecom; answering, holding
and hanging up from the system side drive the call back.

An incoming call with no app UI up gets the built-in incoming-call
notification: a `CallStyle` notification (API 31+) that rings until it is
answered or declined, with Answer and Decline. Its full-screen intent opens
`TelecomOptions.callActivity` (else the launch activity) with the call id in
`MedutTelecom.EXTRA_CALL_ID` (`MedutTelecom.callFor(intent)`); Answer answers
first and opens it with `MedutTelecom.ACTION_ANSWER`. Request
`POST_NOTIFICATIONS` on API 33+. To show your own UI instead, set
`MedutTelecom.onShowIncomingCallUi`. `MedutTelecom.onOutgoingCallRefused`
reports an outgoing call Telecom would not create (another call has the
line).

## Push wake-up (FCM)

The SDK carries no Firebase dependency. Give the server the RFC 8599 push
parameters through `AccountConfig(pushProvider = PushProvider.FCM, pushToken
= token, pushParam = firebaseProjectId)` and hand the messaging service's two
calls to the SDK:

```kotlin
class PushService : FirebaseMessagingService() {
    override fun onMessageReceived(message: RemoteMessage) {
        // Application.onCreate started the stack and re-created the accounts.
        val call = MedutPush.wake(this, message.data)
        // With Telecom on, the phone is already ringing; otherwise show `call`.
    }

    override fun onNewToken(token: String) {
        MedutPush.onNewToken(token)           // live accounts and the account store
    }
}
```

`MedutPush.wake` holds a wake lock and reads the data message
(`type`, `call_id`, `caller_uri`, `caller_name`, `aor`; the names are
configurable in `MedutPush.payloadKeys`):

- **A call.** With Telecom on, the phone rings at once with the caller from
  the push. Every account refreshes its registration in place
  (`Account.refreshRegistration()`), which is what an RFC 8599 server waits
  for before it releases the INVITE. The INVITE takes over the ringing call,
  matched by Call-ID when the push has one. An Answer already tapped is
  carried out; a Decline turns the INVITE away with 603. `wake` returns the
  call, or null when none came in time; the ring then ends as missed.
- **A cancel.** Ends a ring whose INVITE never came.

Send the push as a high-priority data message so Android starts the process.
`MedutPush.reachAccounts()` is the same refresh without the wait, for a
foreground return. The payload, the server side and an OpenSIPS
configuration are in [docs/guides/push.md](../../docs/guides/push.md).

After a reboot nothing registers until the first push or launch.
`MedutAndroid.setStartOnBoot(context, true)` turns on the SDK's boot
receiver, which refreshes the saved accounts' registrations once
`Application.onCreate` has started the stack.

## Accounts across restarts

A push starts the process with no UI, and the stack must register before
the INVITE arrives, so the account configs have to be on the native side.
`MedutAndroid.accountStore` keeps them in a Keystore-encrypted file:

```kotlin
val account = Medut.createAccount(cfg)
MedutAndroid.accountStore.save(cfg)      // survives the process
account.register()

MedutAndroid.accountStore.remove(cfg.uri) // on logout
```

`MedutAndroid.start` re-creates and registers every saved account on a fresh
start (`AndroidOptions.restoreAccounts`, on by default), so
`Application.onCreate` is all a cold start needs. A Flutter app gets the same
with `createAccount(remember: true)`. The stack's own `MedutConfig` is not
stored: pass it in `onCreate`, it is app policy rather than user data.

With `AndroidOptions.logToLogcat` (default) the stack's log and call events
go to logcat under the tag `Medut`, so a process running with no UI attached
still leaves a trace.

## Media tap

```kotlin
call.setMediaTap { call, direction, pcm, samples, sampleRate, channels, timestampMicros ->
    // Audio thread: copy `pcm` (a ShortBuffer over the SDK's frame) and go.
}
```

Both directions, every frame, on the SDK's audio thread; the buffer is valid
only for the duration of the callback. `setMediaTap(null)` removes it.

The Flutter plugin (`bindings/flutter`) is a Pigeon layer over this SDK and
compiles it in from source. Telecom reaches Dart as `Medut.start(telecom:
true)`; push handling and the media tap are native-only, since they have no
UI and no Dart isolate to wait for.

Not yet in this module: Core-Telecom (`androidx.core.telecom`) as an
alternative to the ConnectionService.
