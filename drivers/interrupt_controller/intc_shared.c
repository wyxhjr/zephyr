/*
 * Copyright (c) 2024 Zephyr contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "intc_shared.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/interrupt_controller/riscv_aplic.h>
#include <zephyr/drivers/interrupt_controller/riscv_imsic.h>

LOG_MODULE_REGISTER(intc_shared, CONFIG_INTC_LOG_LEVEL);

/**
 * @brief Shared external interrupt handler for APLIC and IMSIC
 * This function handles both APLIC and IMSIC interrupts on the same IRQ line
 * Implements true hardware signal line sharing with proper ISR dispatching
 */
void shared_ext_isr(const void *arg)
{
	/* Debug: Log that this function was called */
	printk("SHARED_EXT_ISR: Called! Checking for APLIC interrupts...\n");
	
	/* Check APLIC interrupts first */
	const struct device *aplic_dev = riscv_aplic_get_dev();
	if (device_is_ready(aplic_dev)) {
		printk("SHARED_EXT_ISR: APLIC device is ready, getting pending interrupt...\n");
		/* Get pending interrupt from APLIC */
		unsigned int aplic_irq = riscv_aplic_get_irq();
		printk("SHARED_EXT_ISR: APLIC returned IRQ: %u\n", aplic_irq);
		if (aplic_irq != 0 && aplic_irq != UINT_MAX) {
			printk("SHARED_EXT_ISR: Valid APLIC IRQ %u, processing...\n", aplic_irq);
			/* Process APLIC interrupt by calling the registered ISR */
			if (aplic_irq < CONFIG_NUM_IRQS) {
				/* Get the ISR from Zephyr's software ISR table */
				const struct _isr_table_entry *entry = &_sw_isr_table[aplic_irq];
				if (entry->isr != NULL) {
					printk("SHARED_EXT_ISR: Calling ISR for IRQ %u\n", aplic_irq);
					/* Call the actual interrupt service routine */
					entry->isr(entry->arg);
					LOG_DBG("APLIC: Processed interrupt %u", aplic_irq);
					printk("SHARED_EXT_ISR: ISR for IRQ %u completed\n", aplic_irq);
				} else {
					LOG_WRN("APLIC: No ISR registered for interrupt %u", aplic_irq);
					printk("SHARED_EXT_ISR: No ISR registered for IRQ %u\n", aplic_irq);
				}
			} else {
				LOG_ERR("APLIC: Invalid IRQ number %u", aplic_irq);
				printk("SHARED_EXT_ISR: Invalid IRQ number %u\n", aplic_irq);
			}
		} else {
			printk("SHARED_EXT_ISR: No valid APLIC IRQ (got %u)\n", aplic_irq);
		}
	} else {
		printk("SHARED_EXT_ISR: APLIC device not ready\n");
	}
	
	/* Check for pending MSI interrupts in IMSIC */
	/* IMSIC supports up to 64 external interrupt IDs (EIDs) */
	const struct device *imsic_dev = riscv_imsic_get_dev();
	if (imsic_dev != NULL && device_is_ready(imsic_dev)) {
		printk("SHARED_EXT_ISR: IMSIC device is ready, checking for pending MSI interrupts...\n");
		
		/* Read IMSIC EIP registers to check for pending interrupts */
		/* We need to read the actual hardware registers to see what's pending */
		/* Get IMSIC base address from device tree or configuration */
		/* For now, use a hardcoded address based on QEMU virt machine */
		mem_addr_t imsic_base = 0x28000000; /* QEMU virt machine IMSIC base address */
		
		/* Check EIP0 register (EIDs 0-31) */
		/* IMSIC EIP0 register offset: 0x80 */
		mem_addr_t eip0_addr = imsic_base + 0x80;
		uint32_t eip0_pending = *(volatile uint32_t *)eip0_addr;
		
		/* Check EIP1 register (EIDs 32-63) if supported */
		/* IMSIC EIP63 register offset: 0xBF */
		mem_addr_t eip1_addr = imsic_base + 0xBF;
		uint32_t eip1_pending = *(volatile uint32_t *)eip1_addr;
		
		printk("SHARED_EXT_ISR: IMSIC EIP0: 0x%08X, EIP1: 0x%08X\n", eip0_pending, eip1_pending);
		
		/* Process pending interrupts from EIP0 (EIDs 0-31) */
		if (eip0_pending != 0) {
			for (int eid = 0; eid < 32; eid++) {
				if (eip0_pending & BIT(eid)) {
					printk("SHARED_EXT_ISR: Found pending MSI interrupt EID %d\n", eid);
					
					/* Check if this EID is enabled */
					if (riscv_imsic_irq_is_enabled(eid) > 0) {
						printk("SHARED_EXT_ISR: EID %d is enabled, processing...\n", eid);
						
						/* Process MSI interrupt by calling the registered ISR */
						if (eid < CONFIG_NUM_IRQS) {
							/* Get the ISR from Zephyr's software ISR table */
							const struct _isr_table_entry *entry = &_sw_isr_table[eid];
							if (entry->isr != NULL) {
								printk("SHARED_EXT_ISR: Calling ISR for MSI EID %d\n", eid);
								/* Call the actual MSI interrupt service routine */
								entry->isr(entry->arg);
								LOG_DBG("IMSIC: Processed MSI interrupt EID %d", eid);
								printk("SHARED_EXT_ISR: ISR for MSI EID %d completed\n", eid);
							} else {
								LOG_WRN("IMSIC: No ISR registered for EID %d", eid);
								printk("SHARED_EXT_ISR: No ISR registered for MSI EID %d\n", eid);
							}
						} else {
							LOG_ERR("IMSIC: Invalid EID %d", eid);
							printk("SHARED_EXT_ISR: Invalid EID %d\n", eid);
						}
						
						/* Clear the MSI interrupt after processing */
						riscv_imsic_irq_clear_pending(eid);
						printk("SHARED_EXT_ISR: Cleared pending MSI interrupt EID %d\n", eid);
					} else {
						printk("SHARED_EXT_ISR: EID %d is not enabled, skipping\n", eid);
					}
				}
			}
		}
		
		/* Process pending interrupts from EIP1 (EIDs 32-63) */
		if (eip1_pending != 0) {
			for (int eid = 32; eid < 64; eid++) {
				if (eip1_pending & BIT(eid - 32)) {
					printk("SHARED_EXT_ISR: Found pending MSI interrupt EID %d\n", eid);
					
					/* Check if this EID is enabled */
					if (riscv_imsic_irq_is_enabled(eid) > 0) {
						printk("SHARED_EXT_ISR: EID %d is enabled, processing...\n", eid);
						
						/* Process MSI interrupt by calling the registered ISR */
						if (eid < CONFIG_NUM_IRQS) {
							/* Get the ISR from Zephyr's software ISR table */
							const struct _isr_table_entry *entry = &_sw_isr_table[eid];
							if (entry->isr != NULL) {
								printk("SHARED_EXT_ISR: Calling ISR for MSI EID %d\n", eid);
								/* Call the actual MSI interrupt service routine */
								entry->isr(entry->arg);
								LOG_DBG("IMSIC: Processed MSI interrupt EID %d", eid);
								printk("SHARED_EXT_ISR: ISR for MSI EID %d completed\n", eid);
							} else {
								LOG_WRN("IMSIC: No ISR registered for EID %d", eid);
								printk("SHARED_EXT_ISR: No ISR registered for MSI EID %d\n", eid);
							}
						} else {
							LOG_ERR("IMSIC: Invalid EID %d", eid);
							printk("SHARED_EXT_ISR: Invalid EID %d\n", eid);
						}
						
						/* Clear the MSI interrupt after processing */
						riscv_imsic_irq_clear_pending(eid);
						printk("SHARED_EXT_ISR: Cleared pending MSI interrupt EID %d\n", eid);
					} else {
						printk("SHARED_EXT_ISR: EID %d is not enabled, skipping\n", eid);
					}
				}
			}
		}
		
		if (eip0_pending == 0 && eip1_pending == 0) {
			printk("SHARED_EXT_ISR: No pending MSI interrupts found in IMSIC\n");
		}
	} else {
		printk("SHARED_EXT_ISR: IMSIC device not ready\n");
	}
}
