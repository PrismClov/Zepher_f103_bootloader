#!/usr/bin/env python3
"""Negative/robustness tests for the F103 HID IAP protocol.

Runs against a device in bootloader mode and checks that malformed or
out-of-order requests are rejected without disturbing the installed
firmware. No flash write is attempted for a successful upgrade.

SPDX-License-Identifier: Apache-2.0
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "updater"))

import hid

from f103_uploader import (
    CMD_BEGIN,
    CMD_DATA,
    CMD_END,
    CMD_PING,
    DATA_MAX,
    REPORT_SIZE,
    ROLE_APPLICATION,
    ROLE_BOOTLOADER,
    SLOT_SIZE,
    STATUS_TEXT,
    device_role,
    find_iap_device,
    make_packet,
    normalize_response,
    ping,
    request_bootloader,
)

RESULTS: list[tuple[str, bool, str]] = []


def record(name: str, ok: bool, detail: str = "") -> None:
    RESULTS.append((name, ok, detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" - {detail}" if detail else ""))


def transact_raw(dev: hid.device, packet: bytes, timeout_ms: int = 400):
    """Send a raw packet and return the raw response (or b'' on timeout)."""
    dev.write(packet)
    try:
        return normalize_response(dev.read(REPORT_SIZE, timeout_ms))
    except Exception:
        return b""


def expect_status(dev: hid.device, name: str, command: int, address: int,
                  payload: bytes, expected: int, seq: int = 0x40) -> None:
    packet = make_packet(command, seq, address, payload)
    response = transact_raw(dev, packet)
    if not response:
        record(name, False, "no response")
        return
    status = response[1]
    record(name, status == expected,
           f"status {status} ({STATUS_TEXT.get(status, '?')}), "
           f"expected {expected}")


def main() -> int:
    dev, info = find_iap_device(15.0)
    product = info.get("product_string") or ""
    print(f"connected: {product}")

    _version, extra = ping(dev)
    if device_role(extra) == ROLE_APPLICATION:
        dev, info = request_bootloader(dev, info, 15.0)
        _version, extra = ping(dev)
    if device_role(extra) != ROLE_BOOTLOADER:
        print("device is not in bootloader mode")
        return 2

    dev.write(make_packet(CMD_PING, 0x55, 0))
    rsp = normalize_response(dev.read(REPORT_SIZE, 1000))
    record("PING echoes sequence", rsp[2] == CMD_PING and rsp[3] == 0x55,
           f"cmd={rsp[2]} seq={rsp[3]}")

    bad = bytearray(make_packet(CMD_PING, 0x56, 0))
    bad[1] = 0x00
    record("bad magic ignored", transact_raw(dev, bytes(bad)) == b"")

    # Note: the HID interrupt OUT endpoint is fixed at 64 bytes, so hidapi
    # pads short writes; report truncation cannot occur on the wire.

    expect_status(dev, "unknown command rejected", 0x7F, 0, b"", 7)
    expect_status(dev, "BEGIN size 0 rejected", CMD_BEGIN, 0, b"", 1)
    expect_status(dev, "BEGIN oversized rejected", CMD_BEGIN,
                  SLOT_SIZE + 1, b"", 1)
    expect_status(dev, "DATA before BEGIN rejected", CMD_DATA, 0,
                  bytes(DATA_MAX), 3)
    expect_status(dev, "END before BEGIN rejected", CMD_END, 0, b"", 2)

    packet = bytearray(make_packet(CMD_DATA, 0x58, 0, bytes(4)))
    packet[9] = DATA_MAX + 1
    rsp = transact_raw(dev, bytes(packet))
    record("oversized length rejected", bool(rsp) and rsp[1] == 1,
           f"status {rsp[1] if rsp else 'none'}")

    _version, extra = ping(dev)
    record("device alive after negative tests",
           device_role(extra) == ROLE_BOOTLOADER)
    valid = extra[15] if len(extra) > 15 else 0
    record("installed image still valid", valid == 1, f"valid={valid}")

    dev.close()

    failed = [r for r in RESULTS if not r[1]]
    print(f"\n{len(RESULTS) - len(failed)}/{len(RESULTS)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
