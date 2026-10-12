# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(s32g_periodic_tasks)

target_sources(app PRIVATE
  src/main.c
  src/clock.c
  src/clock_setup.c
  src/can_loopback.c
  src/eeprom_read_test.c
  src/network.c
  src/udp_log.c
)
