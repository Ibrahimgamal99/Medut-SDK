import 'dart:ffi';
import 'dart:io';

import 'ffi_bindings.dart';

/// Loads the native library for the current platform.
/// Pass [libPath] to override the default search-path lookup with an
/// absolute path, e.g. '/opt/myapp/medut.so'.
DynamicLibrary loadLib({String? libPath}) {
  if (libPath != null) return DynamicLibrary.open(libPath);
  // iOS: the plugin vendors a dynamic Medut.framework (embedded by
  // CocoaPods). Open it by its @rpath bundle path; fall back to process()
  // for apps that link the core statically themselves.
  if (Platform.isIOS) {
    try {
      return DynamicLibrary.open('Medut.framework/Medut');
    } catch (_) {
      return DynamicLibrary.process();
    }
  }
  // Android: packaged in the plugin's jniLibs; the lib prefix is required
  // for reliable APK extraction and dlopen-by-soname.
  if (Platform.isAndroid) return DynamicLibrary.open('libmedut.so');
  if (Platform.isMacOS) return DynamicLibrary.open('medut.dylib');
  if (Platform.isWindows) return DynamicLibrary.open('medut.dll');
  // Linux
  return DynamicLibrary.open('medut.so');
}

String? _libPath;

/// Set before the first [nativeBindings] access to load from a custom path.
void setLibPath(String path) => _libPath = path;

late final MedutBindings _bindings = MedutBindings(loadLib(libPath: _libPath));

MedutBindings get nativeBindings => _bindings;
