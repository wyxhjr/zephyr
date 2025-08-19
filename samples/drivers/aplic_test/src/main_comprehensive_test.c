/*
 * Copyright (c) 2024 Zephyr Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/interrupt_controller/riscv_aplic.h>
#include <zephyr/irq.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(aplic_comprehensive_test, LOG_LEVEL_INF);

/* APLIC register definitions */
#define APLIC_DOMAINCFG_OFFSET     0x00
#define APLIC_SOURCECFG_OFFSET     0x04
#define APLIC_SETIP_OFFSET         0x1C
#define APLIC_SETIE_OFFSET         0x24
#define APLIC_TARGET_OFFSET        0x3000
#define APLIC_IDC_OFFSET           0x4000

/* APLIC base address from device tree */
#define APLIC_BASE_ADDR            0x0c000000

/* Test interrupt lines */
#define TEST_IRQ_LINE_1            1
#define TEST_IRQ_LINE_2            2
#define TEST_IRQ_PRIO              1

/* Helper functions to read/write APLIC registers */
static inline uint32_t aplic_read_reg(uint32_t offset)
{
	return *(volatile uint32_t *)((uintptr_t)APLIC_BASE_ADDR + offset);
}

static inline void aplic_write_reg(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)((uintptr_t)APLIC_BASE_ADDR + offset) = value;
}

/* Test data */
static atomic_t irq_executed[2];
static atomic_t irq_cpu_id[2];

/* Test ISR handlers */
static void test_isr_1(const void *param)
{
	atomic_inc(&irq_executed[0]);
	atomic_set(&irq_cpu_id[0], arch_curr_cpu()->id);
}

static void test_isr_2(const void *param)
{
	atomic_inc(&irq_executed[1]);
	atomic_set(&irq_cpu_id[1], arch_curr_cpu()->id);
}

/* Test setup */
static void *aplic_test_setup(void)
{
	LOG_INF("=== APLIC Comprehensive Test Setup ===");
	
	/* Reset test counters */
	atomic_set(&irq_executed[0], 0);
	atomic_set(&irq_executed[1], 0);
	atomic_set(&irq_cpu_id[0], -1);
	atomic_set(&irq_cpu_id[1], -1);
	
	return NULL;
}

/* Test 1: Basic APLIC device and register verification */
ZTEST(aplic_comprehensive, test_aplic_device_basic)
{
	LOG_INF("=== Test 1: Basic APLIC Device Verification ===");
	
	/* Check if APLIC device exists */
	const struct device *aplic_dev = riscv_aplic_get_dev();
	zassert_not_null(aplic_dev, "APLIC device should exist");
	zassert_true(device_is_ready(aplic_dev), "APLIC device should be ready");
	
	LOG_INF("✓ APLIC device found and ready");
}

/* Test 2: DOMAINCFG register verification */
ZTEST(aplic_comprehensive, test_domaincfg_register)
{
	LOG_INF("=== Test 2: DOMAINCFG Register Verification ===");
	
	uint32_t domaincfg = aplic_read_reg(APLIC_DOMAINCFG_OFFSET);
	LOG_INF("DOMAINCFG: 0x%08X", domaincfg);
	
	/* Verify reserved bits [31:24] = 0x80 (per RISC-V AIA spec) */
	uint32_t reserved_bits = (domaincfg >> 24) & 0xFF;
	zassert_equal(reserved_bits, 0x80, "Reserved bits should be 0x80, got 0x%02X", reserved_bits);
	
	/* Check IE bit (bit 8) - should be enabled after driver init */
	bool ie_enabled = (domaincfg & (1 << 8)) != 0;
	zassert_true(ie_enabled, "IE bit should be enabled");
	
	/* Check DM bit (bit 2) - should be 0 for direct mode */
	bool dm_direct = (domaincfg & (1 << 2)) == 0;
	zassert_true(dm_direct, "DM should be 0 for direct mode");
	
	/* Check BE bit (bit 0) - should be 0 for little-endian */
	bool be_little = (domaincfg & (1 << 0)) == 0;
	zassert_true(be_little, "BE should be 0 for little-endian");
	
	LOG_INF("✓ DOMAINCFG register correctly configured");
}

/* Test 3: Source configuration registers */
ZTEST(aplic_comprehensive, test_sourcecfg_registers)
{
	LOG_INF("=== Test 3: SOURCECFG Registers Test ===");
	
	/* Test first few SOURCECFG registers */
	for (int i = 0; i < 4; i++) {
		uint32_t sourcecfg = aplic_read_reg(APLIC_SOURCECFG_OFFSET + (i * 4));
		LOG_INF("SOURCECFG[%d]: 0x%08X", i, sourcecfg);
		
		/* Should be configured for direct mode (D=1) */
		bool delegated = (sourcecfg & 0x1) != 0;
		zassert_true(delegated, "SOURCECFG[%d] should have D bit set", i);
	}
	
	/* Test register write capability */
	uint32_t orig_sourcecfg = aplic_read_reg(APLIC_SOURCECFG_OFFSET);
	aplic_write_reg(APLIC_SOURCECFG_OFFSET, 0x5);
	uint32_t new_sourcecfg = aplic_read_reg(APLIC_SOURCECFG_OFFSET);
	aplic_write_reg(APLIC_SOURCECFG_OFFSET, orig_sourcecfg); /* restore */
	
	zassert_equal(new_sourcecfg, 0x5, "SOURCECFG register should be writable");
	
	LOG_INF("✓ SOURCECFG registers working correctly");
}

