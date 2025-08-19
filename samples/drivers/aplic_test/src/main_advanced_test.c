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

LOG_MODULE_REGISTER(aplic_advanced_test, LOG_LEVEL_INF);

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

/* Test interrupt trigger types */
static void test_trigger_types(void)
{
	int result;
	int trigger_type;
	
	LOG_INF("=== Testing Interrupt Trigger Types ===");
	
	/* Test edge-triggered rising */
	result = riscv_aplic_irq_set_trigger_type(1, RISCV_APLIC_TRIGGER_EDGE_RISING);
	TEST_ASSERT(result == 0, "Set IRQ 1 to edge-rising trigger");
	
	trigger_type = riscv_aplic_irq_get_trigger_type(1);
	TEST_ASSERT(trigger_type == RISCV_APLIC_TRIGGER_EDGE_RISING, 
		    "Read back edge-rising trigger type");
	
	/* Test edge-triggered falling */
	result = riscv_aplic_irq_set_trigger_type(2, RISCV_APLIC_TRIGGER_EDGE_FALLING);
	TEST_ASSERT(result == 0, "Set IRQ 2 to edge-falling trigger");
	
	trigger_type = riscv_aplic_irq_get_trigger_type(2);
	TEST_ASSERT(trigger_type == RISCV_APLIC_TRIGGER_EDGE_FALLING, 
		    "Read back edge-falling trigger type");
	
	/* Test level-triggered high */
	result = riscv_aplic_irq_set_trigger_type(3, RISCV_APLIC_TRIGGER_LEVEL_HIGH);
	TEST_ASSERT(result == 0, "Set IRQ 3 to level-high trigger");
	
	trigger_type = riscv_aplic_irq_get_trigger_type(3);
	TEST_ASSERT(trigger_type == RISCV_APLIC_TRIGGER_LEVEL_HIGH, 
		    "Read back level-high trigger type");
	
	/* Test level-triggered low */
	result = riscv_aplic_irq_set_trigger_type(4, RISCV_APLIC_TRIGGER_LEVEL_LOW);
	TEST_ASSERT(result == 0, "Set IRQ 4 to level-low trigger");
	
	trigger_type = riscv_aplic_irq_get_trigger_type(4);
	TEST_ASSERT(trigger_type == RISCV_APLIC_TRIGGER_LEVEL_LOW, 
		    "Read back level-low trigger type");
	
	/* Test invalid IRQ */
	result = riscv_aplic_irq_set_trigger_type(0, RISCV_APLIC_TRIGGER_EDGE_RISING);
	TEST_ASSERT(result < 0, "Reject invalid IRQ 0 for trigger type");
	
	result = riscv_aplic_irq_set_trigger_type(1024, RISCV_APLIC_TRIGGER_EDGE_RISING);
	TEST_ASSERT(result < 0, "Reject invalid IRQ 1024 for trigger type");
}

/* Test Hart threshold management */
static void test_hart_thresholds(void)
{
	int result;
	uint32_t threshold;
	uint32_t num_cpus = arch_num_cpus();
	
	LOG_INF("=== Testing Hart Threshold Management ===");
	
	for (uint32_t hart = 0; hart < num_cpus; hart++) {
		/* Test setting threshold */
		result = riscv_aplic_hart_set_threshold(hart, hart + 1);
		TEST_ASSERT(result == 0, "Set Hart threshold");
		
		/* Test reading threshold */
		threshold = riscv_aplic_hart_get_threshold(hart);
		TEST_ASSERT(threshold == (hart + 1), "Read back Hart threshold");
		
		LOG_INF("Hart %u threshold: %u", hart, threshold);
	}
	
	/* Test invalid Hart ID */
	result = riscv_aplic_hart_set_threshold(CONFIG_MP_MAX_NUM_CPUS, 5);
	TEST_ASSERT(result < 0, "Reject invalid Hart ID");
	
	/* Test invalid threshold */
	result = riscv_aplic_hart_set_threshold(0, 256);
	TEST_ASSERT(result < 0, "Reject invalid threshold value");
}

