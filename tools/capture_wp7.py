#!/usr/bin/env python3
"""Capture the live Passport WP7 display over its USB serial port."""

import argparse
import binascii
from pathlib import Path
import struct
import time
import zlib

import serial


MAGIC = b"WP7_SCREENSHOT_V1 240 320 RGB565LE RECT"
WIDTH, HEIGHT = 240, 320


def read_exact(port, count, deadline):
    data = bytearray()
    while len(data) < count:
        if time.monotonic() > deadline:
            raise TimeoutError(f"screenshot ended after {len(data)} of {count} bytes")
        data.extend(port.read(count - len(data)))
    return bytes(data)


def chunk(kind, data):
    payload = kind + data
    return (struct.pack(">I", len(data)) + payload +
            struct.pack(">I", binascii.crc32(payload) & 0xFFFFFFFF))


def save_png(path, rgb):
    rows = b"".join(b"\x00" + rgb[y * WIDTH * 3:(y + 1) * WIDTH * 3]
                    for y in range(HEIGHT))
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def capture(port_name, output, timeout, wait_for_enter=False):
    rgb = bytearray(WIDTH * HEIGHT * 3)
    covered = bytearray(WIDTH * HEIGHT)
    rectangles = 0

    with serial.Serial(port_name, 115200, timeout=0.2, write_timeout=2) as port:
        # Opening USB Serial/JTAG may reset the ESP32-C3; let its app and
        # receive task start before sending the one-shot command.
        time.sleep(1.2)
        if wait_for_enter:
            print("Serial ready. Leave Passport on the desired page, then press Enter.",
                  flush=True)
            input()
        deadline = time.monotonic() + timeout
        port.reset_input_buffer()
        port.write(b"WP7_SCREENSHOT_V1\n")
        port.flush()
        while True:
            if time.monotonic() > deadline:
                raise TimeoutError("Passport did not answer the screenshot command")
            line = port.readline().strip()
            if line == MAGIC:
                break

        while True:
            tag = read_exact(port, 4, deadline)
            if tag == b"DONE":
                break
            if tag != b"RECT":
                raise ValueError(f"unexpected screenshot record: {tag!r}")
            x, y, width, height = struct.unpack("<HHHH", read_exact(port, 8, deadline))
            if not width or not height or x + width > WIDTH or y + height > HEIGHT:
                raise ValueError(f"invalid rectangle {(x, y, width, height)}")
            pixels = read_exact(port, width * height * 2, deadline)
            for row in range(height):
                for col in range(width):
                    index = (row * width + col) * 2
                    color = pixels[index] | pixels[index + 1] << 8
                    pixel = (y + row) * WIDTH + x + col
                    dest = pixel * 3
                    rgb[dest] = ((color >> 11) & 31) * 255 // 31
                    rgb[dest + 1] = ((color >> 5) & 63) * 255 // 63
                    rgb[dest + 2] = (color & 31) * 255 // 31
                    covered[pixel] = 1
            rectangles += 1

    if not all(covered):
        raise ValueError(f"incomplete frame: {sum(covered)} of {len(covered)} pixels")
    save_png(output, rgb)
    print(f"Captured {WIDTH}x{HEIGHT} from {rectangles} flush rectangles: {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=20)
    parser.add_argument("--wait-for-enter", action="store_true")
    args = parser.parse_args()
    capture(args.port, args.output, args.timeout, args.wait_for_enter)


if __name__ == "__main__":
    main()
