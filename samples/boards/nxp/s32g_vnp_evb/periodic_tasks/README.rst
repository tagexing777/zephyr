.. Copyright The Zephyr Project Contributors
.. SPDX-License-Identifier: Apache-2.0

.. zephyr:code-sample:: s32g-periodic-tasks
   :name: S32G M7 periodic tasks, counted events, and UDP logs
   :relevant-api: gpio_interface

   Run periodic tasks, counted events, and upper-board Ethernet on S32G274A M7_0.

Overview
********

This sample targets the original S32G-VNP-EVB, consisting of the S32G-PROCEVB-S
processor board and S32GRV-PLATEVB carrier. It is not an EVB3 target. All
application threads run in one M7_0 ELF; M7_1 and M7_2 are not started.
This is the primary application for periodic tasks, LED, UART, counted events,
and Ethernet. The sibling ``ethernet`` sample is a network-only reference.

The GPIO mapping follows SCH-32170 Rev B4, sheet 13: green LED D15 is connected
through R715 to 3.3 V, with its cathode on GPIO24/PB8. It is active-low. PB8 is
also a reset-configuration pin, so this sample configures it only after boot.
Do not let another AMP image or Linux control this pin or its GPIO register bank.

.. list-table:: Application threads
   :header-rows: 1

   * - Thread
     - Priority
     - Action
   * - ``task_1ms``
     - 0
     - Wait for a 1 ms timer; toggle D15 every 500 ms.
   * - ``task_extended``
     - 1
     - Wait for 5/10/20 ms event bits and accumulate each event counter.
   * - ``task_10ms``
     - 2
     - Print its name and timer expiration count every 10 ms.
   * - ``task_100ms``
     - 3
     - Print its name, timer expiration count, and three event counters every 100 ms.
   * - ``task_udp_log_10ms``
     - 4
     - Queue a UDP log containing its name and counters every 10 ms.
   * - ``udp_log_sender``
     - 5
     - Send queued logs to the PC without blocking the producer on a socket.
   * - ``task_ethernet``
     - 6
     - Initialize IPv4 and serve UDP echo requests on port 4242.

Lower numeric priorities have higher preemptive priority. D15 starts off, turns
on after 500 ms, and turns off after another 500 ms: a complete on/off cycle is
1 second. The timer-driven loops do not add UART execution time to each period.
The 10/100 ms threads serialize their console calls with a mutex. Deferred
logging drains the UART at lower priority, keeping console output out of the
network producers. Neither the 1 ms thread nor timer callbacks print to the UART.

Each event timer increments a pending atomic count before posting its event bit.
The extended thread atomically consumes event bits with ``k_event_wait_safe()``
and drains the corresponding pending counts. Repeated postings of one bit before
the thread wakes therefore do not collapse into a single counted occurrence.
This is a Zephyr event-driven thread, not an AUTOSAR/OSEK ExtendedTask API.

Timing
******

The sample uses 1000 ticks per second and periodic SysTick interrupts. Before
SysTick initialization, it reads MC_CGM_0 MUX_0 and the selected CORE_PLL/DFS
divider registers, then publishes the nominal M7 clock to the kernel. It follows
the NXP RTD ``Clock_Ip_Frequency.c`` equations: M7 uses XBAR_2X_CLK divided by two.
Supported sources are FIRC, FXOSC, CORE_PLL PHI0/1, and CORE_PLL DFS1 through DFS6.
The sample does not reprogram shared clock, reset, or power-management registers.
Unsupported or unlocked configurations print an error and do not start tasks.
The UART still requires MUX_8 to select the board's 40 MHz FXOSC.

FIRC is treated as nominally 48 MHz; its physical tolerance and PLL spread
spectrum, if enabled by boot firmware, limit absolute timing accuracy. Register
decoding is not a frequency measurement. Use an oscilloscope on the LED GPIO
and host-timestamped UART capture to validate wall-clock periods. Do not change
the clock tree after starting the application.

The clock equations are also described in NXP's
`S32G Software Frequency Change and Spread Spectrum for EMI Optimization`_.

.. _S32G Software Frequency Change and Spread Spectrum for EMI Optimization:
   https://community.nxp.com/pwmxy87654/attachments/pwmxy87654/nxp-designs@tkb/684/3/S32G_FC_SSC_V3-20220121_Eng.pdf

Building and Running
********************

From the Zephyr directory, with the west workspace and toolchain configured:

.. code-block:: shell

   west build -p always -b s32g_vnp_evb/s32g274a/m7_0 \
     samples/boards/nxp/s32g_vnp_evb/periodic_tasks \
     -d build/s32g_vnp_evb_periodic_tasks

The output ELF is ``build/s32g_vnp_evb_periodic_tasks/zephyr/zephyr.elf``. The
existing hello_world image in ``build/s32g_vnp_evb_m7_0`` is not overwritten.
Open the board's J58 USB UART at 115200 baud, 8N1, without flow control. In
TRACE32, replace ``<zephyr>`` with the absolute repository path and execute:

.. code-block:: text

   DO "<zephyr>/boards/nxp/s32g_vnp_evb/support/S32G_CM7_0_ZEPHYR_Load.cmm"
   Go

The script loads the periodic-tasks ELF by default and stops at ``main``.
Its connection and initialization sequence is unchanged. To load hello_world,
pass its absolute ELF path as the first argument to the same script.

