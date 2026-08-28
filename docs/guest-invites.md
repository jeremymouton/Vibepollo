# Guest invites

Let the host owner hand a friend a link that gets them into a game — in a browser or
with a Moonlight client they already have — without giving them an account, the host
password, or full control of the machine.

Status: **browser path complete; native pairing and owner UI outstanding.** This
document is the design the implementation is checked against. Each stage lists how it is
verified.

## Why this exists here

Vibepollo already has every piece of this except the invite itself.

| Piece | Where it already lives |
|---|---|
| Streaming to a browser | `webrtc_stream.cpp`, served at `/webrtc` |
| Streaming to a native client | GameStream, `nvhttp.cpp` |
| Per-client permissions | `crypto::PERM`, bound to a paired cert |
| Per-**session** input grants | `SessionOptions::input_permission` (this fork) |
| Per-player controller slots | `SessionOptions::gamepad_base_slot` (this fork) |
| Enforcement | `input::passthrough()` — per packet type, `input.cpp:1778` |
| ICE/TURN for the browser path | `load_webrtc_ice_servers()`, `confighttp.cpp:638` |

What is missing is a way for someone **without an account** to be handed a bounded slice
of that. Today both paths require the owner: `POST /api/webrtc/sessions` calls
`authenticate()`, and native pairing needs the owner to type the Moonlight PIN into the
admin UI. An invite is the thing that lets the guest complete either flow themselves,
while the owner decides in advance exactly how much it grants.

## The permission chain this rests on

Worth stating explicitly, because the whole design is "put an invite in front of an
existing chain" rather than "build a new one".

```
pairing binds a cert   →  named_cert->perm        nvhttp.cpp:1602
launch reads it        →  launch_session->perm    nvhttp.cpp:2046
RTSP carries it        →  snapshot->perm          rtsp.cpp:200
session holds it       →  session->permission     stream.cpp:3212
enforced per packet    →  input::passthrough()    stream.cpp:1470
```

`PERM::input_controller` without `input_mouse` / `input_kbd` is gamepad-only, and it is
enforced in the host, not in the page. That is the property the whole feature sells, and
it already works for native clients today. The browser path reaches the same
`input::passthrough()` through this fork's per-session grant.

## Model

One invite, persisted in `invites.json` beside the state file, written through
`statefile::write_json_atomic` under `statefile::state_mutex()`.

| Field | Meaning |
|---|---|
| `id` | uuid, owner-facing, safe to put in a URL path for admin routes |
| `label` | what the owner calls it ("Brother") |
| `token` | 32 random bytes, base64url — the secret in the link |
| `pin` | 6 digits, the second factor, sent through a different channel |
| `perm` | `crypto::PERM` bitmask. The **ceiling** for anything this invite grants |
| `gamepad_base_slot` | 0–15, so two players do not share a virtual pad |
| `app_id` | which app the guest may launch, or -1 for "whatever is running" |
| `allow_browser` | may redeem into a `/webrtc` session |
| `allow_pairing` | may complete a native Moonlight pairing |
| `expires_at` | absolute; an invite with no expiry is not offered in the UI |
| `max_uses` / `uses` | 0 = unlimited |
| `revoked` | tombstone rather than delete, so a revoked link stays refused |
| `failed_attempts` / `locked_until` | PIN lockout |

### Two deliberate choices

**The token and PIN are stored in plaintext.** The owner has to be able to re-read both
to send them again, which is the whole point of "make it easy to find and generate the
links" — a show-once secret fails that. The file sits beside `sunshine_state.json` with
the same permissions, and anyone who can read it can already read the paired client
certs, so this is not a new class of exposure. Comparison is still constant-time, because
the threat is an online guessing attack, not someone with the file.

**A revoked or expired invite is kept, not deleted.** A deleted invite is
indistinguishable from a typo, and both would answer "no such invite". Keeping the record
lets the owner see that a link they sent is the one being refused.

## Guest sessions are a separate world

The single most important security property here:

> A guest credential is never accepted by `authenticate()`.

Guests get their own cookie (`__Host-apollo_guest`) and their own store. `authenticate()`
— which gates every admin route in `confighttp.cpp` — does not know it exists. Only the
handful of routes that explicitly ask for a guest session accept one. This is fail-closed
by construction: a new admin route added later cannot accidentally be reachable by a
guest, because the guest cookie is not an authentication token anywhere in the system.

