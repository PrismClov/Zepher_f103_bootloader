#!/usr/bin/env python3
"""USB HID firmware uploader for the F103 IAP bootloader."""

from __future__ import annotations

import argparse
import binascii
import struct
import sys
import time
from pathlib import Path
from typing import Callable

import hid

VID = 0x1209
PID = 0xF103
USAGE_PAGE = 0xFF00

REPORT_SIZE = 64
REPORT_ID_IN = 0x01
REPORT_ID_OUT = 0x02
DATA_OFFSET = 10
DATA_MAX = REPORT_SIZE - DATA_OFFSET

MAGIC0 = 0xA5
MAGIC1 = 0x5A

CMD_PING = 1
CMD_BEGIN = 2
CMD_DATA = 3
CMD_END = 4
CMD_RUN = 5
CMD_ENTER_BOOTLOADER = 6

ROLE_BOOTLOADER = 1
ROLE_APPLICATION = 2

SLOT_A_BASE = 0x08008000
SLOT_B_BASE = 0x08013C00
SLOT_SIZE = 46 * 1024

STATUS_TEXT = {
    0: "OK",
    1: "invalid argument",
    2: "invalid state",
    3: "address out of range",
    4: "flash operation failed",
    5: "CRC mismatch",
    6: "invalid image",
    7: "unknown command",
}


class UploadError(RuntimeError):
    pass


def emit(message: str, callback: Callable[[str], None] | None = None) -> None:
    if callback is not None:
        callback(message)
    else:
        print(message, flush=True)


def find_iap_device(timeout: float,
                    log_cb: Callable[[str], None] | None = None
                    ) -> tuple[hid.device, dict]:
    deadline = time.monotonic() + timeout
    announced = False

    while time.monotonic() < deadline:
        matches = []
        for item in hid.enumerate(VID, PID):
            if item.get("usage_page", 0) == USAGE_PAGE:
                matches.append(item)

        if matches:
            info = matches[0]
            dev = hid.device()
            dev.open_path(info["path"])
            dev.set_nonblocking(0)
            return dev, info

        if not announced:
            emit("正在等待 F103 USB 设备...", log_cb)
            announced = True
        time.sleep(0.1)

    raise UploadError("F103 USB IAP device not found.")


def make_packet(command: int, sequence: int, address: int,
                payload: bytes = b"") -> bytes:
    if len(payload) > DATA_MAX:
        raise ValueError("payload too large")

    packet = bytearray(REPORT_SIZE)
    packet[0] = REPORT_ID_OUT
    packet[1] = MAGIC0
    packet[2] = MAGIC1
    packet[3] = command
    packet[4] = sequence
    packet[5:9] = struct.pack("<I", address)
    packet[9] = len(payload)
    packet[DATA_OFFSET:DATA_OFFSET + len(payload)] = payload
    return bytes(packet)


def normalize_response(data: list[int] | bytes) -> bytes:
    response = bytes(data)
    if len(response) == REPORT_SIZE - 1:
        response = bytes([REPORT_ID_IN]) + response
    if len(response) < 9:
        raise UploadError(f"short HID response ({len(response)} bytes)")
    if response[0] != REPORT_ID_IN:
        # Some hidapi backends consume the report ID for reads.
        response = bytes([REPORT_ID_IN]) + response
    return response


def transact(dev: hid.device, command: int, sequence: int,
             address: int = 0, payload: bytes = b"",
             timeout_ms: int = 2000) -> tuple[int, int, bytes]:
    written = dev.write(make_packet(command, sequence, address, payload))
    if written != REPORT_SIZE:
        raise UploadError(f"HID write returned {written}, expected {REPORT_SIZE}")

    response = normalize_response(dev.read(REPORT_SIZE, timeout_ms))
    status = response[1]
    echoed_command = response[2]
    echoed_sequence = response[3]
    value = struct.unpack_from("<I", response, 4)[0]

    if echoed_command != command or echoed_sequence != sequence:
        raise UploadError(
            f"response mismatch: command {echoed_command}, "
            f"sequence {echoed_sequence}"
        )

    return status, value, response[8:]


def check_status(status: int, operation: str) -> None:
    if status != 0:
        text = STATUS_TEXT.get(status, f"status {status}")
        raise UploadError(f"{operation} failed: {text}")


