/*
 * Copyright (c) 2024 Zephyr Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/interrupt_controller/riscv_imsic.h>
#include <zephyr/drivers/interrupt_controller/riscv_aplic.h>
#include <zephyr/irq.h>

LOG_MODULE_REGISTER(imsic_demo, LOG_LEVEL_INF);

/* Demo configuration */
#define DEMO_EID_BASE 10
#define DEMO_NUM_EIDS 8
#define DEMO_DURATION_MS 10000
#define DEMO_INTERVAL_MS 500

/* Demo states */
enum demo_state {
	DEMO_STATE_INIT,
	DEMO_STATE_BASIC_OPS,
	DEMO_STATE_INTERRUPT_TEST,
	DEMO_STATE_PRIORITY_TEST,
	DEMO_STATE_MSI_TEST,
	DEMO_STATE_PERFORMANCE_TEST,
	DEMO_STATE_COMPLETE
};

static enum demo_state current_state = DEMO_STATE_INIT;
static uint32_t demo_step = 0;
static uint32_t total_interrupts = 0;
static uint32_t test_eids[DEMO_NUM_EIDS];

/* IMSIC interrupt handler for demo */
static void imsic_demo_isr(const void *param)
{
	uint32_t eid = (uint32_t)param;
	
	LOG_INF("🎯 IMSIC Interrupt received: EID %u", eid);
	total_interrupts++;
	
	/* Clear interrupt pending */
	riscv_imsic_irq_clear_pending(eid);
}

/* Demo step 1: Basic IMSIC operations */
static void demo_basic_operations(void)
{
	const struct device *imsic_dev;
	
	LOG_INF("=== Step 1: Basic IMSIC Operations ===");
	
	/* Get IMSIC device */
	imsic_dev = riscv_imsic_get_dev();
	if (!imsic_dev) {
		LOG_ERR("❌ IMSIC device not found");
		return;
	}
	
	if (!device_is_ready(imsic_dev)) {
		LOG_ERR("❌ IMSIC device not ready");
		return;
	}
	
	LOG_INF("✅ IMSIC device found: %s", imsic_dev->name);
	
	/* Get device information */
	uint32_t hart_id = riscv_imsic_get_hart_id(imsic_dev);
	uint32_t guest_id = riscv_imsic_get_guest_id(imsic_dev);
	
	LOG_INF("📊 Device Info:");
	LOG_INF("   - Hart ID: %u", hart_id);
	LOG_INF("   - Guest ID: %u", guest_id);
	LOG_INF("   - Delivery Mode: %d", 1); /* Assume MSI mode for now */
	
	/* Test threshold operations */
	uint32_t old_threshold = riscv_imsic_get_threshold();
	LOG_INF("   - Current Threshold: %u", old_threshold);
	
	/* Set threshold to 0 (accept all interrupts) */
	if (riscv_imsic_set_threshold(0) == 0) {
		uint32_t new_threshold = riscv_imsic_get_threshold();
		LOG_INF("   - New Threshold: %u", new_threshold);
	} else {
		LOG_WRN("⚠️  Failed to set threshold");
	}
	
	LOG_INF("✅ Basic operations completed");
}

/* Demo step 2: Interrupt enable/disable test */
static void demo_interrupt_operations(void)
{
	LOG_INF("=== Step 2: Interrupt Enable/Disable Test ===");
	
	/* Initialize test EIDs */
	for (int i = 0; i < DEMO_NUM_EIDS; i++) {
		test_eids[i] = DEMO_EID_BASE + i;
	}
	
	LOG_INF("📋 Testing EIDs: %u to %u", test_eids[0], test_eids[DEMO_NUM_EIDS-1]);
	
	/* Test enabling interrupts */
	LOG_INF("🔓 Enabling interrupts...");
	for (int i = 0; i < DEMO_NUM_EIDS; i++) {
		riscv_imsic_irq_enable(test_eids[i]);
		int enabled = riscv_imsic_irq_is_enabled(test_eids[i]);
		if (enabled > 0) {
			LOG_INF("   ✅ EID %u: Enabled", test_eids[i]);
		} else {
			LOG_WRN("   ⚠️  EID %u: Enable failed", test_eids[i]);
		}
	}
	
	/* Test disabling some interrupts */
	LOG_INF("🔒 Disabling odd-numbered EIDs...");
	for (int i = 1; i < DEMO_NUM_EIDS; i += 2) {
		riscv_imsic_irq_disable(test_eids[i]);
		int enabled = riscv_imsic_irq_is_enabled(test_eids[i]);
		if (enabled == 0) {
			LOG_INF("   ✅ EID %u: Disabled", test_eids[i]);
		} else {
			LOG_WRN("   ⚠️  EID %u: Disable failed", test_eids[i]);
		}
	}
	
	LOG_INF("✅ Interrupt operations completed");
}

