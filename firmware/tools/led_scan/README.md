# led_scan

Throwaway hardware-identification firmware, kept because the question it answers
recurs every time a new board shows up: *which GPIO is that LED on?*

It drives each candidate GPIO low then high for two seconds, announcing both on
serial, so you can watch the board and match a blink to a number. GPIO8 (the
known blue LED on the C3 bench board) is included as a positive control — if it
doesn't light, the scan is broken and nothing else it says means anything.

Not built by CI and not part of the firmware.

```sh
cd tools/led_scan
idf.py -DIDF_TARGET=esp32c3 build
idf.py -p /dev/ttyACM0 flash monitor
```
