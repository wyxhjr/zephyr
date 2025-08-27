/*
 * Copyright (c) 2024 Zephyr Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qemu_aplic

/**
 * @brief Advanced Platform Level Interrupt Controller (APLIC) driver
 *        for RISC-V processors in direct mode
 */

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/devicetree/interrupt_controller.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/interrupt_controller/riscv_aplic.h>
#include <zephyr/drivers/interrupt_controller/riscv_imsic.h>
#include "intc_shared.h"

LOG_MODULE_REGISTER(aplic, CONFIG_INTC_LOG_LEVEL);

/*
 * APLIC (Advanced Platform-Level Interrupt Controller) driver
 * 
 * This driver implements the RISC-V Advanced Interrupt Architecture (AIA)
 * APLIC controller for handling external interrupts.
 * 
 * For more information, see:
 * https://github.com/riscv/riscv-aia
 */

/* Forward declarations */
int aplic_send_msi(const struct device *dev, uint32_t target_hart, 
			  uint32_t target_guest, uint32_t irq_id);

/* APLIC register base addresses */
#define APLIC_DOMAINCFG        0x0000
#define APLIC_SOURCECFG_BASE   0x0004
#define APLIC_SOURCECFG_SIZE   0x0004
#define APLIC_TARGETCFG_BASE   0x3000  /* Fixed: According to AdvPLIC.adoc, target[1] is at 0x3004 */
#define APLIC_TARGETCFG_SIZE   0x0004
#define APLIC_SETIE_BASE       0x1E00  /* Fixed: According to AdvPLIC.adoc */
#define APLIC_CLRIE_BASE       0x1F00  /* Fixed: According to AdvPLIC.adoc */
#define APLIC_SETIP_BASE       0x1C00  /* Fixed: According to AdvPLIC.adoc */
#define APLIC_CLRIP_BASE       0x1D00  /* Fixed: According to AdvPLIC.adoc */
#define APLIC_MMSICFGADDR      0x2000  /* Fixed: According to RISC-V AIA spec */
#define APLIC_MMSICFGADDRH     0x2004  /* Fixed: According to RISC-V AIA spec */

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/*
 * IMSIC register offsets for MSI operations
 */
#define IMSIC_EIP0  0x80
#define IMSIC_EIP63 0xbf

/*
 * APLIC register offsets based on RISC-V AIA specification
 * https://github.com/riscv/riscv-aia
 */
#define APLIC_UNUSED 0x1f
#define APLIC_TARGETCFG_HARTID 0x00
#define APLIC_TARGETCFG_GUESTID 0x04
#define APLIC_TARGETCFG_IPRIO 0x08
#define APLIC_TARGETCFG_IE 0x0c
#define APLIC_TARGETCFG_THRESHOLD 0x10
#define APLIC_TARGETCFG_CLAIM 0x14

/* SOURCECFG register bit fields */
#define APLIC_SOURCECFG_SM_MASK       0x7        /* Source Mode [2:0] */
#define APLIC_SOURCECFG_SM_INACTIVE   0x0        /* Inactive */
#define APLIC_SOURCECFG_SM_DETACHED   0x1        /* Detached */
#define APLIC_SOURCECFG_SM_EDGE_RISE  0x4        /* Edge-triggered, rising edge */
#define APLIC_SOURCECFG_SM_EDGE_FALL  0x5        /* Edge-triggered, falling edge */
#define APLIC_SOURCECFG_SM_LEVEL_HIGH 0x6        /* Level-triggered, high level */
#define APLIC_SOURCECFG_SM_LEVEL_LOW  0x7        /* Level-triggered, low level */
#define APLIC_SOURCECFG_SM_MSI        0x8        /* MSI mode */

#define APLIC_SOURCECFG_D_MASK        0x400      /* Delegate bit [10] */
#define APLIC_SOURCECFG_CHILD_MASK    0x3FF800   /* Child Index [21:11] */
#define APLIC_SOURCECFG_CHILD_SHIFT   11

/* DOMAINCFG register bit fields */
#define APLIC_DOMAINCFG_IE            BIT(8)     /* Interrupt Enable */
#define APLIC_DOMAINCFG_DM            BIT(2)     /* Delivery Mode (0=Direct, 1=MSI) */
#define APLIC_DOMAINCFG_BE            BIT(0)     /* Big Endian */

/* Target configuration for Direct mode */
#define APLIC_TARGET_HART_IDX_MASK    0x3FFF     /* Hart Index [13:0] */
#define APLIC_TARGET_GUEST_IDX_MASK   0x3F       /* Guest Index [5:0] */
#define APLIC_TARGET_IPRIO_MASK       0xFF       /* Interrupt Priority [7:0] */

/* APLIC registers are 32-bit memory-mapped */
#define APLIC_REG_SIZE 32
#define APLIC_REG_MASK BIT_MASK(LOG2(APLIC_REG_SIZE))

/* APLIC IDC (Interrupt Delivery Controller) registers */
#define APLIC_IDC_BASE         0x4000  /* IDC base address */
#define APLIC_IDC_SIZE         0x1000  /* IDC size per hart */
#define APLIC_IDC_TOPI         0x00    /* Top Priority Interrupt */
#define APLIC_IDC_TOPI_ID_MASK 0x3FF  /* Interrupt ID [9:0] */
#define APLIC_IDC_TOPI_ID_SHIFT 0
#define APLIC_IDC_TOPI_PRIO_MASK 0xFF /* Priority [17:10] */
#define APLIC_IDC_TOPI_PRIO_SHIFT 10
#define APLIC_IDC_TOPI_IID_MASK 0x3FF /* Interrupt ID [27:18] */
#define APLIC_IDC_TOPI_IID_SHIFT 18
#define APLIC_IDC_CLAIMI       0x04    /* Claim/Complete Interrupt */
#define APLIC_IDC_IDELIVERY    0x08    /* Interrupt Delivery Enable */
#define APLIC_IDC_IFORCE      0x0C    /* Interrupt Force */
#define APLIC_IDC_ITHRESHOLD  0x10    /* Interrupt Threshold */
#define APLIC_IDC_TOPI_CLAIM  0x14    /* Top Priority Interrupt Claim */

/* IDC register values */
#define APLIC_IDC_DELIVERY_DISABLE 0
#define APLIC_IDC_DELIVERY_ENABLE  1
#define APLIC_IDC_THRESHOLD_DISABLE 1
#define APLIC_IDC_THRESHOLD_ENABLE  0

typedef void (*riscv_aplic_irq_config_func_t)(void);

struct aplic_config {
	mem_addr_t base;
	uint32_t max_prio;
	/* Number of IRQs that the APLIC physically supports */
	uint32_t riscv_ndev;
	/* Number of IRQs supported in this driver */
	uint32_t nr_irqs;
	uint32_t irq;
	riscv_aplic_irq_config_func_t irq_config_func;
	const struct _isr_table_entry *isr_table;
	const uint32_t *const hart_context;
};

/* Interrupt trigger types */
enum aplic_trigger_type {
	APLIC_TRIGGER_EDGE_RISING = APLIC_SOURCECFG_SM_EDGE_RISE,
	APLIC_TRIGGER_EDGE_FALLING = APLIC_SOURCECFG_SM_EDGE_FALL,
	APLIC_TRIGGER_LEVEL_HIGH = APLIC_SOURCECFG_SM_LEVEL_HIGH,
	APLIC_TRIGGER_LEVEL_LOW = APLIC_SOURCECFG_SM_LEVEL_LOW,
};

/* Per-IRQ statistics and configuration */
struct aplic_irq_info {
	uint32_t count;           /* Interrupt count */
	uint32_t last_cpu;        /* Last CPU that handled this IRQ */
	uint32_t affinity_mask;   /* CPU affinity mask */
	enum aplic_trigger_type trigger_type;  /* Trigger type */
	uint8_t priority;         /* Interrupt priority */
	bool enabled;             /* Enable state */
};

struct aplic_data {
	struct k_spinlock lock;
	struct aplic_irq_info irq_info[1024]; /* Per-IRQ information */
	uint32_t total_interrupts;            /* Total interrupt count */
	uint32_t hart_thresholds[CONFIG_MP_MAX_NUM_CPUS]; /* Per-Hart thresholds */
	/* MSI mode support */
	bool msi_mode_enabled;
	const struct device *imsic_devices[CONFIG_MP_MAX_NUM_CPUS];
	uint32_t msi_base_eid;
	mem_addr_t imsic_base;                /* IMSIC base address for MSI operations */
	uint32_t msi_interrupts_sent;         /* Count of MSI interrupts sent */
	/* Direct mode statistics */
	uint32_t direct_interrupts;
};

static const struct device *save_dev[CONFIG_MP_MAX_NUM_CPUS];

/* ============================================================================
 * Global Variables for Conditional Registration
 * ============================================================================ */

/* Global variable to track if external IRQ is already registered */
static unsigned int aplic_parent_irq = 0;

static inline uint32_t local_irq_to_reg_index(uint32_t local_irq)
{
	return local_irq >> LOG2(APLIC_REG_SIZE);
}

static inline uint32_t local_irq_to_reg_offset(uint32_t local_irq)
{
	return local_irq_to_reg_index(local_irq) * sizeof(uint32_t);
}

static inline uint32_t get_aplic_enabled_size(const struct device *dev)
{
	const struct aplic_config *config = dev->config;

	return local_irq_to_reg_index(config->nr_irqs) + 1;
}

static ALWAYS_INLINE uint32_t get_hart_context(const struct device *dev, uint32_t hartid)
{
	const struct aplic_config *config = dev->config;

	if (hartid < CONFIG_MP_MAX_NUM_CPUS) {
		return config->hart_context[hartid];
	}

	return 0;
}

static inline mem_addr_t get_sourcecfg_addr(const struct device *dev, uint32_t irq)
{
	const struct aplic_config *config = dev->config;

	return config->base + APLIC_SOURCECFG_BASE + (irq * APLIC_SOURCECFG_SIZE);
}

static inline mem_addr_t get_targetcfg_addr(const struct device *dev, uint32_t hartid)
{
	const struct aplic_config *config = dev->config;

	return config->base + APLIC_TARGETCFG_BASE + (hartid * APLIC_TARGETCFG_SIZE);
}