For an already-connected M7_0 debugger with initialized SRAM, the loader also
accepts ``"WARM"`` as the second argument. This stops GMAC0 DMA before replacing
the M7_0 image without a chip reset. It does not establish AMP isolation; no
other core may own GMAC0 or the image's SRAM.

.. warning::

   The loader is for dedicated M7_0 bring-up. It resets the target, initializes
   both SRAM banks, and disables SWT0 through SWT7. It does not preserve running
   Cortex-A53 or other M7 images. This is not a live AMP attachment procedure and
   does not program Flash. Power-cycle if the AXI connection fails.

Verification
************

The UART should show ``Periodic tasks started``, then ``task_10ms`` and
``task_100ms`` lines. The latter include ``e5``, ``e10``, and ``e20``. Over
approximately 1 second, their increments should be 200, 100, and 50. At shared
deadlines a line can precede the last event handler by one occurrence. Compare
increments over multiple seconds rather than requiring an exact first line.

In TRACE32, inspect the ``sample_counters`` structure:

* ``task_1ms``, ``task_10ms``, ``task_100ms`` count timer expirations, not loop
  bodies. Expected per-second increments are 1000, 100, and 10.
* ``event_5ms``, ``event_10ms``, ``event_20ms`` count event occurrences processed
  by the extended thread. Pending occurrences are in ``pending_5ms`` and peers.
* ``missed_1ms``, ``missed_10ms``, ``missed_100ms`` count expirations coalesced
  before their task resumed. These should stay zero under normal load. Late
  tasks account for missed periods but cannot execute retroactively.
* ``led_toggles`` counts actual GPIO toggles. After a large scheduling delay,
  the LED is moved to its current phase, not toggled rapidly to replay history.
* ``gpio_errors`` should remain zero.

Counters use 32-bit atomic storage and wrap modulo 2^32. Stop the CPU before
comparing counters: debugger halts interrupt scheduling and invalidate
wall-clock period measurements. Kernel scheduling, GPIO access, and UART
behavior require target testing; a successful build alone is not a hardware
timing guarantee.

Upper-board Ethernet and UDP Logs
********************************

Connect a dedicated PC adapter to upper-board J108 (KSZ9031 PHY address 4),
not the carrier-board port. Configure the PC adapter as ``192.168.10.1/24``
without a gateway. The target uses ``192.168.10.2/24``. GMAC0, its RGMII pins,
interrupts, and peripheral PLL PHI5 must belong exclusively to M7_0; an A53
image must not enable GMAC0 or PFE MAC1 on these pins. See the sibling
``ethernet/README.rst`` for the full routing and ownership requirements.

The UDP echo server listens on port 4242. The UDP log destination is
``192.168.10.1:4243``, with local source port 4243. Change ``LOG_HOST`` and
``LOG_PORT`` in ``src/network.c`` to select a different destination.
The producer waits for 500 ms of sampled carrier-up state before starting its
10 ms timer, allowing the link partner to settle before initial ARP traffic.
This does not confirm peer availability or guarantee UDP delivery.

``src/udp_log.h`` declares ``udp_log_init()``, ``udp_log_printf()``, and
``udp_log_get_stats()``. The sample initializes the interface once. Application
threads can use it as follows:

.. code-block:: c

   int32_t ret = udp_log_printf("task_10ms count=%u\n", count);

   if (ret != 0) {
           /* Record the formatting or queue failure. */
   }

Each call copies at most 255 text bytes into a static 128-entry queue without
waiting for the socket. Zero means queued, not delivered. Interrupt-context
calls are unsupported. Oversized messages return ``-EMSGSIZE``; a full queue
returns ``-ENOBUFS``. The sender drops failed sends without retrying.
Statistics include queued, locally sent, dropped, and transport-error counts,
the maximum socket-send duration, and the last send error. A nonblocking socket
flag does not guarantee a nonblocking allocation path inside the network stack.

Only one PC log receiver may bind port 4243 at a time:

.. code-block:: shell

   ping 192.168.10.2
   python samples/boards/nxp/s32g_vnp_evb/periodic_tasks/verify_echo.py
   python samples/boards/nxp/s32g_vnp_evb/periodic_tasks/receive_log.py \
     --count 1000 --quiet --verify

The echo verifier checks 600 datagrams up to 1472 bytes. The log verifier checks
consecutive counters and target uptime increments of 9 through 11 ms; host arrival
spacing is reported separately. Run these while checking the original UART task
lines, counted events, and LED GPIO. The console harness checks startup only.
UDP remains best-effort, and unplugged cables or unavailable peers can cause loss.
D-cache is disabled for GMAC bring-up. This integration does not switch the
M7 CORE_PLL reference or change the existing UART clock selection.

Host Logic Tests
================

The clock formulas and LED phase helper can be tested without Zephyr or hardware.
From the repository root, using a host C compiler:

.. code-block:: shell

   cc -std=c11 -Wall -Wextra -Werror \
     samples/boards/nxp/s32g_vnp_evb/periodic_tasks/tests/host/test_logic.c \
     -o build/test_s32g_periodic_logic
   ./build/test_s32g_periodic_logic

These tests cover integer/fractional PLLs, PLL/DFS lock and divider checks, LED
500 ms boundaries, missed periods, and large expiration counts. They do not
exercise Zephyr scheduling, concurrent event delivery, MMIO, or UART/GPIO drivers.
