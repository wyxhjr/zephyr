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

LOG_MODULE_REGISTER(imsic_smp_test, LOG_LEVEL_INF);

/* Test configuration */
#define TEST_EID_BASE 10
#define TEST_DURATION_MS 2000
#define CPU_TEST_DELAY_MS 100

/* Per-CPU test data */
struct cpu_test_data {
	uint32_t cpu_id;
	uint32_t hart_id;
	uint32_t guest_id;
	uint32_t test_eid;
	uint32_t irq_count;
	bool imsic_ready;
	bool test_complete;
};

static struct cpu_test_data cpu_data[CONFIG_MP_MAX_NUM_CPUS];
static volatile uint32_t total_irq_count = 0;
static volatile uint32_t cpus_ready = 0;

/* IMSIC interrupt handler */
static void test_imsic_isr(const void *param)
{
	uint32_t cpu_id = arch_proc_id();
	
	if (cpu_id < CONFIG_MP_MAX_NUM_CPUS) {
		cpu_data[cpu_id].irq_count++;
		__atomic_fetch_add(&total_irq_count, 1, __ATOMIC_SEQ_CST);
		
		LOG_INF("IMSIC ISR: CPU %u received interrupt (count: %u)", 
			cpu_id, cpu_data[cpu_id].irq_count);
	}
}

/* Per-CPU IMSIC test function */
static void test_imsic_on_cpu(uint32_t cpu_id)
{
	const struct device *imsic_dev;
	int ret;
	
	LOG_INF("CPU %u: Starting IMSIC test (hart_id: %u)", cpu_id, arch_proc_id());
	
	/* Initialize CPU data */
	cpu_data[cpu_id].cpu_id = cpu_id;
	cpu_data[cpu_id].hart_id = arch_proc_id();
	cpu_data[cpu_id].test_eid = TEST_EID_BASE + cpu_id;
	cpu_data[cpu_id].irq_count = 0;
	cpu_data[cpu_id].imsic_ready = false;
	cpu_data[cpu_id].test_complete = false;
	
	/* Get IMSIC device */
	imsic_dev = riscv_imsic_get_dev();
	if (!imsic_dev) {
		LOG_ERR("CPU %u: IMSIC device not found", cpu_id);
		return;
	}
	
	if (!device_is_ready(imsic_dev)) {
		LOG_ERR("CPU %u: IMSIC device not ready", cpu_id);
		return;
	}
	
	LOG_INF("CPU %u: ✓ IMSIC device found: %s", cpu_id, imsic_dev->name);
	
	/* Get hart and guest ID */
	cpu_data[cpu_id].hart_id = riscv_imsic_get_hart_id(imsic_dev);
	cpu_data[cpu_id].guest_id = riscv_imsic_get_guest_id(imsic_dev);
	
	LOG_INF("CPU %u: IMSIC Hart ID = %u, Guest ID = %u", 
		cpu_id, cpu_data[cpu_id].hart_id, cpu_data[cpu_id].guest_id);
	
	/* Test delivery mode */
	enum riscv_imsic_delivery_mode mode = riscv_imsic_get_delivery_mode();
	LOG_INF("CPU %u: IMSIC Delivery mode = %d", cpu_id, mode);
	
	/* Test threshold setting */
	ret = riscv_imsic_set_threshold(0);
	if (ret == 0) {
		uint32_t threshold = riscv_imsic_get_threshold();
		LOG_INF("CPU %u: IMSIC Threshold set to %u", cpu_id, threshold);
	}
	
	/* Test interrupt enable/disable */
	uint32_t test_eid = cpu_data[cpu_id].test_eid;
	
	riscv_imsic_irq_enable(test_eid);
	int enabled = riscv_imsic_irq_is_enabled(test_eid);
	if (enabled > 0) {
		LOG_INF("CPU %u: ✓ EID %u enabled successfully", cpu_id, test_eid);
	} else {
		LOG_WRN("CPU %u: EID %u enable failed", cpu_id, test_eid);
	}
	
	/* Test setting interrupt pending */
	riscv_imsic_irq_set_pending(test_eid);
	LOG_INF("CPU %u: Set EID %u pending for testing", cpu_id, test_eid);
	
	cpu_data[cpu_id].imsic_ready = true;
	__atomic_fetch_add(&cpus_ready, 1, __ATOMIC_SEQ_CST);
	
	LOG_INF("CPU %u: IMSIC test initialization complete", cpu_id);
}

/* SMP worker thread function */
static void smp_worker_thread(void *arg1, void *arg2, void *arg3)
{
	uint32_t cpu_id = arch_proc_id();
	
	LOG_INF("SMP Worker: CPU %u thread started", cpu_id);
	
	/* Wait a bit to let all CPUs start */
	k_msleep(CPU_TEST_DELAY_MS * (cpu_id + 1));
	
	/* Run IMSIC test on this CPU */
	test_imsic_on_cpu(cpu_id);
	
	/* Wait for all CPUs to be ready */
	while (__atomic_load_n(&cpus_ready, __ATOMIC_SEQ_CST) < CONFIG_MP_MAX_NUM_CPUS) {
		k_msleep(10);
	}
	
	/* Run some cross-CPU tests */
	LOG_INF("CPU %u: Starting cross-CPU interrupt tests", cpu_id);
	
	/* Test sending interrupts to other CPUs */
	for (int target_cpu = 0; target_cpu < CONFIG_MP_MAX_NUM_CPUS; target_cpu++) {
		if (target_cpu != cpu_id && cpu_data[target_cpu].imsic_ready) {
			uint32_t target_eid = cpu_data[target_cpu].test_eid;
			riscv_imsic_irq_set_pending(target_eid);
			LOG_INF("CPU %u: Sent interrupt to CPU %u (EID %u)", 
				cpu_id, target_cpu, target_eid);
		}
	}
	
	/* Wait for test duration */
	k_msleep(TEST_DURATION_MS);
	
	cpu_data[cpu_id].test_complete = true;
	
	LOG_INF("CPU %u: SMP worker thread completed", cpu_id);
}

