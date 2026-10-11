# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

board_runner_args(nxp_s32dbg
  "--core-name" "M7_0"
  "--soc-name" "S32G274A"
  "--soc-family-name" "s32g2xx"
)

include(${ZEPHYR_BASE}/boards/common/nxp_s32dbg.board.cmake)
