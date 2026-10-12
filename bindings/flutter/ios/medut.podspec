#
# Medut Flutter plugin — iOS.
#
# Vendors the prebuilt dynamic Medut.xcframework (built on macOS/CI by
# scripts/build-ios.sh, which stages it here itself) and compiles the small Swift shim (audio session / speaker routing /
# NWPathMonitor network callbacks).
#
Pod::Spec.new do |s|
  s.name             = 'medut'
  s.version          = '1.0.0'
  s.summary          = 'SIP softphone SDK (baresip-based) for Flutter.'
  s.description      = <<-DESC
UDP/TCP/TLS/WS/WSS SIP registration, calls, codec control, reconnection and
network handover, media statistics and quality alerts — native core driven
over dart:ffi.
                       DESC
  s.homepage         = 'https://github.com/Ibrahimgamal99/Medut-SDK'
  s.license          = { :type => 'BSD-3-Clause', :file => '../LICENSE' }
  s.author           = { 'Medut' => 'noreply@example.com' }
  s.source           = { :path => '.' }

  s.platform         = :ios, '13.0'
  s.swift_version    = '5.0'
  s.source_files     = 'Classes/**/*'
  # MedutExternalAudio.h must reach the generated umbrella header so the
  # Swift shim can instantiate the Objective-C audio engine.
  s.public_header_files = 'Classes/**/*.h'
  s.dependency 'Flutter'

  # Prebuilt native core. Missing? Run scripts/build-ios.sh on macOS (or
  # download the CI artifact) — see docs/quickstart/flutter.md.
  s.vendored_frameworks = 'Frameworks/Medut.xcframework'
  s.preserve_paths      = 'Frameworks/Medut.xcframework'
  s.prepare_command = <<-CMD
    if [ ! -d Frameworks/Medut.xcframework ]; then
      echo "error: Medut.xcframework is missing." >&2
      echo "Build it with scripts/build-ios.sh (macOS) or download the" >&2
      echo "'ios-xcframework' artifact from the build-mobile CI workflow" >&2
      echo "into bindings/flutter/ios/Frameworks/Medut.xcframework." >&2
      exit 1
    fi
  CMD

  s.frameworks = 'AVFoundation', 'AudioToolbox', 'CoreAudio', 'Network'

  # -framework Medut on BOTH sides, and not just vendored_frameworks.
  # CocoaPods copies and embeds the xcframework, but this pod builds as a
  # static library (Flutter's default — no use_frameworks!), so a static lib
  # never links anything itself and the flag does not reach the app target's
  # link line on its own. Everything the Dart side touches is resolved with
  # dlsym at runtime and so never noticed, but MedutExternalAudio.m calls
  # medut_audio_external_push/_pull/_format directly from the VPIO render
  # callback — those are the only link-time references in the app, and without
  # this they fail as "Undefined symbol: _medut_audio_external_push".
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