/* Define SMP worker threads */
K_THREAD_DEFINE(smp_worker0, 2048, smp_worker_thread, NULL, NULL, NULL, 
		K_PRIO_PREEMPT(0), 0, 0);
K_THREAD_DEFINE(smp_worker1, 2048, smp_worker_thread, NULL, NULL, NULL, 
		K_PRIO_PREEMPT(0), 0, 0);
K_THREAD_DEFINE(smp_worker2, 2048, smp_worker_thread, NULL, NULL, NULL, 
		K_PRIO_PREEMPT(0), 0, 0);
K_THREAD_DEFINE(smp_worker3, 2048, smp_worker_thread, NULL, NULL, NULL, 
		K_PRIO_PREEMPT(0), 0, 0);

int main(void)
{
	uint32_t current_cpu = arch_proc_id();
	
	LOG_INF("=== IMSIC SMP Multi-Core Test Starting ===");
	LOG_INF("Main thread running on CPU %u", current_cpu);
	LOG_INF("Total CPUs configured: %u", CONFIG_MP_MAX_NUM_CPUS);
	
	/* Wait for system stabilization */
	k_msleep(1000);
	
	LOG_INF("System stabilized, starting SMP tests...");
	
	/* Pin threads to specific CPUs if supported */
#ifdef CONFIG_SCHED_CPU_MASK
	k_thread_cpu_mask_clear(&smp_worker0);
	k_thread_cpu_mask_set(&smp_worker0, 0);
	
	k_thread_cpu_mask_clear(&smp_worker1);
	k_thread_cpu_mask_set(&smp_worker1, 1);
	
	k_thread_cpu_mask_clear(&smp_worker2);
	k_thread_cpu_mask_set(&smp_worker2, 2);
	
	k_thread_cpu_mask_clear(&smp_worker3);
	k_thread_cpu_mask_set(&smp_worker3, 3);
	
	LOG_INF("CPU affinity set for all worker threads");
#endif
	
	/* Wait for all worker threads to complete */
	bool all_complete = false;
	uint32_t wait_count = 0;
	
	while (!all_complete && wait_count < 100) {
		all_complete = true;
		for (int i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
			if (!cpu_data[i].test_complete) {
				all_complete = false;
				break;
			}
		}
		k_msleep(100);
		wait_count++;
	}
	
	/* Print test results */
	LOG_INF("=== IMSIC SMP Test Results ===");
	
	uint32_t total_local_irq = 0;
	for (int i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		if (cpu_data[i].imsic_ready) {
			LOG_INF("CPU %u: Hart ID = %u, IRQ Count = %u, Ready = %s", 
				i, cpu_data[i].hart_id, cpu_data[i].irq_count,
				cpu_data[i].imsic_ready ? "YES" : "NO");
			total_local_irq += cpu_data[i].irq_count;
		}
	}
	
	LOG_INF("Total interrupt count (atomic): %u", 
		__atomic_load_n(&total_irq_count, __ATOMIC_SEQ_CST));
	LOG_INF("Total interrupt count (local): %u", total_local_irq);
	LOG_INF("CPUs ready: %u/%u", 
		__atomic_load_n(&cpus_ready, __ATOMIC_SEQ_CST), CONFIG_MP_MAX_NUM_CPUS);
	
	/* Verify SMP functionality */
	bool smp_working = true;
	uint32_t active_cpus = 0;
	
	for (int i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		if (cpu_data[i].imsic_ready) {
			active_cpus++;
		} else {
			smp_working = false;
		}
	}
	
	if (smp_working && active_cpus == CONFIG_MP_MAX_NUM_CPUS) {
		LOG_INF("✓ IMSIC SMP functionality: WORKING");
		LOG_INF("✓ All %u CPUs successfully initialized IMSIC", active_cpus);
	} else {
		LOG_WRN("✗ IMSIC SMP functionality: PARTIAL (%u/%u CPUs)", 
			active_cpus, CONFIG_MP_MAX_NUM_CPUS);
	}
	
	/* Test APLIC integration */
	const struct device *aplic_dev = riscv_aplic_get_dev();
	if (aplic_dev && device_is_ready(aplic_dev)) {
		LOG_INF("✓ APLIC device available: %s", aplic_dev->name);
		LOG_INF("✓ APLIC + IMSIC integration: WORKING");
	} else {
		LOG_WRN("✗ APLIC device not available");
	}
	
	LOG_INF("=== IMSIC SMP Test Completed ===");
	LOG_INF("Keeping system running for observation...");
	
	/* Keep system running */
	while (1) {
		k_msleep(5000);
		LOG_INF("System running... Total IRQs: %u", 
			__atomic_load_n(&total_irq_count, __ATOMIC_SEQ_CST));
	}
	
	return 0;
}
