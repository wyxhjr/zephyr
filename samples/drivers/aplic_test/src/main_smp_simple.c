#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(aplic_smp_simple, LOG_LEVEL_INF);

/* Simple work function for each core */
static void core_work_func(void *arg1, void *arg2, void *arg3)
{
	uint32_t cpu_id = arch_proc_id();
	
	LOG_INF("Core %d: Starting work function", cpu_id);
	
	/* Simulate some work */
	for (int i = 0; i < 1000000; i++) {
		__asm__ volatile("nop");
	}
	
	LOG_INF("Core %d: Work function completed", cpu_id);
}

/* Thread definitions for multi-core testing */
K_THREAD_DEFINE(core_1, 2048, core_work_func, NULL, NULL, NULL, 7, 0, 0);
K_THREAD_DEFINE(core_2, 2048, core_work_func, NULL, NULL, NULL, 7, 0, 0);
K_THREAD_DEFINE(core_3, 2048, core_work_func, NULL, NULL, NULL, 7, 0, 0);

void main(void)
{
	LOG_INF("=== Simple SMP Test Starting ===");
	LOG_INF("SMP enabled: %s", CONFIG_SMP ? "YES" : "NO");
	LOG_INF("Max CPUs: %d", CONFIG_MP_MAX_NUM_CPUS);
	LOG_INF("Current CPU: %d", arch_proc_id());
	
	/* Wait a bit for system to stabilize */
	k_sleep(K_MSEC(1000));
	
	LOG_INF("System stabilized, starting multi-core work...");
	
	/* Wait a bit to see if other cores start */
	k_sleep(K_MSEC(2000));
	
	LOG_INF("Main core work completed");
	LOG_INF("=== Simple SMP Test Completed ===");
	
	/* Keep running for a while to see other cores */
	LOG_INF("Keeping system running for 5 seconds...");
	k_sleep(K_MSEC(5000));
	
	LOG_INF("Test finished, shutting down...");
}