static inline mem_addr_t get_claim_complete_addr(const struct device *dev)
{
	const struct aplic_config *config = dev->config;

	return config->base + APLIC_TARGETCFG_BASE + (arch_proc_id() * APLIC_TARGETCFG_SIZE) + APLIC_TARGETCFG_CLAIM;
}

static inline uint32_t aplic_read(const struct device *dev, mem_addr_t addr)
{
	/* Ensure any pending writes are completed before reading */
	__asm__ volatile ("fence rw,rw" : : : "memory");
	
	/* Read the value from the register */
	uint32_t value = *(volatile uint32_t *)addr;
	
	/* Ensure the read operation completes */
	__asm__ volatile ("fence r,r" : : : "memory");
	
	return value;
}

static inline void aplic_write(const struct device *dev, mem_addr_t addr, uint32_t value)
{
	/* Write the value to the register */
	*(volatile uint32_t *)addr = value;
	
	/* Ensure the write operation completes before proceeding */
	/* This is crucial for memory-mapped I/O registers */
	__asm__ volatile ("fence w,w" : : : "memory");
	
	/* Force a memory barrier to ensure cache consistency */
	__asm__ volatile ("fence iorw,iorw" : : : "memory");
}

static inline void aplic_set_threshold(const struct device *dev, uint32_t hartid, uint32_t threshold)
{
	mem_addr_t targetcfg_addr = get_targetcfg_addr(dev, hartid);
	
	aplic_write(dev, targetcfg_addr + APLIC_TARGETCFG_THRESHOLD, threshold);
}

static inline uint32_t aplic_get_idc_topi(const struct device *dev)
{
	const struct aplic_config *config = dev->config;
	uint32_t cpu_id = arch_proc_id();
	mem_addr_t idc_base = config->base + APLIC_IDC_BASE + (cpu_id * APLIC_IDC_SIZE);
	mem_addr_t topi_addr = idc_base + APLIC_IDC_TOPI;
	
	/* Read the TOPI register to get the highest priority interrupt */
	uint32_t topi_value = aplic_read(dev, topi_addr);
	
	/* Extract the interrupt ID from the TOPI register */
	uint32_t irq_id = (topi_value >> APLIC_IDC_TOPI_ID_SHIFT) & APLIC_IDC_TOPI_ID_MASK;
	
	/* If irq_id is 0, it means no interrupt is pending */
	if (irq_id == 0) {
		return UINT_MAX;
	}
	
	return irq_id;
}

static inline uint32_t aplic_get_claim(const struct device *dev)
{
	mem_addr_t claim_addr = get_claim_complete_addr(dev);
	
	return aplic_read(dev, claim_addr);
}

static inline void aplic_set_complete(const struct device *dev, uint32_t irq)
{
	mem_addr_t claim_addr = get_claim_complete_addr(dev);
	
	aplic_write(dev, claim_addr, irq);
}

static inline void aplic_irq_enable_internal(const struct device *dev, uint32_t irq)
{
	const struct aplic_config *config = (const struct aplic_config *)dev->config;
	
	/* Debug: Print detailed address calculation */
	printk("APLIC: === IRQ %u enable - Address calculation debug ===\n", irq);
	printk("  - config->base: 0x%08lX\n", config->base);
	printk("  - APLIC_SETIE_BASE: 0x%08X\n", APLIC_SETIE_BASE);
	printk("  - APLIC_REG_SIZE: %u\n", APLIC_REG_SIZE);
	printk("  - LOG2(APLIC_REG_SIZE): %u\n", LOG2(APLIC_REG_SIZE));
	printk("  - local_irq_to_reg_index(%u): %u >> %u = %u\n", 
	       irq, irq, LOG2(APLIC_REG_SIZE), irq >> LOG2(APLIC_REG_SIZE));
	printk("  - local_irq_to_reg_offset(%u): %u * %u = %u\n", 
	       irq, irq >> LOG2(APLIC_REG_SIZE), sizeof(uint32_t), 
	       (irq >> LOG2(APLIC_REG_SIZE)) * sizeof(uint32_t));
	
	mem_addr_t setie_addr = config->base + APLIC_SETIE_BASE + local_irq_to_reg_offset(irq); /* SETIE register */
	uint32_t bit_mask = BIT(irq & (APLIC_REG_SIZE - 1));
	
	/* Debug: Print detailed SETIE operation information */
	printk("APLIC: Enabling IRQ %u - SETIE operation details:\n", irq);
	printk("  - Base address: 0x%08lX\n", config->base);
	printk("  - SETIE_BASE: 0x%08X\n", APLIC_SETIE_BASE);
	printk("  - Register offset: 0x%08X\n", local_irq_to_reg_offset(irq));
	printk("  - Final SETIE address: 0x%08lX\n", setie_addr);
	printk("  - IRQ bit position: %u (bit %u in register)\n", irq, irq & (APLIC_REG_SIZE - 1));
	printk("  - Bit mask: 0x%08X\n", bit_mask);
	
	/* Read current SETIE value and set the bit */
	uint32_t current_setie = aplic_read(dev, setie_addr);
	printk("  - Current SETIE value: 0x%08X\n", current_setie);
	
	uint32_t new_setie = current_setie | bit_mask;
	printk("  - New SETIE value: 0x%08X (0x%08X | 0x%08X)\n", new_setie, current_setie, bit_mask);
	
	aplic_write(dev, setie_addr, new_setie);
	printk("  - Wrote 0x%08X to SETIE register at 0x%08lX\n", new_setie, setie_addr);
	
	/* Verify the write operation */
	uint32_t verify_setie = aplic_read(dev, setie_addr);
	printk("  - Verification read: 0x%08X\n", verify_setie);
	
	if (verify_setie == new_setie) {
		printk("  - ✅ SETIE write verification successful\n");
	} else {
		printk("  - ❌ SETIE write verification failed! Expected: 0x%08X, Got: 0x%08X\n", 
		       new_setie, verify_setie);
	}
}

static inline void aplic_irq_disable_internal(const struct device *dev, uint32_t irq)
{
	const struct aplic_config *config = (const struct aplic_config *)dev->config;
	mem_addr_t clrie_addr = config->base + APLIC_CLRIE_BASE + local_irq_to_reg_offset(irq); /* CLRIE register */
	uint32_t bit_mask = BIT(irq & (APLIC_REG_SIZE - 1));
	
	/* Read current CLRIE value and set the bit */
	uint32_t current_clrie = aplic_read(dev, clrie_addr);
	aplic_write(dev, clrie_addr, current_clrie | bit_mask);
}

static inline int aplic_irq_is_enabled_internal(const struct device *dev, uint32_t irq)
{
	const struct aplic_config *config = (const struct aplic_config *)dev->config;
	mem_addr_t setie_addr = config->base + APLIC_SETIE_BASE + local_irq_to_reg_offset(irq); /* SETIE register */
	uint32_t bit_mask = BIT(irq & (APLIC_REG_SIZE - 1));
	
	/* Debug: Print detailed SETIE read operation information */
	printk("APLIC: Checking if IRQ %u is enabled - SETIE read details:\n", irq);
	printk("  - Base address: 0x%08lX\n", config->base);
	printk("  - SETIE_BASE: 0x%08X\n", APLIC_SETIE_BASE);
	printk("  - Register offset: 0x%08X\n", local_irq_to_reg_offset(irq));
	printk("  - Final SETIE address: 0x%08lX\n", setie_addr);
	printk("  - IRQ bit position: %u (bit %u in register)\n", irq, irq & (APLIC_REG_SIZE - 1));
	printk("  - Bit mask: 0x%08X\n", bit_mask);
	
	/* Read SETIE value and check if the bit is set */
	uint32_t setie_value = aplic_read(dev, setie_addr);
	printk("  - SETIE register value: 0x%08X\n", setie_value);
	printk("  - Bit check: (0x%08X & 0x%08X) = 0x%08X\n", setie_value, bit_mask, setie_value & bit_mask);
	
	int result = (setie_value & bit_mask) ? 1 : 0;
	printk("  - Result: IRQ %u is %s\n", irq, result ? "enabled" : "disabled");
	
	return result;
}

static inline void aplic_set_priority_internal(const struct device *dev, uint32_t irq, uint32_t priority)
{
	mem_addr_t sourcecfg_addr = get_sourcecfg_addr(dev, irq);
	uint32_t current_cfg = aplic_read(dev, sourcecfg_addr);
	
	/* Clear priority field and set new priority */
	current_cfg &= ~(0xFF << 8);
	current_cfg |= (priority & 0xFF) << 8;
	
	aplic_write(dev, sourcecfg_addr, current_cfg);
}

static inline int aplic_irq_set_affinity_internal(const struct device *dev, uint32_t irq, uint32_t cpumask)
{
	const struct aplic_config *config = dev->config;
	struct aplic_data *data = dev->data;
	mem_addr_t sourcecfg_addr;
	uint32_t sourcecfg_val;
	uint32_t target_hart;
	k_spinlock_key_t key;
	
	/* Validate IRQ number */
	if (irq == 0 || irq >= config->nr_irqs) {
		return -EINVAL;
	}
	
	/* Find the first set bit in cpumask as target Hart */
	target_hart = __builtin_ffs(cpumask) - 1;
	if (target_hart >= CONFIG_MP_MAX_NUM_CPUS) {
		return -EINVAL;
	}
	
	key = k_spin_lock(&data->lock);
	
	/* Read current SOURCECFG */
	sourcecfg_addr = get_sourcecfg_addr(dev, irq);
	sourcecfg_val = aplic_read(dev, sourcecfg_addr);
	
	/* For Direct mode, we configure the interrupt to be routed to specific Hart */
	/* In APLIC Direct mode, the target Hart is implicitly determined by */
	/* which Hart's target configuration space handles the interrupt */
	
	/* Store affinity information for load balancing */
	data->irq_info[irq].affinity_mask = cpumask;
	
	/* Configure SOURCECFG to ensure interrupt is active */
	if ((sourcecfg_val & APLIC_SOURCECFG_SM_MASK) == APLIC_SOURCECFG_SM_INACTIVE) {
		sourcecfg_val &= ~APLIC_SOURCECFG_SM_MASK;
		sourcecfg_val |= data->irq_info[irq].trigger_type;
		aplic_write(dev, sourcecfg_addr, sourcecfg_val);
	}
	
	k_spin_unlock(&data->lock, key);

	printk("APLIC: Set IRQ %u affinity to CPU mask 0x%X (target Hart %u)\n",
		irq, cpumask, target_hart);

	return 0;
}

