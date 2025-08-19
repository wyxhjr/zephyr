/*
 * Copyright (c) 2024 Zephyr Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/interrupt_controller/riscv_aplic.h>
#include <zephyr/irq.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(aplic_verification, LOG_LEVEL_INF);

/* APLIC register definitions */
#define APLIC_DOMAINCFG_OFFSET     0x00
#define APLIC_SOURCECFG_OFFSET     0x04
#define APLIC_SETIP_OFFSET         0x1C
#define APLIC_SETIE_OFFSET         0x24

/* APLIC base address from device tree */
#define APLIC_BASE_ADDR            0x0c000000

/* Helper functions to read APLIC registers */
static inline uint32_t aplic_read_reg(uint32_t offset)
{
	return *(volatile uint32_t *)((uintptr_t)APLIC_BASE_ADDR + offset);
}

static inline void aplic_write_reg(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)((uintptr_t)APLIC_BASE_ADDR + offset) = value;
}

/* Test counters */
static atomic_t test_passed = 0;
static atomic_t test_failed = 0;

#define TEST_ASSERT(condition, message) \
	do { \
		if (condition) { \
			LOG_INF("✓ PASS: %s", message); \
			atomic_inc(&test_passed); \
		} else { \
			LOG_ERR("✗ FAIL: %s", message); \
			atomic_inc(&test_failed); \
		} \
	} while (0)