def ping(dev: hid.device) -> tuple[int, bytes]:
    status, value, extra = transact(dev, CMD_PING, 0xFE)
    check_status(status, "PING")
    return value, extra


def device_role(extra: bytes) -> int:
    return extra[12] if len(extra) > 12 else 0


def request_bootloader(dev: hid.device, info: dict, wait: float,
                       log_cb: Callable[[str], None] | None = None
                       ) -> tuple[hid.device, dict]:
    status, _value, extra = transact(dev, CMD_ENTER_BOOTLOADER, 0xFD)
    check_status(status, "enter bootloader")
    if device_role(extra) != ROLE_APPLICATION:
        raise UploadError("runtime application returned an invalid role")

    product = info.get("product_string") or "F103 USB Application"
    emit(f"正在请求 {product} 进入 Bootloader", log_cb)
    dev.close()

    # The application waits 50 ms before resetting, then the bootloader must
    # enumerate and answer PING before this function returns.
    time.sleep(0.25)
    deadline = time.monotonic() + wait
    while time.monotonic() < deadline:
        candidates = [
            item for item in hid.enumerate(VID, PID)
            if item.get("usage_page", 0) == USAGE_PAGE
        ]
        if candidates:
            candidate_info = candidates[0]
            candidate = hid.device()
            try:
                candidate.open_path(candidate_info["path"])
                candidate.set_nonblocking(0)
                _version, candidate_extra = ping(candidate)
                role = device_role(candidate_extra)
                if role in (0, ROLE_BOOTLOADER):
                    return candidate, candidate_info
            except (UploadError, OSError):
                pass
            candidate.close()
        time.sleep(0.1)

    raise UploadError("application reset, but the bootloader was not found")


def validate_image(data: bytes) -> None:
    """Validate a firmware image single-linked for slot A (the boot slot).

    The bootloader always downloads into slot B and swaps the image into
    slot A before the test boot, so the reset vector must target slot A.
    """
    if not data:
        raise UploadError("firmware file is empty")
    if len(data) > SLOT_SIZE:
        raise UploadError(
            f"firmware is {len(data)} bytes; slot maximum is {SLOT_SIZE} bytes"
        )
    if len(data) < 8:
        raise UploadError("firmware is too small to contain a vector table")

    initial_sp, reset_handler = struct.unpack_from("<II", data, 0)
    if not (0x20000000 <= initial_sp <= 0x20005000) or initial_sp & 3:
        raise UploadError(f"invalid initial stack pointer 0x{initial_sp:08X}")
    if not (reset_handler & 1):
        raise UploadError("reset vector is not a Thumb address")

    reset_address = reset_handler & ~1
    if not SLOT_A_BASE <= reset_address < SLOT_A_BASE + len(data):
        raise UploadError(
            f"reset vector 0x{reset_handler:08X} is not linked for slot A "
            f"(0x{SLOT_A_BASE:08X}); the bootloader swaps images into slot A"
        )


