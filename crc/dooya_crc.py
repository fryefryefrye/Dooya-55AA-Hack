#!/usr/bin/env python3
"""Dooya 55AA TTL protocol - CRC-16 reference implementation and self test.

Parameters, reverse engineered from captures of the official DC1545W adapter:

    width  16
    poly   0x8005
    init   0xFFFF
    refin  false     input bytes are NOT reflected (MSB first)
    refout false     register is NOT reflected
    xorout 0x0000
    check  0xAEE7    CRC of b"123456789"

That is the parameter set known as CRC-16/CMS.

It is NOT CRC-16/MODBUS: same polynomial, but Modbus reflects both the input
bytes and the register, and a Modbus RTU frame puts the CRC low byte first.
The 55AA frame puts it HIGH byte first.

Covered bytes: from 0x55 up to and including the last data byte.

Run this file directly to check every vector in TEST_VECTORS:

    python3 dooya_crc.py
"""

POLY = 0x8005
INIT = 0xFFFF

# (meaning, frame without CRC as bytes, expected whole frame as hex string)
TEST_VECTORS = [
    # Captured from the official adapter talking to the motor
    ("query position", bytes([0x55, 0xAA, 0x04, 0x02, 0x02]),
     "55 AA 04 02 02 38 74"),
    ("open", bytes([0x55, 0xAA, 0x05, 0x01, 0x01, 0x01]),
     "55 AA 05 01 01 01 EA A9"),
    ("close", bytes([0x55, 0xAA, 0x05, 0x01, 0x01, 0x02]),
     "55 AA 05 01 01 02 EA A3"),
    ("stop", bytes([0x55, 0xAA, 0x05, 0x01, 0x01, 0x03]),
     "55 AA 05 01 01 03 6A A6"),
    ("set position 50%", bytes([0x55, 0xAA, 0x06, 0x01, 0x02, 0x32, 0x67]),
     "55 AA 06 01 02 32 67 00 9A"),
    ("reply: position 50%", bytes([0x55, 0xAA, 0x05, 0x02, 0x02, 0x32]),
     "55 AA 05 02 02 32 E0 3F"),
    ("report: closing, 0%",
     bytes([0x55, 0xAA, 0x0C, 0x03, 0x01, 0x02, 0x00, 0x0B, 0x01, 0x00, 0x02, 0x00, 0x00]),
     "55 AA 0C 03 01 02 00 0B 01 00 02 00 00 DB 3B"),
    ("report: stopping, 50%",
     bytes([0x55, 0xAA, 0x0C, 0x03, 0x01, 0x03, 0x32, 0xB4, 0x01, 0x00, 0x02, 0x00, 0x00]),
     "55 AA 0C 03 01 03 32 B4 01 00 02 00 00 D2 A9"),
    ("report: trimming, 50%",
     bytes([0x55, 0xAA, 0x0C, 0x03, 0x01, 0x01, 0x32, 0xA9, 0x01, 0x00, 0x02, 0x00, 0x00]),
     "55 AA 0C 03 01 01 32 A9 01 00 02 00 00 2A BB"),
    ("report: stopped, 50%",
     bytes([0x55, 0xAA, 0x0C, 0x03, 0x01, 0x03, 0x32, 0x64, 0x01, 0x00, 0x02, 0x00, 0x00]),
     "55 AA 0C 03 01 03 32 64 01 00 02 00 00 52 77"),
    # Not captured - computed, listed so others can cross-check their port
    ("set position 0%", bytes([0x55, 0xAA, 0x06, 0x01, 0x02, 0x00, 0x67]),
     "55 AA 06 01 02 00 67 2C 99"),
    ("set position 1%", bytes([0x55, 0xAA, 0x06, 0x01, 0x02, 0x01, 0x67]),
     "55 AA 06 01 02 01 67 AA 9A"),
    ("set position 100%", bytes([0x55, 0xAA, 0x06, 0x01, 0x02, 0x64, 0x67]),
     "55 AA 06 01 02 64 67 74 9F"),
]


def crc16(data):
    """CRC-16/CMS: poly 0x8005, init 0xFFFF, no reflection, no final xor."""
    crc = INIT
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ POLY) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def build_frame(body):
    """Append the CRC (high byte first) to a frame body."""
    crc = crc16(body)
    return bytes(body) + bytes([crc >> 8, crc & 0xFF])


def parse_frame(frame):
    """Return (body, crc_ok) for a whole received frame, or None if too short."""
    if len(frame) < 6:
        return None
    body = frame[:-2]
    return body, crc16(body) == ((frame[-2] << 8) | frame[-1])


def format_hex(frame):
    return " ".join("%02X" % b for b in frame)


def main():
    failures = 0

    check = crc16(b"123456789")
    print("check value for '123456789' = 0x%04X (CRC-16/CMS expects 0xAEE7) %s"
          % (check, "OK" if check == 0xAEE7 else "FAIL"))
    failures += check != 0xAEE7
    print()

    for meaning, body, expected in TEST_VECTORS:
        got = format_hex(build_frame(body))
        ok = got == expected
        failures += not ok
        print("%-4s %-24s %s" % ("OK" if ok else "FAIL", meaning, got))
        if not ok:
            print("     expected                 %s" % expected)

    print()
    if failures:
        print("%d vector(s) FAILED" % failures)
        return 1

    print("all %d vectors passed" % (len(TEST_VECTORS) + 1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
