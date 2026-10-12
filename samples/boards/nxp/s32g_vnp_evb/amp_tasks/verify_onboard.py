# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Read running AMP counters and peripheral evidence without stopping or loading any core."""

import argparse
import importlib
import json
import math
import re
import socket
import sys
import threading
import time
from datetime import UTC, datetime
from pathlib import Path

import serial
import verify_images
from elftools.elf.elffile import ELFFile

COUNTER_FIELDS = (
    "task_1ms",
    "task_10ms",
    "task_100ms",
    "event_5ms",
    "event_10ms",
    "event_20ms",
    "missed_1ms",
    "missed_10ms",
    "missed_100ms",
    "led_toggles",
    "gpio_errors",
)
CAN_FIELDS = (
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
EEPROM_READ_FIELDS = (
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
EEPROM_WRITE_FIELDS = (
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
PRIMARY_GLOBALS = (
    "sample_amp_primary_ready",
    "sample_core_clock_hz",
    "sample_clock_error",
    "sample_clock_source",
    "sample_eeprom_write_arm",
    "sample_network_error",
    "ethernet_echo_count",
    "ethernet_echo_bytes",
    "udp_log_task_count",
    "udp_log_task_missed",
    "udp_log_task_errors",
)
UART_PATTERN = re.compile(r"^(task_(?:10|100)ms) count=(\d+)")
UDP_PATTERN = re.compile(r"^task_udp_log_10ms count=(\d+) uptime_ms=(\d+) missed=(\d+)")
MASK32 = 0xFFFFFFFF


def symbol_objects(path):
    """Read global data addresses and sizes from the matching primary ELF."""
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        table = elf.get_section_by_name(".symtab")
        if table is None:
            raise ValueError(f"{path}: missing symbol table")
        return {
            item.name: (item["st_value"], item["st_size"])
            for item in table.iter_symbols()
            if item["st_info"]["type"] == "STT_OBJECT" and item["st_info"]["bind"] == "STB_GLOBAL"
        }


def primary_layout(path):
    """Require current 32-bit result layouts before constructing numeric read addresses."""
    objects = symbol_objects(path)
    layout = {}
    for name in PRIMARY_GLOBALS:
        if name not in objects or objects[name][1] != 4:
            raise ValueError(f"{path}: missing four-byte global {name}")
        layout[name] = objects[name][0]
    groups = (
        ("sample_counters", "counters", COUNTER_FIELDS, 44),
        ("sample_can_result", "can", CAN_FIELDS, 80),
        ("sample_eeprom_result", "eeprom_read", EEPROM_READ_FIELDS, 176),
        ("sample_eeprom_write_result", "eeprom_write", EEPROM_WRITE_FIELDS, 296),
    )
    for name, prefix, fields, size in groups:
        if name not in objects or objects[name][1] != size:
            raise ValueError(f"{path}: incompatible {name} layout, expected {size} bytes")
        base = objects[name][0]
        layout.update(
            {f"{prefix}.{field}": base + offset * 4 for offset, field in enumerate(fields)}
        )
    return layout


def snapshot(debug, layouts):
    """Sample each field through EAXI; record its actual read midpoint rather than atomicity."""
    result = {"cores": {}, "hardware": {}}
    for core, layout in enumerate(layouts):
        values = {}
        times = {}
        began = time.monotonic()
        for name, address in layout.items():
            before = time.monotonic()
            values[name] = debug.evaluate(f"Data.Long(EAXI:0x{address:08x})")
            times[name] = (before + time.monotonic()) / 2
        result["cores"][str(core)] = {
            "values": values,
            "times": times,
            "started": began,
            "finished": time.monotonic(),
        }
    for core, address in enumerate((0x40088148, 0x40088168, 0x40088188)):
        result["hardware"][f"core_{core}_stat"] = debug.evaluate(f"Data.Long(EAXI:0x{address:x})")
    result["hardware"]["secondary_reset_status"] = debug.evaluate("Data.Long(EAXI:0x40078140)")
    result["hardware"]["primary_running"] = debug.evaluate("STATE.RUN()")
    return result


def collect_uart(args, stop, result):
    """Read COM3 without flushing its buffers or transmitting any byte."""
    result.update({"lines": 0, "tasks": {}, "examples": [], "errors": []})
    try:
        with serial.Serial(args.uart, 115200, timeout=0.05) as uart:
            pending = bytearray()
            while not stop.is_set():
                pending.extend(uart.read(uart.in_waiting or 1))
                while b"\n" in pending:
                    raw, _, pending = pending.partition(b"\n")
                    line = raw.decode("ascii", errors="replace").strip()
                    match = UART_PATTERN.match(line)
                    if match is not None:
                        result["lines"] += 1
                        name, count = match.groups()
                        task = result["tasks"].setdefault(name, {"lines": 0})
                        task["lines"] += 1
                        task.setdefault("first_count", int(count))
                        if "last_count" in task and delta(task["last_count"], int(count)) == 0:
                            task["duplicates"] = task.get("duplicates", 0) + 1
                        task["last_count"] = int(count)
                        task.setdefault("first_received", time.monotonic())
                        task["last_received"] = time.monotonic()
                        if len(result["examples"]) < 8:
                            result["examples"].append(line)
                if len(pending) > 4096:
                    pending.clear()
    except (OSError, serial.SerialException) as error:
        result["errors"].append(str(error))


def collect_udp(args, stop, result):
    """Capture only target log packets on the existing host address without changing adapters."""
    result.update(
        {
            "packets": [],
            "errors": [],
            "other_peers": 0,
            "capture_start": None,
            "capture_end": None,
        }
    )
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind((args.host, 4243))
            sock.settimeout(0.1)
            result["capture_start"] = time.monotonic()
            while not stop.is_set():
                try:
                    data, peer = sock.recvfrom(2048)
                except TimeoutError:
                    continue
                if peer != (args.target, 4243):
                    result["other_peers"] += 1
                    continue
                match = UDP_PATTERN.match(data.decode("ascii", errors="replace"))
                if match is None:
                    result["errors"].append("Unexpected target UDP log payload")
                    continue
                count, uptime, missed = map(int, match.groups())
                result["packets"].append(
                    {
                        "received": time.monotonic(),
                        "count": count,
                        "uptime_ms": uptime,
                        "missed": missed,
                    }
                )
    except OSError as error:
        result["errors"].append(str(error))
    finally:
        result["capture_end"] = time.monotonic()


def echo_test(args):
    """Send ten bounded diagnostic payloads and require the original peer and exact echo."""
    result = {"sent": 0, "passed": 0, "bytes": 0, "errors": []}
    sizes = (1, 60, 128, 512, 1024, 1472)
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind((args.host, 0))
            sock.settimeout(1)
            for sequence in range(10):
                size = sizes[sequence % len(sizes)]
                payload = bytes((offset + sequence) % 256 for offset in range(size))
                sock.sendto(payload, (args.target, 4242))
                result["sent"] += 1
                try:
                    response, peer = sock.recvfrom(2048)
                except TimeoutError:
                    result["errors"].append(f"Packet {sequence + 1}: timeout")
                    continue
                if peer != (args.target, 4242) or response != payload:
                    result["errors"].append(f"Packet {sequence + 1}: peer or payload mismatch")
                    continue
                result["passed"] += 1
                result["bytes"] += size
    except OSError as error:
        result["errors"].append(str(error))
    return result


def delta(before, after):
    """Compute unsigned counter growth across one 32-bit wrap."""
    return (after - before) & MASK32


def check_udp_log(result):
    """Require a sustained 100 Hz stream with strictly consecutive modulo-32-bit counts."""
    packets = result["packets"]
    start = result.get("capture_start")
    end = result.get("capture_end")
    valid_window = (
        isinstance(start, (int, float))
        and isinstance(end, (int, float))
        and math.isfinite(start)
        and math.isfinite(end)
        and end > start
    )
    window = end - start if valid_window else 0
    expected = window * 100
    minimum = max(2, math.ceil(expected * 0.8))
    received = len(packets)
    span = packets[-1]["received"] - packets[0]["received"] if received >= 2 else 0
    rate = (received - 1) / span if span > 0 else 0
    last_age = end - packets[-1]["received"] if valid_window and received > 0 else None
    steps = [
        delta(previous["count"], current["count"])
        for previous, current in zip(packets, packets[1:], strict=False)
    ]
    gaps = sum(max(0, step - 1) for step in steps)
    duplicates = sum(step == 0 for step in steps)
    sequence_errors = sum(step != 1 for step in steps)
    valid_times = (
        valid_window
        and all(
            math.isfinite(packet["received"]) and start <= packet["received"] <= end
            for packet in packets
        )
        and all(
            previous["received"] <= current["received"]
            for previous, current in zip(packets, packets[1:], strict=False)
        )
    )
    passed = (
        not result["errors"]
        and valid_times
        and received >= minimum
        and span >= window * 0.8
        and last_age is not None
        and last_age <= 0.5
        and abs(rate - 100) <= 5
        and sequence_errors == 0
    )
    return {
        "name": "UDP.log_10ms",
        "passed": passed,
        "received": received,
        "rate_hz": rate,
        "sequence_gaps": gaps,
        "duplicates": duplicates,
        "sequence_errors": sequence_errors,
        "capture_seconds": window,
        "expected_packets": expected,
        "minimum_packets": minimum,
        "packet_span_seconds": span,
        "last_packet_age_seconds": last_age,
    }


def check_growth(before, after, name, period, checks, label):
    """Compare growth with each field's actual sampling interval, allowing scheduling skew."""
    seconds = after["times"][name] - before["times"][name]
    growth = delta(before["values"][name], after["values"][name])
    expected = seconds * 1000 / period
    tolerance = max(3, expected * 0.05)
    passed = seconds > 0 and abs(growth - expected) <= tolerance
    checks.append(
        {
            "name": label,
            "passed": passed,
            "delta": growth,
            "seconds": seconds,
            "expected": expected,
            "tolerance": tolerance,
        }
    )


def equal_checks(values, expected, checks, label):
    """Add explicit observed and expected values to the report."""
    for name, wanted in expected.items():
        actual = values[name]
        checks.append(
            {
                "name": f"{label}.{name}",
                "passed": actual == wanted,
                "actual": actual,
                "expected": wanted,
            }
        )


def validate(report, args):
    """Validate both snapshots and report delays without calling them lost counter periods."""
    first, last = report["snapshots"]
    checks = report["checks"]
    delays = report["coalesced_expirations"]
    for index, sample in enumerate((first, last)):
        hardware = sample["hardware"]
        equal_checks(hardware, {"primary_running": 1}, checks, f"sample_{index}.hardware")
        for core in range(3):
            value = hardware[f"core_{core}_stat"] & 1
            checks.append(
                {
                    "name": f"sample_{index}.M7_{core}.clock_enabled",
                    "passed": value == 1,
                    "actual": value,
                    "expected": 1,
                }
            )
        checks.append(
            {
                "name": f"sample_{index}.secondary_reset_released",
                "passed": hardware["secondary_reset_status"] & 6 == 0,
            }
        )
        for core in range(3):
            values = sample["cores"][str(core)]["values"]
            expected = {"sample_core_clock_hz": 400000000, "sample_clock_error": 0}
            if core == 0:
                expected.update(
                    {
                        "sample_amp_primary_ready": verify_images.READY,
                        "sample_eeprom_write_arm": 0,
                        "sample_network_error": 0,
                        "udp_log_task_errors": 0,
                        "counters.gpio_errors": 0,
                    }
                )
                expected.update(
                    {
                        f"eeprom_read.{key}": value
                        for key, value in {
                            "state": 2,
                            "error": 0,
                            "nacks": 0,
                            "timeouts": 0,
                            "arbitration_losses": 0,
                        }.items()
                    }
                )
                expected.update(
                    {
                        f"eeprom_write.{key}": 0
                        for key in EEPROM_WRITE_FIELDS
                        if key not in ("final_hash",)
                    }
                )
                expected.update(
                    {
                        f"can.{key}": value
                        for key, value in {
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
                        }.items()
                    }
                )
                checks.append(
                    {
                        "name": f"sample_{index}.CAN0.disabled",
                        "passed": values["can.final_mcr"] & (1 << 31) != 0,
                    }
                )
            else:
                expected.update(
                    {
                        "sample_amp_core_id": core,
                        "sample_amp_tasks_started": 1,
                        "sample_amp_error": 0,
                        verify_images.LED_CONFIG_SYMBOL: verify_images.LED_DISABLED
                        if args.skip_led or core == 2
                        else verify_images.LED_ENABLED,
                    }
                )
                if core == 1 and not args.skip_led:
                    expected["m7_1_led_error"] = 0
            equal_checks(values, expected, checks, f"sample_{index}.M7_{core}")
    for core in range(3):
        before = first["cores"][str(core)]
        after = last["cores"][str(core)]
        if core == 0:
            periods = {
                "counters.task_1ms": 1,
                "counters.task_10ms": 10,
                "counters.task_100ms": 100,
                "counters.event_5ms": 5,
                "counters.event_10ms": 10,
                "counters.event_20ms": 20,
                "counters.led_toggles": 500,
                "udp_log_task_count": 10,
            }
            missed = [
                "counters.missed_1ms",
                "counters.missed_10ms",
                "counters.missed_100ms",
                "udp_log_task_missed",
            ]
        else:
            periods = {f"m7_{core}_task_{period}ms_cnt": period for period in verify_images.PERIODS}
            missed = [f"m7_{core}_task_{period}ms_missed" for period in verify_images.PERIODS]
            if core == 1 and not args.skip_led:
                periods["m7_1_led_toggles"] = 500
        for name, period in periods.items():
            check_growth(before, after, name, period, checks, f"M7_{core}.{name}.growth")
        delays[str(core)] = {
            name: delta(before["values"][name], after["values"][name]) for name in missed
        }
    checks.append(
        {
            "name": "UART.task_names",
            "passed": not report["uart"]["errors"]
            and all(name in report["uart"]["tasks"] for name in ("task_10ms", "task_100ms")),
        }
    )
    for name, task in report["uart"]["tasks"].items():
        checks.append(
            {
                "name": f"UART.{name}.fresh_log",
                "passed": time.monotonic() - task["last_received"] <= 1
                and delta(task["first_count"], task["last_count"]) > 0
                and task.get("duplicates", 0) == 0,
            }
        )
    checks.append(
        {
            "name": "UDP.echo_ten_packets",
            "passed": report["echo"]["passed"] == 10 and not report["echo"]["errors"],
        }
    )
    primary_before = first["cores"]["0"]["values"]
    primary_after = last["cores"]["0"]["values"]
    for name, minimum in (
        ("ethernet_echo_count", 10),
        ("ethernet_echo_bytes", report["echo"]["bytes"]),
    ):
        growth = delta(primary_before[name], primary_after[name])
        checks.append(
            {
                "name": f"M7_0.{name}.echo_growth",
                "passed": growth >= minimum,
                "delta": growth,
                "minimum": minimum,
            }
        )
    udp_check = check_udp_log(report["udp_log"])
    report["udp_log"].update(
        {key: value for key, value in udp_check.items() if key not in ("name", "passed")}
    )
    checks.append(udp_check)


def verify(args):
    """Collect read-only API evidence with UART and UDP receivers running concurrently."""
    paths = (args.m7_0, args.m7_1, args.m7_2)
    verified = []
    for core, path in enumerate(paths):
        verify_images.check_config(path, core, args.skip_led)
        verified.append(verify_images.check_elf(path, core, args.skip_led))
    layouts = [primary_layout(paths[0]), verified[1][0], verified[2][0]]
    report = {
        "utc": datetime.now(UTC).isoformat(),
        "images": [str(path.resolve()) for path in paths],
        "skip_led": args.skip_led,
        "led_configuration_from_elf": [item[1] for item in verified],
        "notes": [
            "No core halt, reset, ELF load, memory write, or adapter change is performed.",
            "RAM fields are sampled independently through numeric EAXI reads.",
            "LED evidence is a counter, not visual confirmation.",
            "Nonzero missed deltas mean coalesced expirations; cnt preserves their periods.",
        ],
        "snapshots": [],
        "uart": {},
        "udp_log": {},
        "echo": {},
        "checks": [],
        "coalesced_expirations": {},
        "errors": [],
    }
    stop = threading.Event()
    workers = [
        threading.Thread(target=collect_uart, args=(args, stop, report["uart"])),
        threading.Thread(target=collect_udp, args=(args, stop, report["udp_log"])),
    ]
    debug = None
    try:
        for worker in workers:
            worker.start()
        sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "periodic_tasks"))
        debug = importlib.import_module("verify_eeprom").Debugger(
            args.api_dll.resolve(), args.api_port
        )
        report["snapshots"].append(snapshot(debug, layouts))
        deadline = time.monotonic() + args.seconds
        report["echo"] = echo_test(args)
        while time.monotonic() < deadline:
            time.sleep(max(0, min(0.1, deadline - time.monotonic())))
        report["snapshots"].append(snapshot(debug, layouts))
    except (OSError, RuntimeError, ValueError) as error:
        report["errors"].append(str(error))
    finally:
        stop.set()
        for worker in workers:
            worker.join()
        if debug is not None:
            debug.close()
    if len(report["snapshots"]) == 2:
        validate(report, args)
    report["passed"] = (
        len(report["snapshots"]) == 2
        and not report["errors"]
        and all(check["passed"] for check in report["checks"])
    )
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{'PASS' if report['passed'] else 'FAIL'}: {args.report.resolve()}")
    for check in report["checks"]:
        if not check["passed"]:
            print(json.dumps(check))
    print(f"Coalesced expiration deltas: {report['coalesced_expirations']}")
    return 0 if report["passed"] else 1


def main():
    """Default to the no-LED image and expose no target-mutation option."""
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    for core, directory in enumerate(
        ("s32g_vnp_evb_periodic_tasks", "s32g_vnp_evb_m7_1_no_led", "s32g_vnp_evb_m7_2")
    ):
        parser.add_argument(
            f"--m7-{core}", type=Path, default=root / "build" / directory / "zephyr/zephyr.elf"
        )
    parser.add_argument("--skip-led", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--api-dll", type=Path, required=True)
    parser.add_argument("--api-port", type=int, default=20000)
    parser.add_argument("--uart", default="COM3")
    parser.add_argument("--host", default="192.168.10.1")
    parser.add_argument("--target", default="192.168.10.2")
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--report", type=Path, default=root / "build/amp_onboard_report.json")
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or args.seconds <= 0:
        parser.error("--seconds must be a finite positive duration")
    try:
        return verify(args)
    except (OSError, RuntimeError, ValueError) as error:
        parser.exit(1, f"FAIL: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