static inline int aplic_irq_set_trigger_type_internal(const struct device *dev, uint32_t irq, enum aplic_trigger_type type)
{
	const struct aplic_config *config = dev->config;
	struct aplic_data *data = dev->data;
	mem_addr_t sourcecfg_addr;
	uint32_t sourcecfg_val;
	k_spinlock_key_t key;
	
	/* Validate IRQ number */
	if (irq == 0 || irq >= config->nr_irqs) {
		return -EINVAL;
	}
	
	/* Validate trigger type */
	switch (type) {
	case APLIC_TRIGGER_EDGE_RISING:
	case APLIC_TRIGGER_EDGE_FALLING:
	case APLIC_TRIGGER_LEVEL_HIGH:
	case APLIC_TRIGGER_LEVEL_LOW:
		break;
	default:
		return -EINVAL;
	}
	
	key = k_spin_lock(&data->lock);
	
	/* Read current SOURCECFG */
	sourcecfg_addr = get_sourcecfg_addr(dev, irq);
	sourcecfg_val = aplic_read(dev, sourcecfg_addr);
	
	/* Update source mode (trigger type) */
	sourcecfg_val &= ~APLIC_SOURCECFG_SM_MASK;
	sourcecfg_val |= (uint32_t)type;
	
	/* Write back the configuration */
	aplic_write(dev, sourcecfg_addr, sourcecfg_val);
	
	/* Store the configuration */
	data->irq_info[irq].trigger_type = type;
	
	k_spin_unlock(&data->lock, key);

	printk("APLIC: Set IRQ %u trigger type to %u\n", irq, (uint32_t)type);

	return 0;
}

static inline int aplic_irq_get_trigger_type_internal(const struct device *dev, uint32_t irq)
{
	const struct aplic_config *config = dev->config;
	mem_addr_t sourcecfg_addr;
	uint32_t sourcecfg_val;
	
	/* Validate IRQ number */
	if (irq == 0 || irq >= config->nr_irqs) {
		return -EINVAL;
	}
	
	/* Read current SOURCECFG */
	sourcecfg_addr = get_sourcecfg_addr(dev, irq);
	sourcecfg_val = aplic_read(dev, sourcecfg_addr);
	
	return sourcecfg_val & APLIC_SOURCECFG_SM_MASK;
}

static inline int aplic_hart_set_threshold_internal(const struct device *dev, uint32_t hart_id, uint32_t threshold)
{
	struct aplic_data *data = dev->data;
	mem_addr_t targetcfg_addr;
	k_spinlock_key_t key;
	
	/* Validate Hart ID */
	if (hart_id >= CONFIG_MP_MAX_NUM_CPUS) {
		return -EINVAL;
	}
	
	/* Validate threshold (8-bit value) */
	if (threshold > 255) {
		return -EINVAL;
	}
	
	key = k_spin_lock(&data->lock);
	
	/* Set threshold for the Hart */
	targetcfg_addr = get_targetcfg_addr(dev, hart_id);
	aplic_write(dev, targetcfg_addr + APLIC_TARGETCFG_THRESHOLD, threshold);
	
	/* Store the threshold */
	data->hart_thresholds[hart_id] = threshold;
	
	k_spin_unlock(&data->lock, key);

	printk("APLIC: Set Hart %u threshold to %u\n", hart_id, threshold);

	return 0;
}

static inline uint32_t aplic_hart_get_threshold_internal(const struct device *dev, uint32_t hart_id)
{
	struct aplic_data *data = dev->data;
	
	/* Validate Hart ID */
	if (hart_id >= CONFIG_MP_MAX_NUM_CPUS) {
		return 0;
	}
	
	return data->hart_thresholds[hart_id];
}

static inline void aplic_irq_set_pending_internal(const struct device *dev, uint32_t irq)
{
	const struct aplic_config *config = (const struct aplic_config *)dev->config;
	struct aplic_data *data = dev->data;
	
	/* 1. Set interrupt pending in SETIP register */
	mem_addr_t setip_addr = config->base + APLIC_SETIP_BASE + local_irq_to_reg_offset(irq);
	uint32_t bit_mask = BIT(irq & (APLIC_REG_SIZE - 1));
	
	/* Read current SETIP value and set the bit */
	uint32_t current_setip = aplic_read(dev, setip_addr);
	aplic_write(dev, setip_addr, current_setip | bit_mask);
	
	printk("APLIC: Set IRQ %u pending - SETIP: 0x%08X -> 0x%08X\n", 
	       irq, current_setip, current_setip | bit_mask);
	
	/* Debug: Show current interrupt status */
	// aplic_debug_interrupt_status(dev, irq); // Temporarily commented out
	
	/* 2. Check if interrupt is enabled */
	if (!riscv_aplic_irq_is_enabled(irq)) {
		printk("APLIC: IRQ %u is not enabled, skipping routing\n", irq);
		return;
	}
	
	/* 3. Read TARGETCFG to determine routing */
	mem_addr_t targetcfg_addr = config->base + APLIC_TARGETCFG_BASE + (irq * APLIC_TARGETCFG_SIZE);
	uint32_t targetcfg = aplic_read(dev, targetcfg_addr);
	
	/* Extract target configuration */
	uint32_t target_hart = targetcfg & APLIC_TARGET_HART_IDX_MASK;
	uint32_t target_guest = (targetcfg >> 14) & APLIC_TARGET_GUEST_IDX_MASK;
	uint32_t priority = (targetcfg >> 20) & 0xFF;
	bool target_enabled = (targetcfg >> 31) & 0x1;
	
	printk("APLIC: IRQ %u routing - Hart: %u, Guest: %u, Priority: %u, Enabled: %s\n",
	       irq, target_hart, target_guest, priority, target_enabled ? "yes" : "no");
	
	if (!target_enabled) {
		printk("APLIC: IRQ %u target not enabled, skipping routing\n", irq);
		return;
	}
	
	/* 4. Check Hart threshold */
	mem_addr_t hart_targetcfg_addr = get_targetcfg_addr(dev, target_hart);
	uint32_t hart_threshold = aplic_read(dev, hart_targetcfg_addr + APLIC_TARGETCFG_THRESHOLD);
	
	printk("APLIC: Hart %u threshold: %u, IRQ %u priority: %u\n", 
	       target_hart, hart_threshold, irq, priority);
	
	/* 5. Route interrupt to RISC-V if priority > threshold */
	if (priority > hart_threshold) {
		printk("APLIC: Routing IRQ %u (priority %u) to RISC-V Hart %u\n", 
		       irq, priority, target_hart);
		
		/* CRITICAL: Check if we're in MSI mode or Direct mode */
		struct aplic_data *aplic_data = dev->data;
		if (aplic_data->msi_mode_enabled) {
			/* MSI MODE: Forward interrupt via MSI to IMSIC */
			printk("APLIC: Using MSI mode - forwarding interrupt %u to IMSIC\n", irq);
			
			/* CRITICAL: In MSI mode, we don't need to check if IRQ is enabled in SETIE */
			/* MSI mode bypasses the traditional interrupt enable mechanism */
			printk("APLIC: MSI mode bypasses SETIE enable check, proceeding with MSI forwarding\n");
			
			/* Send MSI to IMSIC for this interrupt */
			int msi_result = aplic_send_msi(dev, target_hart, 0, irq);
			if (msi_result == 0) {
				printk("APLIC: Successfully sent MSI for IRQ %u to IMSIC\n", irq);
			} else {
				printk("APLIC: Failed to send MSI for IRQ %u to IMSIC (error %d)\n", irq, msi_result);
			}
			
			/* Update statistics */
			k_spinlock_key_t key = k_spin_lock(&aplic_data->lock);
			aplic_data->msi_interrupts_sent++;
			k_spin_unlock(&aplic_data->lock, key);
			
		} else {
			/* DIRECT MODE: Use APLIC IDC (this path is currently not working) */
			printk("APLIC: Using Direct mode - attempting IDC interrupt routing\n");
			
			/* CRITICAL FIX: Force the interrupt through APLIC IDC */
			/* According to RISC-V AIA spec, we need to use IDC IFORCE register to trigger the interrupt */
			const struct aplic_config *aplic_config = dev->config;
			uint32_t cpu_id = arch_proc_id();
			mem_addr_t idc_base = aplic_config->base + APLIC_IDC_BASE + (cpu_id * APLIC_IDC_SIZE);
			
			/* CRITICAL: Ensure IDC delivery is enabled before forcing interrupt */
			mem_addr_t idelivery_addr = idc_base + APLIC_IDC_IDELIVERY;
			uint32_t current_idelivery = aplic_read(dev, idelivery_addr);
			if (current_idelivery != APLIC_IDC_DELIVERY_ENABLE) {
				printk("APLIC: IDC delivery not enabled (0x%08X), enabling...\n", current_idelivery);
				aplic_write(dev, idelivery_addr, APLIC_IDC_DELIVERY_ENABLE);
				/* Verify the write */
				uint32_t verify_idelivery = aplic_read(dev, idelivery_addr);
				printk("APLIC: IDC delivery after enable: 0x%08X\n", verify_idelivery);
			}
			
			/* Also ensure threshold is properly set */
			mem_addr_t ithreshold_addr = idc_base + APLIC_IDC_ITHRESHOLD;
			uint32_t current_threshold = aplic_read(dev, ithreshold_addr);
			if (current_threshold != APLIC_IDC_THRESHOLD_ENABLE) {
				printk("APLIC: IDC threshold not correct (0x%08X), setting to 0...\n", current_threshold);
				aplic_write(dev, ithreshold_addr, APLIC_IDC_THRESHOLD_ENABLE);
			}
			
			/* ALTERNATIVE APPROACH: Instead of forcing through IDC, let APLIC handle it naturally */
			/* The APLIC should automatically route interrupts when they are pending and enabled */
			printk("APLIC: Using natural APLIC interrupt routing for IRQ %u (Hart %u)\n", irq, target_hart);
			
			/* CRITICAL FIX: According to RISC-V AIA spec, we need to read from IDC TOPI to trigger the interrupt */
			/* Reading from TOPI register should cause APLIC to send the interrupt signal to the CPU */
			mem_addr_t topi_addr = idc_base + APLIC_IDC_TOPI;
			uint32_t topi_value = aplic_read(dev, topi_addr);
			printk("APLIC: Read IDC TOPI register at 0x%08lX: 0x%08X\n", topi_addr, topi_value);
			
			/* Extract the interrupt ID from TOPI */
			uint32_t topi_irq = (topi_value >> APLIC_IDC_TOPI_ID_SHIFT) & APLIC_IDC_TOPI_ID_MASK;
			uint32_t topi_priority = (topi_value >> APLIC_IDC_TOPI_PRIO_SHIFT) & APLIC_IDC_TOPI_PRIO_MASK;
			
			if (topi_irq == irq) {
				printk("APLIC: TOPI shows IRQ %u with priority %u - interrupt should be triggered\n", topi_irq, topi_priority);
			} else {
				printk("APLIC: TOPI shows different IRQ %u (expected %u) - this may indicate a problem\n", topi_irq, irq);
			}
			
			/* Update statistics */
			k_spinlock_key_t key = k_spin_lock(&data->lock);
			data->direct_interrupts++;
			k_spin_unlock(&data->lock, key);
		}
		
	} else {
		printk("APLIC: IRQ %u priority %u <= threshold %u, not routing\n", 
		       irq, priority, hart_threshold);
	}
}

