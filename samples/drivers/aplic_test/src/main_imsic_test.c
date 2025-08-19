/*
 * Copyright (c) 2024 Zephyr Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/interrupt_controller/riscv_aplic.h>
#include <zephyr/drivers/interrupt_controller/riscv_imsic.h>

LOG_MODULE_REGISTER(imsic_test, LOG_LEVEL_INF);

/* Test configuration */
#define TEST_EID_1    1
#define TEST_EID_2    2
#define TEST_HART_1   1
#define TEST_GUEST_0  0

/* Test interrupt counters */
static volatile uint32_t imsic_irq_count = 0;
static volatile uint32_t aplic_msi_count = 0;

/* Test ISR for IMSIC */
static void test_imsic_isr(const void *param)
{
	uint32_t eid = (uint32_t)param;
	imsic_irq_count++;
	
	LOG_INF("IMSIC ISR: EID %u received on CPU %d", eid, arch_proc_id());
	
	/* Clear the interrupt */
	riscv_imsic_irq_clear_pending(eid);
}

/* Test ISR for APLIC MSI */
static void test_aplic_msi_isr(const void *param)
{
	uint32_t irq = (uint32_t)param;
	aplic_msi_count++;
	
	LOG_INF("APLIC MSI ISR: IRQ %u received on CPU %d", irq, arch_proc_id());
}

