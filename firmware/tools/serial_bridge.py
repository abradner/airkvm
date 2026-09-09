#!/usr/bin/env python3
"""Expose a local serial port over the network as an RFC2217 service.

Run this on whatever machine physically holds the board; build and flash from
wherever the toolchain lives:

    # on the machine with the board (needs only pyserial, which is pure Python
    # and can simply be copied — no package manager, no compiler):
    PYTHONPATH=~/.airkvm-tools ./serial_bridge.py /dev/ttyACM0 4000

    # on the machine with ESP-IDF:
    PORT=rfc2217://thathost:4000 mise run fw:flash-bench

RFC2217 rather than a plain TCP pipe because esptool drives the board into its
bootloader by toggling DTR/RTS. A raw socket carries the data but not those
control lines, so flashing over one hangs waiting for a reset that never
happens — the failure looks like a dead board rather than a missing feature.

pyserial ships this as an example rather than an installed module, so it lives
here instead of being invoked as `python -m serial.rfc2217_server`.
"""

from __future__ import annotations

import os
import socket
import sys
import threading

import serial
import serial.rfc2217


def device_identity(path: str) -> tuple | None:
    """Identity of the device node, used to notice re-enumeration.

    Exception handling alone is not enough: when a USB CDC device disappears,
    reads on the stale descriptor quietly return no data rather than raising,
    so a board that reset looks exactly like a board that has nothing to say.
    Watching the inode turns that silent failure into a detectable event.
    """
    try:
        st = os.stat(path)
        return (st.st_ino, st.st_rdev)
    except OSError:
        return None


def reopen(port: serial.Serial) -> bool:
    """Reopens the underlying device in place, keeping the object identity.

    Boards with a native USB-Serial/JTAG peripheral (ESP32-C3/S3 and friends)
    drop off the bus and re-enumerate on every reset, so the device node the
    bridge opened at startup goes stale the first time anything resets the
    board — including a successful flash. The symptom is the worst kind:
    everything reports success and the serial log is simply silent forever.

    Reopening the same Serial object rather than constructing a new one matters
    because PortManager holds a reference to it.
    """
    try:
        if port.is_open:
            port.close()
    except Exception:
        pass
    try:
        port.open()
        port.dtr = False
        port.rts = False
        return True
    except Exception:
        return False


class Bridge:
    """Pumps bytes both ways and lets PortManager handle the RFC2217 protocol."""

    def __init__(self, port: serial.Serial, conn: socket.socket, device: str) -> None:
        self.serial = port
        self.socket = conn
        self.device = device
        self.identity = device_identity(device)
        self.alive = True
        self._write_lock = threading.Lock()
        # PortManager needs an object with .write(); it calls back here to send
        # negotiation and modem-status messages to the client.
        self.manager = serial.rfc2217.PortManager(self.serial, self)

    def write(self, data: bytes) -> None:
        with self._write_lock:
            self.socket.sendall(data)

    def _poll_modem_lines(self) -> None:
        # The client learns about CTS/DSR/RI/CD changes only if something asks
        # for them; without this thread esptool can stall waiting on line state.
        while self.alive:
            try:
                self.manager.check_modem_lines()
            except Exception:
                pass  # port mid-reopen; the reader thread owns recovery
            threading.Event().wait(0.05)

    def _recover(self) -> None:
        print("serial port lost; waiting for re-enumeration", flush=True)
        while self.alive:
            if device_identity(self.device) is not None and reopen(self.serial):
                self.identity = device_identity(self.device)
                print("serial port back", flush=True)
                return
            threading.Event().wait(0.5)

    def _serial_to_socket(self) -> None:
        while self.alive:
            # Cheap re-enumeration check before each read, because a stale
            # descriptor fails silently rather than raising.
            current = device_identity(self.device)
            if current != self.identity:
                self._recover()
                continue
            try:
                data = self.serial.read(self.serial.in_waiting or 1)
            except (serial.SerialException, OSError, TypeError):
                # The board reset and re-enumerated. Keep the client connected
                # and wait for the device to come back rather than tearing the
                # session down — a reset is the normal case here, not a fault.
                self._recover()
                continue
            if data:
                try:
                    self.write(b"".join(self.manager.escape(data)))
                except OSError:
                    break
        self.alive = False

    def run(self) -> None:
        threading.Thread(target=self._poll_modem_lines, daemon=True).start()
        threading.Thread(target=self._serial_to_socket, daemon=True).start()
        while self.alive:
            try:
                data = self.socket.recv(1024)
            except OSError:
                break
            if not data:
                break
            try:
                self.serial.write(b"".join(self.manager.filter(data)))
            except (serial.SerialException, OSError):
                # Same re-enumeration case as the reader; drop this chunk rather
                # than the session. esptool retries at its own layer.
                pass
        self.alive = False


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <serial-port> <tcp-port>", file=sys.stderr)
        return 2
    device, tcp_port = sys.argv[1], int(sys.argv[2])

    port = serial.serial_for_url(device, baudrate=115200, do_not_open=True)
    # esptool drives these itself; asserting them here would reset the board on
    # every connect, which is surprising when you only wanted to read the log.
    port.dtr = False
    port.rts = False
    port.open()

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("", tcp_port))
    listener.listen(1)
    print(f"serving {device} on rfc2217://0.0.0.0:{tcp_port}", flush=True)

    while True:
        conn, addr = listener.accept()
        print(f"client {addr[0]}:{addr[1]} connected", flush=True)
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            Bridge(port, conn, device).run()
        except Exception as err:  # one bad client must not end the service
            print(f"client error: {err}", flush=True)
        finally:
            conn.close()
            print("client disconnected", flush=True)


if __name__ == "__main__":
    sys.exit(main())
