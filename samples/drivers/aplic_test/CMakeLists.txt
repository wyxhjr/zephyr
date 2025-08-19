# Copyright (c) 2024 Zephyr Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aplic_test)

# Choose test mode based on configuration
# Use advanced test when CONF_FILE contains "advanced"
if(CONFIG_ZTEST)
    target_sources(app PRIVATE src/main_comprehensive_test.c)
elseif(DEFINED CONF_FILE AND CONF_FILE MATCHES "advanced")
    target_sources(app PRIVATE src/main_advanced_test.c)
elseif(CONFIG_SMP)
    target_sources(app PRIVATE src/main_smp_verification.c)
else()
    target_sources(app PRIVATE src/main_verification.c)
endif()