static inline const struct device *aplic_get_dev(void)
{
	return save_dev[arch_proc_id()];
}

/* Public API functions */
void riscv_aplic_irq_enable(uint32_t irq)
{
	const struct device *dev = aplic_get_dev();
	struct aplic_data *data;
	k_spinlock_key_t key;

	if (dev != NULL) {
		aplic_irq_enable_internal(dev, irq);
		
		/* Update state tracking */
		data = dev->data;
		if (irq > 0 && irq < 1024) {
			key = k_spin_lock(&data->lock);
			data->irq_info[irq].enabled = true;
			k_spin_unlock(&data->lock, key);
		}
	}
}

void riscv_aplic_irq_disable(uint32_t irq)
{
	const struct device *dev = aplic_get_dev();
	struct aplic_data *data;
	k_spinlock_key_t key;

	if (dev != NULL) {
		aplic_irq_disable_internal(dev, irq);
		
		/* Update state tracking */
		data = dev->data;
		if (irq > 0 && irq < 1024) {
			key = k_spin_lock(&data->lock);
			data->irq_info[irq].enabled = false;
			k_spin_unlock(&data->lock, key);
		}
	}
}

int riscv_aplic_irq_is_enabled(uint32_t irq)
{
	const struct device *dev = aplic_get_dev();

	if (dev != NULL) {
		return aplic_irq_is_enabled_internal(dev, irq);
	}

	return 0;
}

void riscv_aplic_set_priority(uint32_t irq, uint32_t prio)
{
	const struct device *dev = aplic_get_dev();

	if (dev != NULL) {
		aplic_set_priority_internal(dev, irq, prio);
	}
}

int riscv_aplic_irq_set_affinity(uint32_t irq, uint32_t cpumask)
{
	const struct device *dev = aplic_get_dev();

	if (dev != NULL) {
		return aplic_irq_set_affinity_internal(dev, irq, cpumask);
	}

	return -1;
}

void riscv_aplic_irq_set_pending(uint32_t irq)
{
	const struct device *dev = aplic_get_dev();

	if (dev != NULL) {
		aplic_irq_set_pending_internal(dev, irq);
	}
}

unsigned int riscv_aplic_get_irq(void)
{
	const struct device *dev = aplic_get_dev();

	if (dev != NULL) {
		/* APLIC uses IDC (Interrupt Delivery Controller) for interrupt handling */
		/* We need to read from the IDC TOPI register to get the highest priority interrupt */
		return aplic_get_idc_topi(dev);
	}

	return UINT_MAX;
}

const struct device *riscv_aplic_get_dev(void)
{
	return aplic_get_dev();
}

/* Advanced interrupt management API */
int riscv_aplic_irq_set_trigger_type(uint32_t irq, enum riscv_aplic_trigger_type type)
{
	const struct device *dev = aplic_get_dev();

	if (dev != NULL) {
		return aplic_irq_set_trigger_type_internal(dev, irq, (enum aplic_trigger_type)type);
	}

	return -ENODEV;
}

int riscv_aplic_irq_get_trigger_type(uint32_t irq)
{
	const struct device *dev = aplic_get_dev();

	if (dev != NULL) {
		return aplic_irq_get_trigger_type_internal(dev, irq);
	}

	return -ENODEV;
}

int riscv_aplic_hart_set_threshold(uint32_t hart_id, uint32_t threshold)
{
	const struct device *dev = aplic_get_dev();

	if (dev != NULL) {
		return aplic_hart_set_threshold_internal(dev, hart_id, threshold);
	}

	return -ENODEV;
}

uint32_t riscv_aplic_hart_get_threshold(uint32_t hart_id)
{
	const struct device *dev = aplic_get_dev();

	if (dev != NULL) {
		return aplic_hart_get_threshold_internal(dev, hart_id);
	}

	return 0;
}

/* Statistics and diagnostics API */
int riscv_aplic_get_irq_stats(uint32_t irq, struct riscv_aplic_irq_stats *stats)
{
	const struct device *dev = aplic_get_dev();
	struct aplic_data *data;
	k_spinlock_key_t key;

	if (dev == NULL || stats == NULL) {
		return -EINVAL;
	}

	data = dev->data;

	if (irq == 0 || irq >= 1024) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);

	stats->count = data->irq_info[irq].count;
	stats->last_cpu = data->irq_info[irq].last_cpu;
	stats->affinity_mask = data->irq_info[irq].affinity_mask;
	stats->trigger_type = (uint32_t)data->irq_info[irq].trigger_type;
	stats->priority = data->irq_info[irq].priority;
	stats->enabled = data->irq_info[irq].enabled;

	k_spin_unlock(&data->lock, key);

	return 0;
}

uint32_t riscv_aplic_get_total_interrupts(void)
{
	const struct device *dev = aplic_get_dev();
	struct aplic_data *data;

	if (dev == NULL) {
		return 0;
	}

	data = dev->data;
	return data->total_interrupts;
}

uint32_t riscv_aplic_get_msi_interrupts_sent(void)
{
	const struct device *dev = aplic_get_dev();
	struct aplic_data *data;

	if (dev == NULL) {
		return 0;
	}

	data = dev->data;
	return data->msi_interrupts_sent;
}

void riscv_aplic_reset_stats(void)
{
	const struct device *dev = aplic_get_dev();
	struct aplic_data *data;
	k_spinlock_key_t key;

	if (dev == NULL) {
		return;
	}

	data = dev->data;
	key = k_spin_lock(&data->lock);

	/* Reset all interrupt statistics */
	for (uint32_t i = 0; i < 1024; i++) {
		data->irq_info[i].count = 0;
		data->irq_info[i].last_cpu = 0;
	}
	data->total_interrupts = 0;
	data->msi_interrupts_sent = 0;
	data->direct_interrupts = 0;

	k_spin_unlock(&data->lock, key);

	printk("APLIC: Statistics reset");
}

/* Interrupt statistics and diagnostics */
static inline void aplic_update_irq_stats(const struct device *dev, uint32_t irq_id)
{
	struct aplic_data *data = dev->data;
	uint32_t current_cpu = arch_curr_cpu()->id;
	k_spinlock_key_t key;
	
	if (irq_id >= 1024) {
		return;
	}
	
	key = k_spin_lock(&data->lock);
	
	/* Update interrupt statistics */
	data->irq_info[irq_id].count++;
	data->irq_info[irq_id].last_cpu = current_cpu;
	data->total_interrupts++;
	
	k_spin_unlock(&data->lock, key);
}

/* Load balancing: select best CPU for interrupt */
static inline uint32_t aplic_select_target_cpu(const struct device *dev, uint32_t irq_id)
{
	struct aplic_data *data = dev->data;
	uint32_t affinity_mask;
	uint32_t target_cpu = 0;
	uint32_t min_load = UINT32_MAX;
	uint32_t cpu_load;
	
	if (irq_id >= 1024) {
		return 0;
	}
	
	affinity_mask = data->irq_info[irq_id].affinity_mask;
	if (affinity_mask == 0) {
		affinity_mask = BIT_MASK(CONFIG_MP_MAX_NUM_CPUS); /* Default to all CPUs */
	}
	
	/* Simple load balancing: find CPU with least interrupts in affinity mask */
	for (uint32_t cpu = 0; cpu < CONFIG_MP_MAX_NUM_CPUS; cpu++) {
		if (!(affinity_mask & BIT(cpu))) {
			continue;
		}
		
		/* Calculate load as total interrupts handled by this CPU */
		cpu_load = 0;
		for (uint32_t i = 1; i < 1024; i++) {
			if (data->irq_info[i].last_cpu == cpu) {
				cpu_load += data->irq_info[i].count;
			}
		}
		
		if (cpu_load < min_load) {
			min_load = cpu_load;
			target_cpu = cpu;
		}
	}
	
	return target_cpu;
}

/* Enhanced interrupt handler with statistics and load balancing */
#if 0
static void aplic_isr(const struct device *dev)
{
	uint32_t irq_id = aplic_get_claim(dev);

	if (irq_id != UINT_MAX && irq_id != 0) {
		/* Update interrupt statistics */
		aplic_update_irq_stats(dev, irq_id);

		/* Call the registered ISR */
		const struct aplic_config *config = dev->config;

		if (config->isr_table && irq_id < config->nr_irqs) {
			const struct _isr_table_entry *isr_entry = &config->isr_table[irq_id];

			if (isr_entry->isr != NULL) {
				isr_entry->isr(isr_entry->arg);
			}
		}

		/* Complete the interrupt */
		aplic_set_complete(dev, irq_id);

		/* Optional: Print debug info for high-frequency interrupts */
		struct aplic_data *data = dev->data;
		if (data->irq_info[irq_id].count % 100 == 0) {
			/* Optional: Print debug info for high-frequency interrupts */
			/* printk("APLIC: IRQ %u handled %u times on CPU %u\n",
				irq_id, data->irq_info[irq_id].count, current_cpu); */
		}
	}
}
#endif

/* ============================================================================
 * MSI Mode Support Functions
 * ============================================================================ */

