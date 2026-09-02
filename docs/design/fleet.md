# Fleet mode and the coordinator

> **Status: design only. None of this is built, and nothing here has run on any board.**
> Every property described below is intent, not proof (AGENTS.md §4, last rule). Sizes and
> timings marked *(estimate)* are arithmetic, not measurement.
>
> This document exists because the repo has no reachable tracker yet (AGENTS.md §11 names
> GitHub issues on `abradner/airkvm`; no remote is configured). Move the open questions there
> when it exists, and cut this down to the decisions.

## The problem

Everything past a single dongle runs into the same three walls at once:

- **Naming.** The mDNS hostname is a compile-time Kconfig value, so two dongles on one LAN both
  answer to `airkvm.local`.
- **Certificates.** The device certificate is issued for that one name. N devices means N names,
  N certificates, and a CA install per browsing machine — friction that scales the wrong way.
- **Authentication.** R-001 (anyone routable to the dongle can type on the target) has no good
  answer on-device. A PIN is the most an MCU can offer, and it is a session gate, not identity.

A coordinator collapses all three. The browser stops talking to devices and talks to one service
with one origin, one certificate, one trust decision, and one camera-permission grant.

## Decision

Two firmware modes, selected at build time. **Standalone remains the default** — it is the
differentiator, and the only mode that needs no infrastructure at all (AGENTS.md §2, purpose).

| | Standalone | Fleet |
|---|---|---|
| Use case | Walk up, plug in, type. Ad-hoc, ephemeral. | Permanently installed dongles. |
| Console served by | The device | The coordinator |
| Auth | None (R-001, accepted) | Coordinator-owned |
| Trust root | Physical possession | Physical enrollment, then coordinator |
| Needs infrastructure | No | Yes |

The coordinator is a separate program: a single Go binary with embedded assets, plus a container
image wrapping it. It is a CA by default, encourages Let's Encrypt where a real name exists, and
accepts a supplied certificate-chain PEM — the same three-way choice `tools/make_certs.sh`
already offers, so operators who run internal PKI are not forced onto a second one.

### Accepted trade-off

**Coordinator outage is fleet outage.** Operator decision, taken deliberately. A fleet device
holds no local console and has no degraded mode, which means there is no second input path to
keep in sync, no fallback to test, and no ambiguity about where enforcement lives. A device that
cannot reach its coordinator is still a USB keyboard — it enumerates independently of the network
(see repo-map, boot/provisioning flow) — it simply has nothing driving it.

### Non-goals

PiKVM and JetKVM are the inspiration and are roughly 10× the hardware cost. The bet here is cheap
hardware with the polish in software: no additional silicon, no PSRAM requirement, and enrollment
that runs entirely on the 0.42" OLED and the BOOT button this board already has. On-device video
stays out of scope (README); the coordinator does not own capture hardware, so video remains a
local `getUserMedia` capture on the operator's machine.

## Handshake

### Phase 0 — provisioning

Entered when NVS holds no configuration, or when BOOT is held at boot. Never entered on WiFi
failure: otherwise jamming the network becomes a way to force a device into provisioning mode.

The device raises a **WPA2** AP (`airkvm-setup-XXXX`) whose password is shown on the OLED, and
serves one form — WiFi credentials plus coordinator URL, or a coordinator discovered via
`_airkvm-ctl._tcp`. An *open* setup AP would be strictly worse than R-001: it needs no network
credential at all, only RF range.

### Phase 1 — enrollment (once, physically present)

```
device                                          coordinator
  │ ECC P-256 keypair generated into NVS; private key never leaves
  │──── TLS connect (coordinator cert not yet trusted) ─────────►│
  │──── enroll_req{device_id, csr, nonce} ──────────────────────►│
  │                                          ┌── pending device ─┤
  │   SAS = SHA-256(dev_pub ‖ ctl_ca_pub ‖ nonce)[:6 digits]     │
  │   ── six digits on the OLED ──           ── same six digits ─┤
  │        operator types the OLED digits into the coordinator   │
  │◄──── signed device certificate + CA certificate ─────────────│
  └── stores cert, key, CA and coordinator URL in NVS
```

The short authentication string is the load-bearing part. Both public keys are inside the hash,
so a man-in-the-middle proxying enrollment computes a *different* six digits and the comparison
fails. This buys channel binding without an operator typing a fingerprint — the same construction
as Bluetooth numeric comparison.

The enrollment window opens on a physical button press and closes after ~5 minutes.

**Why this shape:** it keeps AGENTS.md §2 principle 5 intact rather than trading it away. Trust
still originates from a person standing at the device; the coordinator manages only what is
derived from that. *Enrollment is physical, operation is remote.*

### Phase 2 — steady state

mTLS to the pinned coordinator over a persistent WebSocket:
`hello{device_id, fw_version, hid_backend, caps}` → `welcome{name, policy, config_version}`.

The coordinator pings every second. **The device releases all keys after three missed pings**
(~3s). This is the keepalive deadline the repo-map currently records as the one gap in the
release-all guarantee; relaying makes it routine rather than exotic, so it stops being optional.

### Phase 3 — operator session

The operator authenticates to the coordinator, which has a real filesystem, real crypto and
somewhere to rate-limit. `session_start{session_id, operator}` goes down the tunnel and the
coordinator then **relays existing `protocol.c` frames verbatim** — it is a relay, not a
translator. The codec and its host tests are untouched, and the added latency is one LAN hop
(AGENTS.md §2 principle 6: to be measured, not asserted).