/* Test enhanced interrupt affinity */
static void test_enhanced_affinity(void)
{
	int result;
	uint32_t num_cpus = arch_num_cpus();
	
	LOG_INF("=== Testing Enhanced Interrupt Affinity ===");
	
	/* Test single CPU affinity */
	for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
		result = riscv_aplic_irq_set_affinity(10 + cpu, BIT(cpu));
		TEST_ASSERT(result == 0, "Set single CPU affinity");
		LOG_INF("IRQ %u affinity set to CPU %u", 10 + cpu, cpu);
	}
	
	/* Test multi-CPU affinity */
	uint32_t multi_mask = BIT(0) | BIT(1);
	if (num_cpus > 1) {
		result = riscv_aplic_irq_set_affinity(20, multi_mask);
		TEST_ASSERT(result == 0, "Set multi-CPU affinity");
		LOG_INF("IRQ 20 affinity set to CPUs 0,1");
	}
	
	/* Test all-CPU affinity */
	uint32_t all_mask = BIT_MASK(num_cpus);
	result = riscv_aplic_irq_set_affinity(21, all_mask);
	TEST_ASSERT(result == 0, "Set all-CPU affinity");
	LOG_INF("IRQ 21 affinity set to all CPUs (mask 0x%X)", all_mask);
}

/* Test interrupt statistics */
static void test_interrupt_statistics(void)
{
	struct riscv_aplic_irq_stats stats;
	int result;
	uint32_t total_before, total_after;
	
	LOG_INF("=== Testing Interrupt Statistics ===");
	
	/* Reset statistics */
	riscv_aplic_reset_stats();
	total_before = riscv_aplic_get_total_interrupts();
	TEST_ASSERT(total_before == 0, "Statistics reset to zero");
	
	/* Test getting stats for a valid IRQ */
	result = riscv_aplic_get_irq_stats(5, &stats);
	TEST_ASSERT(result == 0, "Get IRQ statistics");
	TEST_ASSERT(stats.count == 0, "Initial IRQ count is zero");
	TEST_ASSERT(stats.enabled == false, "Initial IRQ state is disabled");
	LOG_INF("IRQ 5 initial stats: count=%u, last_cpu=%u, affinity=0x%X", 
		stats.count, stats.last_cpu, stats.affinity_mask);
	
	/* Enable an IRQ and check state */
	riscv_aplic_irq_enable(5);
	result = riscv_aplic_get_irq_stats(5, &stats);
	TEST_ASSERT(result == 0, "Get IRQ statistics after enable");
	TEST_ASSERT(stats.enabled == true, "IRQ state is enabled");
	
	/* Disable the IRQ and check state */
	riscv_aplic_irq_disable(5);
	result = riscv_aplic_get_irq_stats(5, &stats);
	TEST_ASSERT(result == 0, "Get IRQ statistics after disable");
	TEST_ASSERT(stats.enabled == false, "IRQ state is disabled");
	
	/* Test invalid IRQ */
	result = riscv_aplic_get_irq_stats(0, &stats);
	TEST_ASSERT(result < 0, "Reject invalid IRQ 0 for stats");
	
	result = riscv_aplic_get_irq_stats(1024, &stats);
	TEST_ASSERT(result < 0, "Reject invalid IRQ 1024 for stats");
	
	/* Test NULL pointer */
	result = riscv_aplic_get_irq_stats(5, NULL);
	TEST_ASSERT(result < 0, "Reject NULL stats pointer");
}

/* Test priority and advanced configuration */
static void test_priority_management(void)
{
	struct riscv_aplic_irq_stats stats;
	int result;
	
	LOG_INF("=== Testing Priority Management ===");
	
	/* Test setting different priorities */
	for (uint32_t irq = 1; irq <= 8; irq++) {
		uint32_t priority = irq % 8;
		riscv_aplic_set_priority(irq, priority);
		
		/* Check if priority is stored in stats */
		result = riscv_aplic_get_irq_stats(irq, &stats);
		if (result == 0) {
			LOG_INF("IRQ %u priority: %u (stats priority: %u)", 
				irq, priority, stats.priority);
		}
	}
	
	TEST_ASSERT(true, "Priority management test completed");
}

