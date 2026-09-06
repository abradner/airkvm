# airkvm — Agent Onboarding Guide

> Read this file first. It is the source of truth for AI agents working in this codebase.
> Plans, status, and decision rationale live in GitHub issues on this repo (`abradner/airkvm`),
> not in a repo file. When this file and the tracker disagree
> on architecture or direction, the tracker wins — update this file to match, never the reverse.
> This file is operational: how to build, run, and write code here.

## 1. What Is This?

airkvm turns a ~$12 ESP32-S3-0.42 OLED "supermini" board into an ad-hoc wireless KVM input
dongle: plugged into any machine's USB port it enumerates as a real USB HID keyboard + mouse
(boot protocol, so BIOS/UEFI works), and it serves a browser-based console over WiFi from which
the operator types and points. Video is deliberately out of scope on-device: the operator's
HDMI→USB capture dongle is a local UVC webcam, and the console displays it in the same browser
tab via `getUserMedia` — the video never crosses the network. Status: greenfield; firmware is
written ahead of hardware arrival, so nothing has run on a real board yet.

Domain nouns: the **target** is the machine the dongle is plugged into (the one being
controlled); the **host** (or operator) is the machine running the browser; the **console** is
the web page the dongle serves; a **combo** is a multi-key chord sent as one action (e.g.
Ctrl-Alt-Del); **release-all** is the safety action that clears every held key and mouse button
on the target. "KVM" here means the K and M only — see the video note above.

## 2. Purpose & Principles

**Purpose:** Make "I need to press some keys on that machine" a 30-second job — walk up, plug in,
open a URL — with nothing installed on the target and nothing installed on the host.

Principles: short, numbered, memorizable. Each one must be able to change a real decision — a
principle that never changes a decision is decoration.

1. **Claims must hold.** A doc, comment, or commit message claiming a property the mechanism
   doesn't deliver is a bug, not documentation. Interim states say so in-place ("stated, not
   enforced — landing in E7"); stopgaps record the bar they miss. *(Portable seed — keep, adapt,
   or replace. It has caught real security bugs, twice, in a sibling repo, by being invoked
   against the project's own earlier claims.)*
2. **The target installs nothing and configures nothing.** Every feature must work against a
   machine seen for the first time, sitting at a BIOS prompt, with no agent, no driver, and no
   network. This is what separates airkvm from every "remote control" tool that already exists;
   a feature that needs cooperation from the target is out of scope, not a future enhancement.
3. **Zero-install on the host too.** The console is a web page the dongle serves. Reach for a
   native helper only when a capability is genuinely impossible in a browser (macOS-reserved
   chords), and then as an optional adjunct — never as the primary path, or the 30-second job
   becomes a 10-minute setup.
4. **The dongle must never be the thing typing.** Held keys, stuck modifiers, and repeat storms
   on someone's production box are the failure mode that matters. Any loss of confidence in
   session state — socket close, timeout, page hidden, reconnect — releases all keys and buttons.
   Prefer a spurious release-all over a plausible held key.
5. **Physical presence is the security model, and it says so out loud.** Anyone on the LAN who
   can reach the console can type on the target; the honest mitigation is that plugging the thing
   in is a deliberate act and unplugging it is a complete revocation. Document that boundary
   plainly (see `docs/repo-map.md`), and never let a convenience feature quietly widen it —
   especially not by reaching the internet.
6. **Latency is a feature, and it is measured, not asserted.** Typing that lags is typing that
   double-fires and gets abandoned. Changes on the input path state their latency effect, and
   "feels fine" is not a measurement.

### The tripwire protocol

A conflict with a principle is a stop-and-check, never something to quietly work around:

- If **your own approach** conflicts with a principle, treat that as a strong signal the approach
  is wrong. Re-derive it before proceeding — the principle has usually seen more failures than
  this session has.
- If the **operator's request** conflicts, say so explicitly, naming the principle. Then exactly
  one of two things is true:
  - **The principles have evolved.** Update the principle in the same change — and demand the
    justification: the *why* gets recorded next to the amendment, because an unjustified
    amendment is indistinguishable from erosion.
  - **The principles stand.** Then the request should be reconsidered — propose the nearest
    conforming alternative and let the operator choose with the conflict in view.
- Never proceed silently with a violation. An unremarked exception teaches every future reader —
  human or agent — that the principles are decorative (see §10, meta-rule 1).

## 3. Stack at a Glance

| Layer | Technology | Notes |
|---|---|---|
| Firmware language | C (C11) | ESP-IDF v5.5, installed by `mise run setup` into `.esp-idf/` |
| Target hardware | ESP32-S3 (`esp32s3` target) | ESP32-S3-0.42 OLED supermini; native USB OTG is non-negotiable (§4) |
| USB stack | TinyUSB via `esp_tinyusb` | Composite HID: boot keyboard + relative mouse |
| Network | `esp_http_server` (WebSocket) + `mdns` | Single origin serves console and socket |
| Console | Vanilla HTML/CSS/JS, no build step | Embedded in firmware via `EMBED_FILES`; no npm, no bundler, no CDN |
| Display | SSD1306 72×40 over I2C, or ST7789 172×320 over SPI | One backend per board behind `main/display.h` (`display_ssd1306.c` / `display_st7789.c`), sharing the drawing layer in `main/canvas.h`. Status only (IP, mDNS name, link state). Both confirmed on real hardware: the OLED's pins and 180° rotation on 2026-08-31, the ST7789's pins, orientation and gap on 2026-09-06 |
| Host tooling | Python 3 (stdlib only) | `tools/poke.py` drives a dongle from a terminal; not shipped to users |
| Testing | CMake/ctest + plain C assertions | Keymap, protocol codec, and HID state are pure and host-tested — `test/host/` |
| Deployment | `idf.py flash` over USB-C | No shipping OTA path yet — `main/ota.c` is a throwaway, URL-gated dev test hook, empty (no-op) in every real build. See §7 |

## 4. Critical Architectural Rules

These are non-negotiable. Violating them will break things or be rejected in review.

### Native USB device mode is a hardware prerequisite, not a build flag

HID output requires a USB OTG device controller: **ESP32-S3, S2, or P4 only**. Classic ESP32,
C3, C6, and H2 expose a fixed-function USB-Serial/JTAG peripheral whose descriptors are burned
into silicon — no amount of firmware makes them enumerate as a keyboard. Do not add a build
target, a `Kconfig` option, or a README line implying otherwise. (This cost a real evaluation
round at design time: the C6 variant of this exact board was on the shortlist until the
peripheral difference surfaced.) The intended path for non-OTG boards is a **CH9329 UART-to-HID
module** sitting behind the same interface `main/usb_hid.h` defines — planned, not built. No
such backend exists today and no doc should imply otherwise.