Session end, operator disconnect, or tunnel loss → release-all.

### The rule that makes coordinator auth real

**In fleet mode the device accepts sessions only over mTLS from its enrolled coordinator, and
serves no console.** If `/ws` stayed open to the LAN, the coordinator's authentication would be
politeness rather than enforcement — anyone routable to the device could bypass it. Enforcement
exists exactly once, at the device's socket (AGENTS.md §8, deletion test).

## Firmware modules

`AIRKVM_MODE_*` is a Kconfig **choice**, mirroring the existing `AIRKVM_HID_BACKEND_*` idiom so
this extends a stated convention rather than inventing one. Per gotcha §9.7, component `REQUIRES`
stay unconditional and code is `#if`-guarded — requirements resolve during early expansion, where
`CONFIG_` values are not reliably visible.

| Module | Contents | Gate |
|---|---|---|
| `core` | `hid_state`, `keymap`, `protocol`, `hid_out` + backend, `status`, `board`, `wifi_sta` | always |
| `session` | active-session ownership, keepalive deadline, release-all | always |
| `ui` | `display`, `ui`, `indicator` | `AIRKVM_UI` |
| `console` | HTTP + WebSocket server, embedded console assets | `MODE_STANDALONE` |
| ↳ `tls` | `esp_https_server` + embedded PEMs | `AIRKVM_TLS` |
| ↳ `mdns` | advertises `airkvm.local` | `AIRKVM_MDNS` |
| `fleet` | coordinator client, enrollment, SAS | `MODE_FLEET` |
| `provisioning` | setup AP, config form, NVS config store | `AIRKVM_PROVISIONING` |
| `ota` | `esp_https_ota` sourced from the coordinator | `AIRKVM_OTA` (fleet) |

### `session.c` is the seam that matters

`net.c` (~15.5 KB) currently mixes WiFi, HTTP, WebSocket, mDNS and TLS, and active-client
tracking (`s_active_fd`) lives inside the socket handler. Split it into `wifi.c` (core),
`console.c` (standalone) and `fleet.c` (fleet), and lift session ownership into its own module
that both transports feed.

Then *who is driving*, the keepalive deadline, single-client enforcement (R-003) and release-all
exist **once**, identically in both modes. This mirrors the rule already held for HID state — one
owner, transports never touch it directly (AGENTS.md §4) — and it is worth doing whether or not
fleet mode ever ships.

## Sequencing

1. **`session.c` extraction + keepalive deadline.** Mode-independent, closes a known gap in the
   release-all guarantee, and is a prerequisite for everything below.
2. **`provisioning`.** Serves both modes; unblocks runtime configuration. Must preserve the
   fail-fast rule (AGENTS.md §8): absent configuration enters provisioning visibly, it does not
   silently degrade.
3. **`fleet`** — enrollment, then the tunnel.
4. **`ota`** last. Only safe once enrollment provides a signed channel to ship over; an
   unauthenticated OTA path on a device with HID access to its target is worse than R-001,
   because it does not need presence on the LAN at the moment of plugging in.

## Open questions and unverified claims

- **Flash size — resolved.** `esptool flash_id` against the real S3 (2026-08-31): 4MB (XMC), plus
  2MB of PSRAM the current design doesn't use. `sdkconfig.defaults` now carries this measurement
  (`CONFIG_ESPTOOLPY_FLASHSIZE_4MB`), not the ESP-IDF 2MB default this section used to describe.
- **OTA fits comfortably; the 2MB contingency below no longer applies.** `sdkconfig.defaults` now
  ships `PARTITION_TABLE_TWO_OTA_LARGE`: 1700K per app slot against a measured image of
  ~1019K (0xfe820 bytes, TLS on, this commit's build), ~40% headroom — real measurement via
  ESP-IDF's own `check_sizes.py` at build time, not an estimate. The question this section used to
  pose (does OTA fit at 2MB, given `esp_http_server` staying linked for provisioning) is moot at
  4MB, exactly as this section predicted it would be.
- **The S3 `sdkconfig` predates the TLS work** and has `CONFIG_AIRKVM_TLS` unset, so the 907,600
  byte S3 image understates the real one by roughly the C3's TLS delta *(estimate)*. Regenerate
  before drawing any conclusion from it.
- **ECC P-256 keygen time on the S3 is unmeasured.** It happens once, at enrollment, but it wants
  a "generating…" state on the display rather than appearing hung.
- **Does the coordinator ever own capture hardware?** Currently no, which means video stays local
  to whoever is browsing and the HDMI-cable proximity limit stands: for genuinely remote use you
  get keyboard and mouse and no picture. Saying yes turns this into PiKVM and changes the cost
  argument above.

## What must change in `docs/repo-map.md` when this lands

Listed, not applied — the map describes what exists, and aspirational entries are exactly what it
warns against. When fleet mode ships:

- **Surfaces**: the coordinator tunnel is a new surface; in fleet mode `GET /` and `/ws` are gone
  and the setup AP is a new (transient) one.
- **Trust boundaries**: *"Dongle → internet: None, and intentionally so"* becomes false. The
  honest replacement is that the device dials out to exactly one pinned peer whose address is
  configuration — the mechanism now exists, which is the boundary change, and the erosion risk
  worth naming.
- **Risk register**: R-001 is *mitigated in fleet mode only* and unchanged in standalone. New
  entries for the coordinator as a concentrated target (one host able to type on every enrolled
  machine, compromise total and silent), and for enrollment-window abuse.
