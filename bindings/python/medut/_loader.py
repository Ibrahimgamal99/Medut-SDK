"""
_loader.py — cffi FFI setup for Medut.

Reads _medut_clean.h (generated from include/medut.h by build.sh) so
the Python binding stays in sync with the C header automatically.

The library is searched in this order:
  1. MEDUT_LIB environment variable (absolute path to .so/.dylib/.dll)
  2. LD_LIBRARY_PATH / DYLD_LIBRARY_PATH directories
  3. The package directory (bundled .so copied by build.sh)
  4. The repo's dist/ directory (source checkout without install)
  5. System library paths
"""

import ctypes
import os
import platform
import sys
from cffi import FFI


# Hard runtime deps on Linux — these are DT_NEEDED entries in medut.so.
# libm/libresolv/libgcc_s/libc are always present; only list libs that can
# realistically be absent on a normal desktop/server install.
#
# libwebrtc-audio-processing-1 is intentionally NOT here — the shim in
# Medut handles it via dlopen; missing = AEC unavailable, not a crash.
_REQUIRED_LINUX_LIBS = {
    "libpulse.so.0": {
        "debian": "apt install libpulse0",
        "fedora": "dnf install pulseaudio-libs",
        "arch":   "pacman -S libpulse",
        "suse":   "zypper install libpulse0",
    },
}

# Only a local build (scripts/build-linux.sh) links the system OpenSSL 3; the
# published wheels carry the portable core with OpenSSL linked in. So these
# are not checked up front, only named when loading the library fails.
_LOCAL_BUILD_LINUX_LIBS = {
    "libssl.so.3": {
        "debian": "apt install libssl3",
        "fedora": "dnf install openssl-libs",
        "arch":   "pacman -S openssl",
        "suse":   "zypper install libopenssl3",
    },
    "libcrypto.so.3": {
        "debian": "apt install libssl3",
        "fedora": "dnf install openssl-libs",
        "arch":   "pacman -S openssl",
        "suse":   "zypper install libopenssl3",
    },
}

# medut.dll is built with <arch>-windows-static-md — OpenSSL and zlib are
# statically embedded, but the MSVC runtime is dynamically linked and may
# be absent on a clean Windows Server install.
_WINDOWS_ARCH = "arm64" if platform.machine().lower() in ("arm64", "aarch64") else "x64"
_REQUIRED_WINDOWS_LIBS = {
    "vcruntime140.dll": (
        "Microsoft Visual C++ Redistributable is not installed.\n"
        f"    → winget install Microsoft.VCRedist.2022.{_WINDOWS_ARCH}\n"
        f"    or download from https://aka.ms/vs/17/release/vc_redist.{_WINDOWS_ARCH}.exe"
    ),
}


def _distro_family():
    try:
        with open("/etc/os-release") as f:
            vals = {}
            for line in f:
                k, _, v = line.partition("=")
                vals[k.strip()] = v.strip().strip('"').lower()
        combined = vals.get("ID", "") + " " + vals.get("ID_LIKE", "")
        for family in ("debian", "ubuntu", "fedora", "rhel", "centos", "arch", "suse"):
            if family in combined:
                if family in ("debian", "ubuntu"):
                    return "debian"
                if family in ("fedora", "rhel", "centos"):
                    return "fedora"
                return family
    except OSError:
        pass
    return None


def _missing_linux_libs(libs):
    """Install hints for the libraries in *libs* that cannot be loaded."""
    missing = []
    seen_pkgs = set()  # deduplicate libs that share the same package (libssl3/libcrypto)
    for lib, cmds in libs.items():
        try:
            ctypes.CDLL(lib)
        except OSError:
            family = _distro_family()
            cmd = cmds.get(family) if family else None
            pkg_key = cmd or lib
            if pkg_key not in seen_pkgs:
                seen_pkgs.add(pkg_key)
                if cmd:
                    missing.append(f"  {lib}\n    → {cmd}")
                else:
                    missing.append(f"  {lib}\n    → install via your distro's package manager")
    return missing


def _check_linux_deps():
    missing = _missing_linux_libs(_REQUIRED_LINUX_LIBS)
    if missing:
        raise ImportError(
            "Medut: missing required system libraries.\n\n"
            + "\n".join(missing)
        )


def _check_windows_deps():
    missing = []
    for dll, message in _REQUIRED_WINDOWS_LIBS.items():
        try:
            ctypes.CDLL(dll)
        except OSError:
            missing.append(message)
    if missing:
        raise ImportError("Medut: missing required runtime.\n\n" + "\n\n".join(missing))


_here = os.path.dirname(os.path.abspath(__file__))
_repo_root = os.path.normpath(os.path.join(_here, "..", "..", ".."))

ffi = FFI()
with open(os.path.join(_here, "_medut_clean.h")) as _f:
    ffi.cdef(_f.read())


def _lib_names():
    if sys.platform == "win32":
        return ["medut.dll"]
    if sys.platform == "darwin":
        return ["medut.dylib"]
    return ["medut.so"]


def _dist_candidates():
    machine = platform.machine().lower()
    if sys.platform == "win32":
        yield os.path.join(_repo_root, "dist", "windows", _WINDOWS_ARCH, "medut.dll")
    elif sys.platform == "darwin":
        yield os.path.join(_repo_root, "dist", "macos", "universal", "medut.dylib")
    else:
        arch = "aarch64" if machine in ("aarch64", "arm64") else "x86_64"
        yield os.path.join(_repo_root, "dist", "linux", arch, "medut.so")


def _find_lib():
    # 1. Explicit override
    env = os.environ.get("MEDUT_LIB")
    if env:
        return env

    names = _lib_names()

    # 2. LD_LIBRARY_PATH / DYLD_LIBRARY_PATH
    if sys.platform == "darwin":
        ld_path = os.environ.get("DYLD_LIBRARY_PATH", "")
    else:
        ld_path = os.environ.get("LD_LIBRARY_PATH", "")
    for directory in filter(None, ld_path.split(":")):
        for name in names:
            candidate = os.path.join(directory, name)
            if os.path.exists(candidate):
                return candidate

    # 3. Bundled next to the package (copied by build.sh)
    for name in names:
        candidate = os.path.join(_here, name)
        if os.path.exists(candidate):
            return candidate

    # 4. Repo dist/ (source checkout)
    for candidate in _dist_candidates():
        if os.path.exists(candidate):
            return candidate

    # 5. System paths
    return names[0]


if sys.platform == "linux":
    _check_linux_deps()
elif sys.platform == "win32":
    _check_windows_deps()

try:
    lib = ffi.dlopen(_find_lib())
except OSError as e:
    missing = []
    if sys.platform == "linux":
        missing = _missing_linux_libs(
            {k: v for k, v in _LOCAL_BUILD_LINUX_LIBS.items() if k in str(e)})
    if missing:
        raise ImportError(
            "Medut: missing required system libraries.\n\n"
            + "\n".join(missing)
        ) from e
    raise

__all__ = ["ffi", "lib"]
