.. SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
.. SPDX-License-Identifier: Apache-2.0

S32G M7 Upper Board Ethernet
###########################

Overview
********

This sample provides IPv4 ICMP replies and a UDP echo server on port 4242 using
GMAC0 on S32G274A M7_0. It targets the original S32G-VNP-EVB, not EVB3.
Connect the cable to **J108 on the upper S32G-PROCEVB-S processor board**.
That connector uses U53 KSZ9031RNXCA at MDIO address 4. J108 on the lower board
and the upper board's J102 SGMII connector are not used.

The default processor-board routing places R579 through R590 in position A
and J129 in the 2-3 position for 3.3 V RGMII. Check the processor-board
schematic and the S32G-VNP-EVB user guide before changing any routing.

AMP Ownership
*************

GMAC0 is not a physically M7-only controller. This image reserves GMAC0,
upper J108, PHY address 4, PE2 through PE13, PD12/PD13, M7 interrupts 57/58/59,
DMA SRAM, peripheral PLL PHI5, and clock muxes 10/11 for M7_0.
An A53 image must not enable GMAC0 or PFE MAC1 RGMII on these same pins.
Other PFE interfaces are not configured by this sample.

The standalone overlay opts into initializing a powered-down peripheral PLL
from the 40 MHz FXOSC. An already-running PLL is checked without reconfiguration.
Remove ``nxp,initialize-periph-pll`` for a boot environment that owns the PLL
and initializes it before M7 startup. Keep the required 2 GHz PLL and
125 MHz PHI5 configuration and reserve PHI5 for GMAC.

The bring-up driver requires D-cache disabled. It limits RX and TX FIFO
allocations to the S32G2 usable capacity of 16 KiB per direction. Allocating
32 KiB from the hardware feature encoding stalls continuous reception on
the tested board. Hardware checksum offload, PTP, other DMA channels,
10/100 Mbps operation, and simultaneous A53 operation have not been validated.

Build and Load
**************

From a configured west workspace, build with:

.. code-block:: console

   west build -b s32g_vnp_evb/s32g274a/m7_0 -d build/s32g_vnp_evb_ethernet samples/boards/nxp/s32g_vnp_evb/ethernet

Use ``boards/nxp/s32g_vnp_evb/support/S32G_CM7_0_ZEPHYR_Load.cmm`` with the
absolute path to ``build/s32g_vnp_evb_ethernet/zephyr/zephyr.elf`` as its first
argument. The loader stops at ``main``; execute ``Go`` to run the application.
Open COM3 at 115200 baud, 8N1, without flow control, to capture its output.

.. warning::

   Without a second argument, the loader resets the target, initializes both
   SRAM banks, and disables SWT0 through SWT7. Only use this mode with A53 and
   other cores stopped. It does not preserve another core's application.

For an already-connected M7_0 debugger with initialized SRAM and exclusive
GMAC0 ownership, append ``WARM`` as the second argument:

.. code-block:: text

   DO "<zephyr>/boards/nxp/s32g_vnp_evb/support/S32G_CM7_0_ZEPHYR_Load.cmm" "<zephyr>/build/s32g_vnp_evb_ethernet/zephyr/zephyr.elf" "WARM"

``WARM`` stops GMAC0 DMA before replacing the image. It does not reset the
chip, initialize SRAM banks, or disable shared watchdogs. It still overwrites
the M7_0 image's SRAM; no other core may use that memory. This mode does not
establish resource ownership or an AMP isolation policy by itself.

Verify
******

Connect a dedicated PC Ethernet adapter directly to upper J108. Set only
that adapter to ``192.168.10.1/24``, without a default gateway. The target uses
``192.168.10.2/24`` and MAC address ``02:00:00:27:4a:00``. No DHCP is used.
Wait for the UART message ``Link speed 1000 Mb, full duplex``.

.. code-block:: console

   ping 192.168.10.2
   python samples/boards/nxp/s32g_vnp_evb/ethernet/verify_echo.py

The verifier checks 600 datagrams with payload sizes 1, 60, 128, 512, 1024,
and 1472 bytes, including the full returned payload and peer address.
The UART prints cumulative echo packet and byte counts every 100 packets.
The console harness verifies startup only; it is not an Ethernet traffic test.

UDP Log Task and Interface
**************************

``task_udp_log_10ms`` waits for the initial PHY link, then starts a periodic
10 ms kernel timer and queues one
formatted message. A separate sender thread transmits it to ``192.168.10.1:4243``
from local UDP port 4243. The echo service on port 4242 continues independently.
The producer and sender use priorities 4 and 5, above the echo thread at priority 6.
Each log contains the task name, activation count, target uptime in milliseconds,
and missed timer periods. Change ``LOG_HOST`` and ``LOG_PORT`` in ``src/main.c``
to select another destination on the test network.

The application-level interface is declared in ``src/udp_log.h``:

.. code-block:: c

   int32_t ret = udp_log_init("192.168.10.1", 4243U);

   if (ret == 0) {
           ret = udp_log_printf("task_10ms count=%u value=%d\n", 10U, -3);
           if (ret != 0) {
                   /* Record the queue or formatting failure. */
           }
   }

Initialize once from a thread after configuring IPv4 and before starting log
producers. The sample already performs this initialization. Multiple threads may
call ``udp_log_printf()`` concurrently; it is not supported in interrupt context.
The call copies its formatted text into a static 128-entry queue without waiting
for the socket. Each message is limited to 255 bytes and becomes one datagram;
no NUL terminator or implicit newline is transmitted. Oversized messages return
``-EMSGSIZE`` without truncation; a full queue returns ``-ENOBUFS``.
An accepted message returns zero, which means queued, not delivered. The queue
covers 1.28 seconds at the sample's 100 Hz rate and occupies about 32 KiB.

``udp_log_get_stats()`` samples queued, locally sent, dropped, and transport-error
counts. Counters are sampled independently and wrap after 32-bit overflow.
Failed sends are dropped without retries. Startup before PHY link, an unplugged
cable, or resource pressure can therefore increase the drop and error counters.
The periodic task also exposes ``udp_log_task_count``, ``udp_log_task_missed``,
and ``udp_log_task_errors`` for debugging.

The 10 ms period is the producer's target schedule, not a guarantee of packet
arrival timing or delivery. UDP and PC scheduling can cause loss or jitter.
There is no change to the global Zephyr logging backend or UART output.

Receive the log with Python on the PC; allow the receiver through the firewall
on this dedicated adapter if necessary:

.. code-block:: console

   python samples/boards/nxp/s32g_vnp_evb/ethernet/receive_log.py
   python samples/boards/nxp/s32g_vnp_evb/ethernet/receive_log.py --count 1000 --quiet --verify

The first command displays logs until Ctrl+C. The second verifies 1000 consecutive
task messages, target uptime steps of 9 through 11 ms, and no newly missed timer
periods. It reports average PC arrival spacing separately. Only one receiver
should bind the PC port at a time.
