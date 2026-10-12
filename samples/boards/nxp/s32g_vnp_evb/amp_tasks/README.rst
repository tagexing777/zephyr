..
   Copyright The Zephyr Project Contributors
   SPDX-License-Identifier: Apache-2.0

S32G274A secondary M7 AMP tasks
###############################

Overview
********

This application produces independent Zephyr images for M7_1 and M7_2 on the
original S32G-VNP-EVB with the S32G-PROCEVB-S Rev B4 upper board, not EVB3.
Each image has its own kernel, vector table, stack, timers, and SRAM partition.
It is AMP, not SMP. M7_0 continues to use the ``periodic_tasks`` application.

Both secondary cores run four tasks at 1, 5, 10, and 20 ms, with priorities
0, 1, 2, and 3 respectively. Each task has a globally visible atomic counter.
The counters add elapsed timer periods, including expirations coalesced while
a task was delayed. Separate ``*_missed`` counters add expirations beyond the
first in each wakeup; they are not additional tasks.

By default, ``CONFIG_SAMPLE_S32G_AMP_LED=y`` on M7_1. This image owns
upper-board D14 on PC13 and toggles it every 500 ms from the 1 ms task.
A full on/off cycle takes 1 second. J164 must be fitted in position 1-2
to connect D14 to PC13; position 2-3 selects the RCON boot-control signal.
Do not load an LED-enabled M7_1 image until the actual jumper position is
confirmed. If J164 is unknown, build the separate no-LED image described
below. M7_0 retains D15 on PB8. M7_2 owns no GPIO or LED.

The secondary images do not enable UART, console, logging, Ethernet, UDP,
I2C, EEPROM, CAN, pinctrl, or watchdog drivers. They do not configure shared
PLL, DFS, or clock mux registers. Their per-core SysTick is the only periodic
timer resource used. The shared clock reader is built with clock setup
disabled; it reads the inherited clock tree before SysTick initialization.
Tasks remain stopped unless the detected M7 clock is exactly 400 MHz.

Building
********

With the existing west workspace and toolchain configured, use PowerShell
from the Zephyr checkout to build into three distinct directories::

   $primary = "samples/boards/nxp/s32g_vnp_evb/periodic_tasks"
   $secondary = "samples/boards/nxp/s32g_vnp_evb/amp_tasks"
   west build -b s32g_vnp_evb/s32g274a/m7_0 $primary -d build/s32g_vnp_evb_periodic_tasks
   west build -b s32g_vnp_evb/s32g274a/m7_1 $secondary -d build/s32g_vnp_evb_m7_1
   west build -b s32g_vnp_evb/s32g274a/m7_2 $secondary -d build/s32g_vnp_evb_m7_2

Each directory contains its own ``zephyr/zephyr.elf``. Rebuild M7_0 as well
so all three images use the non-overlapping SRAM layout. Do not combine an
older M7_0 image that spans the whole SRAM with the secondary images.

GPIO-free M7_1 image
====================

