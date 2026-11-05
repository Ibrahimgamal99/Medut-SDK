#
# Medut Flutter plugin — iOS.
#
# A Pigeon host (Classes/) over MedutKit, the Swift SDK. MedutKit, its
# app-owned audio engine and the prebuilt Medut.xcframework live next to
# this file (MedutKit/, MedutAudioEngine/, Frameworks/) and are compiled in
# from source, so a git dependency on the plugin needs no extra Podfile entry.
# The Swift package in bindings/swift reaches the same directories through
# symlinks: CocoaPods never sees files behind a symlinked directory, so the
# real ones have to be on this side.
#
Pod::Spec.new do |s|
  s.name             = 'medut'
  s.version          = '2.0.0'
  s.summary          = 'SIP softphone SDK for Flutter (Medut stack on libre).'
  s.description      = <<-DESC
UDP/TCP/TLS/WS/WSS SIP registration, calls, codec control, reconnection and
network handover, media statistics and quality alerts — driven by the native
Swift SDK (MedutKit), which owns the stack, with CallKit and PushKit support.
                       DESC
  s.homepage         = 'https://github.com/Ibrahimgamal99/Medut-SDK'
  s.license          = { :type => 'BSD-3-Clause', :file => '../LICENSE' }
  s.author           = { 'Medut' => 'noreply@example.com' }
  s.source           = { :path => '.' }

  s.platform         = :ios, '13.0'
  s.swift_version    = '5.9'
  s.source_files     = 'Classes/**/*.swift',
                       'MedutKit/**/*.swift',
                       'MedutAudioEngine/**/*.{h,m}'
  # The engine's header reaches the generated umbrella header, which is how
  # MedutKit's Swift sees the Objective-C class inside this one module.
  s.public_header_files = 'MedutAudioEngine/include/*.h'
  s.dependency 'Flutter'

  # Prebuilt native core with header and module map (MedutKit and the
  # plugin `import Medut`). Missing? Run scripts/build-ios.sh on macOS,
  # scripts/fetch-prebuilt.sh, or download the CI artifact — see
  # docs/quickstart/flutter.md.
  s.vendored_frameworks = 'Frameworks/Medut.xcframework'
  s.preserve_paths      = 'Frameworks/Medut.xcframework'
  s.prepare_command = <<-CMD
    if [ ! -d Frameworks/Medut.xcframework ]; then
      echo "error: Medut.xcframework is missing." >&2
      echo "Build it with scripts/build-ios.sh (macOS), or fetch a" >&2
      echo "'Build mobile libraries' CI build with scripts/fetch-prebuilt.sh," >&2
      echo "into" >&2
      echo "bindings/flutter/ios/Frameworks/Medut.xcframework." >&2
      exit 1
    fi
  CMD

  s.frameworks = 'AVFoundation', 'AudioToolbox', 'CoreAudio', 'Network', 'CallKit'

  # -framework Medut on BOTH sides, and not just vendored_frameworks.
  # CocoaPods copies and embeds the xcframework, but this pod builds as a
  # static library (Flutter's default — no use_frameworks!), which links
  # nothing itself, so the flag does not reach the app target's link line on
  # its own; MedutKit and MedutAudioEngine.m reference the core's symbols
  # directly and would fail as "Undefined symbol: _medut_...".
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    # Simulator x86_64 slice exists, so no arch exclusions needed.
    'IPHONEOS_DEPLOYMENT_TARGET' => '13.0',
    'OTHER_LDFLAGS' => '-framework Medut',
  }

  s.user_target_xcconfig = {
    'OTHER_LDFLAGS' => '-framework Medut',
  }
end
