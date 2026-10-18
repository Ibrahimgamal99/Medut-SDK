import 'dart:ffi';
import 'dart:io';

import 'ffi_bindings.dart';

/// Loads the C library for the desktop backend (lib/src/ffi_backend.dart).
/// Pass [libPath] to override the default search-path lookup with an
/// absolute path, e.g. '/opt/myapp/medut.so'.
DynamicLibrary loadLib({String? libPath}) {
  if (libPath != null) return DynamicLibrary.open(libPath);
  // Android and iOS never get here: there the native SDKs own the stack.
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
