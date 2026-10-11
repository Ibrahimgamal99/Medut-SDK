# Accounts & registration

## Create an account

```c
echosdk_account_config_t cfg = {
    .uri       = "alice@pbx.example.com",
    .password  = "secret",
    .transport = ECHOSDK_TRANSPORT_TLS,
};
echosdk_account_handle_t acct;
int rc = echosdk_account_create(&cfg, &acct);
```

`echosdk_account_create` does **not** register. Call `echosdk_account_register` to start.

## Register

```c
echosdk_account_register(acct);
```

Fires `ECHOSDK_EV_REG_STATE` events: `REGISTERING → REGISTERED` or `FAILED`.

On failure the SDK automatically retries with exponential backoff (configurable via `reg_retry_*` fields in `echosdk_config_t`).

## Unregister

```c
echosdk_account_unregister(acct);
// fires ECHOSDK_EV_REG_STATE with UNREGISTERING → UNREGISTERED
```

## Destroy

```c
echosdk_account_destroy(acct);
// blocks until unregistered and all calls on this account are terminated
```

## Push notifications

The SDK supports two modes for delivering a push token to the SIP server so it can wake the device when an INVITE arrives.

### Mode 1 — RFC 8599 Contact URI params (self-hosted servers)

Set the token at account creation time via `echosdk_account_config_t`:

```c
echosdk_account_config_t cfg = {
    .uri            = "alice@pbx.example.com",
    .password       = "secret",
    .transport      = ECHOSDK_TRANSPORT_TLS,
    .push_provider  = ECHOSDK_PUSH_PROVIDER_APNS,  // or APNS_SANDBOX / FCM
    .push_token     = "<device-token-hex>",
    .push_param     = "com.example.MyApp",          // bundle ID (APNs) or package name (FCM)
};
```

The SDK embeds the params **inside** the Contact angle brackets on every REGISTER:

```
Contact: <sip:alice@10.0.5.2;pn-provider=apns;pn-prid=TOKEN;pn-param=com.example.MyApp>
```

The server (Kamailio `push_notification` module, drachtio, etc.) reads these params from its registrar table and uses them to wake the device via APNs or FCM.

#### Update the token at runtime

PushKit tokens rotate. Update without re-creating the account:

```c
echosdk_account_set_push_token(acct, new_token);
```

The SDK re-registers immediately (unless a transaction is in flight or a retry backoff is pending — in those cases the new token is applied on the next natural re-registration). Pass `NULL` to clear push params.

> **APNs environment note:** use `ECHOSDK_PUSH_PROVIDER_APNS_SANDBOX` for debug/TestFlight builds (PushKit sandbox APNs endpoint) and `ECHOSDK_PUSH_PROVIDER_APNS` for App Store / production builds. Mismatching the environment causes silent delivery failures at the APNs level — pushes appear to succeed but never arrive.

### Mode 2 — REGISTER-only custom headers (hosted / vendor servers)

For servers you do not control (Twilio, Plivo, hosted PBXes) where push dispatch is server-managed via non-standard headers:

```c
echosdk_account_add_register_header(acct, "X-Push-Token", "<device-token>");
echosdk_account_add_register_header(acct, "X-Apple-Push-Bundle", "com.example.MyApp");
```

These headers appear **only on REGISTER** requests — not on INVITE, BYE, REFER, or any other dialog request. This prevents leaking the push token to call peers.

For comparison, `echosdk_account_add_header()` sends the header on **every** outgoing request from this account. Use that for tenant IDs, app version strings, or other metadata that belongs on all requests.

---

## Custom SIP headers

Add headers to **all outgoing requests** from this account:

```c
echosdk_account_add_header(acct, "X-Tenant-Id", "42");
echosdk_account_add_header(acct, "X-App-Version", "2.1.0");
```

## Messaging — SIP MESSAGE

Send an out-of-dialog instant message. No call is needed, and the account does
not have to be in a call for either direction.

**C**
```c
echosdk_message_send(acct, "bob@pbx.example.com", "on my way", "text/plain");
```

**C++**
```cpp
acct.send_message("bob@pbx.example.com", "on my way");   // content_type defaults to text/plain
```

**Python**
```python
acc.send_message("bob@pbx.example.com", "on my way")
```

**Dart**
```dart
account.sendMessage('bob@pbx.example.com', 'on my way');
```

