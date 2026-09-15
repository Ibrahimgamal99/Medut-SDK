# Curated module set for mobile / embedded targets.
# Platform-specific audio I/O modules are NOT included here; they are
# appended by CMakeLists.txt based on the detected platform:
#   Android → opensles;aaudio
#   Apple   → audiounit;avcapture;coreaudio
#
# Explicitly excluded (too large or desktop-only):
#   ffmpeg, sdl, sdl2, x11, gst, gstVideo, gstRtp, pulse, alsa, pipewire,
#   mosquitto, v4l2, jack, evdev, mqtt, spandsp, aptx, avformat, avcodec

# account/contact/menu are NOT compiled: modules_init.c never loads them
# (they read ~/.baresip/* from disk / drive a CLI — see its header comment).
set(ECHOSDK_MOBILE_MODULES_BASE
  "opus"
  "g711"
  "srtp"
  "dtls_srtp"
  "stun"
  "turn"
  "uuid"
  "mwi"
  "presence"
)
list(JOIN ECHOSDK_MOBILE_MODULES_BASE ";" ECHOSDK_MOBILE_MODULES_BASE)