### One HID state owner; the socket layer never touches USB

All USB HID reports originate from a single owner module (`main/hid_state.c`) that holds the
authoritative keyboard bitmap, modifier byte, and button mask. The WebSocket handler parses
frames into intent structs and hands them over; it never calls `tud_hid_*` directly. Reports go
out from one task, serialized through a queue.

Two things depend on this and would each be a real-world bug if it were violated:

- **Release-all must be total** (§2, principle 4). One owner means "release everything" is one
  function that provably clears every bit, not a best-effort sweep across call sites.
- **HID reports must not be constructed on the HTTP task.** A blocked network task must never be
  able to leave a key held. Also, TinyUSB's device stack is not thread-safe across arbitrary
  callers.

### The keymap is a pure lookup table with no I/O, and it is host-tested

Browser `KeyboardEvent.code` → HID usage code is a table (`main/keymap.c`), not a chain of
conditionals, and it neither logs, allocates, nor touches hardware. Same for the protocol codec
(`main/protocol.c`). These two files must compile and run on the host so their tests execute in
CI without an ESP32 (see §7). Anything requiring `esp_` headers belongs elsewhere. Keymaps are
layout-independent: the browser reports **physical key positions** (`event.code`), which is
exactly what HID usage codes mean — never map `event.key`, which is layout- and modifier-shifted
and will send the wrong key on any non-US layout.

### The console is a single self-contained page, offline-capable

No CDN links, no npm, no build step, no framework. The dongle serves everything the console
needs, because a machine you walked up to may be on a network with no internet at all — and
because the console must not phone anywhere (§2, principle 5). It is embedded in the firmware
image (`EMBED_FILES` in `main/CMakeLists.txt`) and served straight from flash — uncompressed
today; gzipping is worth doing once the page stops changing shape every session. If a dependency
seems necessary, that is a proposal for review (§8, no-stated-convention rule), not a default.

### Never claim latency, safety, or hardware behavior that hasn't been observed on real hardware

The firmware is being written before the board exists. Any statement about enumeration,
BIOS behavior, timing, or OLED pinout is a **hypothesis until it runs**, and must be marked as
such in-place ("unverified — no hardware yet") in both code comments and docs. Pin assignments
for this board variant in particular are vendor-documented, not datasheet-guaranteed. This is
principle 1 (claims must hold) applied to the project's dominant failure mode right now.

## 5. Repository Map

`docs/repo-map.md` is the living boundary map: surfaces, trust boundaries, cross-cutting flows,
and the risk register. It exists because that knowledge lives *between* files and can't be
reconstructed by grep — and because cold-started subagents and future sessions get no
conversation context, only what's written down.

- **Read it before touching anything boundary-sensitive**: auth, tenancy, deletion/visibility,
  serialization, background delivery, routing, any external surface.
- **Update it in the same PR** as any change that moves an edge on it. Never leave it describing
  a stale security boundary — a wrong map is worse than no map, because it is trusted.
- Cite risk-register IDs (R-00N) from rules, PRs, and code comments, so the register stays
  load-bearing instead of decorative.

## 6. Development Essentials

Toolchain is pinned in `mise.toml`. From a fresh shell:

```sh
mise install                    # cmake, ninja, python, ruff, clang-format
mise run setup                  # one-time: clone ESP-IDF v5.5 into .esp-idf/ and install its
                                # esp32s3 toolchain (several GB; not mise-managed)
```

Then, per task:

```sh
mise run config                 # idf.py menuconfig — WiFi credentials, under "airkvm"
mise run build                  # idf.py build (target pinned in sdkconfig.defaults)
mise run flash                  # idf.py -p $PORT flash monitor   (see gotchas re: PORT)
mise run clean                  # idf.py fullclean
mise run test                   # host-side unit tests — no hardware required
mise run lint                   # clang-format --dry-run --Werror + ruff check/format --check
mise run fmt                    # clang-format -i + ruff format
```

`mise run test` is the gate that matters day to day: the keymap and protocol codec are pure and
compile on the host (§4), so their tests run in seconds with no board attached. Everything else
about this project currently cannot be verified without hardware — say so rather than implying
a passing build means working firmware (§8, verify the output not the instrument).

Console development: the console is plain static files under `web/`. Edit and reload against a
flashed board, or serve `web/` with `python3 -m http.server` and point it at a board's WebSocket
via the `?ws=` query parameter for UI-only iteration.

### 6.1 Known Environment Gotchas

Things that cost real time to discover once — don't rediscover them. Add entries as they're found;
add rather than rewrite.

- **The dongle's single USB-C port is both the HID port and the flashing port.** Once firmware
  claims USB for HID, the board no longer enumerates as a serial device: to reflash, hold BOOT
  and tap RESET to enter the ROM bootloader. Set `PORT` explicitly rather than relying on
  autodetect, because the port only exists in bootloader mode.
- **The primary dev machine for this repo is a Raspberry Pi (aarch64 Linux); the operator's host
  is a Mac.** Espressif ships aarch64 Linux toolchains, but any tooling added here must not
  assume x86_64 — and browser-side behavior must be checked on Safari, not only Chromium
  (`keyboard.lock()` in particular is Chromium-only, and the console must degrade without it).
- **Flashing a board attached to another machine needs no toolchain on that machine.** Verified
  against `pi3-unicorn`: build here, flash and read logs over the network.

  ```sh
  # once, on the machine holding the board — pyserial is pure Python, so copy it
  # rather than installing anything (no apt, no sudo, ~215KB):
  #   tar -C <site-packages> -czf - serial | ssh host 'mkdir -p ~/.airkvm-tools && tar -xzf - -C ~/.airkvm-tools'
  #   scp tools/serial_bridge.py host:~/.airkvm-tools/
  PYTHONPATH=~/.airkvm-tools python3 ~/.airkvm-tools/serial_bridge.py /dev/ttyACM0 4000
  # here:
  PORT=rfc2217://thathost:4000 mise run flash-bench
  ```

  Three things that each cost a debugging round, all handled in `tools/serial_bridge.py`:
  - RFC2217, not a raw socket: esptool drives the board into its bootloader with DTR/RTS, which a
    plain TCP pipe does not carry. Over a raw socket the flash hangs on a reset that never lands.
  - `python -m serial.rfc2217_server` does not exist — pyserial ships that as an example, not an
    installed module. `serial.rfc2217.PortManager` is the part you can actually import.
  - **A board that resets re-enumerates, and the stale descriptor goes quiet rather than
    erroring.** Every reset (including a successful flash) invalidates the device node, and reads
    on the old one return no data instead of raising, so a working board is indistinguishable from
    a dead one. The bridge watches the device inode to notice; do not "fix" this by catching
    exceptions alone, because there are none.
