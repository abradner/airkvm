# i2c_scan

Throwaway hardware-identification firmware, the I2C sibling of `led_scan`. It
answers the question AGENTS.md §6.1 says recurs on every new board in this
family: *which GPIOs is the OLED actually on?*

The published pinouts are not trustworthy here. §6.1 records that GPIO8/9 are
tagged "I2C SDA/SCL" in the vendor material for the C3 while the panel is really
on GPIO5/6, and that the C3, C6 and S3 variants differ from each other. On the
first real S3, `board.h`'s carried-over GPIO5/6 did not reach the panel.

Two stages:

1. Probe the SSD1306's address (0x3C, and 0x3D in case it is strapped the other
   way) on every ordered `(SDA, SCL)` pair of candidate pins.
2. Blink the panel on each pair that answered: enable the charge pump, then
   alternate "entire display on" (`0xA5`, lights every pixel regardless of
   GDDRAM) against normal RAM-driven mode (`0xA4`), three times.

Stage 2 exists because an ACK is not a display. A probe tells you something
acknowledged an address; only the panel visibly lighting up tells you it is the
panel. It deliberately does not rely on whatever image was already on the
glass — GDDRAM is volatile SRAM and does not survive the power cycle this board
needs to leave download mode (AGENTS.md §6.1), so by the time this tool runs the
panel is already back at its power-on default (display off, undefined RAM).
Forcing "entire display on" is what makes the test independent of that state;
an earlier version relied on the leftover vendor-demo frame and produced no
visible signal for exactly this reason, on the correct pins.

If nothing answers, that is not a pin assignment. Silence means the panel is not
on any scanned pair or not on I2C at all — do not read a conclusion out of it.

Pins carrying flash, PSRAM, USB D±, and the boot straps are excluded by name in
the source, so what was skipped is visible and reviewable.

Not built by CI and not part of the firmware.

```sh
cd tools/i2c_scan
idf.py -DIDF_TARGET=esp32s3 build
idf.py -p /dev/ttyACM0 flash monitor
```

The board must be in the ROM bootloader to accept a flash, and a USB-triggered
reset from download mode returns to download mode — only a physical power cycle
leaves it. See AGENTS.md §6.1.