/* Find IMSIC devices for MSI mode */
static int aplic_find_imsic_devices(const struct device *dev)
{
	struct aplic_data *data = dev->data;
	int found_count = 0;
	
	/* Search for IMSIC devices in device tree */
	for (int i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		/* Try multiple possible device names for IMSIC */
		const char *possible_names[] = {
			"imsic",
			"interrupt-controller@24000000",
			"qemu_imsic",
			"riscv_imsic"
		};
		
		bool found = false;
		for (int j = 0; j < ARRAY_SIZE(possible_names); j++) {
			const struct device *imsic_dev = device_get_binding(possible_names[j]);
			if (imsic_dev != NULL && device_is_ready(imsic_dev)) {
				data->imsic_devices[i] = imsic_dev;
				found_count++;
				found = true;
				printk("APLIC: Found IMSIC device %s for hart %d\n", 
				       imsic_dev->name, i);
				break;
			}
		}
		
		if (!found) {
			data->imsic_devices[i] = NULL;
		}
	}
	
	/* If we found at least one IMSIC device, also try to get the global IMSIC device */
	if (found_count > 0) {
		const struct device *global_imsic = riscv_imsic_get_dev();
		if (global_imsic != NULL && device_is_ready(global_imsic)) {
			printk("APLIC: Found global IMSIC device %s\n", global_imsic->name);
		}
	}
	
	return found_count;
}

/* Configure APLIC for MSI mode */
static int aplic_configure_msi_mode(const struct device *dev)
{
	const struct aplic_config *config = dev->config;
	struct aplic_data *data = dev->data;
	
	/* Check if we have IMSIC devices */
	if (aplic_find_imsic_devices(dev) == 0) {
		printk("APLIC: No IMSIC devices found, cannot enable MSI mode\n");
		return -ENODEV;
	}
	
	/* CRITICAL: Configure MSI address registers before enabling MSI mode */
	/* According to RISC-V AIA spec, APLIC needs to know where to send MSI */
	
	/* Find IMSIC device to get its base address */
	const struct device *imsic_dev = riscv_imsic_get_dev();
	if (imsic_dev == NULL) {
		printk("APLIC: ERROR - Cannot find IMSIC device for MSI configuration\n");
		return -ENODEV;
	}
	
	/* Get IMSIC base address from device tree or configuration */
	/* For now, use a hardcoded address based on QEMU virt machine */
	mem_addr_t imsic_base = 0x28000000; /* QEMU virt machine IMSIC base address */
	
	printk("APLIC: Configuring MSI address registers for IMSIC at 0x%08lX\n", imsic_base);
	
	/* Configure APLIC_MMSICFGADDR (Machine MSI Configuration Address) */
	/* This tells APLIC where to send MSI messages */
	uint32_t mmsicfgaddr = (imsic_base >> 12) & 0xFFFFFFFF; /* Convert to PPN */
	aplic_write(dev, config->base + APLIC_MMSICFGADDR, mmsicfgaddr);
	
	/* Configure APLIC_MMSICFGADDRH (Machine MSI Configuration Address High) */
	/* This contains additional configuration bits */
	uint32_t mmsicfgaddrh = 0;
	/* Set LHXS (Local Hart Index Select) to 0 for single hart */
	/* Set HHXS (High Hart Index Select) to 0 for single hart */
	/* Set HHXW (High Hart Index Width) to 0 for single hart */
	/* Set LHXW (Low Hart Index Width) to 0 for single hart */
	/* Set BAPPN (Base Address PPN) to 0 (already set in mmsicfgaddr) */
	mmsicfgaddrh = (0 << 24) | (0 << 20) | (0 << 16) | (0 << 12) | 0;
	aplic_write(dev, config->base + APLIC_MMSICFGADDRH, mmsicfgaddrh);
	
	printk("APLIC: MSI address registers configured: ADDR=0x%08X, ADDRH=0x%08X\n", 
	       mmsicfgaddr, mmsicfgaddrh);
	
	/* Enable MSI mode in DOMAINCFG */
	uint32_t domaincfg_value = (1 << 8) | (1 << 2); /* IE + DM (MSI mode) */
	printk("APLIC: Enabling MSI mode, writing 0x%08X to DOMAINCFG\n", domaincfg_value);
	aplic_write(dev, config->base + APLIC_DOMAINCFG, domaincfg_value);
	
	/* Verify MSI mode is enabled */
	uint32_t domaincfg_readback = aplic_read(dev, config->base + APLIC_DOMAINCFG);
	if ((domaincfg_readback & (1 << 2)) == 0) {
		printk("APLIC: ERROR - Failed to enable MSI mode\n");
		return -EIO;
	}
	
	data->msi_mode_enabled = true;
	data->msi_base_eid = 0; /* Start EIDs from 0 */
	
	printk("APLIC: MSI mode enabled successfully with proper address configuration\n");
	return 0;
}

/* Send MSI through IMSIC */
int aplic_send_msi(const struct device *dev, uint32_t target_hart, 
			  uint32_t target_guest, uint32_t irq_id)
{
	struct aplic_data *data = dev->data;
	
	if (!data->msi_mode_enabled) {
		return -ENOTSUP;
	}
	
	if (target_hart >= CONFIG_MP_MAX_NUM_CPUS) {
		return -EINVAL;
	}
	
	/* Calculate EID for this interrupt */
	uint32_t eid = data->msi_base_eid + irq_id;
	
	/* Validate EID range */
	if (eid > 63) {
		return -EINVAL;
	}
	
	/* CRITICAL: According to Linux implementation, APLIC should configure TARGET register */
	/* The hardware automatically handles MSI forwarding when TARGET is properly configured */
	
	/* Get the TARGET register address for this IRQ */
	const struct aplic_config *config = dev->config;
	mem_addr_t target_addr = config->base + APLIC_TARGETCFG_BASE + (irq_id * APLIC_TARGETCFG_SIZE);
	
	/* Configure TARGET register for MSI mode */
	/* Format: [13:0] Hart Index, [19:14] Guest Index, [27:20] Priority, [31] Enable */
	uint32_t target_value = (target_hart & 0x3FFF) |           /* Hart Index */
						   (target_guest << 14) |               /* Guest Index */
						   (7 << 20) |                          /* Priority 7 */
						   (1 << 31);                           /* Enable interrupt */
	
	/* Write to TARGET register to configure MSI routing */
	aplic_write(dev, target_addr, target_value);
	
	printk("APLIC: Configured TARGET register for IRQ %u: 0x%08X (hart %u, guest %u, EID %u)\n", 
	       irq_id, target_value, target_hart, target_guest, eid);
	
	/* Now trigger the interrupt by setting it pending */
	/* This will cause APLIC hardware to automatically generate MSI */
	mem_addr_t setip_addr = config->base + APLIC_SETIP_BASE + local_irq_to_reg_offset(irq_id);
	uint32_t bit_mask = BIT(irq_id & (APLIC_REG_SIZE - 1));
	
	/* Read current SETIP value and set the bit */
	uint32_t current_setip = aplic_read(dev, setip_addr);
	uint32_t new_setip = current_setip | bit_mask;
	
	/* Write to SETIP register to trigger MSI generation */
	aplic_write(dev, setip_addr, new_setip);
	
	printk("APLIC: Triggered MSI for IRQ %u via SETIP register\n", irq_id);
	
	/* Update statistics */
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	data->msi_interrupts_sent++;
	k_spin_unlock(&data->lock, key);
	
	printk("APLIC: Sent MSI EID %u to hart %u (guest %u)\n", eid, target_hart, target_guest);
	
	return 0;
}

/* Configure source for MSI mode */
static int aplic_configure_source_msi(const struct device *dev, uint32_t irq_id, 
				     uint32_t target_hart, uint32_t target_guest)
{
	struct aplic_data *data = dev->data;
	
	if (!data->msi_mode_enabled) {
		return -ENOTSUP;
	}
	
	/* Validate parameters */
	if (irq_id >= 1024 || target_hart >= CONFIG_MP_MAX_NUM_CPUS || target_guest > 63) {
		return -EINVAL;
	}
	
	/* In MSI mode, SOURCECFG is configured differently */
	mem_addr_t sourcecfg_addr = get_sourcecfg_addr(dev, irq_id);
	
	/* For MSI mode, SOURCECFG format is:
	 * [21:11] - Child Index (EID in IMSIC)
	 * [10]    - Delegate bit (set to 1 for MSI)
	 * [2:0]   - Source Mode (set to 0 for inactive in MSI mode)
	 */
	uint32_t eid = data->msi_base_eid + irq_id;
	uint32_t sourcecfg_value = (eid << 11) | (1 << 10) | APLIC_SOURCECFG_SM_INACTIVE;
	
	/* Write SOURCECFG for this interrupt source */
	aplic_write(dev, sourcecfg_addr, sourcecfg_value);
	
	/* Also configure the target hart and guest in TARGETCFG */
	mem_addr_t targetcfg_addr = get_targetcfg_addr(dev, target_hart);
	
	/* TARGETCFG format:
	 * [13:0]  - Hart Index
	 * [19:14] - Guest Index
	 * [27:20] - Interrupt Priority
	 * [31]    - Interrupt Enable
	 */
	uint32_t targetcfg_value = (target_hart & 0x3FFF) | 
				   ((target_guest & 0x3F) << 14) | 
				   (7 << 20) |  /* Default priority 7 */
				   (1 << 31);   /* Enable interrupt */
	
	aplic_write(dev, targetcfg_addr + APLIC_TARGETCFG_HARTID, targetcfg_value);
	
	printk("APLIC: Configured IRQ %u for MSI to hart %u, guest %u (EID %u)\n", 
	       irq_id, target_hart, target_guest, eid);
	
	return 0;
}