- **Bootstrap OTA once and stop flashing over the wire.** The BOOT+RESET+bridge dance is only
  needed to get an *OTA-capable* image on; after that every update goes over WiFi in ~60s with
  the board untouched. Verified end to end 2026-09-04, in both directions (a changed image
  applied, then the original restored). Set in a **local, never-committed** `sdkconfig` — both
  are existing symbols, so edit `sdkconfig` directly and confirm in `build/config/sdkconfig.h`,
  because a `SDKCONFIG_DEFAULTS` file cannot override either:

  ```sh
  CONFIG_AIRKVM_OTA_TEST_URL="http://<this-host>:8000/airkvm.bin"
  CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y
  # then, from the build dir:  python3 -m http.server 8000 --bind 0.0.0.0
  ```

  Three things that are not obvious:
  - **Every image you serve must keep the OTA config**, or the update self-reverts.
    `confirm_boot_if_pending()` lives inside the same empty-URL gate that disables polling, so an
    image without the URL boots, never calls `esp_ota_mark_app_valid_cancel_rollback()`, and the
    bootloader rolls back on the next reset. Safe, but baffling if unexpected.
  - **The HTTP access log is the diagnostic.** Each poll is a `GET /airkvm.bin`; a normal one
    ends in a connection reset because the firmware reads only the header, compares
    `app_elf_sha256`, and closes. An *off-cadence* poll (well under the 60s interval) means the
    device rebooted — that is how you spot an applied update with no console.
  - **This is a real widening of the trust boundary** (§2 principle 5): an unauthenticated
    plain-HTTP fetch baked into the image means anyone on the LAN who can answer at that address
    runs firmware on the dongle. Bench networks only, never committed, and never in anything
    resembling a real build.
- **Do not hand-roll the reset sequence for USB-Serial/JTAG parts.** Toggling DTR/RTS yourself
  lands the C3 in download mode (`boot:0x4 (DOWNLOAD)`) because DTR is the boot-mode line there.
  Use `esptool ... --after hard_reset run`, which knows the chip-specific sequence.
- **The red LED on the C3 bench board is not controllable.** It is a hardwired power indicator;
  `tools/led_scan` walked every free GPIO in both polarities and it never changed. GPIO8 (blue) is
  the only software LED. Settled — see the note in `main/board.h` rather than re-running the hunt.
- **Published pinouts for this board family tag GPIO8/9 as I2C.** Those are the chip's default
  alternate-function labels, not the wiring: GPIO8 is physically the LED, and the OLED is on
  GPIO5/6. Trust a working config over a pinout diagram.
- **Renaming the project directory breaks the ESP-IDF toolchain and every build directory.** The
  Python venv holds absolute shebangs and CMake caches the source path, so a rename needs
  `rm -rf .esp-idf-tools/python_env build build-bench test/host/build tools/led_scan/build &&
  mise run setup`. The symptom is a build complaining it was "configured for project /old/path" —
  and it bites the host tests too, which is easy to miss because they have nothing to do with
  ESP-IDF.
- **Board pinout is vendor-documented, not datasheet-derived.** The 0.42" OLED's I2C pins differ
  between the C3, C6, and S3 variants of this board family. Confirm against the vendor demo for
  the *S3* variant specifically; do not carry over the widely-quoted C3 numbers (GPIO5/6).
- **`gh run list --limit 1` and `gh run watch` will hand you the wrong run.** The newest run on a
  branch here is usually "Running Copilot Code Review", not CI — so the obvious incantation watches
  a reviewer bot go green and reports CI as passing. Select on name *and* head SHA:

  ```sh
  gh run list --branch <b> --json databaseId,name,headSha \
    -q '[.[]|select(.name=="CI")][0].databaseId'
  ```
- **Copilot hides real findings in a collapsed `<details>Suppressed comments</details>` block.** The
  rendered PR summary reads clean while genuine findings sit inside it. Read the comments through
  the API — `gh api repos/abradner/airkvm/pulls/N/comments` — not the web summary. Related: Copilot
  reviews each PR against *its own base*, so in a stack it objects to things a later interstitial
  already fixed; check the cap of the stack before actioning a finding raised low in it.
- **`mise run fmt` (clang-format) reflows across comment boundaries and can orphan a doc comment
  above the wrong function.** It happened in the keepalive PR, and the result compiles and lints
  clean — nothing catches it but reading. Re-read any region you formatted.
- **A config change can silently not reach the image, in two different ways. Read the generated
  `sdkconfig` before trusting any build that depends on one.** Both of these cost a flash cycle
  in one evening, and neither produces a warning — the build goes green around the unchanged
  value, so it presents as "the feature doesn't work".
  - *New symbol, stale generated file.* Adding a `CONFIG_` symbol to `Kconfig.projbuild` and
    running `mise run build` produced an image where the symbol was *absent* from `sdkconfig`
    entirely — not "is not set" — so every `#if CONFIG_X` took the off branch. A
    `CMakeLists.txt` edit triggers a reconfigure on its own; a Kconfig-only edit does not. Force
    it: `mise x -- bash -c '. "$IDF_PATH/export.sh" >/dev/null && idf.py -B build
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.local" reconfigure'`.
  - *Existing symbol — a defaults file cannot change it, and `reconfigure` will not help.*
    `SDKCONFIG_DEFAULTS` files only **seed** symbols that `sdkconfig` does not already contain.
    Putting `CONFIG_AIRKVM_TLS=n` in `sdkconfig.local` and reconfiguring left `sdkconfig` saying
    `=y`, with no diagnostic of any kind. This is why the first trap is easy to misdiagnose: a
    *new* symbol appears to prove defaults files work, because for new symbols they do. To flip
    an existing value, edit `sdkconfig` directly (or `mise run config`), then confirm in
    `build/config/sdkconfig.h` — the header the compiler actually reads is the only real proof.