def upload(dev: hid.device, firmware: bytes, run: bool,
           log_cb: Callable[[str], None] | None = None,
           progress_cb: Callable[[int], None] | None = None) -> None:
    sequence = 0

    def command(command_id: int, address: int = 0,
                payload: bytes = b"") -> tuple[int, bytes]:
        nonlocal sequence
        sequence = (sequence + 1) & 0xFF
        status, value, extra = transact(dev, command_id, sequence, address,
                                        payload)
        check_status(status, f"command {command_id}")
        return value, extra

    version, info = command(CMD_PING)
    if len(info) < 16:
        raise UploadError("invalid PING response")

    active_base, target_base, device_size = struct.unpack_from("<III", info, 0)
    role = device_role(info)
    target_slot = info[14]

    if role != ROLE_BOOTLOADER or target_slot != 1:
        raise UploadError("device is not an A/B-capable bootloader")
    if device_size != SLOT_SIZE:
        raise UploadError(
            f"slot size mismatch: device {device_size}, host {SLOT_SIZE}"
        )
    if target_base != SLOT_B_BASE:
        raise UploadError(
            f"target slot base mismatch: 0x{target_base:08X}, "
            f"expected 0x{SLOT_B_BASE:08X}"
        )

    if len(info) >= 20 and info[16]:
        emit(
            f"设备存在待确认测试镜像：已试启动 {info[17]}/{info[18]}，"
            f"本次升级将覆盖下载槽 B",
            log_cb,
        )

    validate_image(firmware)
    crc = binascii.crc32(firmware) & 0xFFFFFFFF
    emit(
        f"Bootloader {version >> 16}.{version & 0xFFFF} | "
        f"下载槽 B @ 0x{target_base:08X}（运行槽 A 保持可启动） | "
        f"镜像 {len(firmware)} B | CRC32 0x{crc:08X}",
        log_cb,
    )

    command(CMD_BEGIN, len(firmware))

    sent = 0
    started = time.monotonic()
    last_percent = -1
    while sent < len(firmware):
        chunk = firmware[sent:sent + DATA_MAX]
        command(CMD_DATA, sent, chunk)
        sent += len(chunk)

        percent = sent * 100 // len(firmware)
        if percent != last_percent:
            if progress_cb is not None:
                progress_cb(percent)
            else:
                print(f"\rWriting: {percent:3d}%", end="", flush=True)
            last_percent = percent

    if progress_cb is None:
        print()
    if progress_cb is not None:
        progress_cb(100)
    actual_crc, _ = command(CMD_END, crc)
    if actual_crc != crc:
        raise UploadError(
            f"device CRC 0x{actual_crc:08X} does not match 0x{crc:08X}"
        )

    elapsed = max(time.monotonic() - started, 0.001)
    emit(f"Flash 校验通过，耗时 {elapsed:.2f}s", log_cb)
    emit("新镜像已写入 B 区并提交 pending（含 A/B 扇区交换请求）", log_cb)
    emit("复位后 Bootloader 校验候选镜像，按扇区交换到 A 区并以 TEST 模式启动", log_cb)
    emit("应用自检通过并确认后生效；连续 3 次启动未确认将交换回滚", log_cb)

    if run:
        command(CMD_RUN)
        emit("应用启动命令已发送（扇区交换约需数秒，请稍候）", log_cb)


def perform_update(firmware: bytes, wait: float = 20.0,
                   run: bool = True,
                   log_cb: Callable[[str], None] | None = None,
                   progress_cb: Callable[[int], None] | None = None) -> None:
    """Connect, switch from application to bootloader, then update."""
    dev = None
    try:
        dev, info = find_iap_device(wait, log_cb)
        product = info.get("product_string") or "F103 USB IAP device"
        emit(f"已连接 {product}", log_cb)

        _version, probe_extra = ping(dev)
        role = device_role(probe_extra)
        if role == ROLE_APPLICATION:
            dev, info = request_bootloader(dev, info, wait, log_cb)
        elif role not in (0, ROLE_BOOTLOADER):
            raise UploadError(f"unsupported device role {role}")

        upload(dev, firmware, run=run, log_cb=log_cb,
               progress_cb=progress_cb)
    finally:
        if dev is not None:
            dev.close()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Upload a firmware image to the F103 USB HID bootloader"
    )
    parser.add_argument(
        "firmware", type=Path,
        help="firmware single-linked for slot A (.bin); the bootloader "
             "downloads it into slot B and swaps it into slot A"
    )
    parser.add_argument(
        "--wait",
        type=float,
        default=20.0,
        help="seconds to wait for the bootloader (default: 20)",
    )
    parser.add_argument(
        "--no-run",
        action="store_true",
        help="stay in the bootloader after a successful update",
    )
    args = parser.parse_args()

    try:
        firmware = args.firmware.read_bytes()
        validate_image(firmware)
    except (OSError, UploadError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    try:
        dev, info = find_iap_device(args.wait)
        product = info.get("product_string") or "F103 USB IAP device"
        print(f"Connected to {product}")

        _version, probe_extra = ping(dev)
        role = device_role(probe_extra)
        if role == ROLE_APPLICATION:
            dev, info = request_bootloader(dev, info, args.wait)
        elif role not in (0, ROLE_BOOTLOADER):
            raise UploadError(f"unsupported device role {role}")

        upload(dev, firmware, run=not args.no_run)
        dev.close()
    except (UploadError, OSError) as exc:
        print(f"\nerror: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

