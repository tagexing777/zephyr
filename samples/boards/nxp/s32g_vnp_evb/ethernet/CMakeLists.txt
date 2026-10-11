# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(s32g_upper_ethernet)

target_sources(app PRIVATE
  src/main.c
  ../periodic_tasks/src/udp_log.c
  ../periodic_tasks/src/clock.c
)

target_include_directories(app PRIVATE
  ../periodic_tasks/src
)
