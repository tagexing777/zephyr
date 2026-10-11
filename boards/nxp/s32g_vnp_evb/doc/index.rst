.. Copyright The Zephyr Project Contributors
.. SPDX-License-Identifier: Apache-2.0

.. zephyr:board:: s32g_vnp_evb

Overview
********

The S32G-VNP-EVB combines the S32G-PROCEVB-S processor board with the
S32GRV-PLATEVB carrier board. This initial port runs Zephyr on Cortex-M7_0
from the first 2 MiB of internal SRAM.

The other two Cortex-M7 cores are reserved for later AMP images. External
DDR, non-volatile boot, and Cortex-A53 operation are outside the scope of this
initial target.

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