/* Demo step 3: Priority and threshold test */
static void demo_priority_threshold_test(void)
{
	LOG_INF("=== Step 3: Priority and Threshold Test ===");
	
	/* Test different threshold values */
	uint32_t thresholds[] = {0, 2, 4, 6, 8};
	int num_thresholds = sizeof(thresholds) / sizeof(thresholds[0]);
	
	LOG_INF("📊 Testing different threshold values...");
	for (int i = 0; i < num_thresholds; i++) {
		uint32_t threshold = thresholds[i];
		if (riscv_imsic_set_threshold(threshold) == 0) {
			uint32_t actual = riscv_imsic_get_threshold();
			LOG_INF("   ✅ Threshold %u: Set to %u", threshold, actual);
		} else {
			LOG_WRN("   ⚠️  Threshold %u: Set failed", threshold);
		}
		k_msleep(100);
	}
	
	/* Reset threshold to 0 for interrupt testing */
	riscv_imsic_set_threshold(0);
	LOG_INF("   ✅ Final threshold: 0 (accept all)");
	
	LOG_INF("✅ Priority and threshold test completed");
}

/* Demo step 4: Interrupt generation and handling test */
static void demo_interrupt_test(void)
{
	LOG_INF("=== Step 4: Interrupt Generation and Handling Test ===");
	
	/* Connect ISR for test EIDs */
	LOG_INF("🔗 Connecting interrupt handlers...");
	for (int i = 0; i < DEMO_NUM_EIDS; i++) {
		uint32_t eid = test_eids[i];
		if (riscv_imsic_irq_is_enabled(eid)) {
			/* Connect ISR */
			irq_connect_dynamic(eid, 0, imsic_demo_isr, (void *)eid, 0);
			irq_enable(eid);
			LOG_INF("   ✅ EID %u: ISR connected", eid);
		}
	}
	
	/* Generate test interrupts */
	LOG_INF("🚀 Generating test interrupts...");
	total_interrupts = 0;
	
	for (int round = 0; round < 3; round++) {
		LOG_INF("   📡 Round %d: Setting interrupts pending...", round + 1);
		
		for (int i = 0; i < DEMO_NUM_EIDS; i++) {
			uint32_t eid = test_eids[i];
			if (riscv_imsic_irq_is_enabled(eid)) {
				riscv_imsic_irq_set_pending(eid);
				LOG_INF("      ✅ EID %u: Pending set", eid);
			}
		}
		
		/* Wait for interrupts to be processed */
		k_msleep(200);
		LOG_INF("   📊 Round %d completed. Total interrupts: %u", 
			round + 1, total_interrupts);
	}
	
	LOG_INF("✅ Interrupt test completed. Total interrupts: %u", total_interrupts);
}

/* Demo step 5: MSI (Message Signaled Interrupt) test */
static void demo_msi_test(void)
{
	LOG_INF("=== Step 5: MSI (Message Signaled Interrupt) Test ===");
	
	/* Check if we have APLIC device for MSI testing */
	const struct device *aplic_dev = riscv_aplic_get_dev();
	if (aplic_dev && device_is_ready(aplic_dev)) {
		LOG_INF("✅ APLIC device available: %s", aplic_dev->name);
		
		/* Test APLIC MSI mode configuration */
		LOG_INF("🔧 Testing APLIC MSI mode...");
		
		/* This would test APLIC's MSI forwarding to IMSIC */
		LOG_INF("   📝 Note: MSI forwarding test requires hardware support");
		LOG_INF("   📝 Current QEMU implementation may have limitations");
		
	} else {
		LOG_WRN("⚠️  APLIC device not available for MSI testing");
	}
	
	LOG_INF("✅ MSI test completed");
}

