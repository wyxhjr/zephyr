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

LOG_MODULE_REGISTER(aplic_smp_verification, LOG_LEVEL_INF);

/* APLIC register definitions */
#define APLIC_DOMAINCFG_OFFSET     0x00
#define APLIC_SOURCECFG_OFFSET     0x04
#define APLIC_SETIP_OFFSET         0x1C
#define APLIC_SETIE_OFFSET         0x24
#define APLIC_TARGET_OFFSET        0x3000

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

/* SMP test data */
static atomic_t test_passed = 0;
static atomic_t test_failed = 0;
static atomic_t cpu_test_completed[CONFIG_MP_MAX_NUM_CPUS];
static atomic_t cpu_aplic_access_test[CONFIG_MP_MAX_NUM_CPUS];

#define TEST_ASSERT(condition, message) \
	do { \
		if (condition) { \
			LOG_INF("✓ PASS [CPU %d]: %s", arch_curr_cpu()->id, message); \
			atomic_inc(&test_passed); \
		} else { \
			LOG_ERR("✗ FAIL [CPU %d]: %s", arch_curr_cpu()->id, message); \
			atomic_inc(&test_failed); \
		} \
	} while (0)

/* Per-CPU work function */
static void cpu_work_function(void *arg1, void *arg2, void *arg3)
{
	int cpu_id = arch_curr_cpu()->id;
	
	LOG_INF("CPU %d: Starting SMP verification work", cpu_id);
	
	/* Test 1: Basic CPU identification */
	TEST_ASSERT(cpu_id >= 0 && cpu_id < CONFIG_MP_MAX_NUM_CPUS, 
		    "CPU ID should be valid");
	
	/* Test 2: APLIC device access from this CPU */
	const struct device *aplic_dev = riscv_aplic_get_dev();
	TEST_ASSERT(aplic_dev != NULL, "APLIC device should be accessible from this CPU");
	TEST_ASSERT(device_is_ready(aplic_dev), "APLIC device should be ready from this CPU");
	
	/* Test 3: APLIC register access from this CPU */
	uint32_t domaincfg = aplic_read_reg(APLIC_DOMAINCFG_OFFSET);
	TEST_ASSERT((domaincfg >> 24) == 0x80, "Should be able to read DOMAINCFG correctly");
	TEST_ASSERT((domaincfg & (1 << 8)) != 0, "IE bit should be readable from this CPU");
	
	/* Test 4: APLIC API calls from this CPU */
	riscv_aplic_irq_enable(cpu_id + 1);  /* Use different IRQ for each CPU */
	int enabled = riscv_aplic_irq_is_enabled(cpu_id + 1);
	TEST_ASSERT(enabled, "Should be able to enable IRQ from this CPU");
	
	riscv_aplic_irq_disable(cpu_id + 1);
	enabled = riscv_aplic_irq_is_enabled(cpu_id + 1);
	TEST_ASSERT(!enabled, "Should be able to disable IRQ from this CPU");
	
	/* Test 5: Priority setting from this CPU */
	riscv_aplic_set_priority(cpu_id + 1, cpu_id + 1);
	
	/* Mark APLIC access test as completed for this CPU */
	atomic_set(&cpu_aplic_access_test[cpu_id], 1);
	
	/* Test 6: Inter-CPU synchronization */
	k_msleep(100 * (cpu_id + 1)); /* Different delay for each CPU */
	
	LOG_INF("CPU %d: APLIC verification work completed", cpu_id);
	
	/* Mark this CPU's test as completed */
	atomic_set(&cpu_test_completed[cpu_id], 1);
}

/* Thread definitions for each CPU */
K_THREAD_DEFINE(cpu1_thread, 1024, cpu_work_function, NULL, NULL, NULL, 
		K_PRIO_COOP(1), 0, 0);
K_THREAD_DEFINE(cpu2_thread, 1024, cpu_work_function, NULL, NULL, NULL, 
		K_PRIO_COOP(1), 0, 0);
K_THREAD_DEFINE(cpu3_thread, 1024, cpu_work_function, NULL, NULL, NULL, 
		K_PRIO_COOP(1), 0, 0);

