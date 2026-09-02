# Repository Map

> Read this before touching anything boundary-sensitive: auth, tenancy, deletion/visibility,
> serialization, background delivery, routing, any external surface. Update it in the same PR as
> any change that moves an edge here. Never leave it describing a stale security boundary — a
> wrong map is worse than no map, because it is trusted.

## What belongs here — and what doesn't

This is a **boundary map, not a directory tree.** Anything an agent can answer with grep/glob in
seconds — where a symbol is defined, which files match a pattern — does *not* belong here: that
content goes stale fastest and helps least. What belongs is the knowledge that lives *between*
files and can't be reconstructed from any one of them:

- **Surfaces** — every distinct way into the system, each with its auth mechanism.
- **Trust boundaries** — for each edge: who is the caller, how is identity established, and what
  happens if they lie.
- **Cross-cutting flows** — the handful of sequences where one action touches many parts, and
  where a change to one part silently breaks an invariant held elsewhere (the classic: soft-
  deleting a record does nothing to a session that looks it up by ID unless every lookup is
  scoped).
- **Risk register** — numbered, stable IDs for known concentrations of risk, so rules, PRs, and
  code comments can cite them.

Start small and grow by incident: three accurate entries beat thirty aspirational ones. When the
map and the code disagree, the code is the truth and the map is the bug — fix it in the same
change that found the disagreement.

## Surfaces