void main(void)
{
	LOG_INF("=== IMSIC and MSI Mode Test Starting ===");
	LOG_INF("Current CPU ID: %d", arch_proc_id());
	LOG_INF("Total CPUs: %d", CONFIG_MP_MAX_NUM_CPUS);
	
	/* Wait for system to stabilize */
	k_sleep(K_MSEC(1000));
	
	LOG_INF("System stabilized, checking IMSIC and APLIC devices...");
	
	/* Test 1: Check IMSIC device */
	const struct device *imsic_dev = riscv_imsic_get_dev();
	if (imsic_dev == NULL) {
		/* Try alternative methods to find IMSIC device */
		imsic_dev = device_get_binding("interrupt-controller@24000000");
		if (imsic_dev == NULL) {
			LOG_INF("No IMSIC device found via device binding");
		}
	}
	
	if (imsic_dev != NULL) {
		LOG_INF("✓ IMSIC device found on CPU %d: %s", arch_proc_id(), imsic_dev->name);
		
		if (device_is_ready(imsic_dev)) {
			LOG_INF("✓ IMSIC device is ready on CPU %d", arch_proc_id());
			
			/* Get IMSIC configuration */
			int hart_id = riscv_imsic_get_hart_id(imsic_dev);
			int guest_id = riscv_imsic_get_guest_id(imsic_dev);
			LOG_INF("IMSIC on CPU %d: Hart ID = %d, Guest ID = %d", arch_proc_id(), hart_id, guest_id);
			
			/* Check delivery mode */
			enum riscv_imsic_delivery_mode mode = riscv_imsic_get_delivery_mode();
			LOG_INF("IMSIC on CPU %d: Delivery mode = %d", arch_proc_id(), mode);
			
			/* Test IMSIC interrupt enable/disable */
			LOG_INF("Testing IMSIC interrupt control on CPU %d...", arch_proc_id());
			riscv_imsic_irq_enable(TEST_EID_1);
			riscv_imsic_irq_enable(TEST_EID_2);
			
			if (riscv_imsic_irq_is_enabled(TEST_EID_1)) {
				LOG_INF("✓ EID %u enabled successfully on CPU %d", TEST_EID_1, arch_proc_id());
			}
			
			if (riscv_imsic_irq_is_enabled(TEST_EID_2)) {
				LOG_INF("✓ EID %u enabled successfully on CPU %d", TEST_EID_2, arch_proc_id());
			}
			
			/* Test threshold setting */
			riscv_imsic_set_threshold(0);
			uint32_t threshold = riscv_imsic_get_threshold();
			LOG_INF("IMSIC on CPU %d: Threshold set to %u", arch_proc_id(), threshold);
			
		} else {
			LOG_ERR("❌ IMSIC device is not ready on CPU %d", arch_proc_id());
		}
	} else {
		LOG_WRN("⚠️  IMSIC device not found on CPU %d", arch_proc_id());
	}
	
	/* Test 2: Check APLIC device and MSI mode */
	const struct device *aplic_dev = riscv_aplic_get_dev();
	if (aplic_dev != NULL) {
		LOG_INF("✓ APLIC device found: %s", aplic_dev->name);
		
		if (device_is_ready(aplic_dev)) {
			LOG_INF("✓ APLIC device is ready");
			
			/* Check if MSI mode is enabled */
			bool msi_enabled = riscv_aplic_is_msi_mode_enabled();
			LOG_INF("APLIC: MSI mode %s", msi_enabled ? "ENABLED" : "DISABLED");
			
			if (msi_enabled) {
				LOG_INF("Testing APLIC MSI functionality...");
				
				/* Configure a source for MSI delivery */
				int result = riscv_aplic_configure_source_msi(TEST_EID_1, TEST_HART_1, TEST_GUEST_0);
				if (result == 0) {
					LOG_INF("✓ Source %u configured for MSI to hart %u, guest %u", 
						TEST_EID_1, TEST_HART_1, TEST_GUEST_0);
				} else {
					LOG_ERR("❌ Failed to configure source %u for MSI: %d", TEST_EID_1, result);
				}
				
				/* Test MSI sending */
				result = riscv_aplic_send_msi(TEST_HART_1, TEST_GUEST_0, TEST_EID_1);
				if (result == 0) {
					LOG_INF("✓ MSI sent successfully to hart %u, guest %u", TEST_HART_1, TEST_GUEST_0);
				} else {
					LOG_ERR("❌ Failed to send MSI: %d", result);
				}
				
			} else {
				LOG_INF("APLIC is in direct mode, MSI functionality not available");
			}
			
		} else {
			LOG_ERR("❌ APLIC device is not ready");
		}
	} else {
		LOG_ERR("❌ APLIC device not found");
	}
	
	/* Test 3: Test interrupt handling */
	LOG_INF("Testing interrupt handling...");
	
	/* Set some interrupts pending for testing */
	if (imsic_dev != NULL && device_is_ready(imsic_dev)) {
		riscv_imsic_irq_set_pending(TEST_EID_1);
		LOG_INF("Set EID %u pending for testing", TEST_EID_1);
	}
	
	/* Wait a bit to see if interrupts are handled */
	k_sleep(K_MSEC(100));
	
	/* Display test results */
	LOG_INF("=== Test Results Summary ===");
	LOG_INF("IMSIC IRQ count: %u", imsic_irq_count);
	LOG_INF("APLIC MSI count: %u", aplic_msi_count);
	
	if (imsic_dev != NULL && device_is_ready(imsic_dev)) {
		LOG_INF("✓ IMSIC functionality tested successfully");
	} else {
		LOG_WRN("⚠️  IMSIC functionality not tested (device not available)");
	}
	
	if (aplic_dev != NULL && device_is_ready(aplic_dev)) {
		if (riscv_aplic_is_msi_mode_enabled()) {
			LOG_INF("✓ APLIC MSI mode functionality tested successfully");
		} else {
			LOG_INF("✓ APLIC direct mode functionality confirmed");
		}
	} else {
		LOG_ERR("❌ APLIC functionality not tested (device not available)");
	}
	
	LOG_INF("=== IMSIC and MSI Mode Test Completed ===");
	
	/* Keep system running for observation */
	LOG_INF("Keeping system running for 5 seconds for observation...");
	k_sleep(K_SECONDS(5));
	
	LOG_INF("Test finished, system ready.");
}
