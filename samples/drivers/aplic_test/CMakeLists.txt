# Copyright (c) 2024 Zephyr Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(aia_test)

# Add the main source file
target_sources(app PRIVATE src/main_aia_simple.c)

# Add include directories for AIA headers
target_include_directories(app PRIVATE ${ZEPHYR_BASE}/include/zephyr/drivers/interrupt_controller)
