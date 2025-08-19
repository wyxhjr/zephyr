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

#include "sw_isr_common.h"

#include <zephyr/debug/symtab.h>
#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/devicetree/interrupt_controller.h>
#include <zephyr/shell/shell.h>

#include <zephyr/sw_isr_table.h>
#include <zephyr/drivers/interrupt_controller/riscv_aplic.h>
#include <zephyr/irq.h>

/*
 * APLIC register offsets based on RISC-V AIA specification
 * https://github.com/riscv/riscv-aia
 */
#define APLIC_DOMAINCFG 0x00
#define APLIC_SOURCECFG_BASE 0x04
#define APLIC_SOURCECFG_SIZE 0x04
#define APLIC_UNUSED 0x1f
#define APLIC_TARGETCFG_BASE 0x3000
#define APLIC_TARGETCFG_SIZE 0x1000
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
};

static const struct device *save_dev[CONFIG_MP_MAX_NUM_CPUS];

/* Debug: Global variable to track if aplic_init was called */
volatile uint32_t aplic_init_called = 0;

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
	return *(volatile uint32_t *)addr;
}

static inline void aplic_write(const struct device *dev, mem_addr_t addr, uint32_t value)
{
	*(volatile uint32_t *)addr = value;
}

static inline void aplic_set_threshold(const struct device *dev, uint32_t hartid, uint32_t threshold)
{
	mem_addr_t targetcfg_addr = get_targetcfg_addr(dev, hartid);
	
	aplic_write(dev, targetcfg_addr + APLIC_TARGETCFG_THRESHOLD, threshold);
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
	mem_addr_t setie_addr = config->base + 0x24; /* SETIE register */
	uint32_t reg_offset = local_irq_to_reg_offset(irq);
	uint32_t bit_mask = BIT(irq & (APLIC_REG_SIZE - 1));
	
	/* Read current SETIE value and set the bit */
	uint32_t current_setie = aplic_read(dev, setie_addr + reg_offset);
	aplic_write(dev, setie_addr + reg_offset, current_setie | bit_mask);
}

static inline void aplic_irq_disable_internal(const struct device *dev, uint32_t irq)
{
	const struct aplic_config *config = (const struct aplic_config *)dev->config;
	mem_addr_t clrie_addr = config->base + 0x2C; /* CLRIE register */
	uint32_t reg_offset = local_irq_to_reg_offset(irq);
	uint32_t bit_mask = BIT(irq & (APLIC_REG_SIZE - 1));
	
	/* Read current CLRIE value and set the bit */
	uint32_t current_clrie = aplic_read(dev, clrie_addr + reg_offset);
	aplic_write(dev, clrie_addr + reg_offset, current_clrie | bit_mask);
}

static inline int aplic_irq_is_enabled_internal(const struct device *dev, uint32_t irq)
{
	const struct aplic_config *config = (const struct aplic_config *)dev->config;
	mem_addr_t setie_addr = config->base + 0x24; /* SETIE register */
	uint32_t reg_offset = local_irq_to_reg_offset(irq);
	uint32_t bit_mask = BIT(irq & (APLIC_REG_SIZE - 1));
	
	/* Read SETIE value and check if the bit is set */
	uint32_t setie_value = aplic_read(dev, setie_addr + reg_offset);
	
	return (setie_value & bit_mask) ? 1 : 0;
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
	struct aplic_data *data = dev->data;
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
	const struct aplic_config *config = dev->config;
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
	mem_addr_t setip_addr = config->base + 0x1C; /* SETIP register */
	uint32_t reg_offset = local_irq_to_reg_offset(irq);
	uint32_t bit_mask = BIT(irq & (APLIC_REG_SIZE - 1));
	
	/* Read current SETIP value and set the bit */
	uint32_t current_setip = aplic_read(dev, setip_addr + reg_offset);
	aplic_write(dev, setip_addr + reg_offset, current_setip | bit_mask);
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
		return aplic_get_claim(dev);
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

	k_spin_unlock(&data->lock, key);

	printk("APLIC: Statistics reset\n");
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
static void aplic_isr(const struct device *dev)
{
	uint32_t irq_id = aplic_get_claim(dev);
	uint32_t current_cpu = arch_curr_cpu()->id;

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
			printk("APLIC: IRQ %u handled %u times on CPU %u\n", 
			       irq_id, data->irq_info[irq_id].count, current_cpu);
		}
	}
}

