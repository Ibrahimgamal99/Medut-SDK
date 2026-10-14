# Architecture overview

## Thread model

Medut uses three threads internally:

```
Your code (any thread)
      │
      │  public API calls
      ▼
  dispatch bridge  ──────────────────►  re_main thread
  (bsdk_dispatch)                       (SIP state machine, I/O)
                                                │
                                                │  events enqueued
                                                ▼
                                        event queue (max 4096)
                                                │
                                                │  dequeued + decoded
                                                ▼
                                        event dispatch thread
                                                │
                                                │  your event_cb called
                                                ▼
                                        your event callback
```

**Key rules:**
- All public API functions are thread-safe — call from any thread.
- Event callbacks are called from the **event dispatch thread**, not `re_main`. You may call Medut APIs from inside the callback.
- Keep the callback fast (< 10 ms). Heavy work (recording, transcription) should be dispatched to your own thread.

---

## Lifecycle

```
medut_config_init()        ← fill config struct with defaults
medut_init()               ← start re_main + event threads
  medut_account_create()   ← create account (does not register)
  medut_account_register() ← send REGISTER
    ...events...
  medut_account_destroy()  ← unregister + free
medut_shutdown()           ← stop all threads, free everything
```

`medut_init()` is not idempotent: on a stack that is already up it returns
`MEDUT_ERR_ALREADY` and changes nothing. Config applies at init only.

---

## Reattaching to a live stack

The stack belongs to the **process**, not to the runtime that started it. A host
that can lose and rebuild its own runtime while the process survives comes back
to a stack that is still up and still registered — a re-loaded plugin or
scripting VM, or a second SDK layer in the same app that started second.

Re-initializing is not the recovery; re-pointing is:

```c
if (medut_is_initialized()) {
        /* Take over event delivery from the consumer that is gone. */
        medut_set_event_handler(my_event_cb, my_userdata, /*owned=*/true);

        /* Re-derive the handles it held instead of creating duplicates. */
        medut_account_foreach(adopt_account, NULL);   /* + medut_account_get_aor() */
        medut_call_foreach(adopt_call, NULL);         /* + medut_call_get_state() */
}
else {
        medut_init(&cfg);
}
```

Events that fired while nobody was listening are **dropped, not buffered** — so
recover state, not history. An INVITE that arrived during the gap is not
replayed as an event, but the call is still live: `medut_call_foreach()` finds
it and `medut_call_get_state()` reports it still `RINGING`. Registration state
comes back the same way via `medut_account_get_reg_state()`.

Going the other way, a consumer that knows it is about to disappear should call
`medut_set_event_handler(NULL, NULL, false)` so nothing is delivered into a
runtime being torn down. The stack stays up, registered and push-reachable.

The Kotlin and Swift SDKs do this for you when their `start` finds the stack
already running (their `reattached` is then true). The Flutter plugin sits on
top of them, so its `Medut.start()` attaches to a stack native code started at
launch or from a push — see [Flutter](../quickstart/flutter.md).

---

## Config deep-copy

All strings passed to `medut_init()` and `medut_account_create()` are **deep-copied** internally. You may free your config structs immediately after the call returns.

---

## ABI stability

- Fields are only **appended** to structs — never reordered or removed.
- `medut_config_t` carries `version` and `struct_size` fields. Always call `medut_config_init()` to zero-fill and set these correctly before populating.
- Opaque handle types (`medut_account_handle_t`, `medut_call_handle_t`) are stable across minor versions.
