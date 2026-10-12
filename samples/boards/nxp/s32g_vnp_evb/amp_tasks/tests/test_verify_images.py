# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Exercise the read-only AMP loader preflight boundaries without hardware."""

import importlib.util
import struct
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

SPEC = importlib.util.spec_from_file_location(
    "amp_verify_images", Path(__file__).resolve().parents[1] / "verify_images.py"
)
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)


def secondary_config(core, skip_led=False):
    """Make the minimum valid configuration for either secondary image."""
    lower, upper = VERIFY.RAM[core]
    config = {
        f"CONFIG_SOC_S32G274A_M7_{core}": "y",
        "CONFIG_SYS_CLOCK_TICKS_PER_SEC": "1000",
        "CONFIG_TIMER_READS_ITS_FREQUENCY_AT_RUNTIME": "y",
        "CONFIG_SRAM_BASE_ADDRESS": f"0x{lower:x}",
        "CONFIG_SRAM_SIZE": str((upper - lower) // 1024),
    }
    if core == 1 and not skip_led:
        config["CONFIG_GPIO"] = "y"
        config["CONFIG_SAMPLE_S32G_AMP_LED"] = "y"
    return config


def led_elf(core, value):
    """Mock a file-backed LED mode object without generating or loading an ELF."""
    address = VERIFY.RAM[core][0] + 0x100
    symbol = {
        "st_info": {"bind": "STB_GLOBAL", "type": "STT_OBJECT"},
        "st_size": 4,
        "st_value": address,
        "st_shndx": 1,
    }
    fields = {
        "sh_type": "SHT_PROGBITS",
        "sh_flags": 2,
        "sh_addr": address,
        "sh_size": 4,
        "sh_offset": 0x100,
    }
    section = MagicMock()
    section.__getitem__.side_effect = fields.__getitem__
    section.data.return_value = struct.pack("<I", value)
    segment = {
        "p_type": "PT_LOAD",
        "p_vaddr": address,
        "p_paddr": address,
        "p_filesz": 4,
        "p_memsz": 4,
        "p_offset": 0x100,
    }
    elf = MagicMock()
    elf.get_section_by_name.return_value.get_symbol_by_name.return_value = [symbol]
    elf.num_sections.return_value = 3
    elf.get_section.return_value = section
    elf.iter_segments.return_value = [segment]
    return elf, symbol, fields, segment, section


class ImageValidationTest(unittest.TestCase):
    """Cover all RAM slices, reset vector constraints, and peripheral exclusions."""

    def test_ram_boundaries(self):
        """Accept exact allocated ranges and reject every adjacent-core overlap."""
        for core, (lower, upper) in enumerate(VERIFY.RAM):
            VERIFY.check_range("test.elf", "section", lower, upper - lower, core)
            VERIFY.check_range("test.elf", "global", upper - 4, 4, core)
            for address, size in ((lower - 1, 1), (upper - 3, 4), (upper, 1)):
                with (
                    self.subTest(core=core, address=address, size=size),
                    self.assertRaises(ValueError),
                ):
                    VERIFY.check_range("test.elf", "section", address, size, core)

    def test_vectors(self):
        """Require aligned stacks, in-slice reset entries, and Thumb state."""
        for core, (lower, upper) in enumerate(VERIFY.RAM):
            VERIFY.check_vectors(struct.pack("<II", upper, lower + 0x101), "test.elf", core)
            invalid = (
                (lower, lower + 0x101),
                (upper + 8, lower + 0x101),
                (upper - 1, lower + 0x101),
                (upper, lower + 0x100),
                (upper, lower - 1),
                (upper, upper + 1),
            )
            for stack, reset in invalid:
                with (
                    self.subTest(core=core, stack=stack, reset=reset),
                    self.assertRaises(ValueError),
                ):
                    VERIFY.check_vectors(struct.pack("<II", stack, reset), "test.elf", core)
            for vector in (None, b"", b"\0" * 7):
                with self.assertRaises(ValueError):
                    VERIFY.check_vectors(vector, "test.elf", core)

    def test_secondary_configs(self):
        """Unassigned hardware, caches, SMP, and shared clock setup must stay off."""
        forbidden = [
            "CAN",
            "CONSOLE",
            "DCACHE",
            "I2C",
            "LOG",
            "NETWORKING",
            "PINCTRL",
            "PRINTK",
            "SAMPLE_S32G_CORE_PLL_FXOSC",
            "SERIAL",
            "SMP",
            "UART_CONSOLE",
            "WATCHDOG",
        ]
        for core in (1, 2):
            config = secondary_config(core)
            with patch.object(VERIFY, "read_config", return_value=config):
                VERIFY.check_config(Path("zephyr/zephyr.elf"), core)
            for symbol in forbidden + (["GPIO", "SAMPLE_S32G_AMP_LED"] if core == 2 else []):
                with self.subTest(core=core, symbol=symbol):
                    invalid = dict(config, **{f"CONFIG_{symbol}": "y"})
                    with (
                        patch.object(VERIFY, "read_config", return_value=invalid),
                        self.assertRaises(ValueError),
                    ):
                        VERIFY.check_config(Path("zephyr/zephyr.elf"), core)

    def test_no_led_configs(self):
        """NO_LED must disable both the option and GPIO, including on M7_1."""
        for core in (1, 2):
            config = secondary_config(core, skip_led=True)
            with patch.object(VERIFY, "read_config", return_value=config):
                VERIFY.check_config(Path("zephyr/zephyr.elf"), core, skip_led=True)
            disabled = dict(config, CONFIG_GPIO="n", CONFIG_SAMPLE_S32G_AMP_LED="n")
            with patch.object(VERIFY, "read_config", return_value=disabled):
                VERIFY.check_config(Path("zephyr/zephyr.elf"), core, skip_led=True)
            for symbol in ("GPIO", "SAMPLE_S32G_AMP_LED"):
                with self.subTest(core=core, symbol=symbol):
                    invalid = dict(config, **{f"CONFIG_{symbol}": "y"})
                    with (
                        patch.object(VERIFY, "read_config", return_value=invalid),
                        self.assertRaises(ValueError),
                    ):
                        VERIFY.check_config(Path("zephyr/zephyr.elf"), core, skip_led=True)
        config = secondary_config(1)
        for symbol in ("GPIO", "SAMPLE_S32G_AMP_LED"):
            invalid = dict(config, **{f"CONFIG_{symbol}": "n"})
            with (
                patch.object(VERIFY, "read_config", return_value=invalid),
                self.assertRaises(ValueError),
            ):
                VERIFY.check_config(Path("zephyr/zephyr.elf"), 1)

    def test_mode_specific_symbols(self):
        """Only an LED-enabled M7_1 requires LED counters; both retain the mode flag."""
        for core in (1, 2):
            for skip_led in (False, True):
                names = VERIFY.required_symbols(core, skip_led)
                self.assertIn(VERIFY.LED_CONFIG_SYMBOL, names)
                self.assertIn(f"m7_{core}_task_1ms_missed", names)
                for symbol in ("m7_1_led_toggles", "m7_1_led_error"):
                    self.assertEqual(symbol in names, core == 1 and not skip_led)


class LedConfigurationTest(unittest.TestCase):
    """Validate the ELF's initial mode independently of configuration text."""

    def test_expected_modes(self):
        """Accept only LED1 on ordinary M7_1 and NLED on every no-LED image."""
        for core, skip_led, value in (
            (1, False, VERIFY.LED_ENABLED),
            (1, True, VERIFY.LED_DISABLED),
            (2, False, VERIFY.LED_DISABLED),
            (2, True, VERIFY.LED_DISABLED),
        ):
            with self.subTest(core=core, skip_led=skip_led):
                elf, *_ = led_elf(core, value)
                self.assertEqual(VERIFY.check_led_config(elf, "test.elf", core, skip_led), value)
                for wrong in (0, 1, 0xFFFFFFFF, value ^ 1):
                    elf, *_ = led_elf(core, wrong)
                    with self.assertRaises(ValueError):
                        VERIFY.check_led_config(elf, "test.elf", core, skip_led)
                elf, *_ = led_elf(
                    core, VERIFY.LED_DISABLED if value == VERIFY.LED_ENABLED else VERIFY.LED_ENABLED
                )
                with self.assertRaises(ValueError):
                    VERIFY.check_led_config(elf, "test.elf", core, skip_led)

    def test_symbol_metadata(self):
        """Reject missing, duplicated, local, non-object, and mis-sized mode flags."""
        for matches in ([], None):
            elf, *_ = led_elf(1, VERIFY.LED_DISABLED)
            elf.get_section_by_name.return_value.get_symbol_by_name.return_value = matches
            with self.assertRaises(ValueError):
                VERIFY.check_led_config(elf, "test.elf", 1, True)
        for field, value in (
            ("st_info", {"bind": "STB_LOCAL", "type": "STT_OBJECT"}),
            ("st_info", {"bind": "STB_GLOBAL", "type": "STT_FUNC"}),
            ("st_size", 3),
            ("st_size", 8),
            ("st_value", VERIFY.RAM[1][0] - 4),
            ("st_value", VERIFY.RAM[1][1]),
            ("st_value", VERIFY.RAM[1][0] + 0x101),
            ("st_shndx", "SHN_UNDEF"),
            ("st_shndx", "SHN_ABS"),
            ("st_shndx", -1),
            ("st_shndx", 3),
        ):
            with self.subTest(field=field, value=value):
                elf, symbol, *_ = led_elf(1, VERIFY.LED_DISABLED)
                symbol[field] = value
                with self.assertRaises(ValueError):
                    VERIFY.check_led_config(elf, "test.elf", 1, True)
        elf, symbol, *_ = led_elf(1, VERIFY.LED_DISABLED)
        elf.get_section_by_name.return_value.get_symbol_by_name.return_value = [symbol, symbol]
        with self.assertRaises(ValueError):
            VERIFY.check_led_config(elf, "test.elf", 1, True)

    def test_flag_at_ram_boundary(self):
        """A complete four-byte mode flag at the slice's final aligned address fits."""
        for core in (1, 2):
            elf, symbol, fields, segment, _ = led_elf(core, VERIFY.LED_DISABLED)
            address = VERIFY.RAM[core][1] - 4
            symbol["st_value"] = address
            fields["sh_addr"] = address
            segment["p_vaddr"] = address
            segment["p_paddr"] = address
            self.assertEqual(
                VERIFY.check_led_config(elf, "test.elf", core, True), VERIFY.LED_DISABLED
            )

    def test_file_backing(self):
        """BSS, truncated bytes, and non-loaded or inconsistent offsets cannot prove a mode."""
        for field, value in (
            ("sh_type", "SHT_NOBITS"),
            ("sh_flags", 0),
            ("sh_addr", VERIFY.RAM[1][0] + 0x104),
            ("sh_size", 3),
            ("sh_offset", 0x104),
        ):
            with self.subTest(field=field, value=value):
                elf, _, fields, *_ = led_elf(1, VERIFY.LED_DISABLED)
                fields[field] = value
                with self.assertRaises(ValueError):
                    VERIFY.check_led_config(elf, "test.elf", 1, True)
        for field, value in (
            ("p_type", "PT_NOTE"),
            ("p_filesz", 3),
            ("p_memsz", 3),
            ("p_offset", 0x104),
        ):
            with self.subTest(field=field, value=value):
                elf, _, _, segment, _ = led_elf(1, VERIFY.LED_DISABLED)
                segment[field] = value
                with self.assertRaises(ValueError):
                    VERIFY.check_led_config(elf, "test.elf", 1, True)
        elf, _, _, _, section = led_elf(1, VERIFY.LED_DISABLED)
        section.data.return_value = b"\0" * 3
        with self.assertRaises(ValueError):
            VERIFY.check_led_config(elf, "test.elf", 1, True)
        elf, *_ = led_elf(1, VERIFY.LED_DISABLED)
        elf.iter_segments.return_value = []
        with self.assertRaises(ValueError):
            VERIFY.check_led_config(elf, "test.elf", 1, True)


class StartModeTest(unittest.TestCase):
    """Mock the debugger API so startup argument tests never access hardware."""

    @staticmethod
    def arguments(skip_led, confirm=False):
        """Return command-line arguments for a debugger-free startup test."""
        return SimpleNamespace(
            skip_led=skip_led,
            confirm_led_jumper=confirm,
            api_dll=Path("t32api64.dll"),
            api_port=20000,
            m7_1=Path("m7_1.elf"),
            m7_2=Path("m7_2.elf"),
        )

    def test_loader_arguments(self):
        """Pass the validated flag address; only LED1 requires a jumper acknowledgment."""
        address = VERIFY.RAM[1][0] + 0x100
        for skip_led, mode, value in (
            (False, "D14_J164_1_2", VERIFY.LED_ENABLED),
            (True, "NO_LED", VERIFY.LED_DISABLED),
        ):
            with self.subTest(skip_led=skip_led):
                debug = MagicMock()
                debug.evaluate.side_effect = [1, VERIFY.READY, 0, 1, 1]
                debug.api.T32_GetPracticeState.return_value = 0
                module = SimpleNamespace(Debugger=MagicMock(return_value=debug))
                args = self.arguments(skip_led, confirm=not skip_led)
                with (
                    patch.object(VERIFY.importlib, "import_module", return_value=module),
                    patch.object(VERIFY.sys, "path", []),
                    patch.object(VERIFY.time, "sleep"),
                    patch.object(VERIFY, "print", create=True),
                ):
                    VERIFY.start_secondaries(
                        args, Path("checkout"), {VERIFY.LED_CONFIG_SYMBOL: address}, value
                    )
                debug.command.assert_called_once()
                self.assertTrue(
                    debug.command.call_args.args[0].endswith(f'"{mode}" "0x{address:08x}"')
                )
                debug.close.assert_called_once()

    def test_start_preconditions(self):
        """Reject mode/config mismatches and missing acknowledgment before attaching."""
        address = VERIFY.RAM[1][0] + 0x100
        cases = [
            (self.arguments(False), {VERIFY.LED_CONFIG_SYMBOL: address}, VERIFY.LED_ENABLED),
            (self.arguments(True), {VERIFY.LED_CONFIG_SYMBOL: address}, VERIFY.LED_ENABLED),
            (self.arguments(False, True), {VERIFY.LED_CONFIG_SYMBOL: address}, VERIFY.LED_DISABLED),
            (self.arguments(True), {}, VERIFY.LED_DISABLED),
            (self.arguments(True), {VERIFY.LED_CONFIG_SYMBOL: address + 1}, VERIFY.LED_DISABLED),
            (
                self.arguments(True),
                {VERIFY.LED_CONFIG_SYMBOL: VERIFY.RAM[0][0]},
                VERIFY.LED_DISABLED,
            ),
        ]
        no_api = self.arguments(True)
        no_api.api_dll = None
        cases.append((no_api, {VERIFY.LED_CONFIG_SYMBOL: address}, VERIFY.LED_DISABLED))
        for args, symbols, value in cases:
            with (
                self.subTest(args=args, symbols=symbols, value=value),
                patch.object(VERIFY.importlib, "import_module") as imported,
                self.assertRaises(ValueError),
            ):
                VERIFY.start_secondaries(args, Path("checkout"), symbols, value)
            imported.assert_not_called()

    def test_cli_passes_validated_mode(self):
        """The skip option reaches all checks and startup uses the checked flag address."""
        symbols = {VERIFY.LED_CONFIG_SYMBOL: VERIFY.RAM[1][0] + 0x100}
        checked = [({}, None), (symbols, VERIFY.LED_DISABLED), ({}, VERIFY.LED_DISABLED)]
        with (
            patch.object(VERIFY.sys, "argv", ["verify_images.py", "--skip-led", "--start"]),
            patch.object(VERIFY, "check_config") as check_config,
            patch.object(VERIFY, "check_elf", side_effect=checked) as check_elf,
            patch.object(VERIFY, "start_secondaries") as start,
            patch.object(VERIFY, "print", create=True),
        ):
            VERIFY.main()
        for check in (check_config, check_elf):
            self.assertEqual(
                [call.args[1:] for call in check.call_args_list], [(0, True), (1, True), (2, True)]
            )
        start.assert_called_once()
        args, _, checked_symbols, value = start.call_args.args
        self.assertTrue(args.skip_led)
        self.assertFalse(args.confirm_led_jumper)
        self.assertIs(checked_symbols, symbols)
        self.assertEqual(value, VERIFY.LED_DISABLED)


if __name__ == "__main__":
    unittest.main()