| Surface | Path / entry point | Auth mechanism | Notes |
|---|---|---|---|
| Console page | `GET /` (HTTPS, port 443) | **None** — transport encrypted, caller unauthenticated | Static page embedded in firmware. Anyone who can route to the dongle gets it. |
| Input socket | `GET /ws` (WSS upgrade) | **None** — see R-001 | The actual keystroke path. Every frame here becomes a USB HID report on the target. |
| USB HID device | USB-C port → target machine | **None, by design** | The target cannot authenticate a keyboard. This is inherent to HID, not a gap we can close. |
| Serial console | UART/USB-JTAG in bootloader mode | Physical access | Flashing and logs. Only present when firmware hasn't claimed USB (AGENTS.md §6.1). |
| Status display + button | On-board OLED and BOOT button | Physical access | Shows IP, signal, and link state; blanks to an idle animation after 5 minutes with no client input and no local button interaction (no LED on this board to show it's alive otherwise). The button dismisses that animation if it's showing, or toggles a debug view of live input if not — see R-006. |
| WiFi association | Station mode, credentials in NVS | WPA2 (the network's) | The dongle's only network identity is "a device on this LAN". |

## Trust boundaries

- **Browser → dongle (`/ws`).** Caller identity is *not established at all*: any host that can
  open a TCP connection to the dongle can send input frames. If the caller "lies" there is
  nothing to lie about — there is no identity to forge. The dongle will happily type for a second
  concurrent client. This is the honest current state, recorded here deliberately (R-001).
- **Dongle → target (USB).** The target trusts the dongle completely and unconditionally: it is
  a keyboard. No mitigation exists on this edge and none can — a HID device is authorized by
  being plugged in. Everything protecting the target therefore has to live on the *previous*
  edge, which is why R-001 is the top entry.
- **Operator → capture video.** Not a dongle boundary at all: the HDMI capture dongle is a local
  UVC device the *browser* opens via `getUserMedia`. Video never traverses the network and never
  reaches the ESP32. If a future change makes video pass through the dongle, this line is wrong
  and the whole risk profile changes — update it in the same PR.
- **Dongle → internet.** None, and intentionally so. The dongle makes no outbound connections;
  the console loads no remote assets (AGENTS.md §4). Any new outbound call is a boundary change
  requiring an entry here.

## Cross-cutting flows

**Keystroke path** (the one that matters): browser `keydown` → `event.code` → protocol frame over
WebSocket → `main/protocol.c` decode → intent struct → queue → `main/hid_state.c` (single owner,
mutates authoritative bitmap) → TinyUSB report → target. The invariant that must survive the whole
traversal: **every key-down has a matching key-up, or a release-all that supersedes it.** Any
stage that can drop a frame (socket close, queue full, task starve) must trigger release-all
rather than let a partial state stand.

**Session lifecycle**: WebSocket open → capture engaged (pointer lock) → frames flow → *any*
terminal event (close, error, page hidden, pointer-lock exit) → release-all. There
is deliberately no "reconnect and resume": resumption would require trusting a state snapshot
across a gap the dongle can't verify, and the failure mode is a stuck key on someone's machine.
Reconnect starts a fresh session with everything released.

**Keepalive**: the console sends a `PING` frame every second, and the device tears the session
down — releasing everything — after 3s without any frame from the owner. A spectator's frames
deliberately do not refresh the deadline; only the owner's.

This **bounds** what was previously an unbounded gap, rather than closing it. A client that
vanishes *without* closing its TCP connection (a suspended laptop, a backgrounded mobile browser)
used to hold state until the socket layer noticed, which could be indefinitely; it is now at most
~3s. What it does not do is measure client liveness: `esp_http_server` runs a single task for
accepting connections, performing TLS handshakes, serving assets and dispatching WebSocket
frames — serially — so a PING that arrived on time but is queued behind that work is
indistinguishable from silence. A TLS handshake measured ~4.8s on
this hardware at −85 dBm — longer than the deadline — and R-001 means any LAN device can trigger
one unauthenticated. **So an operator holding a chord can lose it because someone reloaded the
console.** That trade is deliberate (principle 4 prefers a spurious release to a held key), and
the real fix — detecting a vanished peer where head-of-line blocking cannot reach it — is issue
#6, not done.

**Boot/provisioning**: NVS credentials → WiFi station → mDNS advertise (`airkvm.local`) → HTTPS
server up → address shown on the OLED and logged to serial. USB HID
initializes independently of, and does not wait
on, the network: a dongle with no WiFi is still a keyboard that can be driven once the network
returns, and must never block enumeration on an association that may never happen.

## Risk register

| ID | Risk | Where it lives | Mitigation / status |
|---|---|---|---|
| R-001 | Unauthenticated LAN input: anyone routable to the dongle can type on the target | `/ws` handler | **Accepted, documented, unmitigated.** The security model is physical: plugging it in is deliberate, unplugging revokes completely. Do not deploy on untrusted networks. A shared-secret/PIN gate is the obvious first mitigation if this ever runs somewhere less trusted — not built. |
| R-002 | Stuck keys on the target after a network or firmware fault | `main/hid_state.c`, `main/session.c` | Single-owner HID state + release-all on every terminal event (AGENTS.md §4, §2 principle 4). Session ownership is its own state machine, so every terminal event funnels through one release path, and a silent client is torn down on a 3s deadline rather than held indefinitely. The state machine is host-tested; the release path itself is unverified on hardware. |
| R-003 | Two concurrent console clients interleaving input into one HID state | `/ws` handler | **Partially mitigated.** One controlling session is tracked (`s_active_fd`); a second client's frames are accepted at the HTTP layer and then ignored (`main/net.c`), so streams never interleave. Not built: telling that second client it is a spectator, and explicit takeover. |
| R-004 | WiFi credentials stored in plaintext NVS; physical recovery of the dongle exposes them | NVS partition | Accepted for now — a lost dongle is already a lost keyboard-on-the-LAN. Flash encryption is the real answer if this ever leaves a home network. |
| R-005 | Firmware claims about hardware behavior that has never run on hardware | Everywhere | Marked in-place as unverified (AGENTS.md §4, last rule). Clears when a board arrives and the phase-1 enumeration test passes. |
| R-007 | Keystrokes cross the network in plaintext | `/ws` | **Mitigated.** The console is served over HTTPS/WSS with a certificate issued by an operator-run CA (`tools/make_certs.sh`); plain HTTP is not served at all, and TLS failing to start is fatal rather than falling back — a silent downgrade would leave keystrokes readable while everything still appeared to work. Note this encrypts the transport only: R-001 is untouched, because anyone who trusts the CA and can reach the device can still connect and type. |
| R-006 | The status display can show what is being typed — passwords included — to anyone who can see the device | `main/ui.c`, `main/status.c` | **Gated on physical presence.** Debug rows are off at boot, toggled only by the on-board button, and there is deliberately no network path to enable them. The button does double duty (also dismissing the idle screensaver), so what a press does depends on whether the screensaver is showing at the moment of that press — latched at press time specifically so a client HID event landing mid-press can't retarget an already-committed press onto the other action. While off, keystroke text is not merely hidden but never stored (`status_note_event` discards it), and toggling off wipes the buffer — so a future logging change cannot leak a value that was captured "just in case". Anyone who can press the button can already unplug the device. |