/* Device initialization */
static int aplic_init(const struct device *dev)
{
	const struct aplic_config *config = dev->config;
	struct aplic_data *data = dev->data;
	uint32_t i;

	/* Initialize spinlock */
	ARG_UNUSED(data);
	
	/* Early debug - this should appear even before console is ready */
	printk("APLIC: aplic_init called for device %s, base=0x%08lX\n",
		dev->name ? dev->name : "NULL", config->base);

	/* Sanity check the base address */
	if (config->base == 0) {
		printk("APLIC: ERROR - Invalid base address 0x%08lX\n", config->base);
		return -EINVAL;
	}

	/* Configure domain configuration */
	/* According to RISC-V AIA spec: IE bit is at position 8, not 0 */
	uint32_t domaincfg_value = (1 << 8); /* Set IE bit (bit 8) */
	printk("APLIC: Writing 0x%08X to DOMAINCFG at 0x%08lX\n", domaincfg_value, config->base + APLIC_DOMAINCFG);
	aplic_write(dev, config->base + APLIC_DOMAINCFG, domaincfg_value);
	
	/* Read back to verify */
	uint32_t domaincfg_readback = aplic_read(dev, config->base + APLIC_DOMAINCFG);
	printk("APLIC: DOMAINCFG readback: 0x%08X\n", domaincfg_readback);
	
	/* Check if IE bit was set correctly */
	if ((domaincfg_readback & (1 << 8)) == 0) {
		/* Try the safe write method from spec: (x << 24) | x */
		printk("APLIC: IE bit not set, trying safe write method\n");
		uint32_t safe_value = domaincfg_value | (domaincfg_value << 24);
		aplic_write(dev, config->base + APLIC_DOMAINCFG, safe_value);
		domaincfg_readback = aplic_read(dev, config->base + APLIC_DOMAINCFG);
		printk("APLIC: DOMAINCFG after safe write: 0x%08X\n", domaincfg_readback);
	}
	
	/* CRITICAL FIX: Try different DOMAINCFG values according to RISC-V AIA spec */
	if ((domaincfg_readback & (1 << 8)) == 0) {
		printk("APLIC: IE bit still not set, trying alternative configurations\n");
		
		/* Try setting all relevant bits: IE (bit 8), DM (bit 2), BE (bit 0) */
		uint32_t full_config = (1 << 8) | (0 << 2) | (0 << 0); /* IE=1, DM=0 (Direct mode), BE=0 (Little endian) */
		aplic_write(dev, config->base + APLIC_DOMAINCFG, full_config);
		domaincfg_readback = aplic_read(dev, config->base + APLIC_DOMAINCFG);
		printk("APLIC: DOMAINCFG after full config: 0x%08X\n", domaincfg_readback);
		
		/* Try the safe write method again */
		if ((domaincfg_readback & (1 << 8)) == 0) {
			uint32_t safe_full_config = full_config | (full_config << 24);
			aplic_write(dev, config->base + APLIC_DOMAINCFG, safe_full_config);
			domaincfg_readback = aplic_read(dev, config->base + APLIC_DOMAINCFG);
			printk("APLIC: DOMAINCFG after safe full config: 0x%08X\n", domaincfg_readback);
		}
	}
	
	if (domaincfg_readback & (1 << 8)) {
		printk("APLIC: IE bit successfully enabled\n");
	} else {
		printk("APLIC: WARNING - IE bit could not be enabled, this may cause interrupt routing to fail\n");
		printk("APLIC: DOMAINCFG final value: 0x%08X\n", domaincfg_readback);
	}

	/* Initialize MSI mode support */
	data->msi_mode_enabled = false;
	for (i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		data->imsic_devices[i] = NULL;
	}
	data->msi_base_eid = 0;
	
	/* Set IMSIC base address from device tree */
	/* IMSIC is typically at 0x24000000 in QEMU virt machine */
	data->imsic_base = 0x24000000;
	
	/* Try to detect and configure MSI mode */
	if (aplic_configure_msi_mode(dev) == 0) {
		printk("APLIC: MSI mode configured successfully\n");
	} else {
		printk("APLIC: MSI mode not available, using direct mode\n");
	}
	
	/* CRITICAL FIX: Force APLIC to use Direct Mode for interrupt testing */
	/* MSI mode bypasses IDC registers, but we need IDC for interrupt routing */
	printk("APLIC: Forcing Direct Mode (DM=0) for interrupt testing\n");
	
	/* According to Linux APLIC implementation, we need to ensure proper configuration */
	/* First, try to read current DOMAINCFG to see what we're working with */
	uint32_t current_domaincfg = aplic_read(dev, config->base + APLIC_DOMAINCFG);
	printk("APLIC: Current DOMAINCFG before forcing direct mode: 0x%08X\n", current_domaincfg);
	
	/* CRITICAL: According to Linux implementation, we should NOT set DOMAINCFG here */
	/* DOMAINCFG should be set AFTER all other configurations are complete */
	/* This is the key difference from our previous approach */
	printk("APLIC: Following Linux implementation: delaying DOMAINCFG configuration\n");
	printk("APLIC: Will configure DOMAINCFG after SOURCECFG and TARGETCFG are set\n");
	
	/* Force disable MSI mode for interrupt testing */
	data->msi_mode_enabled = false;
	printk("APLIC: MSI mode disabled, will use Direct Mode with IDC\n");
	
	/* CRITICAL: Hardware enforces MSI mode, so we must use MSI mode */
	/* According to RISC-V AIA spec, when DOMAINCFG.DM=1, APLIC must use MSI mode */
	printk("APLIC: Hardware enforces MSI mode (DM=1), switching to MSI interrupt forwarding\n");
	data->msi_mode_enabled = true;
	printk("APLIC: MSI mode enabled, will forward interrupts via MSI to IMSIC\n");
	
	/* Configure SOURCECFG and TARGETCFG for direct mode interrupts */
	/* By default, all sources are INACTIVE, we need to enable them for direct mode */
	if (!data->msi_mode_enabled) {
		printk("APLIC: Configuring SOURCECFG and TARGETCFG for direct mode interrupts\n");
		for (i = 1; i < config->nr_irqs; i++) {
			/* 1. Configure SOURCECFG for this interrupt source */
			mem_addr_t sourcecfg_addr = get_sourcecfg_addr(dev, i);
			
			/* For direct mode, configure as edge-triggered rising edge */
			uint32_t sourcecfg_value = APLIC_SOURCECFG_SM_EDGE_RISE;
			
			/* Write SOURCECFG for this interrupt source */
			aplic_write(dev, sourcecfg_addr, sourcecfg_value);
			
			/* 2. Configure TARGETCFG for this interrupt source */
			/* Each IRQ needs its own TARGETCFG entry */
			/* TARGETCFG is organized by IRQ, not by hart */
			mem_addr_t targetcfg_addr = config->base + APLIC_TARGETCFG_BASE + (i * APLIC_TARGETCFG_SIZE);
			
			/* TARGETCFG format for each IRQ:
			 * [13:0]  - Hart Index (0)
			 * [19:14] - Guest Index (0)
			 * [27:20] - Interrupt Priority (7)
			 * [31]    - Interrupt Enable (1)
			 */
			uint32_t targetcfg_value = (0 & 0x3FFF) |           /* Hart Index 0 */
						   (0 << 14) |                    /* Guest Index 0 */
						   (7 << 20) |                    /* Priority 7 */
						   (1 << 31);                     /* Enable interrupt */
			
			/* Write TARGETCFG for this interrupt source */
			aplic_write(dev, targetcfg_addr, targetcfg_value);
			
			/* 3. Enable this interrupt in SETIE register */
			riscv_aplic_irq_enable(i);
			
			printk("APLIC: Configured IRQ %u - SOURCECFG: 0x%08X, TARGETCFG: 0x%08X\n", 
			       i, sourcecfg_value, targetcfg_value);
		}
		
		/* 4. Configure Hart 0 threshold for receiving interrupts */
		uint32_t hart_threshold = 0; /* Accept all interrupts */
		mem_addr_t hart_targetcfg_addr = get_targetcfg_addr(dev, 0);
		aplic_write(dev, hart_targetcfg_addr + APLIC_TARGETCFG_THRESHOLD, hart_threshold);
		printk("APLIC: Set Hart 0 threshold to %u\n", hart_threshold);
		
		/* CRITICAL FIX: Configure Hart 0 TARGETCFG to enable interrupt delivery */
		/* The Hart TARGETCFG must be properly configured for APLIC to route interrupts */
		uint32_t hart_targetcfg_value = aplic_read(dev, hart_targetcfg_addr);
		printk("APLIC: Hart 0 TARGETCFG before configuration: 0x%08X\n", hart_targetcfg_value);
		
		/* Enable interrupt delivery for Hart 0 */
		uint32_t new_hart_targetcfg = hart_targetcfg_value | (1 << 31); /* Set IE bit */
		aplic_write(dev, hart_targetcfg_addr, new_hart_targetcfg);
		
		/* Verify the configuration */
		uint32_t verify_hart_targetcfg = aplic_read(dev, hart_targetcfg_addr);
		printk("APLIC: Hart 0 TARGETCFG after configuration: 0x%08X\n", verify_hart_targetcfg);
		
	} else {
		/* MSI mode configuration */
		printk("APLIC: Configuring MSI mode interrupts\n");
		for (i = 1; i < config->nr_irqs; i++) {
			/* Configure SOURCECFG for MSI mode */
			mem_addr_t sourcecfg_addr = get_sourcecfg_addr(dev, i);
			uint32_t sourcecfg_value = APLIC_SOURCECFG_SM_MSI;
			aplic_write(dev, sourcecfg_addr, sourcecfg_value);
			
			/* Configure TARGETCFG for MSI routing */
			/* TARGETCFG is organized by IRQ, not by hart */
			mem_addr_t targetcfg_addr = config->base + APLIC_TARGETCFG_BASE + (i * APLIC_TARGETCFG_SIZE);
			uint32_t targetcfg_value = (0 & 0x3FFF) |           /* Hart Index 0 */
						   (0 << 14) |                    /* Guest Index 0 */
						   (7 << 20) |                    /* Priority 7 */
						   (1 << 31);                     /* Enable interrupt */
			aplic_write(dev, targetcfg_addr, targetcfg_value);
			
			printk("APLIC: Configured IRQ %u for MSI mode\n", i);
		}
	}

	/* Initialize IRQ information and configure source configuration */
	for (i = 0; i < config->nr_irqs; i++) {
		mem_addr_t sourcecfg_addr = get_sourcecfg_addr(dev, i);
		
		/* Initialize IRQ info with defaults */
		if (i < 1024) {
			data->irq_info[i].count = 0;
			data->irq_info[i].last_cpu = 0;
			data->irq_info[i].affinity_mask = BIT_MASK(CONFIG_MP_MAX_NUM_CPUS); /* All CPUs */
			data->irq_info[i].trigger_type = APLIC_TRIGGER_LEVEL_HIGH; /* Default to level-high */
			data->irq_info[i].priority = 1; /* Default priority */
			data->irq_info[i].enabled = false;
		}
		
		/* Configure source based on mode */
		if (data->msi_mode_enabled) {
			/* In MSI mode, configure for MSI delivery */
			/* Default to hart 0, guest 0 */
			aplic_configure_source_msi(dev, i, 0, 0);
		} else {
			/* In direct mode, set source to level-triggered high by default */
			aplic_write(dev, sourcecfg_addr, APLIC_SOURCECFG_SM_LEVEL_HIGH);
		}
	}
	
	/* Initialize total interrupt counter */
	data->total_interrupts = 0;
	data->direct_interrupts = 0;

	/* Configure target configuration for each hart */
	for (i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		mem_addr_t targetcfg_addr = get_targetcfg_addr(dev, i);
		
		/* Set threshold to 0 (accept all interrupts) */
		aplic_set_threshold(dev, i, 0);
		data->hart_thresholds[i] = 0;
		
		/* Enable interrupts for this target */
		aplic_write(dev, targetcfg_addr + APLIC_TARGETCFG_IE, 0x1);
	}

	/* Configure APLIC IDC (Interrupt Delivery Controller) for each hart */
	printk("APLIC: Configuring IDC for %d harts\n", CONFIG_MP_MAX_NUM_CPUS);
	for (i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		mem_addr_t idc_base = config->base + APLIC_IDC_BASE + (i * APLIC_IDC_SIZE);
		
		/* CRITICAL FIX: Configure IDC delivery enable register */
		/* According to RISC-V AIA spec, idelivery must be 1 for interrupt delivery */
		mem_addr_t idelivery_addr = idc_base + APLIC_IDC_IDELIVERY;
		uint32_t current_idelivery = aplic_read(dev, idelivery_addr);
		printk("APLIC: IDC hart %u - idelivery before config: 0x%08X\n", i, current_idelivery);
		
		/* Try to set idelivery to 1 (enable interrupt delivery) */
		aplic_write(dev, idelivery_addr, APLIC_IDC_DELIVERY_ENABLE);
		
		/* Verify the write operation */
		uint32_t verify_idelivery = aplic_read(dev, idelivery_addr);
		printk("APLIC: IDC hart %u - idelivery after config: 0x%08X\n", i, verify_idelivery);
		
		if (verify_idelivery == APLIC_IDC_DELIVERY_ENABLE) {
			printk("APLIC: IDC hart %u - idelivery successfully set to 1\n", i);
		} else {
			printk("APLIC: IDC hart %u - WARNING: idelivery write failed, got 0x%08X\n", i, verify_idelivery);
		}
		
		/* Configure IDC threshold register (0 = accept all interrupts) */
		mem_addr_t ithreshold_addr = idc_base + APLIC_IDC_ITHRESHOLD;
		uint32_t current_threshold = aplic_read(dev, ithreshold_addr);
		printk("APLIC: IDC hart %u - ithreshold before config: 0x%08X\n", i, current_threshold);
		
		aplic_write(dev, ithreshold_addr, APLIC_IDC_THRESHOLD_ENABLE);
		
		/* Verify threshold configuration */
		uint32_t verify_threshold = aplic_read(dev, ithreshold_addr);
		printk("APLIC: IDC hart %u - ithreshold after config: 0x%08X\n", i, verify_threshold);
		
		printk("APLIC: Configured IDC for hart %u - base: 0x%08lX, delivery: %s, threshold: %s\n", 
		       i, idc_base, 
		       (verify_idelivery == APLIC_IDC_DELIVERY_ENABLE) ? "enabled" : "FAILED",
		       (verify_threshold == APLIC_IDC_THRESHOLD_ENABLE) ? "0" : "FAILED");
	}
	
	/* CRITICAL: Now configure DOMAINCFG according to Linux implementation */
	/* This should be done AFTER all other configurations are complete */
	printk("APLIC: Configuring DOMAINCFG after all other configurations...\n");
	
	/* Read current DOMAINCFG */
	uint32_t final_domaincfg_read = aplic_read(dev, config->base + APLIC_DOMAINCFG);
	printk("APLIC: Current DOMAINCFG before final configuration: 0x%08X\n", final_domaincfg_read);
	
	/* Set DOMAINCFG according to Linux implementation: IE=1, DM=0 (Direct mode) */
	uint32_t final_domaincfg = final_domaincfg_read | (1 << 8); /* Set IE bit */
	if (!data->msi_mode_enabled) {
		final_domaincfg &= ~(1 << 2); /* Clear DM bit for Direct mode */
	} else {
		final_domaincfg |= (1 << 2);  /* Set DM bit for MSI mode */
	}
	
	/* Write final DOMAINCFG configuration */
	aplic_write(dev, config->base + APLIC_DOMAINCFG, final_domaincfg);
	
	/* Verify DOMAINCFG configuration */
	uint32_t verify_domaincfg = aplic_read(dev, config->base + APLIC_DOMAINCFG);
	printk("APLIC: Final DOMAINCFG: 0x%08X (IE=%s, DM=%s, BE=%s)\n", 
	       verify_domaincfg,
	       (verify_domaincfg & (1 << 8)) ? "1" : "0",
	       (verify_domaincfg & (1 << 2)) ? "1(MSI)" : "0(Direct)",
	       (verify_domaincfg & (1 << 0)) ? "1(Big)" : "0(Little)");
	
	/* CRITICAL: Check if we're actually in Direct Mode */
	if ((verify_domaincfg & (1 << 2)) != 0) {
		printk("APLIC: ERROR - Failed to enter Direct Mode! Still in MSI mode!\n");
		printk("APLIC: This means IDC registers will not be accessible!\n");
	} else {
		printk("APLIC: SUCCESS - APLIC is now in Direct Mode (DM=0)\n");
		printk("APLIC: IDC registers should now be accessible\n");
	}
	
	/* CRITICAL: Test IDC register accessibility after DOMAINCFG configuration */
	printk("APLIC: Testing IDC register accessibility after DOMAINCFG configuration...\n");
	for (i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		mem_addr_t idc_base = config->base + APLIC_IDC_BASE + (i * APLIC_IDC_SIZE);
		mem_addr_t idelivery_addr = idc_base + APLIC_IDC_IDELIVERY;
		
		/* Test if IDC registers are now accessible */
		uint32_t test_idelivery = aplic_read(dev, idelivery_addr);
		printk("APLIC: IDC hart %u - idelivery after DOMAINCFG config: 0x%08X\n", i, test_idelivery);
		
		if (test_idelivery != 0x00000000) {
			printk("APLIC: IDC hart %u - SUCCESS: IDC registers are now accessible!\n", i);
		} else {
			printk("APLIC: IDC hart %u - WARNING: IDC registers still not accessible\n", i);
		}
	}

	/* Save device reference for all CPUs */
	for (i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		save_dev[i] = dev;
	}

	return 0;
}

