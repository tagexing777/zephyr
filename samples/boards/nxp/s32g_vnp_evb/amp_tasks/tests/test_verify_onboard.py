# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Check sustained UDP log evidence using synthetic captures without hardware."""

import importlib.util
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

SAMPLE_DIR = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "amp_verify_onboard", SAMPLE_DIR / "verify_onboard.py"
)
VERIFY = importlib.util.module_from_spec(SPEC)
with patch.object(sys, "path", [str(SAMPLE_DIR), *sys.path]):
    SPEC.loader.exec_module(VERIFY)


def full_capture(count=1000, first_count=0):
    """Make a continuous 100 Hz stream over a ten-second receiver window."""
    return {
        "capture_start": 0.0,
        "capture_end": 10.0,
        "errors": [],
        "packets": [
            {
                "received": 0.005 + index * 0.01,
                "count": (first_count + index) & VERIFY.MASK32,
                "uptime_ms": index * 10,
                "missed": 0,
            }
            for index in range(count)
        ],
    }


class UdpLogEvidenceTest(unittest.TestCase):
    """Reject short captures, end silence, and broken count sequences."""

    def test_full_ten_second_stream(self):
        result = VERIFY.check_udp_log(full_capture())
        self.assertTrue(result["passed"])
        self.assertEqual(result["received"], 1000)
        self.assertAlmostEqual(result["rate_hz"], 100)
        self.assertEqual(result["sequence_errors"], 0)

    def test_two_packets_then_silence(self):
        result = VERIFY.check_udp_log(full_capture(count=2))
        self.assertAlmostEqual(result["rate_hz"], 100)
        self.assertLess(result["received"], result["minimum_packets"])
        self.assertFalse(result["passed"])

    def test_end_of_window_stream_stops(self):
        result = VERIFY.check_udp_log(full_capture(count=900))
        self.assertGreaterEqual(result["received"], result["minimum_packets"])
        self.assertGreater(result["packet_span_seconds"], 8)
        self.assertGreater(result["last_packet_age_seconds"], 0.5)
        self.assertFalse(result["passed"])

    def test_duplicate_count(self):
        capture = full_capture()
        packet = dict(capture["packets"][499])
        packet["received"] += 0.001
        capture["packets"].insert(500, packet)
        result = VERIFY.check_udp_log(capture)
        self.assertEqual(result["duplicates"], 1)
        self.assertEqual(result["sequence_gaps"], 0)
        self.assertFalse(result["passed"])

    def test_count_gap(self):
        capture = full_capture()
        del capture["packets"][499]
        result = VERIFY.check_udp_log(capture)
        self.assertEqual(result["sequence_gaps"], 1)
        self.assertEqual(result["sequence_errors"], 1)
        self.assertFalse(result["passed"])

    def test_count_wrap(self):
        result = VERIFY.check_udp_log(full_capture(first_count=0xFFFFFFFA))
        self.assertEqual(result["sequence_errors"], 0)
        self.assertTrue(result["passed"])

    def test_windows_receive_bursts(self):
        capture = full_capture()
        for index, packet in enumerate(capture["packets"]):
            packet["received"] = (index // 10 + 1) * 0.1
        result = VERIFY.check_udp_log(capture)
        self.assertTrue(result["passed"])

    def test_compressed_delivery_does_not_cover_capture_window(self):
        capture = full_capture()
        for index, packet in enumerate(capture["packets"]):
            packet["received"] = 8 + index * 0.002
        result = VERIFY.check_udp_log(capture)
        self.assertLess(result["packet_span_seconds"], 8)
        self.assertFalse(result["passed"])

    def test_empty_capture(self):
        result = VERIFY.check_udp_log(full_capture(count=0))
        self.assertEqual(result["rate_hz"], 0)
        self.assertFalse(result["passed"])

    def test_zero_span_does_not_divide_by_zero(self):
        capture = full_capture()
        for packet in capture["packets"]:
            packet["received"] = 9.9
        result = VERIFY.check_udp_log(capture)
        self.assertEqual(result["rate_hz"], 0)
        self.assertFalse(result["passed"])

    def test_missing_receiver_window(self):
        capture = full_capture()
        capture["capture_start"] = None
        result = VERIFY.check_udp_log(capture)
        self.assertFalse(result["passed"])

    def test_capture_error(self):
        capture = full_capture()
        capture["errors"].append("socket error")
        self.assertFalse(VERIFY.check_udp_log(capture)["passed"])


if __name__ == "__main__":
    unittest.main()
