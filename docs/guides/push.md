# Push notifications (incoming calls on a sleeping phone)

A SIP phone receives an INVITE only while the server can reach it. Mobile
systems freeze or kill background apps, so a phone that is not in the
foreground has no live connection and no fresh registration. Push closes the
gap: the phone gives the server a push token, the server holds an INVITE for a
device it cannot reach and asks Google (FCM) or Apple (APNs) to wake the app,
and the app registers again and receives the INVITE.

```
Caller ──INVITE──▶ SIP server ── device unreachable ──▶ holds the INVITE
                       │
                       └── push ──▶ FCM / APNs ──▶ phone: OS starts the app
                                                        │
          SIP server ◀──── REGISTER (refreshes the binding) ┘
                       │
                       └── forwards the held INVITE ──▶ phone rings
```

Nothing here is specific to one server. The SDK speaks RFC 8599 and the push
payload below; any server that does the same works. [OpenSIPS](#opensips) is
worked through as an example.

| Who | Does what |
|---|---|
| **App** | Gets the token from FCM / PushKit and gives it to the SDK; passes each push to the SDK. |
| **SDK** | Puts the token in every REGISTER; on a push, rings and refreshes the registration; binds the INVITE to the ring. |
| **SIP server** | Stores the token with the binding; on an INVITE for a sleeping device, sends the push, holds the INVITE, and forwards it when the binding is refreshed; sends a cancel push if the caller gives up first. |
| **FCM / APNs** | Delivers the push. |

## What goes on the wire

The token rides in the Contact URI of every REGISTER (RFC 8599):

```
Contact: <sip:alice@192.0.2.10:5061;transport=tls;pn-provider=fcm;pn-prid=<FCM token>;pn-param=<project ID>>
```

| Param | From | Value |
|---|---|---|
| `pn-provider` | `pushProvider` | `fcm`, `apns`, or `apns-sandbox` (not in RFC 8599; for debug and TestFlight builds) |
| `pn-prid` | `pushToken` | The FCM registration token, or the hex PushKit token |
| `pn-param` | `pushParam` | Passed through as given. RFC 8599: the Firebase **project ID** for FCM; `<Team ID>.<bundle ID>.voip` for APNs VoIP pushes. |

When a push arrives, the SDK **refreshes** the registration: the REGISTER
goes out from the client that holds the binding, with the same Call-ID and the
same Contact (RFC 8599 s4.1.3). An RFC 8599 server forwards the held INVITE
on that refresh. An OPTIONS probe is not enough, and neither is removing the
binding and adding it again: OpenSIPS, for one, sees the second as a new
device and forwards nothing. A process the push has just started has no
client yet; its first REGISTER carries the same push params, which is what
the server matches the binding on.

When the token changes, the SDK sends a REGISTER with the new `pn-prid` at once
and, over UDP, TCP and TLS, removes the binding with the old one, so the server
stops pushing to a dead token.

## The push payload

The SDK reads these fields from the push. All are optional strings; the names
are configurable (`MedutPush.payloadKeys`), so a server that already sends
other names needs no change.

| Field | Meaning |
|---|---|
| `type` | `call` (an INVITE is on its way) or `cancel` (the caller gave up before it arrived). Missing means `call`: a bare wake-up works. |
| `call_id` | SIP Call-ID of the held INVITE. The SDK binds the INVITE with this Call-ID to the ring; without it, the next incoming INVITE takes the ring. A B2BUA must send the Call-ID of the leg it sends to the phone. |
| `caller_uri` | Shown while ringing, before the INVITE arrives. |
| `caller_name` | Same. |
| `aor` | The callee's AOR, for a device with more than one account. |

On Android, send it as an FCM **data-only** message with high priority and a
short TTL. A message with a `notification` block is shown by the system and
does not start the app's code. FCM values must be strings.

```json
{
  "message": {
    "token": "<pn-prid>",
    "android": { "priority": "high", "ttl": "30s" },
    "data": {
      "type": "call",
      "call_id": "a84b4c76e66710@pc33.example.com",
      "caller_uri": "sip:bob@example.com",
      "caller_name": "Bob",
      "aor": "sip:alice@example.com"
    }
  }
}
```

Post it to `https://fcm.googleapis.com/v1/projects/<project ID>/messages:send`
with an OAuth 2 bearer token for a service account that has the
`firebase.messaging` scope. The cancel push is the same with `"type": "cancel"`.

FCM lowers the priority of an app whose high-priority messages do not lead to
something the user sees. With Telecom on, every call push rings, and a ring
whose INVITE never comes ends as a missed call, so that holds.

## Android

The SDK has no Firebase dependency. The app adds Firebase Messaging and
forwards two calls:

```kotlin
class PushService : FirebaseMessagingService() {
    override fun onMessageReceived(message: RemoteMessage) {
        // Application.onCreate has started the stack and restored the accounts.
        MedutPush.wake(this, message.data)
    }

    override fun onNewToken(token: String) {
        MedutPush.onNewToken(token)   // live accounts and the account store
    }
}
```

```kotlin
class App : Application() {
    override fun onCreate() {
        super.onCreate()
        MedutAndroid.start(this, MedutConfig(), AndroidOptions(telecom = TelecomOptions()))
    }
}
```

Create accounts with the push params and save them, so a process that a push
starts registers them before any UI exists:

```kotlin
val cfg = AccountConfig("sip:alice@example.com", password,
    transport = Transport.TLS,
    pushProvider = PushProvider.FCM,
    pushToken = FirebaseMessaging.getInstance().token.await(),
    pushParam = "example-voip")              // Firebase project ID
Medut.createAccount(cfg).register()
MedutAndroid.accountStore.save(cfg)
```

`MedutPush.wake(context, data)`:

1. **`call`.** With Telecom on, the phone rings at once with the caller from
   the payload. This is a self-managed connection, with the built-in
   notification or `MedutTelecom.onShowPushIncomingCallUi`.
   - Every account refreshes its registration (`Account.refreshRegistration()`).
   - When the INVITE arrives, it takes over the ringing call. An Answer the
     user already tapped is carried out. A Decline sends 603 to the INVITE
     when it comes.
   - No INVITE within the wait ends the ring as missed. `wake` returns the
     `Call`, or null.
2. **`cancel`.** Ends a ring that is still waiting for its INVITE: the one
   with that `call_id`, or every such ring when the push has none. A call
   whose INVITE has arrived ends through SIP CANCEL as usual.
3. **Without Telecom.** Nothing rings early. `wake` refreshes the accounts,
   waits for the INVITE and returns the call for the app to show.
   `PushPayload.parse(data)` gives the app what the push said.

`MedutPush.ringOnPush = false` waits for the INVITE even with Telecom on. The
SDK's manifest already declares `MANAGE_OWN_CALLS`, `POST_NOTIFICATIONS`
(runtime permission on API 33+: request it) and `USE_FULL_SCREEN_INTENT`.

From Flutter: push handling is native on purpose, since no Dart isolate is
running when a push starts the process. Put the `FirebaseMessagingService`
above in the app's Android code. `Account.setPushToken` from Dart also
updates a remembered account.

### Testing without Firebase

The example app's debug build has a receiver that stands in for FCM:

```sh
adb shell am force-stop dev.medut.medut
adb shell am broadcast -n dev.medut.medut/.FakePushReceiver \
    --es type call --es caller_name Bob --es caller_uri sip:bob@example.com
# then call the extension within 20 s; or end the ring:
adb shell am broadcast -n dev.medut.medut/.FakePushReceiver --es type cancel
```

## iOS

`MedutPush.wake(payload:completion:)` is called from PushKit's
`didReceiveIncomingPushWith`. It reports the call to CallKit at once (iOS ends
an app that does not) with `caller_name` / `caller_uri` from the payload, and
re-registers the accounts. A new client with the same push params hands the
binding over without removing it, so the server sees it refreshed. `MedutPush.updatePushToken(_:)` is called from
`didUpdate pushCredentials`, and it updates the account store too. Use
`apns-sandbox` for debug and TestFlight builds and `apns` for the App Store; a
mismatch fails silently at APNs.

Not on iOS yet: `call_id` matching and `cancel` pushes. iOS is the next step
after Android.

## What the server must do

Whatever the server, the contract is:

1. Store `pn-provider`, `pn-prid` and `pn-param` with the binding, and match
   the binding on them: the device's address changes between registrations.
2. On an INVITE for a device with push params, send a `call` push and hold
   the INVITE for about 25–30 s. That is long enough for push delivery, a cold
   app start, TLS and the REGISTER.
3. When the binding is refreshed, forward the held INVITE to the new contact.
   Over TCP or TLS, use the connection the REGISTER came in on.
4. When the caller gives up (CANCEL) or the wait times out, send a `cancel`
   push with the same `call_id`.
5. On a REGISTER with a new `pn-prid`, treat it as the device's new binding;
   the SDK removes the old one.

A B2BUA (Asterisk, FreeSWITCH) usually does this through a push proxy in
front of it or a dialplan hook. Either way, `call_id` must be the Call-ID of
the leg to the phone.

## OpenSIPS

OpenSIPS supports RFC 8599 in its `registrar` module (since 3.1). `lookup()`
holds the INVITE, raises `E_UL_CONTACT_REFRESH` so the script can send the
push, and forks the INVITE to the device when its binding is updated: matched
on the push params, within `pn_refresh_timeout`. Use **3.4.18 or later, or
3.6.7 or later**. Earlier 3.4 releases and every 3.5 release lack fixes to how
a push-only INVITE waits and fails.

This is a starting point, not a tested configuration. It follows the registrar
and usrloc docs and the OpenSIPS blog series "SIP Push Notification with
OpenSIPS 3.1 LTS"; the cancel push is our own addition, as OpenSIPS has none.

```
loadmodule "tm.so"
loadmodule "usrloc.so"
loadmodule "event_routing.so"     # required with pn_enable
loadmodule "registrar.so"
loadmodule "event_route.so"       # 3.4 only: part of the core in 3.6
loadmodule "rest_client.so"
loadmodule "json.so"
loadmodule "cachedb_local.so"

modparam("registrar", "pn_enable", true)
modparam("registrar", "pn_providers", "fcm, apns, apns-sandbox")
modparam("registrar", "pn_ct_match_params", "pn-provider, pn-prid, pn-param")
modparam("registrar", "pn_refresh_timeout", 25)   # default 6 s: too short for a cold start
modparam("usrloc", "contact_refresh_timer", true) # pushes before a binding expires, too
modparam("cachedb_local", "cachedb_url", "local://")

route {
    # ...
    if (is_method("REGISTER")) {
        fix_nated_register();         # forward the held INVITE to where the REGISTER came from
        if (!save("location"))
            sl_reply_error();
        exit;
    }

    if (is_method("CANCEL")) {
        if (t_check_trans()) {
            route(PN_CANCEL, $ci);
            t_relay();
        }
        exit;
    }

    if (is_method("INVITE") && !has_totag()) {
        # E_UL_CONTACT_REFRESH runs with no SIP message: keep the caller by Call-ID.
        cache_store("local", "pn_from_$ci", "$fu", 120);
        cache_store("local", "pn_name_$ci", "$fn", 120);
        t_on_failure("PN_FAILED");
    }

    lookup("location", "method-filtering");
    switch ($retcode) {
    case 1:     # reachable contacts; any sleeping ones are pushed and join later
        route(relay);
        exit;
    case 2:     # only push contacts: lookup() holds the INVITE itself. No t_relay().
        exit;
    default:
        t_reply(404, "Not Found");
        exit;
    }
}

# Raised by lookup() for each push contact; asynchronous, in another process.
event_route[E_UL_CONTACT_REFRESH] {
    if ($param(reason) != "ini-INVITE") {
        $var(type) = "register";    # "reg-refresh": the binding is about to expire
    } else {
        $var(type) = "call";
        # Remember each device pushed for this call, for the cancel push.
        cache_add("local", "pn_n_$param(req_callid)", 1, 120, $var(n));
        cache_store("local", "pn_dev_$param(req_callid)_$var(n)",
            "$(param(uri){uri.param,pn-provider})|$(param(uri){uri.param,pn-prid})|$(param(uri){uri.param,pn-param})", 120);
    }
    cache_fetch("local", "pn_from_$param(req_callid)", $var(from));
    cache_fetch("local", "pn_name_$param(req_callid)", $var(name));
    route(PN_SEND, $(param(uri){uri.param,pn-provider}), $(param(uri){uri.param,pn-prid}),
          $(param(uri){uri.param,pn-param}), $var(type), $param(req_callid),
          $var(from), $var(name), $param(aor));
}

# $param(1..8): provider, token, pn-param, type, call_id, caller_uri, caller_name, aor
route[PN_SEND] {
    if ($param(1) != "fcm") {
        # APNs VoIP pushes need HTTP/2 and a signed JWT: hand them to a gateway.
        # ...
        return;
    }
    $json(m) := "{\"message\":{\"android\":{\"priority\":\"high\",\"ttl\":\"30s\"},\"data\":{}}}";
    $json(m/message/token) = $param(2);
    $json(m/message/data/type) = $param(4);
    $json(m/message/data/call_id) = $param(5);
    $json(m/message/data/caller_uri) = $param(6);
    $json(m/message/data/caller_name) = $param(7);
    $json(m/message/data/aor) = $param(8);
    cache_fetch("local", "fcm_bearer", $var(bearer));   # see below
    rest_append_hf("Authorization: Bearer $var(bearer)");
    # pn-param is the Firebase project ID (RFC 8599 s11).
    $var(rc) = rest_post("https://fcm.googleapis.com/v1/projects/$param(3)/messages:send",
                         $json(m), "application/json", $var(body), , $var(code));
    if ($var(rc) < 0 || $var(code) != 200)
        xlog("L_ERR", "FCM push failed: rc=$var(rc) code=$var(code) $var(body)\n");
}

# The caller gave up, or nobody answered: tell every device pushed for $param(1).
route[PN_CANCEL] {
    if (!cache_counter_fetch("local", "pn_n_$param(1)", $var(n)))
        return;
    cache_remove("local", "pn_n_$param(1)");
    $var(i) = 1;
    while ($var(i) <= $var(n)) {
        if (cache_fetch("local", "pn_dev_$param(1)_$var(i)", $var(d)))
            route(PN_SEND, $(var(d){s.select,0,|}), $(var(d){s.select,1,|}),
                  $(var(d){s.select,2,|}), "cancel", $param(1), "", "", "");
        $var(i) = $var(i) + 1;
    }
}

failure_route[PN_FAILED] {
    route(PN_CANCEL, $ci);
}
```

Notes:

- **FCM credentials.** OpenSIPS cannot mint the OAuth 2 token from a service
  account. Either a small job refreshes it about hourly into the cache:

  ```sh
  opensips-cli -x mi cache_store system=local attr=fcm_bearer value=<token> expire=3300
  ```

  Or, simpler, point `rest_post` at a push gateway of your own that holds
  the FCM and APNs credentials and sends both. A gateway is also where APNs
  goes.
- **A refresh, not a new binding.** OpenSIPS forwards the held INVITE on the
  usrloc *update* of the push binding. A REGISTER with a different token is a
  new binding; an un-REGISTER is a deletion; neither forwards anything. The
  SDK's refresh is an update.
- **The cancel push has gaps.** A CANCEL that comes before the event route
  has stored the device sends no cancel push. The SDK ends an unanswered
  ring on its own, after the wait passed to `MedutPush.wake` (10 s by default).
- **Unchecked:** whether `t_on_branch` routes run for the forwarded branch
  (matters for rtpengine per branch), and how a CANCEL behaves while
  `lookup()` holds the INVITE with no branch out. Check both on your build.

## When it does not ring

| Symptom | Look at |
|---|---|
| The server never pushes | Is `pn-prid` in the REGISTER Contact (SIP trace)? Is the provider listed in `pn_providers`? A provider OpenSIPS does not know is refused with 555. |
| The push arrives, the INVITE does not | Did the device send a REGISTER, not an OPTIONS? Was it within the server's hold time (`pn_refresh_timeout`)? Did the binding's push params match? |
| It works in the foreground only | Is the FCM message data-only and `priority: high`? Is the stack started from `Application.onCreate`? |
| It stops working after some days | A token rotation that was not saved: use `MedutPush.onNewToken`, which updates the account store. |
| iOS: pushes accepted, never delivered | `apns` against `apns-sandbox`. |