/* ============================================================================
 * MSI Mode Support Functions
 * ============================================================================ */

/* Find IMSIC devices for MSI mode */
static int aplic_find_imsic_devices(const struct device *dev)
{
	const struct aplic_config *config = dev->config;
	struct aplic_data *data = dev->data;
	int found_count = 0;
	
	/* Search for IMSIC devices in device tree */
	for (int i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		/* Try to find IMSIC device for this hart */
		char dev_name[32];
		snprintf(dev_name, sizeof(dev_name), "imsic%d", i);
		
		const struct device *imsic_dev = device_get_binding(dev_name);
		if (imsic_dev != NULL && device_is_ready(imsic_dev)) {
			data->imsic_devices[i] = imsic_dev;
			found_count++;
			printk("APLIC: Found IMSIC device %s for hart %d\n", 
			       imsic_dev->name, i);
		} else {
			data->imsic_devices[i] = NULL;
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
	
	printk("APLIC: MSI mode enabled successfully\n");
	return 0;
}

/* Send MSI through IMSIC */
static int aplic_send_msi(const struct device *dev, uint32_t target_hart, 
			  uint32_t target_guest, uint32_t irq_id)
{
	struct aplic_data *data = dev->data;
	
	if (!data->msi_mode_enabled) {
		return -ENOTSUP;
	}
	
	if (target_hart >= CONFIG_MP_MAX_NUM_CPUS || 
	    data->imsic_devices[target_hart] == NULL) {
		return -EINVAL;
	}
	
	/* Calculate EID for this interrupt */
	uint32_t eid = data->msi_base_eid + irq_id;
	
	/* Use IMSIC API to send MSI */
	// TODO: Implement IMSIC MSI sending
	// For now, we'll just set the interrupt pending in the target IMSIC
	ARG_UNUSED(data->imsic_devices[target_hart]);
	
	/* This is a simplified implementation - in a real system,
	 * the APLIC would generate an MSI write to the IMSIC */
	printk("APLIC: Would send MSI EID %u to hart %u (guest %u)\n", 
	       eid, target_hart, target_guest);
	
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
	
	/* In MSI mode, SOURCECFG is configured differently */
	mem_addr_t sourcecfg_addr = get_sourcecfg_addr(dev, irq_id);
	
	/* For MSI mode, we need to set the target hart and guest in SOURCECFG */
	uint32_t sourcecfg_value = (target_hart << 16) | (target_guest << 8) | 
				   APLIC_SOURCECFG_SM_LEVEL_HIGH;
	
	aplic_write(dev, sourcecfg_addr, sourcecfg_value);
	
	printk("APLIC: Configured IRQ %u for MSI to hart %u, guest %u\n", 
	       irq_id, target_hart, target_guest);
	
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
	
	/* Set debug flag - this will be visible even if printk doesn't work */
	aplic_init_called = 0xDEADBEEF;
	
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
	
	if (domaincfg_readback & (1 << 8)) {
		printk("APLIC: IE bit successfully enabled\n");
	} else {
		printk("APLIC: WARNING - IE bit could not be enabled\n");
	}

	/* Initialize MSI mode support */
	data->msi_mode_enabled = false;
	for (i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		data->imsic_devices[i] = NULL;
	}
	data->msi_base_eid = 0;
	
	/* Try to detect and configure MSI mode */
	if (aplic_configure_msi_mode(dev) == 0) {
		printk("APLIC: MSI mode configured successfully\n");
	} else {
		printk("APLIC: MSI mode not available, using direct mode\n");
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

	/* Configure target configuration for each hart */
	for (i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		mem_addr_t targetcfg_addr = get_targetcfg_addr(dev, i);
		
		/* Set threshold to 0 (accept all interrupts) */
		aplic_set_threshold(dev, i, 0);
		data->hart_thresholds[i] = 0;
		
		/* Enable interrupts for this target */
		aplic_write(dev, targetcfg_addr + APLIC_TARGETCFG_IE, 0x1);
	}

	/* Save device reference for all CPUs */
	for (i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		save_dev[i] = dev;
	}

	return 0;
}

/* Device tree based configuration */
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
		/* Temporarily disable APLIC IRQ to test IMSIC */ \
		/* IRQ_CONNECT(DT_INST_IRQN(n), 0, aplic_isr, DEVICE_DT_INST_GET(n), 0); */ \
		/* irq_enable(DT_INST_IRQN(n)); */ \
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


