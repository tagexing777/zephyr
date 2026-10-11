# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Verify IPv4 UDP echo payloads on an isolated S32G test network."""

import argparse
import socket
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--host", default="192.168.10.1")
    parser.add_argument("--target", default="192.168.10.2")
    parser.add_argument("--count", type=int, default=600)
    args = parser.parse_args()
    if args.count <= 0:
        parser.error("--count must be positive")

    sizes = (1, 60, 128, 512, 1024, 1472)
    start = time.perf_counter()
    total = 0
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((args.host, 0))
        sock.settimeout(1)
        for sequence in range(args.count):
            size = sizes[sequence % len(sizes)]
            payload = bytes((offset + sequence) % 256 for offset in range(size))
            sock.sendto(payload, (args.target, 4242))
            try:
                response, peer = sock.recvfrom(2048)
            except TimeoutError as error:
                raise SystemExit(f"FAIL: timeout at packet {sequence + 1}, size {size}") from error
            if peer != (args.target, 4242) or response != payload:
                raise SystemExit(f"FAIL: payload or peer mismatch at packet {sequence + 1}")
            total += size

    elapsed = time.perf_counter() - start
    print(f"PASS: {args.count}/{args.count} packets, {total} bytes, {elapsed:.3f} seconds")


if __name__ == "__main__":
    main()
