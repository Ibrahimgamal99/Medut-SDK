import Foundation
#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif

extension NSLock {
    @inline(__always)
    func locked<T>(_ body: () throws -> T) rethrows -> T {
        lock()
        defer { unlock() }
        return try body()
    }
}

/// Keeps native handles from being used across a shutdown.
///
/// medut_shutdown() frees every account and live call, so a handle from before
/// it is a dangling pointer.  Every API call that passes a handle to C runs
/// under the read side of this lock and checks the handle's generation;
/// shutdown (and account destruction) take the write side, so they wait for
/// in-flight calls and nothing can start against a stack that is going away.
///
/// The C event thread must never take this lock: shutdown holds the write side
/// while medut_shutdown() joins that thread.  It reads `currentGeneration`
/// instead, which has its own lock.
final class StackGate: @unchecked Sendable {
    private let rw: UnsafeMutablePointer<pthread_rwlock_t>
    // Written only under the write side; read under either side.
    private var running = false
    private var generation: UInt64 = 0
    // Copy of `generation` for readers that cannot take `rw`.
    private let snapshotLock = NSLock()
    private var snapshot: UInt64 = 0

    init() {
        rw = .allocate(capacity: 1)
        rw.initialize(to: pthread_rwlock_t())
        pthread_rwlock_init(rw, nil)
    }

    deinit {
        pthread_rwlock_destroy(rw)
        rw.deallocate()
    }

    /// Run `body` with the stack up, on generation `expected` (any generation
    /// when nil), for the whole of `body`.  Not reentrant: never nest it.
    func read<T>(_ expected: UInt64?, _ operation: String,
                 _ body: () throws -> T) throws -> T {
        pthread_rwlock_rdlock(rw)
        defer { pthread_rwlock_unlock(rw) }
        guard running, expected == nil || expected == generation else {
            throw MedutError(.notRunning, operation: operation)
        }
        return try body()
    }

    /// Run `body` exclusively.  `body` gets the state accessors below.
    func write<T>(_ body: (Exclusive) throws -> T) rethrows -> T {
        pthread_rwlock_wrlock(rw)
        defer { pthread_rwlock_unlock(rw) }
        return try body(Exclusive(gate: self))
    }

    /// State access while holding the write side.
    struct Exclusive {
        fileprivate let gate: StackGate

        var running: Bool {
            get { gate.running }
            nonmutating set { gate.running = newValue }
        }

        var generation: UInt64 { gate.generation }

        /// Start a new generation; handles from earlier ones stop working.
        @discardableResult
        func advanceGeneration() -> UInt64 {
            gate.generation += 1
            let g = gate.generation
            gate.snapshotLock.locked { gate.snapshot = g }
            return g
        }

        func isCurrent(_ g: UInt64) -> Bool { gate.running && gate.generation == g }
    }

    var isRunning: Bool {
        pthread_rwlock_rdlock(rw)
        defer { pthread_rwlock_unlock(rw) }
        return running
    }

    /// The generation new handles belong to.  Safe from the event thread.
    var currentGeneration: UInt64 {
        snapshotLock.locked { snapshot }
    }
}