When J164 has not been checked, build an additional M7_1 image using
``no_led.conf``. It sets ``CONFIG_SAMPLE_S32G_AMP_LED=n`` and ``CONFIG_GPIO=n``
without changing the default LED-enabled build::

   west build -b s32g_vnp_evb/s32g274a/m7_1 $secondary `
     -d build/s32g_vnp_evb_m7_1_no_led -- '-DEXTRA_CONF_FILE=no_led.conf'

The no-LED ELF is ``build/s32g_vnp_evb_m7_1_no_led/zephyr/zephyr.elf``.
Keep ``build/s32g_vnp_evb_m7_1`` for the default LED-enabled image; the two
build directories must remain separate. M7_2 needs no additional configuration.

Loading and resource ownership
******************************

First run ``verify_images.py`` in this sample directory against all three
builds. It checks image placement and configurations without touching the
target. SRAM partitioning prevents image overlap; it does not enforce a
hardware access reservation against arbitrary future applications. XRDC
permissions, power domains, and the shared clock tree are not isolated by
these images. M7_0 remains the shared clock owner.

From the Zephyr checkout, the default build directories can be checked with::

   python samples/boards/nxp/s32g_vnp_evb/amp_tasks/verify_images.py

Check the no-LED M7_1 image with ``--skip-led`` and its explicit path::

   $verify = "samples/boards/nxp/s32g_vnp_evb/amp_tasks/verify_images.py"
   $noLedElf = "build/s32g_vnp_evb_m7_1_no_led/zephyr/zephyr.elf"
   python $verify --skip-led --m7-1 $noLedElf

``--skip-led`` requires GPIO to be disabled in the M7_1 configuration and
the file-backed ``sample_amp_led_config`` value to identify the no-LED image.
It does not bypass image placement, core identity, or other ownership checks.

Load M7_0 with ``S32G_CM7_0_ZEPHYR_Load.cmm`` and run it first. Wait until its
``sample_amp_primary_ready`` variable becomes ``0x414d5030``. Only then use
``S32G_CM7_AMP_Start.cmm`` to load and release the two secondary cores.
The secondary loader does not reset the chip, reinitialize SRAM banks,
or change shared clocks while M7_0 is running.

The recommended API wrapper validates the ELF files and passes the correct
flag address automatically. Replace ``<trace32>`` with the installation directory.
To start the no-LED image without acknowledging an unknown jumper::

   $apiDll = "<trace32>/demo/api/capi/dll/t32api64.dll"
   python $verify --skip-led --m7-1 $noLedElf --start --api-dll $apiDll

After physically confirming J164 in position 1-2, use the default
LED-enabled M7_1 build instead::

   python $verify --start --confirm-led-jumper --api-dll $apiDll

Neither command loads M7_0. ``--confirm-led-jumper`` acknowledges the physical
jumper position rather than detecting it electronically. Both commands refuse
to overwrite an already enabled secondary core.

For a manual TRACE32 call, run the static checker first and copy the actual
``sample_amp_led_config`` address printed for M7_1. The address depends on the
ELF and must be obtained again after each rebuild. Supply all four arguments
in quotes; there is no default fourth argument. For the no-LED image::

   CD "<zephyr>/boards/nxp/s32g_vnp_evb/support"
   DO "S32G_CM7_AMP_Start.cmm" "<M7_1 ELF>" "<M7_2 ELF>" "NO_LED" "<flag address>"

Replace the ELF placeholders with absolute paths and ``<flag address>`` with
the verified hexadecimal address, not the symbol name or a stale example.
For the default LED image, use ``D14_J164_1_2`` instead of ``NO_LED`` only
after checking J164. The script writes an invalid sentinel to the flag before
download, then requires ``0x4e4c4544`` for ``NO_LED`` or ``0x4c454431`` for
``D14_J164_1_2`` before enabling either secondary core clock or releasing reset.

Never use the original M7_0 cold-loading path while a secondary core is
running. To restart all images, power-cycle the board and repeat the
primary-first sequence. Merely halting or resetting a secondary core does not
disable its MC_ME clock status, so the primary clock initialization will refuse
to proceed. A single-core debugger halt does not stop the other cores or their timers.

Debugger checks
***************

The same status symbol names exist independently in each ELF; select the
correct core and image when inspecting them:

* ``sample_amp_core_id`` is 1 or 2.
* ``sample_amp_tasks_started`` becomes 1 after all four threads are started.
* ``sample_amp_error`` remains 0 on successful startup.
* ``sample_core_clock_hz`` is 400000000.
* ``sample_clock_error`` remains 0.
* ``sample_amp_led_config`` is ``0x4c454431`` for an LED-enabled M7_1 image,
  or ``0x4e4c4544`` for a no-LED image.

M7_1 exports ``m7_1_task_1ms_cnt``, ``m7_1_task_5ms_cnt``,
``m7_1_task_10ms_cnt``, and ``m7_1_task_20ms_cnt``. M7_2 exports the same four
names with the ``m7_2_`` prefix. Each also exports corresponding
``m7_N_task_PERIOD_missed`` counters. Over a steady one-second interval,
the expected counter increases are approximately 1000, 200, 100, and 50.
The counters wrap modulo 2^32.

An LED-enabled M7_1 image additionally exports ``m7_1_led_toggles`` and
``m7_1_led_error``. The first increases approximately twice per second and
the second remains 0.
When late wakeups cross multiple LED boundaries, the thread applies only
the final LED state; it does not replay past transitions in a tight loop.
The no-LED M7_1 image and M7_2 have no LED toggle/error symbols or GPIO driver.

The Twister metadata is build-only because neither secondary image has a
console.

Read-only onboard report
************************

With all three cores already running, collect a ten-second report from the
Zephyr checkout::

   python samples/boards/nxp/s32g_vnp_evb/amp_tasks/verify_onboard.py `
     --api-dll "<trace32>/demo/api/capi/dll/t32api64.dll" --seconds 10 `
     --report build/amp_onboard_report.json

The tool defaults to the no-LED M7_1 ELF in ``build/s32g_vnp_evb_m7_1_no_led``
and ``--skip-led``. It reads numeric EAXI addresses without halting a core,
loading an ELF, resetting the target, or writing target memory. It also
receives COM3 UART and UDP logs and sends ten UDP echo packets. Close other
COM3 readers and UDP log receivers before running it. The defaults are
115200 baud, host ``192.168.10.1``, and target ``192.168.10.2``.

After loading the default LED-enabled image with J164 confirmed, select
its matching ELF and enable the LED counter checks::

   python samples/boards/nxp/s32g_vnp_evb/amp_tasks/verify_onboard.py `
     --api-dll "<trace32>/demo/api/capi/dll/t32api64.dll" --no-skip-led `
     --m7-1 build/s32g_vnp_evb_m7_1/zephyr/zephyr.elf --seconds 10

LED counter evidence does not establish that the physical LED was observed.
The report checks retained boot-time CAN and EEPROM results; it does not
repeat those peripheral tests or arm the EEPROM write test.

Hardware validation
*******************

Simultaneous M7_0, M7_1, and M7_2 execution was verified on the original
S32G-VNP-EVB on October 12, 2026, using the no-LED M7_1 image. Both secondary
cores reported 400 MHz, the correct core identity, started tasks, and no
startup error. Their 1/5/10/20 ms counters advanced at approximately
1000/200/100/50 counts per second.

The ten-second onboard report passed all 133 checks with zero coalesced
expiration deltas across all three cores. M7_0 periodic tasks, counted events,
UART logs, approximately 100 Hz UDP logs, and UDP echo remained operational.
Its boot-time FlexCAN0 internal loopback passed 1000 frames. The EEPROM
test was read-only; no EEPROM write test was armed or performed in this run.

D14 operation is not hardware-validated because J164 was not physically
confirmed. The no-LED result does not validate the LED-enabled M7_1 image
on the board, physical D14 blinking, non-volatile boot, or long-duration operation.
