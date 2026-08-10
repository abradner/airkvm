# airhid

> 🤖 **AI Agents:** read [`AGENTS.md`](AGENTS.md) instead. This README is for humans.

A ~$12 ESP32-S3 board that plugs into any machine's USB port, enumerates as a **real USB keyboard
and mouse**, and serves a web console you drive from your laptop's browser. Walk up to a machine,
plug it in, open a URL, type. Nothing installed on the target. Nothing installed on your laptop.

Because the target sees a plain HID keyboard, it works where software remote-control doesn't:
BIOS/UEFI setup, boot menus, disk-unlock prompts, a hung desktop, a machine with no OS yet.

**Status:** early. Firmware is being written ahead of the hardware arriving, so nothing here has
run on a real board yet — treat every claim below as intent, not proof. See
[open issues](https://github.com/abradner/airhid/issues) for what's actually done.

## The shape of it

```
your laptop's browser                    airhid dongle              the target machine
┌──────────────────────────┐  WiFi/WS   ┌─────────────────┐   USB   ┌──────────────┐
│ keyboard + pointer lock ─┼──────────► │ WebSocket → HID │ ──────► │ sees a plain │
│                          │            │ reports         │         │ kbd + mouse  │
│ video ◄──── (local USB) ─┼─ ─ ─ ─ ─ ─ ┼ (not involved)  │         │              │
│   from your HDMI capture │            │                 │         └──────────────┘
└──────────────────────────┘            └─────────────────┘
```

Video is deliberately **not** the dongle's job. If you have an HDMI→USB capture stick, it's just a
webcam to your laptop — the console shows it in the same tab via `getUserMedia`, so the video path
stays local and full-framerate instead of being squeezed through a microcontroller.

## Hardware

| Part | Notes |
|---|---|
| ESP32-S3-0.42 OLED "supermini" | ~$12. The 0.42" OLED will show the dongle's IP so you don't have to hunt for it — not wired up yet; for now the IP is on the serial log. |
| That's it | The board's own USB-C is the HID port and the power source. |

**The S3 part is not optional.** HID output needs a native USB OTG controller, which means
ESP32-S3, S2, or P4. Classic ESP32, C3, C6 and H2 have a fixed-function USB-serial peripheral that
physically cannot present as a keyboard. If you have non-OTG boards you want to use, the planned
path is a CH9329 UART-to-HID module — not built yet.

## Getting started

```sh
mise install        # toolchain
mise run setup      # ESP-IDF v5.5 + esp32s3 compilers (one-time, several GB)
mise run config     # set WiFi SSID/password under "airhid" — required, no default
mise run build
PORT=/dev/tty.usbmodem1101 mise run flash
```

The dongle logs its address on boot (`http://<ip>/`, also `http://airhid.local/`). Open that on
your laptop, click the video pane to capture, and type.

To reflash after the first flash, hold **BOOT** and tap **RESET** first: once the firmware claims
USB for HID, the board stops being a serial device until you put it back in bootloader mode.

No browser handy? `tools/poke.py` speaks the same protocol from a terminal:

```sh
./tools/poke.py airhid.local type "hello world"
./tools/poke.py airhid.local key ControlLeft+AltLeft+Delete
```

`mise run test` runs the host-side unit tests for the keymap, protocol codec, and HID state
machine — no board required, and the only check here that proves behavior rather than compilation.

## Security, stated plainly

**Anyone who can reach the dongle on the network can type on the target.** There is no
authentication. That is the current design, not an oversight in progress: the security model is
physical — plugging it in is a deliberate act, unplugging it is complete revocation. Use it on
networks you trust, and unplug it when you're done. The trust boundaries and what would fix this
are written up in [`docs/repo-map.md`](docs/repo-map.md).

## Prior art

This project exists because of, and borrows design from, work that got there first:

- [KMChris/esp32-kvm-ip](https://github.com/KMChris/esp32-kvm-ip) — the direct inspiration:
  ESP32-S3 HID output driven over the network. Its host-side capture app is Windows-only (Win32
  low-level hooks over UDP), which is what prompted the browser-based console here.
- [espkvm/espkvm](https://github.com/espkvm/espkvm) — a full IP-KVM on the ESP32-P4 with HDMI
  capture on-device. The absolute-pointer approach is worth stealing.

## License

Apache-2.0. See [LICENSE](LICENSE).