- **A git worktree does not get its own ESP-IDF, and must not clone one.** `mise.toml` derives
  `IDF_PATH` from `{{config_root}}`, which in a worktree is the worktree — so `export.sh` fails
  with "No such file or directory" and `mise run setup` would happily clone a second ~4GB
  toolchain. Symlink the main checkout's instead, once per worktree:

  ```sh
  ln -sfn ~/code/airkvm/.esp-idf .esp-idf && ln -sfn ~/code/airkvm/.esp-idf-tools .esp-idf-tools
  ```

  Both are gitignored. The build then prints a harmless `IDF_PATH is changed from ...` warning as
  it resolves the symlink. Worktrees also need `mise trust` and their own `sdkconfig.local`
  (credentials are per-machine and gitignored, so a fresh worktree has none).
- **An S3 that boots to `waiting for download` has one of two unrelated causes, and the
  strapping register tells you which in one command.** Do not guess, and do not assume it is the
  flash — a verified write plus `boot:0x0 (DOWNLOAD(USB/UART0))` is the ROM reading GPIO0 at
  reset, before any of your code exists.

  ```sh
  esptool --port $PORT --before no_reset --after no_reset read_mem 0x60004038  # GPIO_STRAP_REG
  esptool --port $PORT --before no_reset --after no_reset read_mem 0x6000812C  # RTC_CNTL_OPTION1
  ```

  Bit 3 of `GPIO_STRAP_REG` set means "not download mode". **Clear means GPIO0 is physically
  low** — a held or stuck BOOT button, which no amount of software fixes. Bit 0 of
  `RTC_CNTL_OPTION1_REG` is `FORCE_DOWNLOAD_BOOT`, a retention bit esptool sets that survives
  reset; `esptool --after hard_reset` clears it, and a power cycle always does. Sanity-check the
  read path first (`read_mem 0x40000000` must return non-zero ROM code) — an all-zero register
  read is exactly what a broken read looks like too. Observed 2026-09-06: strap `0x00000000`
  across repeated reads with the force bit clear, i.e. a stuck BOOT button, diagnosed without
  touching the board.
- **Disabling a USB hub port does not power-cycle the device.** `echo 1 >
  /sys/bus/usb/devices/<hub>:1.0/<hub>-portN/disable` (root) does drop the device off the bus and
  re-enumerate it on re-enable, and it looks like a power cycle in `dmesg` — but the cheap hubs
  here have no per-port power switching, so VBUS never drops and the chip keeps its state. It is
  not a substitute for unplugging, and `uhubctl` cannot help on a hub without PPPS either.
- **Do not open the serial port with pyserial while setting DTR/RTS.** Same root cause as the
  hand-rolled-reset entry below, one level up: pyserial asserts DTR on open by default, DTR is
  the boot-mode line on USB-Serial/JTAG parts, and the board lands in download mode the moment
  you attach. Construct with `port=None`, set `dtr = rts = False`, assign `.port`, then `.open()`
  — or just use `idf.py monitor`.
- **`tools/serial_bridge.py` goes stale every time the board changes USB mode — restart it.**
  Seen ~8 times in one evening: after any flash whose image moved the board between USB HID
  (TinyUSB) and USB-Serial/JTAG, in either direction, the next esptool connect failed with
  *connection refused* or *"Remote does not seem to support RFC2217"*, and restarting the bridge
  fixed it every time. Each mode change re-enumerates the board as a different USB device, so
  the node the bridge opened is gone — the same silent-stale behaviour the inode watch above
  exists for; why the watch does not recover this case was not chased. Check
  `nc -z <host> 4000` before every flash. The good news, same root: when the board is *in*
  USB-Serial/JTAG mode, esptool's `--before default_reset` enters the bootloader on its own —
  the manual BOOT+RESET in the first bullet above is only needed from HID mode, where there is
  no serial control channel to send it through.

## 7. CI & Deployment

`.github/workflows/ci.yml` runs on every PR and on pushes to `main`, with no path filters (the
repo is small; filtering would buy seconds and cost trust):

- **host-tests** — builds and runs the pure-logic tests (keymap, protocol, hid_state). Fast,
  and the only job that can actually fail for a real logic bug.
- **firmware-build** — builds the ESP32-S3 image in the official `espressif/idf:release-v5.5`
  container. Proves it compiles and links; proves nothing about behavior.
- **lint** — `clang-format` on firmware sources; `ruff check` and `ruff format --check` on
  `tools/`. The format check is the one most likely to redden an otherwise-clean PR: run
  `mise run fmt` before pushing.

Deployment is `idf.py flash` over USB by a human holding the board. There is no shipping OTA path
and no release artifact — don't build those until asked. What does exist is `main/ota.c`: a
throwaway, dev-only proof that the underlying `esp_https_ota` mechanism (fetch, write the inactive
slot, switch boot partition, roll back on failure) works on real hardware, gated on a build-time-
only URL that's empty in every real build — not the fleet delivery mechanism, no signing, no
coordinator. Fleet mode *has* been asked for and is designed but not built: `docs/design/fleet.md`
is the spec, and its sequencing items are the roadmap. A green CI run means the code
compiles and the pure functions behave; it says nothing about USB enumeration, HID delivery, or
latency, none of which CI can observe (§4, last rule).

Two universal cautions, whatever the pipeline:

- **A skipped job is not a passing job.** A green run where path filtering skipped half the suite
  means that half never saw the commit. Check what actually ran before trusting the check.
- **A build is not a behavior.** `firmware-build` going green means the image links, nothing more.
  Every claim about enumeration, HID delivery, BIOS support, or latency needs a board, and until
  one exists those claims are marked unverified in-place (§4).

## 8. Working Rules

The portable core. These rules are pre-filled from hard-won convention across this repo's sibling
projects; adjust only with reason, and record the reason (see §10).

### Planning & approval

- Propose an implementation plan for any moderately or highly complex change, and get it reviewed
  and approved before making edits. Don't dive into large builds or refactors on your own read of
  the situation.
- For anything genuinely ambiguous or not yet decided by the operator, prefer the reversible
  option and leave a clear marker rather than picking silently. Two-way doors over one-way doors.
