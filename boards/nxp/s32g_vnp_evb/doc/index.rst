.. Copyright The Zephyr Project Contributors
.. SPDX-License-Identifier: Apache-2.0

.. zephyr:board:: s32g_vnp_evb

Overview
********

The S32G-VNP-EVB combines the S32G-PROCEVB-S processor board with the
S32GRV-PLATEVB carrier board. Each Cortex-M7 has an independent AMP image.
The first 2 MiB of internal SRAM is partitioned into non-overlapping regions:

.. list-table:: AMP image ownership
   :header-rows: 1

   * - Core
     - SRAM region
     - Assigned external resources
   * - M7_0
     - ``0x34000000..0x340fffff`` (1 MiB)
     - D15/PB8, UART, GMAC0, I2C2 EEPROM, FlexCAN0 loopback
   * - M7_1
     - ``0x34100000..0x3417ffff`` (512 KiB)
     - D14/PC13 with J164 in position 1-2, or none in the no-LED image
   * - M7_2
     - ``0x34180000..0x341fffff`` (512 KiB)
     - None

This layout is a software allocation, not an XRDC-enforced access policy.
Power domains and the shared clock tree are not isolated between the M7 cores.
External DDR, non-volatile boot, and Cortex-A53 operation remain outside this port.

Programming and Debugging
*************************

Connect an NXP S32 Debug Probe to J64 and connect the J58 USB UART. Configure
the board jumpers to route PC_09 and PC_10 to the USB UART.

Build ``hello_world`` for the M7_0 target with:

.. zephyr-app-commands::
   :zephyr-app: samples/hello_world
   :board: s32g_vnp_evb/s32g274a/m7_0
   :goals: build

The UART configuration requires a 40 MHz LIN baud-rate clock from FXOSC.
The LINFlexD functional clock is separate from the baud-rate clock and must
not be used to calculate the UART divisor. Verify that MC_CGM_0 MUX_8 selects
FXOSC before running the image. The console uses 115200 baud, eight data bits,
no parity, and one stop bit.

The current target declares a 48 MHz M7 clock. Its frequency must match the
clock tree established by the boot firmware or debugger for correct kernel
timing. A complete clock initialization driver is still required before this
target can boot independently or use a different clock configuration.

Periodic Tasks and GPIO LED
==========================

The :zephyr:code-sample:`s32g-periodic-tasks` sample adds 1/10/100 ms tasks and
a counted 5/10/20 ms event-driven task. Its startup code reads the existing
M7 clock tree before initializing SysTick, instead of relying on the default
48 MHz declaration. This does not provide a full SoC clock initialization driver.

The ``led0`` alias maps to processor-board green LED D15 on GPIO24/PB8,
active-low. The sample uses the existing SIUL2 GPIO driver. PB8 and the GPIO bank
must be owned exclusively by the M7_0 image during this test.

Build into ``build/s32g_vnp_evb_periodic_tasks`` and use
``support/S32G_CM7_0_ZEPHYR_Load.cmm`` to load that image. The reset
and other-core restrictions of the common loader below also apply.

Secondary AMP Tasks
===================

The ``samples/boards/nxp/s32g_vnp_evb/amp_tasks`` application builds separately
for ``s32g_vnp_evb/s32g274a/m7_1`` and ``s32g_vnp_evb/s32g274a/m7_2``.
Both cores run 1/5/10/20 ms tasks with core-specific global counters.
The default M7_1 image sets ``CONFIG_SAMPLE_S32G_AMP_LED=y`` and toggles D14
every 500 ms from its 1 ms task. Confirm J164 in position 1-2 before loading it.
When J164 is unknown, ``no_led.conf`` disables that option and ``CONFIG_GPIO``.
Build this variant in ``build/s32g_vnp_evb_m7_1_no_led`` to preserve the default
LED-enabled image in ``build/s32g_vnp_evb_m7_1``. M7_2 has no GPIO driver.
The secondary images have no UART, console, network, I2C, CAN, or watchdog driver.
They read the shared 400 MHz clock without changing its configuration.