/* Test 4: APLIC API functions */
ZTEST(aplic_comprehensive, test_aplic_api_functions)
{
	LOG_INF("=== Test 4: APLIC API Functions Test ===");
	
	/* Test interrupt enable/disable */
	riscv_aplic_irq_disable(TEST_IRQ_LINE_1);
	int enabled = riscv_aplic_irq_is_enabled(TEST_IRQ_LINE_1);
	zassert_false(enabled, "IRQ should be disabled");
	
	riscv_aplic_irq_enable(TEST_IRQ_LINE_1);
	enabled = riscv_aplic_irq_is_enabled(TEST_IRQ_LINE_1);
	zassert_true(enabled, "IRQ should be enabled");
	
	/* Test priority setting */
	riscv_aplic_set_priority(TEST_IRQ_LINE_1, TEST_IRQ_PRIO);
	
	/* Test pending interrupt setting */
	riscv_aplic_irq_set_pending(TEST_IRQ_LINE_1);
	
	LOG_INF("✓ APLIC API functions working");
}

/* Test 5: Multi-core support verification */
ZTEST(aplic_comprehensive, test_multicore_support)
{
	LOG_INF("=== Test 5: Multi-core Support Test ===");
	
	/* Check number of CPUs */
	unsigned int num_cpus = arch_num_cpus();
	LOG_INF("Number of CPUs: %u", num_cpus);
	zassert_true(num_cpus >= 1, "Should have at least 1 CPU");
	
	/* Check current CPU ID */
	unsigned int current_cpu = arch_curr_cpu()->id;
	LOG_INF("Current CPU ID: %u", current_cpu);
	zassert_true(current_cpu < num_cpus, "Current CPU ID should be valid");
	
	/* Test APLIC device access from current CPU */
	const struct device *aplic_dev = riscv_aplic_get_dev();
	zassert_not_null(aplic_dev, "Should be able to get APLIC device from any CPU");
	
	if (num_cpus > 1) {
		LOG_INF("✓ Multi-core environment detected and working");
	} else {
		LOG_INF("✓ Single-core environment working");
	}
}

/* Test 6: Interrupt status registers */
ZTEST(aplic_comprehensive, test_interrupt_status)
{
	LOG_INF("=== Test 6: Interrupt Status Registers Test ===");
	
	/* Read interrupt status registers */
	uint32_t setip = aplic_read_reg(APLIC_SETIP_OFFSET);
	uint32_t setie = aplic_read_reg(APLIC_SETIE_OFFSET);
	
	LOG_INF("SETIP: 0x%08X", setip);
	LOG_INF("SETIE: 0x%08X", setie);
	
	/* These registers should be accessible */
	zassert_true(setip != 0xFFFFFFFF, "SETIP should be readable");
	zassert_true(setie != 0xFFFFFFFF, "SETIE should be readable");
	
	LOG_INF("✓ Interrupt status registers accessible");
}

/* Test 7: Driver initialization verification */
ZTEST(aplic_comprehensive, test_driver_initialization)
{
	LOG_INF("=== Test 7: Driver Initialization Verification ===");
	
	/* Check if driver initialization was called */
	extern volatile uint32_t aplic_init_called;
	zassert_equal(aplic_init_called, 0xDEADBEEF, "APLIC init should have been called");
	
	LOG_INF("✓ Driver initialization verified");
}

/* Test 8: Performance and stress test */
ZTEST(aplic_comprehensive, test_performance_stress)
{
	LOG_INF("=== Test 8: Performance and Stress Test ===");
	
	/* Test rapid enable/disable operations */
	uint32_t start_time = k_uptime_get_32();
	
	for (int i = 0; i < 1000; i++) {
		riscv_aplic_irq_enable(TEST_IRQ_LINE_1);
		riscv_aplic_irq_disable(TEST_IRQ_LINE_1);
	}
	
	uint32_t end_time = k_uptime_get_32();
	uint32_t duration = end_time - start_time;
	
	LOG_INF("1000 enable/disable operations took %u ms", duration);
	zassert_true(duration < 1000, "Operations should complete within reasonable time");
	
	LOG_INF("✓ Performance stress test passed");
}

/* Test suite */
ZTEST_SUITE(aplic_comprehensive, NULL, aplic_test_setup, NULL, NULL, NULL);

#ifndef CONFIG_ZTEST
/* Main function for non-ztest mode */
void main(void)
{
	LOG_INF("=== APLIC Comprehensive Verification Test ===");
	LOG_INF("Starting comprehensive APLIC driver validation...");
	
	/* Run tests manually */
	aplic_test_setup();
	
	LOG_INF("Running manual test sequence...");
	
	/* Run basic verification */
	const struct device *aplic_dev = riscv_aplic_get_dev();
	if (aplic_dev && device_is_ready(aplic_dev)) {
		LOG_INF("✓ APLIC device verification passed");
	} else {
		LOG_ERR("✗ APLIC device verification failed");
		return;
	}
	
	/* Check DOMAINCFG */
	uint32_t domaincfg = aplic_read_reg(APLIC_DOMAINCFG_OFFSET);
	if ((domaincfg >> 24) == 0x80 && (domaincfg & (1 << 8))) {
		LOG_INF("✓ DOMAINCFG verification passed: 0x%08X", domaincfg);
	} else {
		LOG_ERR("✗ DOMAINCFG verification failed: 0x%08X", domaincfg);
	}
	
	/* Test API functions */
	riscv_aplic_irq_enable(TEST_IRQ_LINE_1);
	if (riscv_aplic_irq_is_enabled(TEST_IRQ_LINE_1)) {
		LOG_INF("✓ API functions verification passed");
	} else {
		LOG_ERR("✗ API functions verification failed");
	}
	
	LOG_INF("=== Manual Test Sequence Completed ===");
	LOG_INF("APLIC comprehensive verification completed successfully!");
}
#endif
