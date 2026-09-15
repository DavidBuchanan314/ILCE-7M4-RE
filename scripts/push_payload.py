"""
Works with tools/uart_boot/uart_boot.ino
"""

import argparse
import enum
import os
import select
import struct
import sys
import termios
import time
import tty
import zlib

import serial
from tqdm import tqdm


class Cmd(enum.IntEnum):
    PING    = 0x01
    RESET   = 0x02
    CARRIER = 0x03
    BLOB    = 0x04
    BAUD    = 0x05
    UART    = 0x06


class Evt(enum.IntEnum):
    ACK  = 0x81
    ERR  = 0x82
    UART = 0x83

VERSION = b"uart_boot 1"

UNLOCK_WORD = 0x2CF25621
LOAD_ADDR = 0xFE020000
MAX_RECORD_PAYLOAD = 512  # true max is 4086, but this keeps progress chunking smooth

CARRIER_START = 0.150
TRANSMIT_START = 0.250
RECORD_GAP = 0.0144


def record(rtype, value, payload=b""):
    body = struct.pack("<IHI", 10 + len(payload), rtype, value) + payload
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def boot_script(payload, addr=LOAD_ADDR, entry=None):
    if entry is None:
        entry = addr
    recs = [record(0x0000, UNLOCK_WORD)]
    for off in range(0, len(payload), MAX_RECORD_PAYLOAD):
        recs.append(record(0x0001, addr + off, payload[off:off + MAX_RECORD_PAYLOAD]))
    recs += [
        record(0xFFFF, entry),
        record(0x0000, UNLOCK_WORD),
        record(0xFFFF, entry),
        record(0xFFFF, entry),
    ]
    return recs


class Error(Exception):
    pass


class Device:
    def __init__(self, port, timeout=5.0):
        self.ser = serial.Serial(port, 115200, timeout=timeout)

    def close(self):
        self.ser.close()

    def send(self, cmd, data=b""):
        self.ser.write(bytes([cmd]) + struct.pack("<H", len(data)) + data)

    def _read_exact(self, n):
        out = b""
        while len(out) < n:
            chunk = self.ser.read(n - len(out))
            if not chunk:
                return None
            out += chunk
        return out

    def recv(self):
        hdr = self._read_exact(3)
        if hdr is None:
            return None
        (n,) = struct.unpack("<H", hdr[1:3])
        data = self._read_exact(n) if n else b""
        if data is None:
            return None
        return Evt(hdr[0]), data

    def ack(self):
        while True:
            pkt = self.recv()
            if pkt is None:
                raise Error("timed out waiting for the device")
            evt, data = pkt
            if evt == Evt.ACK:
                return data
            if evt == Evt.ERR:
                raise Error(data)
            if evt == Evt.UART:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()

    def ping(self):
        self.send(Cmd.PING)
        ver = self.ack()
        if ver != VERSION:
            raise Exception(f"bad version: {ver}")

    def reset(self):
        self.send(Cmd.RESET)
        self.ack()

    def carrier(self, on):
        self.send(Cmd.CARRIER, bytes([1 if on else 0]))
        self.ack()

    def blob(self, rec):
        self.send(Cmd.BLOB, rec)
        self.ack()

    def baud(self, baud):
        self.send(Cmd.BAUD, struct.pack("<I", baud))
        self.ack()


def forward(dev, log=None):
    stdin_tty = sys.stdin.isatty()
    saved = None
    if stdin_tty:
        saved = termios.tcgetattr(sys.stdin)
        tty.setraw(sys.stdin.fileno())
    try:
        while True:
            ready, _, _ = select.select([dev.ser, sys.stdin], [], [])

            if dev.ser in ready:
                pkt = dev.recv()
                if pkt is not None:
                    evt, data = pkt
                    if evt == Evt.UART:
                        sys.stdout.buffer.write(data)
                        sys.stdout.buffer.flush()
                        if log:
                            log.write(data)
                            log.flush()
                    elif evt == Evt.ERR:
                        msg = data.decode("utf-8", "replace")
                        print(f"\r\n[dev] error: {msg}\r\n", end="", file=sys.stderr)

            if sys.stdin in ready:
                data = os.read(sys.stdin.fileno(), 256)
                if not data:
                    break
                if stdin_tty and b"\x03" in data:
                    break
                dev.send(Cmd.UART, data)
    finally:
        if saved is not None:
            termios.tcsetattr(sys.stdin, termios.TCSADRAIN, saved)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("payload")
    ap.add_argument("-p", "--port", default="/dev/ttyACM0")
    ap.add_argument("-a", "--addr", type=lambda s: int(s, 0), default=LOAD_ADDR)
    ap.add_argument("-e", "--entry", type=lambda s: int(s, 0), default=None)
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--no-forward", action="store_true")
    ap.add_argument("--log")
    args = ap.parse_args()

    with open(args.payload, "rb") as f:
        payload = f.read()

    entry = args.addr if args.entry is None else args.entry
    records = boot_script(payload, args.addr, entry)
    total = sum(len(r) for r in records)

    dev = Device(args.port)
    try:
        dev.ping()
        dev.reset()
        time.sleep(CARRIER_START)
        dev.carrier(1)
        time.sleep(TRANSMIT_START)

        with tqdm(total=total, unit="B", unit_scale=True, desc="pushing",
                  file=sys.stderr) as bar:
            for i, rec in enumerate(records):
                dev.blob(rec)
                bar.update(len(rec))
                if i + 1 < len(records):
                    time.sleep(RECORD_GAP)

        dev.carrier(0)

        if args.no_forward:
            dev.baud(0)
            return

        dev.baud(args.baud)
        print(f"--- payload uart @ {args.baud}, ^C to exit ---", file=sys.stderr)

        log = open(args.log, "wb") if args.log else None
        try:
            dev.ser.timeout = None
            forward(dev, log)
        finally:
            if log:
                log.close()
            dev.ser.timeout = 5.0
            dev.baud(0)
    finally:
        dev.close()


if __name__ == "__main__":
    main()