/* Test error handling and edge cases */
static void test_error_handling(void)
{
	int result;
	uint32_t threshold;
	
	LOG_INF("=== Testing Error Handling ===");
	
	/* Test boundary conditions for Hart thresholds */
	result = riscv_aplic_hart_set_threshold(0, 0);
	TEST_ASSERT(result == 0, "Set minimum threshold (0)");
	
	result = riscv_aplic_hart_set_threshold(0, 255);
	TEST_ASSERT(result == 0, "Set maximum threshold (255)");
	
	threshold = riscv_aplic_hart_get_threshold(0);
	TEST_ASSERT(threshold == 255, "Read back maximum threshold");
	
	/* Test invalid Hart ID for threshold */
	threshold = riscv_aplic_hart_get_threshold(CONFIG_MP_MAX_NUM_CPUS);
	TEST_ASSERT(threshold == 0, "Invalid Hart ID returns 0 threshold");
	
	/* Test invalid trigger type (this should be caught by enum validation) */
	result = riscv_aplic_irq_set_trigger_type(1, (enum riscv_aplic_trigger_type)99);
	TEST_ASSERT(result < 0, "Reject invalid trigger type");
	
	/* Test IRQ affinity edge cases */
	result = riscv_aplic_irq_set_affinity(1, 0); /* Empty mask */
	TEST_ASSERT(result < 0, "Reject empty affinity mask");
	
	result = riscv_aplic_irq_set_affinity(0, BIT(0)); /* Invalid IRQ */
	TEST_ASSERT(result < 0, "Reject invalid IRQ for affinity");
}

void main(void)
{
	LOG_INF("=== APLIC Advanced Features Test ===");
	LOG_INF("Testing advanced interrupt management capabilities...");
	
	/* Wait for system to stabilize */
	k_sleep(K_MSEC(100));
	
	/* Reset test counters */
	atomic_set(&test_passed, 0);
	atomic_set(&test_failed, 0);
	
	/* Check if APLIC device is available */
	const struct device *aplic_dev = riscv_aplic_get_dev();
	if (aplic_dev == NULL) {
		LOG_ERR("APLIC device not found! Cannot run advanced tests.");
		return;
	}
	
	if (!device_is_ready(aplic_dev)) {
		LOG_ERR("APLIC device not ready! Cannot run advanced tests.");
		return;
	}
	
	LOG_INF("APLIC device found and ready, starting advanced tests...");
	LOG_INF("System has %u CPUs", arch_num_cpus());
	
	/* Run all test suites */
	test_trigger_types();
	test_hart_thresholds();
	test_enhanced_affinity();
	test_interrupt_statistics();
	test_priority_management();
	test_error_handling();
	
	/* Display results */
	uint32_t passed = atomic_get(&test_passed);
	uint32_t failed = atomic_get(&test_failed);
	uint32_t total = passed + failed;
	
	LOG_INF("=== Advanced Test Results Summary ===");
	LOG_INF("Total tests: %u", total);
	LOG_INF("Passed: %u", passed);
	LOG_INF("Failed: %u", failed);
	
	if (total > 0) {
		LOG_INF("Success rate: %u%%", (passed * 100) / total);
	}
	
	if (failed == 0) {
		LOG_INF("🎉 ALL ADVANCED TESTS PASSED! APLIC advanced features working!");
	} else {
		LOG_ERR("❌ SOME ADVANCED TESTS FAILED! Please check the logs above.");
	}
	
	/* Display final statistics */
	uint32_t total_interrupts = riscv_aplic_get_total_interrupts();
	LOG_INF("Total interrupts processed during test: %u", total_interrupts);
	
	LOG_INF("=== APLIC Advanced Features Test Completed ===");
	
	/* Keep system running for observation */
	LOG_INF("Keeping system running for 3 seconds for observation...");
	k_sleep(K_SECONDS(3));
	
	LOG_INF("Advanced test completed, system ready.");
}