Incoming messages arrive as `ECHOSDK_EV_MESSAGE` (`message` in Python,
`MessageEvent` in Dart) with `from_uri`, `body` and `content_type` — see
[Events reference](events.md#echosdk_ev_message).

## Presence — PUBLISH

Tell the server your status:

```c
echosdk_account_publish_presence(acct, ECHOSDK_PRESENCE_OPEN);   // available
echosdk_account_publish_presence(acct, ECHOSDK_PRESENCE_BUSY);   // on a call
echosdk_account_publish_presence(acct, ECHOSDK_PRESENCE_CLOSED); // DND / offline
```

## Presence — SUBSCRIBE

Watch another extension's presence (BLF):

```c
echosdk_account_subscribe_presence(acct, "bob@pbx.example.com");
// fires ECHOSDK_EV_PRESENCE_STATE when bob's state changes

echosdk_account_unsubscribe_presence(acct, "bob@pbx.example.com");
```

## Registration retry control

The SDK retries failed registrations automatically with exponential backoff. These functions let you override the policy or control the retry loop at runtime.

### Override retry policy

```c
// Override per-account (takes effect on the next retry)
echosdk_account_set_retry_policy(
    acct,
    2000,    // initial_ms   — first retry delay
    60000,   // max_ms       — delay cap
    1.5f,    // backoff      — multiplier per attempt
    10       // max_attempts — 0 = retry forever
);
```

Per-account policy overrides the global `reg_retry_*` fields in `echosdk_config_t` for that account only.

### Cancel a pending retry

```c
echosdk_account_cancel_retry(acct);
// Stops the backoff timer and resets the attempt counter.
// An account that was RECONNECTING reports FAILED — the SDK is no longer
// recovering it — and stays there until echosdk_account_register().
```

### Force immediate retry

```c
echosdk_account_retry_now(acct);
// Cancels the current backoff delay and re-registers immediately.
// Resets the attempt counter. No-op if not in a retry loop.
```

## ICE servers at runtime

```c
echosdk_ice_server_t srv[] = {
    { "stun:stun.l.google.com:19302", NULL, NULL },
    { "turn:turn.example.com:3478?transport=udp", user, short_lived_pass },
};
echosdk_account_set_ice_servers(acct, srv, 2, ECHOSDK_ICE_TRANSPORT_ALL);
```

Replaces the account's STUN/TURN list and transport policy. It takes effect on
the next gather — a new call, or the re-gather of an ICE restart on a live
call — which is how TURN REST credentials (RFC 7635 style, valid for minutes)
are refreshed without re-creating the account: fetch new ones before they
expire, call this, and the next handover allocates with them. A `count` of 0
clears the per-account list so the global `ice_servers` apply again. The array
is copied; the caller keeps ownership of its memory.

### Retry events

A registration the SDK will retry is reported as `ECHOSDK_REG_RECONNECTING`,
not `ECHOSDK_REG_FAILED` — the failure itself, and then each scheduled retry
with its countdown:

```c
case ECHOSDK_EV_REG_STATE:
    if (ev->u.reg.state == ECHOSDK_REG_RECONNECTING) {
        if (ev->u.reg.retry_attempt)
            printf("reconnecting: attempt %u in %u ms\n",
                   ev->u.reg.retry_attempt,
                   ev->u.reg.retry_delay_ms);
        else
            printf("reconnecting: %s\n",
                   ev->u.reg.error_str ? ev->u.reg.error_str : "");
    }
    else if (ev->u.reg.state == ECHOSDK_REG_FAILED) {
        /* The SDK has given up: auth, an exhausted retry budget, or a retry
         * this app cancelled.  This is the one worth showing the user. */
    }
```

`RECONNECTING` also covers the losses that are not a REGISTER failure at all —
a dead keepalive path, and a network handover — so a status indicator bound to
the registration state tracks them without also subscribing to
`ECHOSDK_EV_NETWORK`.  See [`ECHOSDK_EV_REG_STATE`](events.md#echosdk_ev_reg_state).

---

## Audio codec selection

By default each account uses the global `cfg.audio_codecs` list set at SDK init. Set the per-account codec list to override it for a specific account.

### C

```c
echosdk_account_config_t cfg = {
    .uri      = "alice@pbx.example.com",
    .password = "secret",
};

// String names — highest priority, most flexible
strcpy(cfg.audio_codec_names[0], "ulaw");   // G.711 µ-law
strcpy(cfg.audio_codec_names[1], "alaw");   // G.711 A-law
strcpy(cfg.audio_codec_names[2], "opus");
cfg.audio_codec_name_count = 3;
```

Accepted name aliases:

| You write | Baresip codec |
|---|---|
| `"opus"` | Opus 48 kHz stereo |
| `"ulaw"` / `"pcmu"` / `"g711u"` | G.711 µ-law (PCMU/8000) |
| `"alaw"` / `"pcma"` / `"g711a"` | G.711 A-law (PCMA/8000) |
| anything else | passed as-is to baresip |

Opus and G.711 are the only codecs the library compiles in. A name that no
loaded module registers is offered to nobody and logs a warning.

**Priority:** per-account string names → per-account enum list → global `cfg.audio_codecs`.
When none are set, the default offer is `opus, PCMU, PCMA`.

### Python

```python
import echo_sdk as sdk

account = sdk.create_account(
    "alice@pbx.example.com", "secret",
    audio_codecs=["ulaw", "alaw", "opus"],
)
account.register()
```

### Flutter

```dart
final account = sdk.createAccount(
  "alice@pbx.example.com", "secret",
  audioCodecs: ["ulaw", "alaw", "opus"],
);
```

### C++

```cpp
auto acct = sdk.create_account(
    "alice@pbx.example.com", "secret",
    ECHOSDK_TRANSPORT_UDP,
    {ECHOSDK_CODEC_PCMU, ECHOSDK_CODEC_PCMA, ECHOSDK_CODEC_OPUS}
);
```

Or with the full account config struct for string names:

```cpp
echosdk_account_config_t acfg{};
acfg.uri      = "alice@pbx.example.com";
acfg.password = "secret";
std::strcpy(acfg.audio_codec_names[0], "ulaw");
std::strcpy(acfg.audio_codec_names[1], "opus");
acfg.audio_codec_name_count = 2;
auto acct = sdk.create_account(acfg);
```

---

## 100rel (RFC 3262 PRACK)

```c
echosdk_account_set_100rel(acct, ECHOSDK_100REL_ENABLED);   // support if offered
echosdk_account_set_100rel(acct, ECHOSDK_100REL_REQUIRED);  // require
echosdk_account_set_100rel(acct, ECHOSDK_100REL_DISABLED);  // never (default)
```

Must be called before the first `echosdk_call_invite` or `echosdk_call_answer`.
