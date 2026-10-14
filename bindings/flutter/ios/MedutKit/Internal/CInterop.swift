import Foundation
import Medut

// Helpers for crossing the C ABI.  Nothing here is public.

/// Keeps the C memory behind a config struct alive until the C call that reads
/// it has returned.  medut_init() and medut_account_create() deep-copy
/// everything, so a pool lives exactly as long as one call.
final class CStringPool {
    private var cleanups: [() -> Void] = []

    func string(_ s: String?) -> UnsafePointer<CChar>? {
        guard let s = s, let p = strdup(s) else { return nil }
        cleanups.append { free(p) }
        return UnsafePointer(p)
    }

    /// NULL-terminated array of C strings, or nil for an empty list.
    func stringArray(_ list: [String]) -> UnsafeMutablePointer<UnsafePointer<CChar>?>? {
        guard !list.isEmpty else { return nil }
        let arr = UnsafeMutablePointer<UnsafePointer<CChar>?>.allocate(capacity: list.count + 1)
        for (i, s) in list.enumerated() { arr[i] = string(s) }
        arr[list.count] = nil
        cleanups.append { arr.deallocate() }
        return arr
    }

    func iceServers(_ list: [ICEServer]) -> UnsafePointer<medut_ice_server_t>? {
        guard !list.isEmpty else { return nil }
        let arr = UnsafeMutablePointer<medut_ice_server_t>.allocate(capacity: list.count)
        for (i, s) in list.enumerated() {
            arr[i] = medut_ice_server_t(url: string(s.url),
                                        user: string(s.username),
                                        pass: string(s.password))
        }
        cleanups.append { arr.deallocate() }
        return UnsafePointer(arr)
    }

    deinit { cleanups.forEach { $0() } }
}

/// String from a nullable C string.
func cString(_ p: UnsafePointer<CChar>?) -> String? {
    p.map { String(cString: $0) }
}

/// Strings from a NULL-terminated array, as the SDP event carries them.
func cStringList(_ p: UnsafePointer<UnsafePointer<CChar>?>?) -> [String] {
    guard var p = p else { return [] }
    var out: [String] = []
    while let s = p.pointee {
        out.append(String(cString: s))
        p += 1
    }
    return out
}

/// String from a fixed `char name[N]` field, which Swift imports as a tuple.
/// Stops at the first NUL, or at the end of the array if there is none.
func fixedCString<T>(_ tuple: T) -> String {
    withUnsafeBytes(of: tuple) { raw in
        let len = raw.firstIndex(of: 0) ?? raw.count
        return String(decoding: raw.prefix(len), as: UTF8.self)
    }
}

/// Write up to `rows` names into a `char names[rows][width]` field, each
/// truncated to width-1 bytes and NUL-terminated.  Returns how many were
/// written, for the matching `_count` field.
func writeFixedCStrings<T>(_ names: [String], into tuple: inout T,
                           rows: Int, width: Int) -> Int32 {
    let used = Array(names.prefix(rows))
    withUnsafeMutableBytes(of: &tuple) { raw in
        for i in 0..<raw.count { raw[i] = 0 }
        for (row, name) in used.enumerated() {
            let bytes = Array(name.utf8.prefix(width - 1))
            for (j, b) in bytes.enumerated() { raw[row * width + j] = b }
        }
    }
    return Int32(used.count)
}

// ── Enum bridging ─────────────────────────────────────────────────────────
//
// The Swift enums carry the same numeric values as the C ones (the C values
// are ABI and never renumbered), so conversion is a raw-value copy in both
// directions.

extension RawRepresentable where RawValue == Int {
    /// The C enum constant with this case's numeric value.
    func cValue<C: RawRepresentable>() -> C where C.RawValue: BinaryInteger {
        C(rawValue: C.RawValue(rawValue))!
    }

    /// The Swift case for a C enum value, or `fallback` for a value this
    /// binding predates.
    init<C: RawRepresentable>(c: C, fallback: Self) where C.RawValue: BinaryInteger {
        self = Self(rawValue: Int(c.rawValue)) ?? fallback
    }
}
