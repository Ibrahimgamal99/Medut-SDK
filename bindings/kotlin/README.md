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
  in force. Under `TELECOM` or `MANUAL`, report call audio with
  `MedutAndroid.setCallAudioActive()`.

The Flutter plugin (`bindings/flutter`) is a Pigeon layer over this SDK and
compiles it in from source.

Not yet in this module: ConnectionService / Core-Telecom and FCM integration,
the media tap, and the cellular network request for ICE multipath.
