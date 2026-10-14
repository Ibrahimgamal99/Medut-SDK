#
# Medut Flutter plugin — iOS.
#
# A Pigeon host (Classes/) over MedutKit, the Swift SDK. MedutKit, its
# app-owned audio engine and the prebuilt Medut.xcframework live in
# bindings/swift and are compiled in from source through the symlinks next to
# this file (MedutKit, MedutAudioEngine, Frameworks), so a git dependency on
# the plugin needs no extra Podfile entry.
#
Pod::Spec.new do |s|
  s.name             = 'medut'
  s.version          = '1.0.0'
  s.summary          = 'SIP softphone SDK (baresip-based) for Flutter.'
  s.description      = <<-DESC
UDP/TCP/TLS/WS/WSS SIP registration, calls, codec control, reconnection and
network handover, media statistics and quality alerts — driven by the native
Swift SDK, which owns the stack.
                       DESC
  s.homepage         = 'https://github.com/Ibrahimgamal99/Medut-SDK'
  s.license          = { :type => 'BSD-3-Clause', :file => '../LICENSE' }
  s.author           = { 'Medut' => 'noreply@example.com' }
  s.source           = { :path => '.' }

  s.platform         = :ios, '13.0'
  s.swift_version    = '5.9'
  # Paths through the symlinks are spelled out: CocoaPods' ** glob does not
  # descend into symlinked directories.
  s.source_files     = 'Classes/**/*.swift',
                       'MedutKit/**/*.swift',
                       'MedutAudioEngine/**/*.{h,m}'
  # The engine's header reaches the generated umbrella header, which is how
  # MedutKit's Swift sees the Objective-C class inside this one module.
  s.public_header_files = 'MedutAudioEngine/include/*.h'
  s.dependency 'Flutter'

  # Prebuilt native core with header and module map (MedutKit `import Medut`).
  # Missing? Run scripts/build-ios.sh on macOS or scripts/fetch-prebuilt.sh.
  s.vendored_frameworks = 'Frameworks/Medut.xcframework'
  s.preserve_paths      = 'Frameworks/Medut.xcframework'
  s.prepare_command = <<-CMD
    if [ ! -d Frameworks/Medut.xcframework ]; then
      echo "error: Medut.xcframework is missing." >&2
      echo "Build it with scripts/build-ios.sh (macOS) or fetch the CI build" >&2
      echo "with scripts/fetch-prebuilt.sh; both put it in" >&2
      echo "bindings/swift/Frameworks/Medut.xcframework." >&2
      exit 1
    fi
  CMD

  s.frameworks = 'AVFoundation', 'AudioToolbox', 'CoreAudio', 'Network'

  # -framework Medut on both sides, not just vendored_frameworks: this pod
  # builds as a static library (Flutter's default), which links nothing
  # itself, and MedutAudioEngine.m references the core's symbols directly.
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'IPHONEOS_DEPLOYMENT_TARGET' => '13.0',
    'OTHER_LDFLAGS' => '-framework Medut',
  }

  s.user_target_xcconfig = {
    'OTHER_LDFLAGS' => '-framework Medut',
  }
end