- **Building structure where no convention is stated is a decision, not a default.** When a change
  introduces more than trivial structure (a new page/screen, a new service layer, a new module
  shape) and §4 states no convention for that surface, do not silently invent one — and do not
  treat "match the surrounding code" as cover: over undesigned code that instruction *propagates*
  the blur. Adopt a pattern, then name it in the PR body as an explicit proposal ("no stated
  convention for X; this PR uses Y; codify or correct"). The operator's review then decides a
  standard once, instead of re-litigating shape per PR. (Motivating instance: a sibling repo's
  first UI-heavy PR mixed layout, scaffolding, and domain components with no container/presenter
  separation — the agent had nothing to follow, faithfully extended the existing blur, and the
  standard got stated for the first time as a review comment, the most expensive place to state
  it.)

### Destructive & outward-facing actions

- Destructive actions (dropping data, deleting files you didn't just create, rewriting published
  history) and outward-facing actions (publishing, sending, deploying) need explicit approval,
  every time. Approval for one instance is not approval for the next.
- Never merge to a shared branch, force-push, or rewrite published history without an explicit,
  current go-ahead. A green build and an auto-mode session default are not that signal — if the
  work is ready, say so and stop.
- If you encounter a violation of a safety rule already committed (a plaintext secret, a
  destructive migration lying in wait), flag it immediately — finding it is not the same as
  having caused it, and silence helps nobody.

### Configuration

- Fail fast on boot: never provide fallback defaults when reading *required* configuration.
  Missing required config must raise at startup rather than silently degrade. Defaults are fine
  for genuinely optional tuning knobs. (This isn't hypothetical: a sibling repo baked a stand-in
  value into an image-wide ENV to make a build step pass, which would have silently defeated this
  rule in production. If a build step needs a stand-in, scope it to that step, never image-wide.)

### Review feedback

- **A finding is a claim, not a verdict.** Whether it comes from a bot, a human, or your own
  earlier session: trace or reproduce it before acting on it. Ask whether the flagged path is
  actually reachable. When a finding says code and docs disagree, work out which end is wrong
  before "fixing" either. Declining findings has to actually happen — a round that accepts every
  finding is a warning sign, not a good score.
- Reject suggestions that violate the rules in this file, and say why. Automated reviewers read
  this file too; that's expected — reviewer-side agents should review fully as normal, and rules
  here that bind only author-side agents say so explicitly.

### Testing & verification

- **Verify the output, not the instrument.** Green means nothing threw, and nothing more. Before
  claiming something works, name the artifact it should have produced and go look at it — the
  rendered page, the written file, the actual rows.
- **Prove a new test can fail.** For any bug fix: write the regression test, confirm it passes
  with the fix, revert just the fix, confirm the test fails for the right reason, restore the fix.
  A test that was never seen to fail hasn't proven anything.
- Ask what else satisfies your assertion — a count-based check that an empty-state row also
  matches, a visibility check that passes for a scrolled-away element. When asserting absence,
  include a positive control so a broken probe can't read as success.
- Every conditional branch that encodes real logic gets a test that exercises it — especially the
  rare/edge branches, not just the happy path.

### Layered checks

- Apply the **deletion test** to any validation that exists in more than one place: enforcement
  exists exactly once; anything layered on top must, if deleted, change only politeness (a
  friendly error instead of an ugly one), never possibility. If deleting either check would make a
  new state possible, you have enforcement in two shapes, and they will drift.

### Documentation & discovered work

- Docs describing a boundary or behavior change in the same PR as the change. A doc describing a
  boundary that no longer exists is worse than none, because it is trusted.
- Log discovered work (bugs found in passing, deferred improvements) in the external tracker —
  never a PR body or a code comment. A triage table in a PR body goes stale between rounds and
  vanishes on merge.
- When you fix a subtle bug or get burned by a non-obvious behavior, write the general form of the
  lesson into §9 before moving on — same session, not later.
- **A review comment that names a missing standard forks.** "We need a convention for X" /
  "this pattern was never established" is not an instance finding — resolving it means both
  fixing the instance *and* codifying the standard into §4 (or the doc §4 points to) in the same
  round; if the standard needs real design time, ticket it in the tracker with the review link.
  A convention stated only in a review thread doesn't exist for the next session — the thread
  dies with the PR, and the next agent re-improvises. (This is §9's ratchet, aimed at
  conventions: incidents accrete into gotchas, review-discovered standards accrete into §4.)

### Tooling

- **Audience: Antigravity only.** Use the agent's structured file tools rather than `cat`/`sed`/shell
  here-docs for inspecting and modifying files during a session. (Scope: session file operations;
  committed shell scripts do what shell scripts do.)

  This was written as a universal rule and was the wrong shape. It encodes one harness's ergonomics,
  not a property of this codebase — and a harness that actively directs the agent to use shell file
  operations (Claude Code's auto mode does) puts the agent in a standoff between two live
  instructions, with no way to tell that one of them is a stale import. **If your harness directs
  otherwise, follow your harness.** Say in-session which instruction you followed and why, so the
  conflict stays visible rather than being resolved silently in either direction.

### Multi-PR batches

- The default flow is one PR at a time: react to feedback immediately, merge when green.
- Several PRs open together as one body of work is a different regime: use
  `.claude/skills/batch-review` (fan out, feedback write-only until synthesis, one followup PR).
  It is opt-in for genuine multi-PR fan-outs, not a replacement for the default.

## 9. Gotchas & Lessons Learned

Numbered, accreting. Add to it rather than rewriting it — the numbering is stable so entries can
be cited from code comments and PR descriptions.

Entry shape: **what happened → the actual root cause → the general rule → where the regression
test lives** (if one exists).

*Environment-specific quirks go in §6.1 instead; this section is for lessons that changed how the
code is written.*

1. **A WebSocket handler must return early on the handshake invocation, or every session's first
   frame is silently corrupted.** `esp_http_server` calls the `/ws` handler once at handshake time
   with no frame pending. A handler that immediately tries to read consumes the *next* frame's
   opcode byte (`0x82`) and parses it as a length byte — `0x82 & 0x7F` = 2 — so the first frame of
   every session decodes as a masked 2-byte frame of mask material, and the stream desyncs from
   there. The fingerprint is `len=2` on every first frame, with a real payload byte visible at
   offset 1. The general rule: **when a callback is invoked for more than one kind of event,
   handle the discrimination first, before any I/O that assumes which event it is.**
   Root cause was found by enabling `httpd_ws`/`httpd_parse` DEBUG logging and noticing the
   *absence* of a `httpd_ws_get_frame_type` line before the first handler call — the missing log
   line was the evidence, not any present one. Fixed in `main/net.c` (`req->method == HTTP_GET`).

2. **A code review claim about a framework is a hypothesis until hardware rules on it.** The
   handshake branch in entry 1 was *removed* on a reviewer's well-argued, source-cited finding
   that `httpd_uri()` returns before invoking the handler, making the branch dead code. The
   reading was plausible and specific, and it was wrong for this path — restoring the branch is
   what made the device work. Reviews earn their keep (the same round found five real bugs,
   including one that truncated every served asset), but AGENTS.md §8's "a finding is a claim, not
   a verdict" applies with full force to framework-behavior claims: those are cheap to test and
   expensive to get wrong.

3. **`httpd_ws_recv_frame`'s `max_len` is the frame's length, not the buffer's capacity.** Passing
   `sizeof(buf)` makes the read pull bytes belonging to the next frame. Symptom overlaps with
   entry 1, which made both harder to see; they were separate bugs and both had to be fixed.

4. **"It got slow" is a measurement, not a diagnosis — and the first suspect is usually wrong.**
   A report of pages taking 20+ seconds looked like a memory or socket leak introduced by the UI
   work. It was neither. The evidence that settled it, in the order it should be gathered:
   *heap free and low-water flat across minutes* (no leak), *every request eventually returning
   200* (no fd exhaustion), *the periodic health log arriving exactly on schedule* (no starved
   scheduler or blocked task), and decisively *ICMP round trips already showing 30–1130ms* —
   latency below the application layer cannot be caused by application code. The actual cause was
   an RSSI of −85 dBm after the board moved to a different room. The general rule: **before
   attributing a slowdown to the code you just wrote, measure a layer underneath it.** Ping costs
   nothing and would have pointed the right way in the first minute. The health line in `ui.c`
   exists so the heap half of this is answerable without a rebuild.

5. **ESP-IDF defaults WiFi to `WIFI_PS_MIN_MODEM`, which is wrong for anything interactive.**
   The radio parks between beacons; measured cost here was 30ms best-case round trips becoming
   450ms average and 1130ms worst, with zero packet loss to hint at what was happening. It
   presents as "the page won't load", not as power management. `esp_wifi_set_ps()` in
   `wifi.c` (moved there when WiFi bring-up was split out of `net.c`), justified by
   principle 6 — this device is mains-powered by definition. **Refined since:** the measurement stands, but the conclusion was too broad. Power save is wrong *while someone is typing*, which is not most of this device's life; the radio now follows the operator across three tiers (`wifi_set_activity()`), and full speed is kept exactly when it is earned.

6. **`EMBED_FILES` and `EMBED_TXTFILES` are not interchangeable, and both directions have bitten
   this repo.** TXTFILES appends a NUL terminator; FILES does not. PEM certificates *must* use
   TXTFILES because `mbedtls_x509_crt_parse` requires the terminator — with FILES the parse fails
   `-0x2180`, every TLS handshake is refused, and the port still listens, so it presents as a
   cipher or certificate-content problem rather than a build one. Gzipped assets must use FILES,
   because a stray trailing byte corrupts them; an earlier bug subtracted one from the length to
   compensate for a terminator that was never there, truncating every asset. Rule: **binary in
   `EMBED_FILES`, anything a C string parser will read in `EMBED_TXTFILES`**, and never adjust a
   length to paper over the difference.

7. **Component `REQUIRES` cannot be made conditional on `CONFIG_` values.** Requirements are
   resolved in CMake's early-expansion pass, where those values are not reliably visible, so
   `if(CONFIG_X) list(APPEND ... )` silently fails to add the include path and the build dies on a
   missing header with the option plainly enabled in `sdkconfig`. Require unconditionally and
   guard the code with `#if` instead. (Conditional `EMBED_*` lists are fine — those are consumed
   in the real pass.)

8. **Bench hardware that cannot do the headline feature still finds the headline bugs.** Entries
   1-3 above were found on an ESP32-C3, a board that physically cannot be a USB keyboard, via
   the `log` HID backend (`main/hid_out_log.c`). None were reachable from host tests or a green
   firmware build. When the real board is late, ask what fraction of the system the wrong board
   can still exercise — here it was everything except the last inch.

9. **An app-level liveness deadline on `esp_http_server` measures server-task availability, not
   peer liveness.** One task handles accepts, TLS handshakes, asset serving and WebSocket dispatch,
   so anything slow on that task — a ~4.8s TLS handshake at low RSSI is the measured case — stalls
   keepalive processing and can expire a session whose client never went anywhere. The keepalive
   deadline stays at 3s regardless: principle 4 settles the tension toward releasing, and
   lengthening it trades a spurious release-all for the held-key failure the principle exists to
   prevent. TCP keepalive (`httpd_config_t.keep_alive_*`) complements it — ~20s detection, but it
   cannot false-positive on a busy task — it does not replace it. **Rejected, so nobody re-derives
   it:** gating expiry on "was the server task busy". An idle server with a genuinely vanished
   client also processes nothing, so the heuristic cannot distinguish the two cases it exists to
   separate. Tracked in issue #6; never observed on hardware, reasoned from the single-task design.

10. **On the S3, starting TinyUSB before `esp_wifi_start()` leaves WiFi unable to ever post
    `WIFI_EVENT_STA_START`.** The real USB HID backend's first run on real hardware showed
    "NO WIFI" forever at idle current: `wifi_start()` returned `ESP_OK` all the way through, the
    heap was flat, no WiFi event ever fired — and nothing was logged, because claiming USB for
    HID also removes the only console. Root cause is a shared resource, not a bug in either
    stack: the RF PHY and the USB PHY share the BBPLL (`soc_caps.h`:
    `SOC_WIFI_PHY_NEEDS_USB_WORKAROUND`, "SoC has WiFi and USB PHYs interference";
    `phy_init.c` writes the PLL's USB enable via `phy_bbpll_en_usb()` before handing the PLL to
    the closed calibration blob). With TinyUSB already clocked off that PLL, RF init silently
    never completes. Three hardware results triangulated it (2026-09-04): HID-first never
    associates; WiFi-first-then-HID-immediately associates and both coexist; an established
    connection survives HID starting 20s later. Fix: `net_start()` before `hid_out_start()` in
    `airkvm_main.c` — HID waits on WiFi's *synchronous* init only, never on association, and
    still starts if that init fails. Two general rules. **(a) "Verified on the bench board" is
    not verification when the bench board runs a no-op backend** — the USB-first order had only
    ever run on the C3, where `hid_out_start()` is the logger; that is R-005 exactly, and it will
    recur for every feature whose real backend the C3 cannot host. **(b) When the console is
    gone, the display is a log channel, and an init-order bisect on hardware beats reasoning
    about a closed blob** — four one-line OLED probes (heap, the `esp_err_t`, event counters)
    plus a deferred-start test build found this in one evening; a typed console command for
    the same purpose never registered and cost three flashes. No host test can cover it: the
    regression guard is the comment at `app_main()` and this entry, and any reorder of that
    function is a hardware re-test, not a review-time judgment.

11. **Coalescing the keyboard to "latest state" destroys the keypress; the first real typing test
    delivered 3 characters of 17.** `hid_out_usb.c`'s report task drained the whole intent queue
    and then emitted one report, justified by a comment claiming the keyboard "is a pure function
    of hid_state, so the newest state supersedes any older one". That is true of the *bitmap* and
    false of the *keystroke*: a key-down and its key-up applied in the same pass leave the bitmap
    exactly as it started, so the one report that follows says "nothing pressed" and the character
    is lost outright. Over TCP a down/up pair almost always arrives together, so a character
    survived only when task timing happened to split the pair across two 2ms passes — giving a
    ~20% delivery rate that barely moved when the sender slowed 6×, which is what ruled out
    "too fast" as the cause. Fix: the drain stops at a keyboard intent whenever a report is still
    pending (peek, don't receive, so it stays queued in order), costing one endpoint interval per
    state change and losing nothing. **Two general rules.** *(a) A state-machine optimisation is
    only safe if the states it skips carry no information.* Ask what the intermediate state
    *meant*, not just whether the final state is correct — here the intermediate state was the
    entire payload. *(b) Instrument the boundary, not the endpoints.* What made this diagnosable
    in minutes was the on-screen event counter: 82 applied intents against exactly 34+38+10 sent
    proved the frames all arrived and the loss was downstream, collapsing the search space from
    "anywhere in WiFi, TCP, WebSocket, protocol, HID" to one function. Then a *second* bug hid
    behind it: the tail of every string vanished (counter 24 of 34 sent, precisely the last five
    characters). **Closing the socket destroys queued input** — `close_fn` → `net_end_session()`
    → `hid_out_release_all()`, which sets the release flag *directly* rather than queueing it, so
    the report task's `xQueueReset` discards whatever has not been applied. Note this is *not*
    the release-all frame the tool also sends, which is queued and applied in order and therefore
    cannot overtake anything — an early version of this entry blamed the frame, which is wrong
    and would have sent the next reader to the wrong file. The distinction matters: because it is
    the *close*, it hits every sender that hangs up promptly, so it truncated chords too
    (`ControlLeft+AltLeft+Delete` is six frames now draining an endpoint interval apart) even
    though chords involve no repeated keys. `poke.py` now settles before closing, scaled by
    frames sent, in `WebSocket.settle()` where every subcommand gets it rather than in
    `cmd_type` alone. **General rule: a teardown path that is correct in isolation can still be
    wrong in composition** — release-on-close is right (principle 4), and the bug was entirely in
    who is responsible for not hanging up mid-sentence. Neither bug is reachable from host tests or from the C3
    bench board, whose log backend prints every intent and therefore cannot lose one — gotcha 8
    again, from the other direction. Verified on hardware 2026-09-04: 17 of 17 characters, event
    counter advancing by exactly 34.

12. **`pdMS_TO_TICKS()` silently rounds to zero, and `vTaskDelay(0)` is not a delay — one task
    had been spinning a core flat out since the initial commit.** `hid_out_usb.c`'s report task
    ended its loop with `vTaskDelay(pdMS_TO_TICKS(2))`. The macro is integer arithmetic, so at
    `CONFIG_FREERTOS_HZ=100` that is `(2 * 100) / 1000 == 0`, and the kernel guards on
    `if( xTicksToDelay > 0 )` with the comment "A delay time of zero just forces a reschedule" —
    the task is never added to the delayed list. Being priority 5 and always ready, it was
    rescheduled immediately, forever, pegging one core at 160MHz doing nothing and preventing the
    idle task from gating that core's clock via `WAITI`. Nothing failed; it just ran hot, and the
    file's own comments described a 2ms pacing that had never existed. **General rules.**
    *(a) A duration constant is not a duration until it survives the conversion.* Any
    `pdMS_TO_TICKS()` value below one tick period is a bug that compiles, and the only honest fix
    is a mechanism, not care: `_Static_assert(pdMS_TO_TICKS(X) > 0, ...)` next to the constant,
    which was confirmed to fail for the right reason before being trusted (meta-rule 5).
    *(b) Removing waste can expose what the waste was accidentally buying.* Fixing the spin
    dropped typing throughput by half — measured, 26 of 60 characters against 60 of 60 — because
    at 100Hz the smallest possible wait is 10ms, which aliases against the USB endpoint's 10ms
    `bInterval`: a task waking every 10ms catches the ready window only every other time. The
    tick rate then had to be raised to 1kHz deliberately (`sdkconfig.defaults`) to buy back
    granularity the spin had been providing by accident. Ask what a wasteful thing was doing
    before deleting it, and re-measure the thing it was near.
    *(c) The instrument for all of this was new.* `tools/hid_listen.py` reads the dongle's own
    evdev node on the host it is plugged into, so "did it type correctly" became a machine check
    instead of a question for a human — which is what made a throughput regression visible at all
    rather than being discovered weeks later as "typing feels slow".

13. **ESP-IDF's TLS 1.3 *server* crashes this firmware, and the client cannot avoid it.** The
    HTTPS console accepted TCP on 443, answered with nothing, and reset — for days. It presented
    as a certificate or resource problem and was neither: every handshake took a NULL dereference
    (`LoadProhibited`, `EXCVADDR 0x00000000`) in `ssl_parse_record_header`, reached from
    `ssl_tls13_process_client_hello` with `ssl->conf` NULL, and rebooted the board. Because the
    device reboots, the client sees only a reset — no alert, no log, no server. Fix:
    `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=n` in `sdkconfig.defaults`. The console is a LAN page for a
    browser that has imported our CA; 1.2 is sufficient, and dropping 1.3 also saves ~55KB.
    **Four general rules, each of which cost real time here.**
    *(a) Forcing the client to an older protocol does not keep the server out of the newer code
    path.* `openssl -tls1_2` still crashed it, because the server begins in the 1.3 state machine
    and negotiates down. A client-side restriction is not a server-side one.
    *(b) Eliminate by construction, not by plausibility.* Certificates were the obvious suspect
    and stayed suspect through several rounds. What actually killed the hypothesis was compiling
    the *exact* vendored mbedTLS (3.6.3) natively and running the firmware's own calls with the
    firmware's own byte lengths — parse OK, key OK, pair matches. A native harness for anything
    host-portable is worth building early; it is minutes, and it is proof rather than argument.
    *(c) Your instrument can be the thing lying to you.* `openssl s_client </dev/null` closes
    stdin immediately, and against a slow ESP32 handshake that produced
    `MBEDTLS_ERR_SSL_CONN_EOF` — a real error code, caused entirely by the test. Re-run with the
    connection held open before believing any handshake error.
    *(d) A crash is invisible when it takes the log with it.* The panic could only be read over
    serial, which a HID build does not have; the network log tap (`AIRKVM_LOG_TAP`) could not
    help because its ring buffer is RAM and the reboot clears it. Two channels, each blind to a
    different failure — keep both, and know which one a given failure will survive.

14. **Generalising a hardcoded constant hides three different constants in one number, and only
    a rendered picture finds them.** Extracting the 0.42" panel's drawing code into a shared
    `canvas.h` for a second board meant replacing literals with font metrics. The 3x5 font's
    numbers are 3 (glyph width), 4 (horizontal advance), 5 (glyph height) and 6 (line pitch) —
    and the original code used them interchangeably in places where they happened to coincide or
    where the difference did not show. Three separate bugs came out of that, each of which
    compiled, linked and passed a build:
    *(a)* `display_bar()` wrote `row * 6` while `display_text()` went through the cell macro, so
    the two would have disagreed the moment a board changed cell size. Caught by reading, before
    it ran.
    *(b)* The 8x16 font's descriptor omitted `line_h` entirely, leaving it 0 — every bar would
    have been drawn at y=0, on top of the header. Caught by a host test asserting each font's
    metrics against its board's grid, within seconds of that test existing.
    *(c)* `canvas_bar()` drew each cell `glyph_w` wide at `advance` pitch. On the 3x5 font that
    leaves a 1px gap and the bar reads as four boxes; on the 8x16, glyph_w == advance == 8, so
    every border merged and the signal bar became one solid block. **Nothing caught this but
    looking at a picture** — it is not a crash, not an overflow, and every assertion about
    position still held. The fix is `advance - 1`, which is *exactly* `glyph_w` for the 3x5 font
    and therefore provably changes nothing on the shipping board.
    **General rules.** *(1) When you replace a literal with a named quantity, enumerate every
    distinct quantity that literal was standing for, and check the ones that are equal by
    coincidence — those are where the next board diverges silently.* *(2) For anything whose
    correctness is visual, build a host renderer before building the hardware.*
    `tools/preview_ui.py` draws the real font tables at the real layout offsets into a PNG; it
    found (c) in one look, and it exists because the panel was unavailable, which turned out to
    be the more useful constraint. *(3) A golden test against the pre-change implementation is
    what makes an extraction like this safe to do at all* — `test/host/test_canvas.c` compares
    against a framebuffer captured by compiling the ORIGINAL `display.c` from git, so "the
    refactor changed no pixel on the working board" is a check rather than a claim.

## 10. Maintaining This Document

Meta-rules for editing this file — they exist because each was violated somewhere first:

1. **A rule everyone knows is wrong is worse than no rule** — it teaches agents that the rules are
   decorative, which devalues the ones that are load-bearing. When reality has moved, revise the
   rule openly: state what the old rule said, why it was the wrong shape, and what replaces it.
2. **Ground rules in incidents.** When adding a rule, say what happened. When correcting a wrong
   rule, record the correction in-place so the old rule doesn't get quietly reintroduced.
3. **Scope absolutes.** Name the exceptions at authoring time, or agents will either violate the
   rule or over-apply it.
4. **Name the audience** when a rule doesn't bind every reader — author-side agents, reviewer
   bots, and humans all read this same file.
5. **Prefer structural guarantees to written claims.** If a rule can be made mechanically true (a
   build-context exclusion, a CI gate, an import stub instead of a second copy), do that and
   document the mechanism, rather than adding another sentence.
6. **Expect to compress.** First-draft steering prose runs vague and grandiose; cut it to concrete
   nouns and commands the same day. When compressing an established doc, treat it like a refactor:
   diff the imperatives before/after and confirm nothing operative was dropped.
7. **Porting rules between sibling repos is judgment, not copying.** Verify each candidate rule
   against this repo's own files, CI, and conventions; adopt, adapt, or reject explicitly — and
   note rejections in the Adaptation Record below. Watch especially for rules that silently assume
   another repo's merge strategy, bot roster, or deploy shape.

## 11. Adaptation Record

What was pruned or changed from the keel template when this repo was initialized, and why — kept
in the doc (not just a commit message) so future readers don't need git archaeology to know what
was deliberately excluded.

Initialized 2026-08-10 from the keel template.

- **Tracker**: GitHub issues on this repo. The header's tracker pointer names issues rather than
  an external tool; §8's "log discovered work in the external tracker" means "open an issue."
- **Visibility**: public is the plan; **the repo is private today** (`gh repo view --json isPrivate`
  → true, checked 2026-08-30). Everything here is written for the public end state — which is why
  `README.md` links an issues URL that outside readers cannot yet resolve. Flip it before inviting
  anyone in. The template-lineage block was deleted per the init procedure — a
  pointer to a private template is unresolvable for outside contributors and would mis-steer
  their agents. Lineage is tracked in the private fleet registry (`abradner/fleet`) instead.
  **Operator TODO:** the fleet-registry row for `airkvm` was not added during init (the registry
  wasn't reachable from that session) — add it, with keel SHA `8f6c55e` as the synced-through
  point.
- **License**: kept Apache-2.0, appropriate for a public hardware/firmware project that
  references other open-source work (esp32-kvm-ip, espkvm) as design precedent.
- **Merge strategy**: squash-merge. The batch-review skill's `[MERGE-COMMIT]` rule variants were
  deleted and only the `[SQUASH]` variants kept, per meta-rule 7 — porting merge-strategy rules
  unexamined has produced subtly wrong instructions in sibling repos.
- **Caveman mode**: kept, including the per-tool config paths and the AGENTS.md block below.
- **Repo map**: kept (`docs/repo-map.md`). This project has a genuinely interesting trust
  boundary — an unauthenticated LAN surface that can type on a machine — which is exactly the
  kind of thing the map exists to keep honest.
- **Both skills kept**. `batch-review` is speculative for a solo hardware project but inert until
  invoked; `independent-commit-review` is expected to earn its keep immediately, since firmware
  written before hardware arrives gets no feedback from reality.

## Caveman Mode

Respond terse like smart caveman. All technical substance stay. Only fluff die.

Rules:
- Drop: articles (a/an/the), filler (just/really/basically), pleasantries, hedging
- Fragments OK. Short synonyms. Technical terms exact. Code unchanged.
- Pattern: [thing] [action] [reason]. [next step].
- Not: "Sure! I'd be happy to help you with that."
- Yes: "Bug in auth middleware. Fix:"

Switch level: /caveman lite|full|ultra|wenyan
Stop: "stop caveman" or "normal mode"

Auto-Clarity: drop caveman for security warnings, irreversible actions, user confused. Resume after.

Boundaries: code/commits/PRs written normal.