The alternative — reusing `SessionToken` with a `guest:` username — was rejected for the
opposite reason: it would make every existing `authenticate()` call site a place where a
guest might slip through, and correctness would depend on auditing all of them forever.

## Routes

### Owner (authenticated, existing `authenticate()`)

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/invites` | list, including token and PIN so the UI can show and copy them |
| POST | `/api/invites` | create |
| PATCH | `/api/invites/{id}` | relabel, change permissions, extend or revoke |
| DELETE | `/api/invites/{id}` | remove |
| POST | `/api/invites/{id}/rotate` | new token and PIN, invalidating the old link |

### Guest (unauthenticated, gated by the token in the path)

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/join/{token}` | what this invite offers. No secrets, no raw bitmask |
| POST | `/api/join/{token}/redeem` | PIN → guest cookie, for the browser path |
| POST | `/api/join/{token}/pair` | PIN + the Moonlight pairing PIN → paired cert carrying `invite.perm` |

`GET /join/{token}` serves the landing page itself: a small standalone page, not the
admin SPA, because it is shown to someone who must never load the admin bundle.

## The two ways in

### Browser

`redeem` sets the guest cookie. `createWebRTCSession` gains a guest branch: when there is
no owner session but a valid guest cookie, the options are **overwritten** — not
defaulted — from the invite:

```
options.input_permission  = invite.perm & PERM::_all_inputs
options.gamepad_base_slot = invite.gamepad_base_slot
options.app_id            = invite.app_id        (when set)
```

Overwritten rather than validated, so a guest cannot ask for more by sending a larger
bitmask; whatever they send in those fields is discarded.

### Native Moonlight

Pairing today is: the client starts `/pair`, then **the owner** types the PIN into the
admin UI (`POST /api/pin` → `nvhttp::pin(pin, name)`), and the new cert lands with
`PERM::_default`.

An invite lets the **guest** complete that themselves. `POST /api/join/{token}/pair`
takes both secrets — the invite PIN proves they were invited, the Moonlight PIN proves
they are the client currently at the door — and completes the pairing with `invite.perm`
instead of `PERM::_default`.

This needs a `nvhttp::pin()` variant taking an explicit permission. The existing
signature is kept and delegates, so the admin path is unchanged.

> **Reachability is a separate problem.** A native client speaks GameStream and needs
> inbound TCP 47984/47989 plus UDP media. An invite grants permission, not a route to the
> host. The guest still needs to reach it — same tailnet, or a relay. Nothing in this
> document changes that, and the landing page says so rather than offering a native
> option that cannot connect.

## Stages

| Stage | Contents | Status |
|---|---|---|
| 1 | Invite model, store, persistence, owner CRUD | **Done.** 33 unit tests (expiry, lockout onset/release/escalation/cap, refusal ordering, use limits, permission clamping, constant-time compare, persistence round-trip); every touched translation unit compiles clean |
| 2 | Guest sessions, `/api/join/*`, landing page, browser redeem | **Done.** Landing page verified in a browser against a stand-in backend across all six paths; guest branch of `createWebRTCSession` compiles but is unreachable in a non-WebRTC build |
| 3 | Native pairing with `invite.perm` | Outstanding. Needs `nvhttp::pin()` to take an explicit permission, plus `POST /api/join/{token}/pair` |
| 4 | Owner UI — invites page, copy link, PIN, QR | Outstanding. The API is complete and returns a ready-to-paste `path`, a `permission_summary` phrase, and derived `live` / `locked_for_seconds` so the page needs no bitmask logic |

### Still to check on the Windows host

- A guest session actually reaching `input::passthrough` with the clamped permission —
  the property the whole feature sells. Compile-verified only; `SUNSHINE_ENABLE_WEBRTC`
  is off on the macOS build machine.
- Two simultaneous guests landing on different controller slots.
- Whether a native GameStream guest gets their own virtual pad, or collides with the
  owner's the way two `/webrtc` sessions do.

## What is not verifiable on the build machine

The development machine is macOS and builds without `SUNSHINE_ENABLE_WEBRTC` (libwebrtc
is a multi-hour dependency). Everything in stages 1 and 3 is platform-independent and
compiles and tests there. The guest branch of `createWebRTCSession` sits behind that flag
and can only be exercised on the Windows host — it is written, compiled where possible,
and flagged as needing a live check before release.
