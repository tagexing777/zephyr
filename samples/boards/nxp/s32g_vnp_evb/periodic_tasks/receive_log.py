# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Receive S32G periodic-application UDP logs and verify the 10 ms log task."""

import argparse
import re
import socket
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--host", default="192.168.10.1")
    parser.add_argument("--port", type=int, default=4243)
    parser.add_argument("--count", type=int, default=0, help="0 receives until interrupted")
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--verify", action="store_true", help="check sample sequence and uptime")
    args = parser.parse_args()
    if args.count < 0 or not 1 <= args.port <= 65535:
        parser.error("--count must be nonnegative and --port must be 1..65535")

    pattern = re.compile(rb"task_udp_log_10ms count=(\d+) uptime_ms=(\d+) missed=(\d+)\n")
    previous = None
    received = 0
    first_time = None
    last_time = None
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((args.host, args.port))
        sock.settimeout(10)
        print(f"Listening on {args.host}:{args.port}; Ctrl+C stops reception", flush=True)
        try:
            while args.count == 0 or received < args.count:
                try:
                    payload, peer = sock.recvfrom(2048)
                except TimeoutError as error:
                    if args.count > 0 or args.verify:
                        raise SystemExit("FAIL: no log received for 10 seconds") from error
                    continue
                now = time.perf_counter()
                if first_time is None:
                    first_time = now
                last_time = now
                if args.verify:
                    match = pattern.fullmatch(payload)
                    if match is None or peer != ("192.168.10.2", args.port):
                        raise SystemExit(f"FAIL: unexpected log or peer {peer}: {payload!r}")
                    current = tuple(map(int, match.groups()))
                    if previous is not None:
                        sequence_delta = (current[0] - previous[0]) & 0xFFFFFFFF
                        uptime_delta = (current[1] - previous[1]) & 0xFFFFFFFF
                        if sequence_delta != 1 or not 9 <= uptime_delta <= 11:
                            raise SystemExit(f"FAIL: sequence/period {previous} -> {current}")
                        if current[2] != previous[2]:
                            raise SystemExit(f"FAIL: missed timer periods {previous} -> {current}")
                    previous = current
                received += 1
                if not args.quiet:
                    print(payload.decode("utf-8", errors="replace"), end="", flush=True)
        except KeyboardInterrupt:
            pass

    if received > 1:
        average = (last_time - first_time) * 1000 / (received - 1)
        print(f"Received {received} logs; average arrival interval {average:.3f} ms")
    else:
        print(f"Received {received} logs")
    if args.verify and args.count > 0:
        if received != args.count:
            raise SystemExit("FAIL: interrupted before the requested log count")
        print(f"PASS: {received} consecutive sample logs with 10 ms target uptime steps")


if __name__ == "__main__":
    main()
