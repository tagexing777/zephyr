# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Verify upper-board EEPROM through the first ELF and TRACE32's local API."""

import argparse
import ctypes
import json
import os
import time
from pathlib import Path

import serial

READ_FIELDS = (
    "state",
    "error",
    "transactions",
    "bytes",
    "block_matches",
    "offset_matches",
    "nacks",
    "timeouts",
    "arbitration_losses",
    "bus_hz",
    "last_status",
    "checksum",
)
WRITE_FIELDS = (
    "state",
    "error",
    "restore_error",
    "page_matches",
    "byte_matches",
    "write_cycles",
    "restore_attempts",
    "restored",
    "final_hash",
    "write_bytes",
)
WRITE_ARM = 0x45575254


class Debugger:
    def __init__(self, dll, port):
        self.api = ctypes.CDLL(str(dll))
        for key, value in [
            (b"NODE=", b"localhost"),
            (b"PORT=", str(port).encode()),
            (b"PACKLEN=", b"1024"),
        ]:
            if self.api.T32_Config(key, value) != 0:
                raise RuntimeError("TRACE32 configuration failed")
        if self.api.T32_Init() != 0:
            raise RuntimeError("TRACE32 connection failed")
        if self.api.T32_Attach(1) != 0:
            self.api.T32_Exit()
            raise RuntimeError("TRACE32 attach failed")

    def close(self):
        self.api.T32_Exit()

    def command(self, text):
        self.api.T32_Cmd(b"PRINT")
        rc = self.api.T32_Cmd(text.encode("ascii"))
        self.check_message()
        if rc != 0:
            raise RuntimeError(text)

    def check_message(self):
        message = ctypes.create_string_buffer(4096)
        kind = ctypes.c_uint16()
        self.api.T32_GetMessage(message, ctypes.byref(kind))
        if kind.value & 0x12:
            raise RuntimeError(message.value.decode("latin1"))

    def evaluate(self, expression):
        self.command(f"EVAL {expression}")
        value = ctypes.c_uint32()
        if self.api.T32_EvalGet(ctypes.byref(value)) != 0:
            raise RuntimeError(expression)
        return value.value

    def address(self, symbol):
        return self.evaluate(f"ADDRESS.OFFSET({symbol})")

    def read_result(self, symbol, fields):
        base = self.address(symbol)
        return {
            name: self.evaluate(f"Data.Long(D:0x{base + i * 4:x})") for i, name in enumerate(fields)
        }

    def read_bytes(self, address, length):
        return bytes(self.evaluate(f"Data.Byte(D:0x{address + i:x})") for i in range(length))

    def load(self, loader, elf):
        self.command(f'DO "{loader.as_posix()}" "{elf.as_posix()}" "WARM"')
        time.sleep(0.25)
        deadline = time.monotonic() + 20
        state = ctypes.c_int()
        while True:
            if self.api.T32_GetPracticeState(ctypes.byref(state)) != 0:
                raise RuntimeError("Cannot read PRACTICE state")
            if state.value == 0:
                break
            if time.monotonic() > deadline:
                raise RuntimeError("Warm loader timed out")
            time.sleep(0.05)
        self.check_message()
        if self.evaluate("STATE.RUN()") != 0:
            raise RuntimeError("Expected stopped target at main")
        if self.evaluate("Register(PC)") != self.address("main"):
            raise RuntimeError("Target did not reach main")


