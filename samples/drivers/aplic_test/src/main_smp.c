#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/interrupt_controller/riscv_aplic.h>

LOG_MODULE_REGISTER(aplic_smp_test, LOG_LEVEL_INF);

/* APLIC register definitions */
#define APLIC_DOMAINCFG_OFFSET     0x00
#define APLIC_SOURCECFG_OFFSET     0x04
#define APLIC_SETIP_OFFSET         0x1C
#define APLIC_SETIE_OFFSET         0x24
#define APLIC_TARGET_OFFSET        0x3000
#define APLIC_IDC_OFFSET           0x4000

/* APLIC base address from device tree */
#define APLIC_BASE_ADDR            0x0c000000

/* Helper function to read 32-bit register */
static inline uint32_t aplic_read_reg(uint32_t offset)
{
	return *(volatile uint32_t *)((uintptr_t)APLIC_BASE_ADDR + offset);
}

/* Helper function to write 32-bit register */
static inline void aplic_write_reg(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)((uintptr_t)APLIC_BASE_ADDR + offset) = value;
}

void main(void)
{
	LOG_INF("=== APLIC SMP Test Starting ===");
	LOG_INF("Current CPU ID: %d", arch_proc_id());
	LOG_INF("Total CPUs: %d", CONFIG_MP_MAX_NUM_CPUS);

	/* Wait for SMP to stabilize */
	k_sleep(K_MSEC(2000));

	LOG_INF("System stabilized, checking APLIC device...");

	/* Test if APLIC device exists */
	const struct device *aplic_dev = riscv_aplic_get_dev();

	if (aplic_dev != NULL) {
		LOG_INF("APLIC device found: %s", aplic_dev->name);

		if (device_is_ready(aplic_dev)) {
			LOG_INF("APLIC device is ready");

			/* Read and display key APLIC registers */
			LOG_INF("=== APLIC Register Values (CPU %d) ===", arch_proc_id());

			/* Domain Configuration */
			uint32_t domaincfg = aplic_read_reg(APLIC_DOMAINCFG_OFFSET);
			LOG_INF("DOMAINCFG (0x%02X): 0x%08X", APLIC_DOMAINCFG_OFFSET, domaincfg);
			LOG_INF("  - IE (Interrupt Enable): %s", (domaincfg & 0x1) ? "ENABLED" : "DISABLED");
			LOG_INF("  - DM (Direct Mode): %s", (domaincfg & 0x2) ? "ENABLED" : "DISABLED");
			LOG_INF("  - BE (Big Endian): %s", (domaincfg & 0x4) ? "ENABLED" : "DISABLED");
			
			/* Try to enable domain if it's disabled */
			if ((domaincfg & 0x1) == 0) {
				LOG_INF("Domain is disabled, trying to enable it...");
				aplic_write_reg(APLIC_DOMAINCFG_OFFSET, 0x1);
				domaincfg = aplic_read_reg(APLIC_DOMAINCFG_OFFSET);
				LOG_INF("DOMAINCFG after enable attempt: 0x%08X", domaincfg);
			}

			/* Check target configuration for current CPU */
			uint32_t target_offset = APLIC_TARGET_OFFSET + (arch_proc_id() * 0x1000);
			uint32_t targetcfg = aplic_read_reg(target_offset);
			uint32_t target_ie = aplic_read_reg(target_offset + 0x04);
			uint32_t target_threshold = aplic_read_reg(target_offset + 0x08);
			
			LOG_INF("=== Target Configuration (CPU %d) ===", arch_proc_id());
			LOG_INF("Target offset: 0x%08X", target_offset);
			LOG_INF("TARGETCFG: 0x%08X", targetcfg);
			LOG_INF("Target IE: 0x%08X", target_ie);
			LOG_INF("Target Threshold: 0x%08X", target_threshold);

		} else {
			LOG_ERR("APLIC device is not ready");
		}
	} else {
		LOG_ERR("APLIC device not found");
	}

	LOG_INF("=== APLIC SMP Test Completed (CPU %d) ===", arch_proc_id());

	/* Keep running for a while to see output from all CPUs */
	LOG_INF("Keeping system running for 10 seconds...");
	k_sleep(K_MSEC(10000));

	LOG_INF("Test finished, shutting down...");
}
