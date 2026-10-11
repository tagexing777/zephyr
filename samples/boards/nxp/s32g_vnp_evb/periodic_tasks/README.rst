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
With ``CONFIG_SAMPLE_S32G_CORE_PLL_FXOSC=y`` (the sample default), startup
switches XBAR/M7 temporarily to FIRC, resets the enabled CORE_DFS output,
reconfigures CORE_PLL from the stable 40 MHz crystal, waits for PLL/DFS lock,
and selects CORE_DFS1 again. The PLL uses RDIV=2, MFI=80, and no fractional or
spread-spectrum modulation: VCO=1600 MHz, DFS1=800 MHz, M7=400 MHz.
This is crystal-referenced PLL operation, not direct 40 MHz M7 operation.

The configuration requires a boot-initialized CORE_DFS master and exclusive
ownership of CORE_PLL and XBAR clocks. An asserted DFS master reset is rejected;
this sample does not perform the boot owner's paired CORE/PERIPH DFS initialization.
Startup rejects enabled A53/other M7 cores, other enabled CORE_PLL outputs,
or other selected CORE_PLL consumers. These state checks cannot establish AMP
ownership; other firmware must not start a core or change these clocks concurrently.
No reset or power-management registers are written. Peripheral PLL, GMAC clock
muxes, and the UART clock selection are not modified by this configuration.
Polls are bounded iteration counts because SysTick does not yet exist.
On an error the application does not start tasks; XBAR may remain on FIRC.

Disable ``CONFIG_SAMPLE_S32G_CORE_PLL_FXOSC`` when boot firmware owns the clock
tree. Startup then only decodes the existing clock without reprogramming it.
Unsupported, unlocked, or failed configurations print an error and do not start tasks.
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
D-cache is disabled for GMAC bring-up. The M7 crystal configuration runs before
these application threads and does not change the existing UART clock selection.

Upper-board EEPROM Read Test
============================

The application runs a bounded, polled hardware I2C2 test before starting the
periodic application threads. Upper processor board S32G-PROCEVB-S Rev B4 has
U65 (AT24C01B/C, 128 bytes) on PB5/SCL and PB6/SDA. A0 is strapped high and
A1/A2 low, so the seven-bit address is 0x51. The input muxes are SIUL2_1 IMCR
207/208, while the pad output muxes are on SIUL2_0. Both pads use open-drain
outputs and the board's external pull-ups. I2C4 and the PMIC are not accessed.

``src/eeprom_read_test.c`` is a sample-local bring-up helper, not a general
Zephyr I2C driver. It requires the existing 400 MHz M7 clock. The controller
uses XBAR_DIV3_CLK and IBFD 0xa9 (divider 1536), giving a calculated bus rate
of approximately 86.8 kHz within Standard-mode. This rate has not been measured
with an oscilloscope. The test refuses to take over an already enabled I2C2
controller and bounds each status poll to 20 ms.

Each random read sends only the EEPROM word address, then a repeated START
and the read address. No EEPROM data bytes are transmitted for programming.
The final received byte is NACKed and followed by STOP. The test compares 100
full 128-byte reads with an initial baseline, then verifies single-byte reads
at offsets 0, 1, 7, 8, 63, and 127. Successful runs complete 107 transactions
and read 12934 bytes, with the bus idle after each transaction. The controller
is disabled afterward; the sample leaves the pinmux configured.

UART prints the baseline and an ``I2C2 U65 PASS`` or ``FAIL`` line. TRACE32 can
inspect ``sample_eeprom_result``: state 2 means pass, state 3 means fail. The
structure also records counts, the last status, and an FNV-1a checksum. A test
failure is reported but does not prevent the existing application threads
from starting. Reuse ``S32G_CM7_0_ZEPHYR_Load.cmm`` with the same first-project
ELF to repeat the boot test; no second application is needed.

The hardware read test passed on the connected board with checksum 0x77f9fbc7:
bytes 0 through 3 were 0x0a, 0x05, 0x0c, 0x0d; bytes 4 through 127 were 0xff.
These are observed contents, not an assumed factory image or an expected value
hard-coded into the test. This verifies address ACK, repeated START, sequential
read, final NACK/STOP, and repeatability. It does not verify write cycles,
write protection, retention, every memory cell, or Fast-mode. Other peripheral
functions were not regression-tested during this I2C bring-up.

Optional EEPROM Write Test
--------------------------

Normal boot leaves ``sample_eeprom_write_arm`` zero and performs no EEPROM
programming. A debugger may arm a one-shot read/write test by setting this RAM
variable to 0x45575254 at entry to ``sample_eeprom_write_test()``. The arm is
consumed before testing. Save the entire read-test baseline to the host before
arming; do not reset or remove power until restoration completes. A power loss
or unrecoverable bus fault during testing can leave the last page modified.

The write helper permits only offsets 0x78 through 0x7f, with at most eight
bytes per transaction and no page wrap. It never writes bytes 0 through 119.
The test backs up the complete array, checks that it still matches the host
baseline, and tests full-page patterns 0x00, 0xff, 0xaa, 0x55 and an
address-dependent pattern. It also tests single-byte writes at 0x78 and 0x7f.
Each STOP is followed by a 6 ms delay before readback, exceeding the
AT24C01B/C maximum 5 ms internal write cycle.

After success or a possibly partial failed write, the test restores the
original final page and reads the entire array for byte-for-byte comparison.
Restoration has at most three attempts. A successful test uses eight write
cycles and transmits 50 programming bytes, including restoration. These writes
consume EEPROM endurance; this is not a periodic task. Any test failure keeps
the existing application tasks stopped so that recovery can be investigated.
The RAM baseline and host backup remain available if restoration fails.

``verify_eeprom.py`` connects only to the local TRACE32 API and COM3. It uses the
existing warm-load CMM and first-project ELF, breaks at the write-test entry,
checks the read-only prerequisite and saves a new recovery JSON before arming.
Existing backup files are never overwritten. The verifier checks UART results,
debugger counters, and all restored bytes against its host backup. Supply the
installed 64-bit API DLL path and keep TRACE32's UDP remote API enabled:

.. code-block:: shell

   python samples/boards/nxp/s32g_vnp_evb/periodic_tasks/verify_eeprom.py \
     --api-dll E:/ruanjian/trace32/T32/demo/api/capi/dll/t32api64.dll \
     --write --backup build/s32g_vnp_evb_periodic_tasks/u65_before_write.json

Repeat without ``--write`` to prove that normal boot skips programming, and
optionally compare a fresh EEPROM read with the previous recovery image:

.. code-block:: shell

   python samples/boards/nxp/s32g_vnp_evb/periodic_tasks/verify_eeprom.py \
     --api-dll E:/ruanjian/trace32/T32/demo/api/capi/dll/t32api64.dll \
     --compare-backup build/s32g_vnp_evb_periodic_tasks/u65_before_write.json

The connected upper-board U65 passed five page-pattern checks and two
single-byte checks. All 128 original bytes were restored with checksum
0x77f9fbc7, and NACK, timeout and arbitration-loss counters remained zero.
The same ELF passed a subsequent unarmed boot and fresh whole-array comparison
with the host backup. Retention across power loss, hardware write protection,
Fast-mode, fault-injected recovery and other-peripheral regression were not
tested. This helper is not a general Zephyr I2C or EEPROM driver.

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
500 ms boundaries, missed periods, large expiration counts and the EEPROM
write-range guard, including page-end and oversized lengths. They do not
exercise Zephyr scheduling, concurrent event delivery, MMIO, or UART/GPIO drivers.