def hash_bytes(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def capture(uart, lines, pending):
    pending.extend(uart.read(uart.in_waiting or 1))
    while b"\n" in pending:
        raw, _, remainder = pending.partition(b"\n")
        pending[:] = remainder
        line = raw.decode("ascii", errors="replace").strip()
        if "I2C2 U65" in line:
            lines.append(line)
            print(line, flush=True)


def verify(args):
    debug = Debugger(args.api_dll.resolve(), args.api_port)
    breakpoint_set = False
    armed = False
    try:
        with serial.Serial(args.uart, 115200, timeout=0.02) as uart:
            debug.load(args.loader.resolve(), args.elf.resolve())
            debug.command("Break.Set sample_eeprom_write_test /Onchip")
            breakpoint_set = True
            uart.reset_input_buffer()
            debug.command("Go")
            lines = []
            pending = bytearray()
            deadline = time.monotonic() + 10
            while debug.evaluate("STATE.RUN()") != 0:
                capture(uart, lines, pending)
                if time.monotonic() > deadline:
                    raise RuntimeError("Read-only test did not reach write-test entry")
            if debug.evaluate("Register(PC)") != debug.address("sample_eeprom_write_test"):
                raise RuntimeError("Unexpected target stop before write-test entry")
            read_result = debug.read_result("sample_eeprom_result", READ_FIELDS)
            if read_result["state"] != 2:
                raise RuntimeError(f"Read-only prerequisite failed: {read_result}")
            baseline_address = debug.address("sample_eeprom_result") + len(READ_FIELDS) * 4
            baseline = debug.read_bytes(baseline_address, 128)
            if hash_bytes(baseline) != read_result["checksum"]:
                raise RuntimeError("Baseline checksum mismatch")
            if args.compare_backup is not None:
                with args.compare_backup.open(encoding="ascii") as stream:
                    expected = json.load(stream)
                if baseline != bytes.fromhex(expected["baseline_hex"]):
                    raise RuntimeError("Fresh EEPROM read differs from the saved host backup")
            if args.write:
                backup = {
                    "device": "upper-board U65 AT24C01B/C",
                    "i2c": 2,
                    "address": 0x51,
                    "range": [0x78, 0x7F],
                    "baseline_hex": baseline.hex(),
                    "fnv1a": f"{hash_bytes(baseline):08x}",
                }
                # Exclusive creation prevents overwriting an earlier recovery image.
                with args.backup.open("x", encoding="ascii") as stream:
                    json.dump(backup, stream, indent=2)
                    stream.write("\n")
                    stream.flush()
                    os.fsync(stream.fileno())
                print(f"Complete 128-byte backup saved: {args.backup.resolve()}", flush=True)
                arm = debug.address("sample_eeprom_write_arm")
                debug.command(f"Data.Set D:0x{arm:x} %Long 0x{WRITE_ARM:x}")
                armed = True
            debug.command("Break.Delete sample_eeprom_write_test")
            breakpoint_set = False
            debug.command("Go")
            deadline = time.monotonic() + 10
            marker = " RW PASS " if args.write else " PASS "
            while time.monotonic() < deadline:
                capture(uart, lines, pending)
                if any(marker in line or " FAIL " in line for line in lines):
                    break
            read_result = debug.read_result("sample_eeprom_result", READ_FIELDS)
            write_result = debug.read_result("sample_eeprom_write_result", WRITE_FIELDS)
            if args.write:
                final_address = debug.address("sample_eeprom_write_result") + 40 + 128
                final = debug.read_bytes(final_address, 128)
                if final != baseline:
                    raise RuntimeError("EEPROM restore differs from the complete host backup")
                if (
                    write_result["state"] != 2
                    or write_result["restored"] != 1
                    or write_result["page_matches"] != 5
                    or write_result["byte_matches"] != 2
                    or write_result["write_cycles"] != 8
                    or write_result["write_bytes"] != 50
                    or write_result["error"] != 0
                    or write_result["restore_error"] != 0
                ):
                    raise RuntimeError(f"Write verification failed: {write_result}")
            elif write_result["state"] != 0 or write_result["write_cycles"] != 0:
                raise RuntimeError("Ordinary boot unexpectedly ran the write test")
            if read_result["state"] != 2 or any(
                read_result[name] != 0 for name in ("nacks", "timeouts", "arbitration_losses")
            ):
                raise RuntimeError(f"Bus errors: {read_result}")
            if not any(marker in line for line in lines):
                raise RuntimeError("Expected UART result was not captured")
            status = debug.evaluate("Data.Byte(D:0x401ec003)")
            control = debug.evaluate("Data.Byte(D:0x401ec002)")
            if status & 0x20 or control != 0x80 or debug.evaluate("STATE.RUN()") != 1:
                raise RuntimeError("Expected idle, disabled I2C2 and running application")
            print(
                json.dumps(
                    {"read": read_result, "write": write_result, "bus_idle": True, "running": True},
                    indent=2,
                ),
                flush=True,
            )
    finally:
        try:
            if breakpoint_set and not armed:
                debug.command("Break.Delete sample_eeprom_write_test")
        finally:
            # Never reset/reload after arming: let the target's restore path finish.
            debug.close()


def main():
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--api-dll", type=Path, required=True)
    parser.add_argument("--api-port", type=int, default=20000)
    parser.add_argument("--uart", default="COM3")
    parser.add_argument(
        "--elf", type=Path, default=root / "build/s32g_vnp_evb_periodic_tasks/zephyr/zephyr.elf"
    )
    parser.add_argument(
        "--loader",
        type=Path,
        default=root / "boards/nxp/s32g_vnp_evb/support/S32G_CM7_0_ZEPHYR_Load.cmm",
    )
    parser.add_argument("--write", action="store_true", help="test and restore only the final page")
    parser.add_argument("--backup", type=Path, help="new recovery JSON path, required with --write")
    parser.add_argument(
        "--compare-backup",
        type=Path,
        help="compare the fresh whole-device read with a saved recovery JSON",
    )
    args = parser.parse_args()
    if args.write and (args.backup is None or args.backup.exists()):
        parser.error("--write requires --backup pointing to a new file in an existing directory")
    if args.write and not args.backup.parent.is_dir():
        parser.error("Backup parent directory does not exist")
    verify(args)


if __name__ == "__main__":
    main()