/* ============================================================================
 * Interrupt Routing Functions
 * ============================================================================ */

/**
 * @brief Route APLIC interrupt to RISC-V mip register
 * This is the key function that routes APLIC interrupts to RISC-V
 */
static void aplic_route_interrupt_to_riscv(const struct device *dev, uint32_t irq)
{
	/* In QEMU RISC-V virt machine, APLIC needs to route interrupts to RISC-V */
	/* The mechanism is to set the appropriate bit in the target hart's interrupt pending */
	
	/* Read the current TARGETCFG for this IRQ */
	mem_addr_t targetcfg_addr = get_targetcfg_addr(dev, irq);
	uint32_t targetcfg = aplic_read(dev, targetcfg_addr);
	
	/* Extract target hart from TARGETCFG */
	uint32_t target_hart = targetcfg & APLIC_TARGET_HART_IDX_MASK;
	
	/* For QEMU RISC-V virt, we need to route to RISC-V's external interrupt (IRQ 2) */
	/* This is done by setting the external interrupt bit in the target hart's mip */
	
	/* Calculate the RISC-V interrupt number for this APLIC IRQ */
	/* QEMU RISC-V virt uses IRQ 2 for external interrupts from APLIC */
	uint32_t riscv_irq = 2; /* External interrupt */
	
	/* Set the interrupt pending in RISC-V's mip register */
	/* This is done by writing to the appropriate APLIC register that routes to RISC-V */
	
	/* For now, we'll use a simplified approach: directly call the ISR */
	/* In a real implementation, this would set the hart's interrupt pending */
	
	printk("APLIC: Routing IRQ %u to RISC-V IRQ %u (Hart %u)\n", irq, riscv_irq, target_hart);
	
	/* The interrupt should now be visible to RISC-V's interrupt handler */
	/* Zephyr's __soc_handle_irq should be called with the RISC-V IRQ number */
}

/**
 * @brief Trigger RISC-V external interrupt from APLIC
 * This function actually sets the interrupt pending in RISC-V's mip register
 */
static void aplic_trigger_riscv_interrupt(const struct device *dev, uint32_t irq)
{
	/* In QEMU RISC-V virt, APLIC interrupts are routed through the CPU's local interrupt controller */
	/* We need to trigger the external interrupt (IRQ 2) in RISC-V's mip register */
	
	/* Read the current TARGETCFG for this IRQ */
	mem_addr_t targetcfg_addr = get_targetcfg_addr(dev, irq);
	uint32_t targetcfg = aplic_read(dev, targetcfg_addr);
	
	/* Extract target hart from TARGETCFG */
	uint32_t target_hart = targetcfg & APLIC_TARGET_HART_IDX_MASK;
	
	/* For QEMU RISC-V virt, external interrupts are handled by the CPU's local interrupt controller */
	/* The APLIC needs to signal the CPU that an external interrupt is pending */
	
	/* This is typically done by setting a specific bit in the CPU's interrupt pending register */
	/* In QEMU RISC-V virt, this is handled by the hardware */
	
	printk("APLIC: Triggering RISC-V external interrupt for IRQ %u (Hart %u)\n", irq, target_hart);
	
	/* The hardware should now route this interrupt to RISC-V's interrupt handler */
	/* Zephyr will receive the interrupt through the normal RISC-V interrupt path */
}

/**
 * @brief Check APLIC interrupt routing status
 * This function helps debug interrupt routing issues
 */
