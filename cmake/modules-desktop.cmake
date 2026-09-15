# Curated module set for Linux/macOS/Windows desktop targets.
#
# Audio:   pulse  = PulseAudio / PipeWire (Linux)
#          Replace with audiounit (macOS) or wasapi (Windows) per platform.
# Codecs:  g711 covers both PCMU (G.711 μ-law) and PCMA (G.711 A-law).
# Crypto:  srtp = SDES-SRTP, dtls_srtp = DTLS-SRTP (RFC 5764).
# NAT:     stun/turn/ice = RFC 5389/5766/8445.

set(ECHOSDK_DESKTOP_MODULES
  # Audio codecs — must stay identical to ECHOSDK_MOBILE_MODULES_BASE so the
  # default SDP offer (see BSDK_DEFAULT_AUDIO_CODECS in src/account.c) is
  # satisfiable on every platform.
  "opus"
  "g711"
  # Audio processing
  "aubridge"
  "auconv"
  "auresamp"
  # Audio device (Linux — override per platform)
  "pulse"
  # Crypto
  "srtp"
  "dtls_srtp"
  # NAT traversal
  "stun"
  "turn"
  # SIP features — account/contact/menu intentionally excluded:
  # the SDK creates accounts/contacts programmatically; those modules
  # would load from ~/.baresip/{accounts,contacts} which is not wanted.
  "mwi"
  "presence"
  "uuid"
)
if(ECHOSDK_WITH_WEBRTC_AEC)
  list(APPEND ECHOSDK_DESKTOP_MODULES "webrtc_aec")
endif()
list(JOIN ECHOSDK_DESKTOP_MODULES ";" ECHOSDK_DESKTOP_MODULES)
