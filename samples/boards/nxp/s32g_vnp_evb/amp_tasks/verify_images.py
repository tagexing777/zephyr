# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Check three AMP images before optionally starting the two secondary cores."""

import argparse
import ctypes
import importlib
import struct
import sys
import time
from pathlib import Path

from elftools.elf.elffile import ELFFile

RAM = ((0x34000000, 0x34100000), (0x34100000, 0x34180000), (0x34180000, 0x34200000))
PERIODS = (1, 5, 10, 20)
READY = 0x414D5030
LED_ENABLED = 0x4C454431
LED_DISABLED = 0x4E4C4544
LED_CONFIG_SYMBOL = "sample_amp_led_config"


def read_config(path):
    """Return enabled/value Kconfig entries; unset symbols are absent."""
    return {
        key: value
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.startswith("CONFIG_") and "=" in line
        for key, value in [line.split("=", 1)]
    }


def check_config(path, core, skip_led=False):
    """Reject secondary images which can initialize unassigned peripherals."""
    config = read_config(path.parent / ".config")
    required = {
        f"CONFIG_SOC_S32G274A_M7_{core}": "y",
        "CONFIG_SMP": None,
        "CONFIG_DCACHE": None,
        "CONFIG_SYS_CLOCK_TICKS_PER_SEC": "1000",
        "CONFIG_TIMER_READS_ITS_FREQUENCY_AT_RUNTIME": "y",
        "CONFIG_SRAM_BASE_ADDRESS": f"0x{RAM[core][0]:x}",
        "CONFIG_SRAM_SIZE": str((RAM[core][1] - RAM[core][0]) // 1024),
    }
    if core != 0:
        for symbol in (
            "SERIAL",
            "CONSOLE",
            "UART_CONSOLE",
            "PRINTK",
            "LOG",
            "NETWORKING",
            "I2C",
            "CAN",
            "WATCHDOG",
            "SAMPLE_S32G_CORE_PLL_FXOSC",
            "PINCTRL",
        ):
            required[f"CONFIG_{symbol}"] = None
        led_enabled = core == 1 and not skip_led
        required["CONFIG_GPIO"] = "y" if led_enabled else None
        required["CONFIG_SAMPLE_S32G_AMP_LED"] = "y" if led_enabled else None
    for key, expected in required.items():
        actual = config.get(key)
        if expected is None:
            if actual not in (None, "n"):
                raise ValueError(f"{path}: unassigned feature {key}={actual}")
        elif actual != expected:
            raise ValueError(f"{path}: {key}={actual}, expected {expected}")


def check_range(path, name, address, size, core):
    """Require an allocated range to fit the core's exclusive RAM slice."""
    lower, upper = RAM[core]
    if address < lower or address + size > upper:
        raise ValueError(f"{path}: {name} escapes its RAM slice")


def check_sections(elf, path, core):
    """Validate allocated sections and return the boot vector bytes."""
    vectors = None
    for section in elf.iter_sections():
        if section["sh_flags"] & 2 and section["sh_size"] != 0:
            address = section["sh_addr"]
            check_range(path, section.name, address, section["sh_size"], core)
            if address == RAM[core][0] and section["sh_type"] != "SHT_NOBITS":
                vectors = section.data()[:8]
    return vectors


def check_segments(elf, path, core):
    """Validate physical and virtual PT_LOAD ranges without accepting relocation."""
    for segment in elf.iter_segments():
        if segment["p_type"] == "PT_LOAD" and segment["p_memsz"] != 0:
            for address in (segment["p_vaddr"], segment["p_paddr"]):
                check_range(path, "load segment", address, segment["p_memsz"], core)
            if segment["p_vaddr"] != segment["p_paddr"]:
                raise ValueError(f"{path}: different physical and virtual load addresses")


def check_vectors(vectors, path, core):
    """Require aligned SP and Thumb reset entry inside the same image slice."""
    lower, upper = RAM[core]
    if vectors is None or len(vectors) != 8:
        raise ValueError(f"{path}: missing vector table at 0x{lower:08x}")
    stack, reset = struct.unpack("<II", vectors)
    if not lower < stack <= upper or stack % 8 != 0:
        raise ValueError(f"{path}: invalid initial SP 0x{stack:08x}")
    if reset & 1 == 0 or not lower <= reset & ~1 < upper:
        raise ValueError(f"{path}: invalid reset vector 0x{reset:08x}")


def required_symbols(core, skip_led=False):
    """List each image's required debugger-visible globals."""
    names = ["sample_core_clock_hz", "sample_clock_error"]
    if core == 0:
        names += ["sample_amp_primary_ready"]
    else:
        names += [
            f"m7_{core}_task_{period}ms_{suffix}"
            for period in PERIODS
            for suffix in ("cnt", "missed")
        ]
        names += [
            "sample_amp_core_id",
            "sample_amp_tasks_started",
            "sample_amp_error",
            LED_CONFIG_SYMBOL,
        ]
    if core == 1 and not skip_led:
        names += ["m7_1_led_toggles", "m7_1_led_error"]
    return names


def check_symbols(elf, path, core, skip_led=False):
    """Require retained four-byte global objects in exclusive image RAM."""
    table = elf.get_section_by_name(".symtab")
    if table is None:
        raise ValueError(f"{path}: debug symbols are required")
    symbols = {symbol.name: symbol for symbol in table.iter_symbols()}
    names = required_symbols(core, skip_led)
    for name in names:
        symbol = symbols.get(name)
        if (
            symbol is None
            or symbol["st_info"]["bind"] != "STB_GLOBAL"
            or symbol["st_info"]["type"] != "STT_OBJECT"
            or symbol["st_size"] != 4
        ):
            raise ValueError(f"{path}: missing/invalid global counter or status {name}")
        check_range(path, name, symbol["st_value"], symbol["st_size"], core)
    return {name: symbols[name]["st_value"] for name in names}


def check_led_config(elf, path, core, skip_led=False):
    """Require the LED mode magic in a retained, file-backed four-byte object."""
    if core == 0:
        return None
    table = elf.get_section_by_name(".symtab")
    matches = [] if table is None else table.get_symbol_by_name(LED_CONFIG_SYMBOL) or []
    if len(matches) != 1:
        raise ValueError(f"{path}: one retained {LED_CONFIG_SYMBOL} is required")
    symbol = matches[0]
    if (
        symbol["st_info"]["bind"] != "STB_GLOBAL"
        or symbol["st_info"]["type"] != "STT_OBJECT"
        or symbol["st_size"] != 4
        or symbol["st_value"] % 4 != 0
    ):
        raise ValueError(f"{path}: invalid LED configuration object")
    check_range(path, LED_CONFIG_SYMBOL, symbol["st_value"], 4, core)
    index = symbol["st_shndx"]
    if not isinstance(index, int) or not 0 <= index < elf.num_sections():
        raise ValueError(f"{path}: LED configuration has no file-backed section")
    section = elf.get_section(index)
    offset = symbol["st_value"] - section["sh_addr"]
    if (
        section["sh_type"] != "SHT_PROGBITS"
        or section["sh_flags"] & 2 == 0
        or offset < 0
        or offset + 4 > section["sh_size"]
    ):
        raise ValueError(f"{path}: LED configuration is not file-backed allocated data")
    data = section.data()[offset : offset + 4]
    if len(data) != 4:
        raise ValueError(f"{path}: truncated LED configuration data")
    address = symbol["st_value"]
    if not any(
        segment["p_type"] == "PT_LOAD"
        and segment["p_vaddr"] <= address
        and address + 4 <= segment["p_vaddr"] + min(segment["p_filesz"], segment["p_memsz"])
        and segment["p_offset"] + address - segment["p_vaddr"] == section["sh_offset"] + offset
        for segment in elf.iter_segments()
    ):
        raise ValueError(f"{path}: LED configuration is not backed by loaded file bytes")
    value = struct.unpack("<I", data)[0]
    expected = LED_ENABLED if core == 1 and not skip_led else LED_DISABLED
    if value != expected:
        raise ValueError(f"{path}: LED configuration 0x{value:08x}, expected 0x{expected:08x}")
    return value


def check_elf(path, core, skip_led=False):
    """Check all allocated sections, load segments, vectors, and task symbols."""
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        if elf.elfclass != 32 or not elf.little_endian or elf["e_machine"] != "EM_ARM":
            raise ValueError(f"{path}: expected a little-endian ARM ELF32")
        check_vectors(check_sections(elf, path, core), path, core)
        check_segments(elf, path, core)
        symbols = check_symbols(elf, path, core, skip_led)
        return symbols, check_led_config(elf, path, core, skip_led)


def start_secondaries(args, root, m7_1_symbols, led_config):
    """Start validated images; only an LED-enabled image needs jumper confirmation."""
    expected = LED_DISABLED if args.skip_led else LED_ENABLED
    if led_config != expected:
        raise ValueError("Verified LED configuration does not match the requested start mode")
    if args.api_dll is None:
        raise ValueError("--start requires --api-dll")
    if led_config == LED_ENABLED and not args.confirm_led_jumper:
        raise ValueError("LED-enabled --start requires --confirm-led-jumper")
    led_address = m7_1_symbols.get(LED_CONFIG_SYMBOL)
    if not isinstance(led_address, int) or led_address % 4 != 0:
        raise ValueError("Verified LED configuration address is required")
    check_range("M7_1", LED_CONFIG_SYMBOL, led_address, 4, 1)
    mode = "NO_LED" if led_config == LED_DISABLED else "D14_J164_1_2"
    # Reuse the existing API wrapper; this path does not load or reset M7_0.
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "periodic_tasks"))
    debugger_class = importlib.import_module("verify_eeprom").Debugger
    debug = debugger_class(args.api_dll.resolve(), args.api_port)
    try:
        loader = root / "boards/nxp/s32g_vnp_evb/support/S32G_CM7_AMP_Start.cmm"
        if debug.evaluate("STATE.RUN()") != 1:
            raise RuntimeError("M7_0 must already be running")
        if debug.evaluate("Data.Long(EAXI:sample_amp_primary_ready)") != READY:
            raise RuntimeError("M7_0 has not completed shared initialization")
        debug.command(
            f'DO "{loader.as_posix()}" "{args.m7_1.resolve().as_posix()}" '
            f'"{args.m7_2.resolve().as_posix()}" "{mode}" "0x{led_address:08x}"'
        )
        time.sleep(0.1)
        deadline = time.monotonic() + 10
        state = ctypes.c_int()
        while True:
            if debug.api.T32_GetPracticeState(ctypes.byref(state)) != 0:
                raise RuntimeError("Cannot read PRACTICE state")
            if state.value == 0:
                break
            if time.monotonic() > deadline:
                raise RuntimeError("AMP start script timed out")
            time.sleep(0.05)
        debug.check_message()
        if (debug.evaluate("Data.Long(EAXI:0x40078140)") & 6) != 0:
            raise RuntimeError("Secondary resets remain asserted")
        if any(
            debug.evaluate(f"Data.Long(EAXI:0x{address:x})") & 1 == 0
            for address in (0x40088168, 0x40088188)
        ):
            raise RuntimeError("Secondary core clocks are not enabled")
        print("Secondary resets released; running counters still require hardware verification.")
    finally:
        debug.close()