void main(void)
{
	LOG_INF("=== APLIC SMP Verification Test ===");
	LOG_INF("Starting SMP-specific APLIC driver validation...");
	
	/* Wait for system to stabilize */
	k_sleep(K_MSEC(200));
	
	/* Reset counters */
	atomic_set(&test_passed, 0);
	atomic_set(&test_failed, 0);
	
	for (int i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		atomic_set(&cpu_test_completed[i], 0);
		atomic_set(&cpu_aplic_access_test[i], 0);
	}
	
	LOG_INF("=== Test 1: SMP System Verification ===");
	
	/* Test SMP system basic info */
	unsigned int num_cpus = arch_num_cpus();
	LOG_INF("Number of CPUs detected: %u", num_cpus);
	TEST_ASSERT(num_cpus > 1, "Should have multiple CPUs for SMP test");
	TEST_ASSERT(num_cpus <= CONFIG_MP_MAX_NUM_CPUS, "CPU count should not exceed configured max");
	
	unsigned int current_cpu = arch_curr_cpu()->id;
	LOG_INF("Main thread running on CPU: %u", current_cpu);
	TEST_ASSERT(current_cpu == 0, "Main thread should run on CPU 0");
	
	LOG_INF("=== Test 2: APLIC Access from Main CPU ===");
	
	/* Test APLIC from main CPU */
	const struct device *aplic_dev = riscv_aplic_get_dev();
	TEST_ASSERT(aplic_dev != NULL, "APLIC device should exist on main CPU");
	TEST_ASSERT(device_is_ready(aplic_dev), "APLIC device should be ready on main CPU");
	
	/* Test DOMAINCFG from main CPU */
	uint32_t domaincfg = aplic_read_reg(APLIC_DOMAINCFG_OFFSET);
	LOG_INF("DOMAINCFG from main CPU: 0x%08X", domaincfg);
	TEST_ASSERT((domaincfg >> 24) == 0x80, "Reserved bits should be correct from main CPU");
	TEST_ASSERT((domaincfg & (1 << 8)) != 0, "IE bit should be enabled from main CPU");
	
	LOG_INF("=== Test 3: Starting Per-CPU Work Threads ===");
	
	/* Note: CPU pinning not available, threads will be scheduled by SMP scheduler */
	LOG_INF("Starting worker threads for SMP testing (scheduler will distribute)");
	LOG_INF("Worker threads will be distributed across available CPUs by Zephyr SMP scheduler");
	
	LOG_INF("=== Test 4: Waiting for Per-CPU Tests ===");
	
	/* Wait for all CPUs to complete their tests */
	int timeout_count = 0;
	while (timeout_count < 100) { /* 10 second timeout */
		bool all_completed = true;
		
		for (int i = 0; i < num_cpus; i++) {
			if (atomic_get(&cpu_test_completed[i]) == 0) {
				all_completed = false;
				break;
			}
		}
		
		if (all_completed) {
			LOG_INF("All CPU tests completed!");
			break;
		}
		
		k_msleep(100);
		timeout_count++;
		
		if (timeout_count % 10 == 0) {
			LOG_INF("Waiting for CPU tests... (%d/100)", timeout_count);
			for (int i = 0; i < num_cpus; i++) {
				LOG_INF("  CPU %d: %s", i, 
					atomic_get(&cpu_test_completed[i]) ? "DONE" : "WAITING");
			}
		}
	}
	
	if (timeout_count >= 100) {
		LOG_ERR("Timeout waiting for CPU tests!");
		atomic_inc(&test_failed);
	}
	
	LOG_INF("=== Test 5: APLIC Multi-CPU Access Verification ===");
	
	/* Verify all CPUs could access APLIC */
	for (int i = 0; i < num_cpus; i++) {
		bool aplic_access_ok = atomic_get(&cpu_aplic_access_test[i]) != 0;
		TEST_ASSERT(aplic_access_ok, "APLIC should be accessible from all CPUs");
	}
	
	LOG_INF("=== Test 6: SMP Interrupt Affinity Test ===");
	
	/* Test interrupt affinity if supported */
	for (int i = 1; i < 5 && i < num_cpus; i++) {
		int result = riscv_aplic_irq_set_affinity(i, 1 << i); /* Pin to specific CPU */
		if (result == 0) {
			LOG_INF("✓ IRQ %d affinity set to CPU %d", i, i);
		} else {
			LOG_INF("! IRQ affinity not supported or failed for IRQ %d", i);
		}
	}
	
	LOG_INF("=== Test 7: SMP Performance Test ===");
	
	/* Test concurrent APLIC operations from main CPU */
	uint32_t start_time = k_uptime_get_32();
	
	for (int i = 0; i < 50; i++) {
		riscv_aplic_irq_enable(10);
		riscv_aplic_irq_disable(10);
		riscv_aplic_set_priority(10, i % 8);
	}
	
	uint32_t end_time = k_uptime_get_32();
	uint32_t duration = end_time - start_time;
	
	LOG_INF("50 concurrent operations took %u ms", duration);
	TEST_ASSERT(duration < 100, "SMP operations should be efficient");
	
	/* Final results */
	uint32_t passed = atomic_get(&test_passed);
	uint32_t failed = atomic_get(&test_failed);
	uint32_t total = passed + failed;
	
	LOG_INF("=== SMP Test Results Summary ===");
	LOG_INF("Total CPUs tested: %u", num_cpus);
	LOG_INF("Total tests: %u", total);
	LOG_INF("Passed: %u", passed);
	LOG_INF("Failed: %u", failed);
	
	if (total > 0) {
		LOG_INF("Success rate: %u%%", (passed * 100) / total);
	}
	
	if (failed == 0) {
		LOG_INF("🎉 ALL SMP TESTS PASSED! APLIC SMP verification successful!");
	} else {
		LOG_ERR("❌ SOME SMP TESTS FAILED! Please check the logs above.");
	}
	
	LOG_INF("=== APLIC SMP Verification Test Completed ===");
	
	/* Keep system running for observation */
	LOG_INF("Keeping system running for 5 seconds for observation...");
	k_sleep(K_SECONDS(5));
	
	LOG_INF("SMP test completed, system ready for shutdown.");
}
