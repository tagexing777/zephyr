# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Verify CAN0 internal loopback in the first ELF through TRACE32 and UART."""

import argparse
import json
import time
from pathlib import Path

import serial
from verify_eeprom import Debugger

FIELDS = (
    "state",
    "error",
    "stage",
    "transmitted",
    "received",
    "standard",
    "extended",
    "bytes",
    "mismatches",
    "timeouts",
    "clock_hz",
    "bitrate",
    "running_mcr",
    "running_ctrl1",
    "error_status",
    "error_count",
    "cleanup_error",
    "final_mcr",
    "memory_errors",
    "dlc_seen",
)


def verify(args):
    debug = Debugger(args.api_dll.resolve(), args.api_port)
    try:
        with serial.Serial(args.uart, 115200, timeout=0.02) as uart:
            for run in range(1, args.runs + 1):
                debug.load(
                    args.loader.resolve(), args.elf.resolve(), warm=not (args.cold and run == 1)
                )
                uart.reset_input_buffer()
                debug.command("Go")
                pending = bytearray()
                lines = []
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    pending.extend(uart.read(uart.in_waiting or 1))
                    while b"\n" in pending:
                        raw, _, remainder = pending.partition(b"\n")
                        pending[:] = remainder
                        line = raw.decode("ascii", errors="replace").strip()
                        if "CAN0 loopback" in line:
                            lines.append(line)
                            print(f"run {run}: {line}", flush=True)
                result = debug.read_result("sample_can_result", FIELDS)
                print(json.dumps({"run": run, **result}, sort_keys=True), flush=True)
                expected = {
                    "state": 2,
                    "error": 0,
                    "transmitted": 1000,
                    "received": 1000,
                    "standard": 500,
                    "extended": 500,
                    "bytes": 3996,
                    "mismatches": 0,
                    "timeouts": 0,
                    "clock_hz": 40000000,
                    "bitrate": 500000,
                    "error_status": 0,
                    "error_count": 0,
                    "cleanup_error": 0,
                    "memory_errors": 0,
                    "dlc_seen": 0x1FF,
                }
                for name, value in expected.items():
                    if result[name] != value:
                        raise RuntimeError(f"CAN0 {name}: expected {value}, got {result[name]}")
                if result["running_ctrl1"] != 0x03731007:
                    raise RuntimeError("Unexpected loopback bit timing")
                stopped_or_other_mode = (
                    (1 << 31)
                    | (1 << 30)
                    | (1 << 29)
                    | (1 << 28)
                    | (1 << 27)
                    | (1 << 24)
                    | (1 << 17)
                    | (1 << 11)
                )
                if result["running_mcr"] & stopped_or_other_mode:
                    raise RuntimeError("CAN0 was not in classical self-receiving mailbox mode")
                if result["final_mcr"] & ((1 << 31) | (1 << 20)) != (1 << 31) | (1 << 20):
                    raise RuntimeError("CAN0 was not disabled after the test")
                uart_pass = (
                    "CAN0 loopback PASS: tx=1000 rx=1000 std=500 ext=500 bytes=3996 "
                    "mismatch=0 timeout=0"
                )
                if not any(uart_pass in line for line in lines):
                    raise RuntimeError("Missing UART PASS result")
                if not any("cleanup=0 disabled=1" in line for line in lines):
                    raise RuntimeError("Missing UART cleanup result")
                if debug.evaluate("STATE.RUN()") != 1:
                    raise RuntimeError("Target stopped unexpectedly")
        mode = "cold debugger load followed by warm reloads" if args.cold else "warm reloads"
        print(f"PASS: {args.runs} runs ({mode}), {args.runs * 1000} matched CAN0 frames")
    finally:
        debug.close()


def main():
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--api-dll", type=Path, required=True)
    parser.add_argument("--api-port", type=int, default=20000)
    parser.add_argument("--uart", default="COM3")
    parser.add_argument("--runs", type=int, choices=range(1, 11), default=3)
    parser.add_argument(
        "--cold",
        action="store_true",
        help="Use the cold connection/SRAM initialization path for the first run",
    )
    parser.add_argument(
        "--loader",
        type=Path,
        default=root / "boards/nxp/s32g_vnp_evb/support/S32G_CM7_0_ZEPHYR_Load.cmm",
    )
    parser.add_argument(
        "--elf", type=Path, default=root / "build/s32g_vnp_evb_periodic_tasks/zephyr/zephyr.elf"
    )
    verify(parser.parse_args())


if __name__ == "__main__":
    main()