def main():
    """Validate image files by default; hardware writes require an explicit option."""
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    for core, directory in enumerate(
        ("s32g_vnp_evb_periodic_tasks", "s32g_vnp_evb_m7_1", "s32g_vnp_evb_m7_2")
    ):
        parser.add_argument(
            f"--m7-{core}", type=Path, default=root / "build" / directory / "zephyr/zephyr.elf"
        )
    parser.add_argument("--start", action="store_true")
    parser.add_argument(
        "--skip-led", action="store_true", help="require an M7_1 image without GPIO"
    )
    parser.add_argument("--confirm-led-jumper", action="store_true")
    parser.add_argument("--api-dll", type=Path)
    parser.add_argument("--api-port", type=int, default=20000)
    args = parser.parse_args()
    images = (args.m7_0, args.m7_1, args.m7_2)
    try:
        verified = []
        for core, path in enumerate(images):
            check_config(path, core, args.skip_led)
            symbols, led_config = check_elf(path, core, args.skip_led)
            verified.append((symbols, led_config))
            print(f"M7_{core} PASS RAM 0x{RAM[core][0]:08x}..0x{RAM[core][1] - 1:08x}")
            for name, address in symbols.items():
                print(f"  {name}: 0x{address:08x}")
        if args.start:
            start_secondaries(args, root, *verified[1])
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, f"FAIL: {error}\n")


if __name__ == "__main__":
    main()