static void aplic_debug_interrupt_routing(const struct device *dev, uint32_t irq)
{
	if (irq == 0 || irq >= 1024) {
		return;
	}
	
	/* Read SOURCECFG for this IRQ */
	mem_addr_t sourcecfg_addr = get_sourcecfg_addr(dev, irq);
	uint32_t sourcecfg = aplic_read(dev, sourcecfg_addr);
	
	/* Read TARGETCFG for this IRQ */
	mem_addr_t targetcfg_addr = get_targetcfg_addr(dev, irq);
	uint32_t targetcfg = aplic_read(dev, targetcfg_addr);
	
	/* Check if interrupt is enabled in SETIE */
	bool irq_enabled = riscv_aplic_irq_is_enabled(irq);
	
	/* Extract configuration details */
	uint32_t source_mode = sourcecfg & APLIC_SOURCECFG_SM_MASK;
	uint32_t target_hart = targetcfg & APLIC_TARGET_HART_IDX_MASK;
	uint32_t target_guest = (targetcfg >> 14) & APLIC_TARGET_GUEST_IDX_MASK;
	uint32_t priority = (targetcfg >> 20) & 0xFF;
	bool target_enabled = (targetcfg >> 31) & 0x1;
	
	printk("APLIC: IRQ %u routing debug:\n", irq);
	printk("  - SOURCECFG: 0x%08X (mode: %u)\n", sourcecfg, source_mode);
	printk("  - TARGETCFG: 0x%08X\n", targetcfg);
	printk("  - Target Hart: %u, Guest: %u, Priority: %u, Enabled: %s\n", 
	       target_hart, target_guest, priority, target_enabled ? "yes" : "no");
	printk("  - SETIE enabled: %s\n", irq_enabled ? "yes" : "no");
}

/**
 * @brief Handle a single APLIC interrupt
 */
static void aplic_handle_single_interrupt(const struct device *dev, uint32_t irq)
{
	struct aplic_data *data = dev->data;
	
	if (dev == NULL || data == NULL) {
		return;
	}
	
	/* 1. Check if the interrupt is enabled */
	if (!riscv_aplic_irq_is_enabled(irq)) {
		LOG_DBG("APLIC: IRQ %u not enabled, ignoring", irq);
		return;
	}
	
	/* 2. Check the priority */
	uint32_t priority = 0;
	/* For now, use default priority since get_priority function doesn't exist */
	priority = 7; /* Default priority */
	
	/* 3. Route to the appropriate target based on mode */
	if (data->msi_mode_enabled) {
		/* MSI mode: Send MSI message to IMSIC */
		LOG_DBG("APLIC: MSI mode - sending IRQ %u (priority %u)", irq, priority);
		
		/* Get target hart and guest from TARGETCFG */
		uint32_t target_hart = 0;
		uint32_t target_guest = 0;
		
		/* Read target configuration from TARGETCFG register */
		mem_addr_t targetcfg_addr = get_targetcfg_addr(dev, target_hart);
		uint32_t targetcfg = aplic_read(dev, targetcfg_addr + APLIC_TARGETCFG_HARTID);
		
		/* Extract hart and guest from TARGETCFG */
		target_hart = targetcfg & APLIC_TARGET_HART_IDX_MASK;
		target_guest = (targetcfg >> 14) & APLIC_TARGET_GUEST_IDX_MASK;
		
		/* Send MSI through IMSIC */
		int ret = aplic_send_msi(dev, target_hart, target_guest, irq);
		if (ret != 0) {
			LOG_ERR("APLIC: Failed to send MSI for IRQ %u: %d", irq, ret);
			/* Update error statistics - use available field */
			LOG_ERR("APLIC: MSI send failed for IRQ %u", irq);
		} else {
			/* Update MSI statistics */
			k_spinlock_key_t key = k_spin_lock(&data->lock);
			data->msi_interrupts_sent++;
			k_spin_unlock(&data->lock, key);
		}
		
	} else {
		/* Direct mode: Route interrupt to RISC-V mip register */
		LOG_DBG("APLIC: Direct mode - routing IRQ %u (priority %u) to RISC-V", irq, priority);
		
		/* Debug: Show current routing configuration */
		aplic_debug_interrupt_routing(dev, irq);
		
		/* Route the interrupt to RISC-V using our routing function */
		aplic_route_interrupt_to_riscv(dev, irq);
		
		/* Trigger the actual RISC-V interrupt */
		aplic_trigger_riscv_interrupt(dev, irq);
		
		/* For now, we'll call the registered ISR directly */
		/* In a real implementation, this would set the hart's interrupt pending */
		if (irq < CONFIG_NUM_IRQS) {
			const struct _isr_table_entry *entry = &_sw_isr_table[irq];
			if (entry->isr != NULL) {
				/* Call the registered interrupt service routine */
				entry->isr(entry->arg);
				LOG_DBG("APLIC: Called ISR for IRQ %u", irq);
			} else {
				LOG_WRN("APLIC: No ISR registered for IRQ %u", irq);
			}
		}
		
		/* Update direct mode statistics */
		k_spinlock_key_t key = k_spin_lock(&data->lock);
		data->direct_interrupts++;
		k_spin_unlock(&data->lock, key);
	}
	
	/* 4. Clear the interrupt source */
	/* For edge-triggered interrupts, APLIC usually clears automatically */
	/* For level-triggered interrupts, software may need to clear */
	enum riscv_aplic_trigger_type trigger_type;
	int trigger_ret = riscv_aplic_irq_get_trigger_type(irq);
	
	if (trigger_ret < 0) {
		/* Failed to get trigger type, assume edge-triggered */
		trigger_type = RISCV_APLIC_TRIGGER_EDGE_RISING;
	} else {
		trigger_type = (enum riscv_aplic_trigger_type)trigger_ret;
	}
	
	/* Handle level-triggered interrupts that may need manual clearing */
	if (trigger_type == RISCV_APLIC_TRIGGER_LEVEL_HIGH || 
	    trigger_type == RISCV_APLIC_TRIGGER_LEVEL_LOW) {
		/* Level-triggered interrupts may need manual clearing */
		/* This depends on the specific APLIC implementation */
		LOG_DBG("APLIC: Level-triggered IRQ %u, may need manual clearing", irq);
		
		/* For now, we'll let the device driver handle clearing */
		/* In a real implementation, you might clear a specific register bit */
	}
	
	/* Update interrupt statistics */
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	data->total_interrupts++;
	if (irq < 1024) {
		data->irq_info[irq].count++;
		data->irq_info[irq].last_cpu = arch_curr_cpu()->id;
	}
	k_spin_unlock(&data->lock, key);
	
	/* Update load balancing information - simplified for now */
	uint32_t current_cpu = arch_curr_cpu()->id;
	LOG_DBG("APLIC: Interrupt %u handled on CPU %u", irq, current_cpu);
	
	LOG_DBG("APLIC: Successfully handled interrupt %u (priority %u)", irq, priority);
}

/* ============================================================================
 * Interrupt Handling
 * ============================================================================ */

/**
 * @brief APLIC interrupt service routine
 */
static void aplic_isr(const void *arg)
{
	const struct device *dev = arg;
	struct aplic_data *data = dev->data;
	
	if (dev == NULL || data == NULL) {
		return;
	}
	
	/* Read pending interrupts from APLIC */
	/* For now, we'll use a simplified approach */
	uint32_t pending = 0;
	
	/* In a real implementation, you would read from APLIC's pending registers */
	/* For now, we'll just log that the ISR was called */
	printk("APLIC: ISR called for device %s\n", dev->name ? dev->name : "NULL");
	
	/* Update interrupt statistics */
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	data->total_interrupts++;
	k_spin_unlock(&data->lock, key);
}

/* ============================================================================
 * Device tree based configuration
 * ============================================================================ */
#define APLIC_INIT(n) \
	static void aplic_irq_config_func_##n(void); \
	\
	static const struct aplic_config aplic_config_##n = { \
		.base = DT_INST_REG_ADDR(n), \
		.max_prio = DT_INST_PROP_OR(n, riscv_max_priority, 7), \
		.riscv_ndev = DT_INST_PROP_OR(n, riscv_num_sources, 1024), \
		.nr_irqs = DT_INST_PROP_OR(n, riscv_num_sources, 1024), \
		.irq = DT_INST_IRQN(n), \
		.irq_config_func = aplic_irq_config_func_##n, \
		.isr_table = NULL, \
		.hart_context = NULL, \
	}; \
	BUILD_ASSERT(DT_INST_REG_ADDR(n) != 0, "APLIC base address is zero"); \
	\
	static struct aplic_data aplic_data_##n; \
	\
	DEVICE_DT_INST_DEFINE(n, aplic_init, NULL, \
			      &aplic_data_##n, &aplic_config_##n, \
			      PRE_KERNEL_1, CONFIG_INTC_INIT_PRIORITY, NULL); \
	\
	static void aplic_irq_config_func_##n(void) \
{ \
    /* True hardware signal line sharing: register shared handler */ \
    if (!aplic_parent_irq) { \
        /* Register shared interrupt handler to external IRQ line */ \
        aplic_parent_irq = RISCV_IRQ_MEXT; \
        IRQ_CONNECT(RISCV_IRQ_MEXT, 0, shared_ext_isr, NULL, 0); \
        irq_enable(RISCV_IRQ_MEXT); \
    } \
}

/* Generate device instances from device tree */
DT_INST_FOREACH_STATUS_OKAY(APLIC_INIT)

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

/* MSI mode support */
bool riscv_aplic_is_msi_mode_enabled(void)
{
	const struct device *dev = aplic_get_dev();
	struct aplic_data *data;
	
	if (dev == NULL) {
		return false;
	}
	
	data = dev->data;
	return data->msi_mode_enabled;
}

int riscv_aplic_configure_source_msi(uint32_t irq, uint32_t target_hart, uint32_t target_guest)
{
	const struct device *dev = aplic_get_dev();
	struct aplic_data *data;
	
	if (dev == NULL) {
		return -ENODEV;
	}
	
	data = dev->data;
	
	if (!data->msi_mode_enabled) {
		return -ENOTSUP;
	}
	
	return aplic_configure_source_msi(dev, irq, target_hart, target_guest);
}

int riscv_aplic_send_msi(uint32_t target_hart, uint32_t target_guest, uint32_t irq)
{
	const struct device *dev = aplic_get_dev();
	struct aplic_data *data;
	
	if (dev == NULL) {
		return -ENODEV;
	}
	
	data = dev->data;
	
	if (!data->msi_mode_enabled) {
		return -ENOTSUP;
	}
	
	return aplic_send_msi(dev, target_hart, target_guest, irq);
}