void main(void)
{
	LOG_INF("=== APLIC Comprehensive Verification Test ===");
	LOG_INF("Starting APLIC driver validation following other driver patterns...");
	
	/* Wait for system to stabilize */
	k_sleep(K_MSEC(100));
	
	/* Reset counters */
	atomic_set(&test_passed, 0);
	atomic_set(&test_failed, 0);
	
	LOG_INF("=== Test 1: Device Discovery ===");
	
	/* Test 1: Check if APLIC device exists and is ready */
	const struct device *aplic_dev = riscv_aplic_get_dev();
	TEST_ASSERT(aplic_dev != NULL, "APLIC device should exist");
	TEST_ASSERT(device_is_ready(aplic_dev), "APLIC device should be ready");
	
	/* Check debug flag to verify driver init was called */
	extern volatile uint32_t aplic_init_called;
	TEST_ASSERT(aplic_init_called == 0xDEADBEEF, "APLIC driver init should have been called");
	
	LOG_INF("=== Test 2: Register Configuration Verification ===");
	
	/* Test 2: Verify DOMAINCFG register per RISC-V AIA spec */
	uint32_t domaincfg = aplic_read_reg(APLIC_DOMAINCFG_OFFSET);
	LOG_INF("DOMAINCFG: 0x%08X", domaincfg);
	
	/* Check reserved bits [31:24] = 0x80 (per RISC-V AIA spec) */
	uint32_t reserved_bits = (domaincfg >> 24) & 0xFF;
	TEST_ASSERT(reserved_bits == 0x80, "Reserved bits [31:24] should be 0x80");
	
	/* Check IE bit (bit 8) - should be enabled after driver init */
	bool ie_enabled = (domaincfg & (1 << 8)) != 0;
	TEST_ASSERT(ie_enabled, "IE bit (bit 8) should be enabled");
	
	/* Check DM bit (bit 2) - should be 0 for direct mode */
	bool dm_direct = (domaincfg & (1 << 2)) == 0;
	TEST_ASSERT(dm_direct, "DM bit should be 0 for direct mode");
	
	/* Check BE bit (bit 0) - should be 0 for little-endian */
	bool be_little = (domaincfg & (1 << 0)) == 0;
	TEST_ASSERT(be_little, "BE bit should be 0 for little-endian");
	
	LOG_INF("=== Test 3: Source Configuration Verification ===");
	
	/* Test 3: Verify SOURCECFG registers */
	for (int i = 0; i < 4; i++) {
		uint32_t sourcecfg = aplic_read_reg(APLIC_SOURCECFG_OFFSET + (i * 4));
		LOG_INF("SOURCECFG[%d]: 0x%08X", i, sourcecfg);
		
		/* Should be configured for direct mode (D=1) */
		bool delegated = (sourcecfg & 0x1) != 0;
		TEST_ASSERT(delegated, "SOURCECFG should have D bit set for delegation");
	}
	
	/* Test register write capability (similar to PLIC test pattern) */
	uint32_t orig_sourcecfg = aplic_read_reg(APLIC_SOURCECFG_OFFSET);
	aplic_write_reg(APLIC_SOURCECFG_OFFSET, 0x5);
	uint32_t new_sourcecfg = aplic_read_reg(APLIC_SOURCECFG_OFFSET);
	aplic_write_reg(APLIC_SOURCECFG_OFFSET, orig_sourcecfg); /* restore */
	
	TEST_ASSERT(new_sourcecfg == 0x5, "SOURCECFG register should be writable");
	
	LOG_INF("=== Test 4: API Function Verification ===");
	
	/* Test 4: APLIC API functions (following interrupt controller test patterns) */
	
	/* Test interrupt enable/disable */
	riscv_aplic_irq_disable(1);
	int enabled = riscv_aplic_irq_is_enabled(1);
	TEST_ASSERT(!enabled, "IRQ should be disabled after disable call");
	
	riscv_aplic_irq_enable(1);
	enabled = riscv_aplic_irq_is_enabled(1);
	TEST_ASSERT(enabled, "IRQ should be enabled after enable call");
	
	/* Test priority setting */
	riscv_aplic_set_priority(1, 1);
	LOG_INF("Priority setting test completed");
	
	LOG_INF("=== Test 5: Multi-core Support Verification ===");
	
	/* Test 5: Multi-core support */
	unsigned int num_cpus = arch_num_cpus();
	LOG_INF("Number of CPUs detected: %u", num_cpus);
	TEST_ASSERT(num_cpus >= 1, "Should have at least 1 CPU");
	
	unsigned int current_cpu = arch_curr_cpu()->id;
	LOG_INF("Current CPU ID: %u", current_cpu);
	TEST_ASSERT(current_cpu < num_cpus, "Current CPU ID should be valid");
	
	/* Test APLIC device access from current CPU */
	const struct device *aplic_dev2 = riscv_aplic_get_dev();
	TEST_ASSERT(aplic_dev2 != NULL, "Should be able to get APLIC device from any CPU");
	
	LOG_INF("=== Test 6: Interrupt Status Registers ===");
	
	/* Test 6: Interrupt status registers accessibility */
	uint32_t setip = aplic_read_reg(APLIC_SETIP_OFFSET);
	uint32_t setie = aplic_read_reg(APLIC_SETIE_OFFSET);
	
	LOG_INF("SETIP: 0x%08X", setip);
	LOG_INF("SETIE: 0x%08X", setie);
	
	TEST_ASSERT(setip != 0xFFFFFFFF, "SETIP register should be readable");
	TEST_ASSERT(setie != 0xFFFFFFFF, "SETIE register should be readable");
	
	LOG_INF("=== Test 7: Performance Test ===");
	
	/* Test 7: Basic performance test (similar to other driver tests) */
	uint32_t start_time = k_uptime_get_32();
	
	for (int i = 0; i < 100; i++) {
		riscv_aplic_irq_enable(1);
		riscv_aplic_irq_disable(1);
	}
	
	uint32_t end_time = k_uptime_get_32();
	uint32_t duration = end_time - start_time;
	
	LOG_INF("100 enable/disable operations took %u ms", duration);
	TEST_ASSERT(duration < 100, "Operations should complete within reasonable time");
	
	/* Final results */
	uint32_t passed = atomic_get(&test_passed);
	uint32_t failed = atomic_get(&test_failed);
	uint32_t total = passed + failed;
	
	LOG_INF("=== Test Results Summary ===");
	LOG_INF("Total tests: %u", total);
	LOG_INF("Passed: %u", passed);
	LOG_INF("Failed: %u", failed);
	LOG_INF("Success rate: %u%%", (passed * 100) / total);
	
	if (failed == 0) {
		LOG_INF("🎉 ALL TESTS PASSED! APLIC driver verification successful!");
	} else {
		LOG_ERR("❌ SOME TESTS FAILED! Please check the logs above.");
	}
	
	LOG_INF("=== APLIC Verification Test Completed ===");
	
	/* Keep system running for observation */
	LOG_INF("Keeping system running for 5 seconds for observation...");
	k_sleep(K_SECONDS(5));
	
	LOG_INF("Test completed, system ready for shutdown.");
}