Read :zephyr_file:`samples/boards/nxp/s32g_vnp_evb/amp_tasks/README.rst` for
build commands and static ELF checks. Use ``verify_images.py --skip-led``
with the explicit no-LED M7_1 ELF when validating that variant. Rebuild M7_0
with the partitioned layout before using either secondary image.
Load M7_0 through ``S32G_CM7_0_ZEPHYR_Load.cmm`` and run it first. After its
``sample_amp_primary_ready`` flag is ``0x414d5030``, the separate
``S32G_CM7_AMP_Start.cmm`` script loads and releases the secondary images without
resetting the chip or reinitializing SRAM. Its third argument is ``NO_LED`` for
the GPIO-free image or ``D14_J164_1_2`` after physical jumper confirmation.
Both modes require a fourth argument: the actual M7_1 ``sample_amp_led_config``
address printed by ``verify_images.py`` for that ELF. The API wrapper passes
the verified address automatically; it is preferred over a manual script call.

The no-LED M7_1 and M7_2 images were verified running simultaneously with M7_0
at 400 MHz. Their 1/5/10/20 ms tasks advanced at the expected rates. A ten-second
``verify_onboard.py`` report passed all 133 checks with no coalesced expirations;
M7_0 UART logs, periodic tasks, counted events, UDP logs, and UDP echo continued.
The primary boot-time CAN0 internal loopback passed 1000 frames, and the EEPROM
test was read-only without an armed write test. D14 remains unverified because
J164 was not physically confirmed; LED counters do not prove visible blinking.

``verify_onboard.py`` defaults to the no-LED M7_1 build and observes all three
cores without debugger halt, reset, download, or target-memory writes. To check
a subsequently loaded default LED-enabled image, supply ``--no-skip-led`` and
``--m7-1 build/s32g_vnp_evb_m7_1/zephyr/zephyr.elf``. The sample README contains
the API DLL and report-command examples.

After all cores have been started, power-cycle the board
before restarting the primary clock owner; simply halting the other cores leaves
their clock status active and causes the primary initialization to refuse startup.

TRACE32 SRAM Download
=====================

The ``support/S32G_CM7_0_ZEPHYR_Load.cmm`` script configures the master
debugger for M7_0 at a 1 MHz JTAG clock. It attempts ``SYStem.Up`` and calls
the installed ``s32g2_connect_m7_0.cmm`` helper if the direct connection fails.
The TRACE32 ``SYS`` setting must identify an installation containing the
``demo/arm/hardware/s32g2/scripts`` directory.

From the TRACE32 command line, run the following command with the repository
path substituted for ``<zephyr>``:

.. code-block:: text

   DO "<zephyr>/boards/nxp/s32g_vnp_evb/support/S32G_CM7_0_ZEPHYR_Load.cmm"

The default image is
``<zephyr>/build/s32g_vnp_evb_periodic_tasks/zephyr/zephyr.elf``.
An absolute ELF path can be supplied as the first script argument. The image
must place its vector table at ``0x34000000`` and provide a ``main`` symbol.
The script reads the initial stack pointer and reset entry from the vector
table, starts the reset handler, and stops at ``main``. Open the UART at
115200 baud, 8N1, without flow control, then execute ``Go`` to see the sample
output. Open the UART before running the script to capture the boot banner.

To load the hello_world image instead, supply
``<zephyr>/build/s32g_vnp_evb_m7_0/zephyr/zephyr.elf`` as the first argument.

.. warning::

   This procedure resets the target, initializes both SRAM banks, and disables
   internal SWT0 through SWT7. It is for dedicated M7_0 bring-up and does not
   preserve Cortex-A53 or other core operation. It does not program Flash.
   If the AXI connection fails, power-cycle the board before retrying.
