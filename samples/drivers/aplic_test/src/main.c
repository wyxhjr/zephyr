/*
 * Copyright (c) 2024 Zephyr Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/interrupt_controller/riscv_aplic.h>

LOG_MODULE_REGISTER(aplic_test, LOG_LEVEL_INF);

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
	LOG_INF("=== APLIC Register Test Starting ===");
	
	/* Wait a bit for system to stabilize */
	k_sleep(K_MSEC(100));
	
	LOG_INF("System stabilized, checking APLIC device...");
	
	/* Debug: Check if aplic_init was called */
	LOG_INF("APLIC init debug flag: 0x%08X", aplic_init_called);
	if (aplic_init_called == 0xDEADBEEF) {
		LOG_INF("APLIC init was called successfully!");
	} else {
		LOG_INF("APLIC init was NOT called (flag=0x%08X)", aplic_init_called);
	}
	
	/* Test if APLIC device exists - try multiple ways */
	const struct device *aplic_dev = device_get_binding("aplic");
	if (aplic_dev == NULL) {
		aplic_dev = device_get_binding("aplic0");
	}
	if (aplic_dev == NULL) {
		/* Try to get device by DT node label */
		aplic_dev = DEVICE_DT_GET(DT_NODELABEL(aplic));
	}
	if (aplic_dev == NULL) {
		/* Try to get device through APLIC API */
		aplic_dev = riscv_aplic_get_dev();
	}
	
	if (aplic_dev != NULL) {
		LOG_INF("APLIC device found: %s", aplic_dev->name);
		
		if (device_is_ready(aplic_dev)) {
			LOG_INF("APLIC device is ready");
			
			/* Read and display key APLIC registers */
			LOG_INF("=== APLIC Register Values ===");
			
			/* Domain Configuration */
			uint32_t domaincfg = aplic_read_reg(APLIC_DOMAINCFG_OFFSET);
			LOG_INF("DOMAINCFG (0x%02X): 0x%08X", APLIC_DOMAINCFG_OFFSET, domaincfg);
			LOG_INF("  - Reserved[31:24]: 0x%02X (should be 0x80)", (domaincfg >> 24) & 0xFF);
			LOG_INF("  - IE (Interrupt Enable, bit 8): %s", (domaincfg & (1 << 8)) ? "ENABLED" : "DISABLED");
			LOG_INF("  - DM (Delivery Mode, bit 2): %s", (domaincfg & (1 << 2)) ? "MSI" : "DIRECT");
			LOG_INF("  - BE (Big Endian, bit 0): %s", (domaincfg & (1 << 0)) ? "BIG" : "LITTLE");
			
			/* Try to enable IE bit if it's disabled */
			if ((domaincfg & (1 << 8)) == 0) {
				LOG_INF("IE bit is disabled, attempting to enable...");
				
				/* Try writing IE=1 (bit 8), keep other bits as they are */
				uint32_t new_value = domaincfg | (1 << 8); /* Set IE bit */
				LOG_INF("Attempting to write: 0x%08X", new_value);
				aplic_write_reg(APLIC_DOMAINCFG_OFFSET, new_value);
				k_msleep(10);
				uint32_t new_domaincfg = aplic_read_reg(APLIC_DOMAINCFG_OFFSET);
				LOG_INF("After writing IE=1: DOMAINCFG = 0x%08X", new_domaincfg);
				
				if ((new_domaincfg & (1 << 8)) == 0) {
					/* Try the safe write method from spec: (x << 24) | x */
					LOG_INF("First attempt failed, trying safe write method...");
					uint32_t safe_value = (1 << 8) | ((1 << 8) << 24); /* IE bit in both positions */
					LOG_INF("Safe write value: 0x%08X", safe_value);
					aplic_write_reg(APLIC_DOMAINCFG_OFFSET, safe_value);
					k_msleep(10);
					new_domaincfg = aplic_read_reg(APLIC_DOMAINCFG_OFFSET);
					LOG_INF("After safe write: DOMAINCFG = 0x%08X", new_domaincfg);
				}
				
				if ((new_domaincfg & (1 << 8)) != 0) {
					LOG_INF("SUCCESS: IE bit enabled!");
				} else {
					LOG_INF("FAILED: Could not enable IE bit");
					LOG_INF("This might be a QEMU APLIC implementation limitation");
				}
			} else {
				LOG_INF("IE bit is already enabled!");
			}
			
			/* Test writing to other registers to see if they work */
			LOG_INF("=== Testing Register Write Capability ===");
			
			/* Test SOURCECFG register (should be writable) */
			uint32_t orig_sourcecfg = aplic_read_reg(APLIC_SOURCECFG_OFFSET);
			LOG_INF("Original SOURCECFG[0]: 0x%08X", orig_sourcecfg);
			
			aplic_write_reg(APLIC_SOURCECFG_OFFSET, 0x5); /* Try writing 0x5 */
			uint32_t new_sourcecfg = aplic_read_reg(APLIC_SOURCECFG_OFFSET);
			LOG_INF("After writing 0x5 to SOURCECFG[0]: 0x%08X", new_sourcecfg);
			
			/* Restore original value */
			aplic_write_reg(APLIC_SOURCECFG_OFFSET, orig_sourcecfg);
			uint32_t restored_sourcecfg = aplic_read_reg(APLIC_SOURCECFG_OFFSET);
			LOG_INF("After restoring SOURCECFG[0]: 0x%08X", restored_sourcecfg);
			
			/* Source Configuration for first few sources */
			LOG_INF("=== Source Configuration ===");
			for (int i = 0; i < 4; i++) {
				uint32_t sourcecfg = aplic_read_reg(APLIC_SOURCECFG_OFFSET + (i * 4));
				LOG_INF("SOURCECFG[%d] (0x%02X): 0x%08X", i, 
					APLIC_SOURCECFG_OFFSET + (i * 4), sourcecfg);
				if (sourcecfg != 0) {
					LOG_INF("  - D (Delegated): %s", (sourcecfg & 0x1) ? "YES" : "NO");
					LOG_INF("  - DM (Direct Mode): %s", (sourcecfg & 0x2) ? "YES" : "NO");
					LOG_INF("  - H (Hardwired): %s", (sourcecfg & 0x4) ? "YES" : "NO");
					LOG_INF("  - Priority: %d", (sourcecfg >> 8) & 0xFF);
				}
			}
			
			/* Interrupt Pending and Enable status */
			LOG_INF("=== Interrupt Status ===");
			uint32_t setip = aplic_read_reg(APLIC_SETIP_OFFSET);
			uint32_t setie = aplic_read_reg(APLIC_SETIE_OFFSET);
			LOG_INF("SETIP (0x%02X): 0x%08X", APLIC_SETIP_OFFSET, setip);
			LOG_INF("SETIE (0x%02X): 0x%08X", APLIC_SETIE_OFFSET, setie);
			
		} else {
			LOG_ERR("APLIC device is not ready");
		}
	} else {
		LOG_ERR("APLIC device not found");
	}
	
	LOG_INF("=== APLIC Register Test Completed ===");
	
	/* Keep running for a while to see output */
	LOG_INF("Keeping system running for 5 seconds...");
	k_sleep(K_MSEC(5000));
	
	LOG_INF("Test finished, shutting down...");
}