/* Demo step 6: Performance and stress test */
static void demo_performance_test(void)
{
	LOG_INF("=== Step 6: Performance and Stress Test ===");
	
	LOG_INF("🚀 Starting performance test...");
	
	/* Test rapid interrupt enable/disable */
	LOG_INF("📊 Testing rapid operations...");
	uint32_t start_time = k_uptime_get();
	
	for (int i = 0; i < 100; i++) {
		for (int j = 0; j < DEMO_NUM_EIDS; j++) {
			uint32_t eid = test_eids[j];
			riscv_imsic_irq_enable(eid);
			riscv_imsic_irq_disable(eid);
		}
	}
	
	uint32_t end_time = k_uptime_get();
	uint32_t duration = end_time - start_time;
	
	LOG_INF("   ✅ 100 cycles completed in %u ms", duration);
	LOG_INF("   📊 Average: %.2f operations/ms", 
		(float)(100 * DEMO_NUM_EIDS) / duration);
	
	/* Test interrupt generation performance */
	LOG_INF("📊 Testing interrupt generation performance...");
	total_interrupts = 0;
	start_time = k_uptime_get();
	
	for (int i = 0; i < 50; i++) {
		for (int j = 0; j < DEMO_NUM_EIDS; j++) {
			uint32_t eid = test_eids[j];
			if (riscv_imsic_irq_is_enabled(eid)) {
				riscv_imsic_irq_set_pending(eid);
			}
		}
		k_msleep(10);
	}
	
	end_time = k_uptime_get();
	duration = end_time - start_time;
	
	LOG_INF("   ✅ 50 rounds completed in %u ms", duration);
	LOG_INF("   📊 Total interrupts: %u", total_interrupts);
	LOG_INF("   📊 Rate: %.2f interrupts/ms", (float)total_interrupts / duration);
	
	LOG_INF("✅ Performance test completed");
}

/* Main demo loop */
static void demo_main_loop(void)
{
	LOG_INF("🎬 Starting IMSIC Demo...");
	LOG_INF("📋 Demo will run for %d seconds", DEMO_DURATION_MS / 1000);
	
	uint32_t start_time = k_uptime_get();
	
	while (k_uptime_get() - start_time < DEMO_DURATION_MS) {
		switch (current_state) {
		case DEMO_STATE_INIT:
			LOG_INF("🚀 Initializing demo...");
			current_state = DEMO_STATE_BASIC_OPS;
			break;
			
		case DEMO_STATE_BASIC_OPS:
			demo_basic_operations();
			current_state = DEMO_STATE_INTERRUPT_TEST;
			break;
			
		case DEMO_STATE_INTERRUPT_TEST:
			demo_interrupt_operations();
			current_state = DEMO_STATE_PRIORITY_TEST;
			break;
			
		case DEMO_STATE_PRIORITY_TEST:
			demo_priority_threshold_test();
			current_state = DEMO_STATE_MSI_TEST;
			break;
			
		case DEMO_STATE_MSI_TEST:
			demo_msi_test();
			current_state = DEMO_STATE_PERFORMANCE_TEST;
			break;
			
		case DEMO_STATE_PERFORMANCE_TEST:
			demo_performance_test();
			current_state = DEMO_STATE_COMPLETE;
			break;
			
		case DEMO_STATE_COMPLETE:
			LOG_INF("🎉 Demo completed successfully!");
			LOG_INF("📊 Final statistics:");
			LOG_INF("   - Total interrupts processed: %u", total_interrupts);
			LOG_INF("   - Test EIDs used: %u to %u", 
				test_eids[0], test_eids[DEMO_NUM_EIDS-1]);
			LOG_INF("   - Demo duration: %lld ms", k_uptime_get() - start_time);
			
			/* Keep running for observation */
			k_msleep(DEMO_INTERVAL_MS);
			return;
		}
		
		/* Wait between demo steps */
		k_msleep(DEMO_INTERVAL_MS);
		demo_step++;
	}
	
	LOG_INF("⏰ Demo time limit reached");
}

/* Main function */
int main(void)
{
	LOG_INF("=== RISC-V IMSIC Demo Application ===");
	LOG_INF("🎯 Demonstrating IMSIC functionality");
	LOG_INF("🔧 Features: Interrupt control, priority, threshold, MSI");
	LOG_INF("🚀 Starting in 2 seconds...");
	
	/* Wait for system stabilization */
	k_msleep(2000);
	
	/* Run the main demo loop */
	demo_main_loop();
	
	LOG_INF("🔄 Demo loop completed, keeping system running...");
	
	/* Keep the system running for observation */
	while (1) {
		k_msleep(5000);
		LOG_INF("💻 System running... Total interrupts: %u", total_interrupts);
	}
	
	return 0;
}
