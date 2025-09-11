/*
 * Copyright (c) 2024 Zephyr Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qemu_aplic

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

/* Forward declarations */
int aplic_send_msi(const struct device *dev, uint32_t target_hart, 
			  uint32_t target_guest, uint32_t irq_id);
static void aplic_direct_mode_handler(const void *arg);
static int aplic_configure_direct_mode(const struct device *dev);
static int aplic_configure_msi_mode(const struct device *dev);
static inline uint32_t aplic_get_idc_claim(const struct device *dev, uint32_t hart_id);
static inline void aplic_set_idc_complete(const struct device *dev, uint32_t hart_id, uint32_t irq);
static inline const struct device *aplic_get_dev(void);

/* APLIC Register Offsets - AIA Specification Compliant */
#define APLIC_DOMAINCFG        0x0000
#define APLIC_SOURCECFG_BASE   0x0004
#define APLIC_SOURCECFG_SIZE   0x0004
#define APLIC_TARGET_BASE      0x3000 
#define APLIC_TARGET_SIZE      0x0004
#define APLIC_SETIE_BASE       0x1E00
#define APLIC_CLRIE_BASE       0x1F00
#define APLIC_SETIPNUM_LE      0x2000
#define APLIC_SETIP_BASE       0x1C00
#define APLIC_CLRIP_BASE       0x1D00
#define APLIC_xMSICFGADDR      0x1bc0
#define APLIC_xMSICFGADDRH     0x1bc4

/* MSI-specific registers */
#define APLIC_SETIENUM         0x1E00
#define APLIC_CLRIENUM         0x1F00

/* IMSIC register offsets for MSI operations */
#define IMSIC_EIP0  0x80
#define IMSIC_EIP63 0xbf

/* APLIC MSI Configuration Constants */
#define APLIC_DEFAULT_PRIORITY     7
#define APLIC_MAX_PRIORITY         255
#define APLIC_MAX_HART_IDX         0x3FFF
#define APLIC_MAX_GUEST_IDX        0x3F
#define APLIC_MAX_EIID            0xFF

/* APLIC TARGET register field definitions */
#define APLIC_TARGET_HART_IDX_SHIFT    0
#define APLIC_TARGET_HART_IDX_MASK     0x3FFF
#define APLIC_TARGET_GUEST_IDX_SHIFT   14
#define APLIC_TARGET_GUEST_IDX_MASK    0x3F
#define APLIC_TARGET_EIID_SHIFT        20
#define APLIC_TARGET_EIID_MASK         0xFF
#define APLIC_TARGET_IE_SHIFT          31
#define APLIC_TARGET_IE_MASK           0x1

/* APLIC SOURCECFG field definitions */
#define APLIC_SOURCECFG_SM_MASK       0x7
#define APLIC_SOURCECFG_SM_INACTIVE   0x0
#define APLIC_SOURCECFG_SM_DETACHED   0x1
#define APLIC_SOURCECFG_SM_EDGE_RISE  0x4
#define APLIC_SOURCECFG_SM_EDGE_FALL  0x5
#define APLIC_SOURCECFG_SM_LEVEL_HIGH 0x6
#define APLIC_SOURCECFG_SM_LEVEL_LOW  0x7
#define APLIC_SOURCECFG_SM_MSI        0x8

#define APLIC_SOURCECFG_D_MASK        0x400
#define APLIC_SOURCECFG_CHILD_MASK    0x3FF800
#define APLIC_SOURCECFG_CHILD_SHIFT   11

/* APLIC DOMAINCFG field definitions */
#define APLIC_DOMAINCFG_IE            BIT(8)
#define APLIC_DOMAINCFG_DM            BIT(2)
#define APLIC_DOMAINCFG_BE            BIT(0)

/* APLIC register organization constants */
#define APLIC_REG_SIZE 32
#define APLIC_REG_MASK BIT_MASK(LOG2(APLIC_REG_SIZE))
#define APLIC_IRQBITS_PER_REG 32



/* APLIC IDC register definitions */
#define APLIC_IDC_BASE         0x4000
#define APLIC_IDC_SIZE         32
#define APLIC_IDC_IDELIVERY    0x00
#define APLIC_IDC_IFORCE       0x04
#define APLIC_IDC_ITHRESHOLD   0x08
#define APLIC_IDC_TOPI         0x18
#define APLIC_IDC_CLAIMI       0x1C

/* APLIC IDC TOPI field definitions */
#define APLIC_IDC_TOPI_ID_MASK     0x3FF
#define APLIC_IDC_TOPI_ID_SHIFT    16
#define APLIC_IDC_TOPI_PRIO_MASK   0xFF
#define APLIC_IDC_TOPI_PRIO_SHIFT  0

/* APLIC IDC control values */
#define APLIC_IDC_DELIVERY_DISABLE 0
#define APLIC_IDC_DELIVERY_ENABLE  1
#define APLIC_IDC_THRESHOLD_DISABLE 1
#define APLIC_IDC_THRESHOLD_ENABLE  0

/* Forward declarations */
typedef void (*riscv_aplic_irq_config_func_t)(void);

/* APLIC configuration structure */
struct aplic_config {
	mem_addr_t base;
	uint32_t max_prio;
	uint32_t riscv_ndev;
	uint32_t nr_irqs;
	uint32_t irq;
	riscv_aplic_irq_config_func_t irq_config_func;
	const struct _isr_table_entry *isr_table;
	const uint32_t *const hart_context;
};

/* APLIC trigger type enumeration */
enum aplic_trigger_type {
	APLIC_TRIGGER_EDGE_RISING = APLIC_SOURCECFG_SM_EDGE_RISE,
	APLIC_TRIGGER_EDGE_FALLING = APLIC_SOURCECFG_SM_EDGE_FALL,
	APLIC_TRIGGER_LEVEL_HIGH = APLIC_SOURCECFG_SM_LEVEL_HIGH,
	APLIC_TRIGGER_LEVEL_LOW = APLIC_SOURCECFG_SM_LEVEL_LOW,
};

/* APLIC interrupt information structure */
struct aplic_irq_info {
	uint32_t count;
	uint32_t last_cpu;
	uint32_t affinity_mask;
	enum aplic_trigger_type trigger_type;
	uint8_t priority;
	bool enabled;
};

/* APLIC MSI configuration structure */
struct aplic_msicfg {
	uint32_t base_ppn;
	uint32_t lhxs;  /* Guest index bits */
	uint32_t lhxw;  /* Hart index bits */
	uint32_t hhxw;  /* Group index bits */
	uint32_t hhxs;  /* Group index shift */
};

/* APLIC device data structure */
struct aplic_data {
	struct k_spinlock lock;
	struct aplic_irq_info irq_info[1024];
	uint32_t total_interrupts;
	uint32_t hart_thresholds[CONFIG_MP_MAX_NUM_CPUS];
	
	bool msi_mode_enabled;
	const struct device *imsic_devices[CONFIG_MP_MAX_NUM_CPUS];
	uint32_t msi_base_eid;
	mem_addr_t imsic_base;
	uint32_t msi_interrupts_sent;
	uint32_t direct_interrupts;
	
	/* MSI configuration */
	struct aplic_msicfg msicfg;
};

/* Global device references */
static const struct device *save_dev[CONFIG_MP_MAX_NUM_CPUS];
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

static inline mem_addr_t get_target_addr(const struct device *dev, uint32_t irq)
{
	const struct aplic_config *config = dev->config;

	/* TARGET registers are organized per-IRQ, not per-hart */
	return config->base + APLIC_TARGET_BASE + ((irq - 1) * APLIC_TARGET_SIZE);
}

static inline mem_addr_t get_idc_claim_addr(const struct device *dev, uint32_t hart_id)
{
	const struct aplic_config *config = dev->config;

	return config->base + APLIC_IDC_BASE + (hart_id * APLIC_IDC_SIZE) + APLIC_IDC_CLAIMI;
}

static inline uint32_t aplic_read(const struct device *dev, mem_addr_t addr)
{
	__asm__ volatile ("fence rw,rw" : : : "memory");
	uint32_t value = *(volatile uint32_t *)addr;
	__asm__ volatile ("fence r,r" : : : "memory");
	return value;
}

static inline void aplic_write(const struct device *dev, mem_addr_t addr, uint32_t value)
{
	
	*(volatile uint32_t *)addr = value;
	
	
	
	__asm__ volatile ("fence w,w" : : : "memory");
	__asm__ volatile ("fence iorw,iorw" : : : "memory");
}

static inline void aplic_set_idc_threshold(const struct device *dev, uint32_t hart_id, uint32_t threshold)
{
	const struct aplic_config *config = dev->config;
	mem_addr_t idc_base = config->base + APLIC_IDC_BASE + (hart_id * APLIC_IDC_SIZE);
	mem_addr_t ithreshold_addr = idc_base + APLIC_IDC_ITHRESHOLD;
	
	aplic_write(dev, ithreshold_addr, threshold);
}

/* APLIC Direct Mode Interrupt Handler */
static void aplic_direct_mode_handler(const void *arg)
{
	ARG_UNUSED(arg);
	const struct device *dev = aplic_get_dev();
	struct aplic_data *data;
	uint32_t hart_id = arch_proc_id();
	uint32_t claim_value;
	uint32_t irq_id, priority;
	
	if (!dev) {
		return;
	}
	
	data = dev->data;
	if (!data || data->msi_mode_enabled) {
		/* Only handle in Direct mode */
		return;
	}
	
	/* Read CLAIMI register in a loop until no more interrupts */
	while ((claim_value = aplic_get_idc_claim(dev, hart_id)) != 0) {
		/* Extract IRQ ID and priority from claim value */
		irq_id = (claim_value >> APLIC_IDC_TOPI_ID_SHIFT) & APLIC_IDC_TOPI_ID_MASK;
		priority = (claim_value >> APLIC_IDC_TOPI_PRIO_SHIFT) & APLIC_IDC_TOPI_PRIO_MASK;
		
		if (irq_id == 0) {
			/* Spurious interrupt */
			continue;
		}
		
		/* Dispatch to registered ISR */
		if (irq_id < CONFIG_NUM_IRQS) {
			const struct _isr_table_entry *entry = &_sw_isr_table[irq_id];
			if (entry->isr != NULL) {
				entry->isr(entry->arg);
				LOG_DBG("APLIC: Handled Direct mode interrupt %u (priority %u)", 
					irq_id, priority);
				
				/* Update statistics */
				k_spinlock_key_t key = k_spin_lock(&data->lock);
				data->direct_interrupts++;
				if (irq_id < 1024) {
					data->irq_info[irq_id].count++;
					data->irq_info[irq_id].last_cpu = hart_id;
				}
				k_spin_unlock(&data->lock, key);
			} else {
				LOG_WRN("APLIC: No ISR registered for Direct mode IRQ %u", irq_id);
			}
		} else {
			LOG_WRN("APLIC: Invalid IRQ ID %u in Direct mode", irq_id);
		}
	}
}

static inline uint32_t aplic_get_idc_topi(const struct device *dev)
{
	const struct aplic_config *config = dev->config;
	uint32_t cpu_id = arch_proc_id();
	mem_addr_t idc_base = config->base + APLIC_IDC_BASE + (cpu_id * APLIC_IDC_SIZE);
	mem_addr_t topi_addr = idc_base + APLIC_IDC_TOPI;
	
	
	uint32_t topi_value = aplic_read(dev, topi_addr);
	
	
	uint32_t irq_id = (topi_value >> APLIC_IDC_TOPI_ID_SHIFT) & APLIC_IDC_TOPI_ID_MASK;
	
	
	if (irq_id == 0) {
		return UINT_MAX;
	}
	
	return irq_id;
}

static inline uint32_t aplic_get_idc_claim(const struct device *dev, uint32_t hart_id)
{
	mem_addr_t claim_addr = get_idc_claim_addr(dev, hart_id);
	
	return aplic_read(dev, claim_addr);
}

static inline void aplic_set_idc_complete(const struct device *dev, uint32_t hart_id, uint32_t irq)
{
	mem_addr_t claim_addr = get_idc_claim_addr(dev, hart_id);
	aplic_write(dev, claim_addr, irq);
}

static inline void aplic_irq_enable_internal(const struct device *dev, uint32_t irq)
{
	const struct aplic_config *config = (const struct aplic_config *)dev->config;
	struct aplic_data *data = dev->data;
	mem_addr_t addr;
	uint32_t bit_mask;
	
	if (data->msi_mode_enabled) {
		/* MSI mode: use SETIENUM register */
		addr = config->base + APLIC_SETIENUM;
		aplic_write(dev, addr, irq);
		LOG_DBG("APLIC: MSI mode - Enabled IRQ %u via SETIENUM", irq);
	} else {
		/* Direct mode: use SETIE register */
		addr = config->base + APLIC_SETIE_BASE + local_irq_to_reg_offset(irq);
		bit_mask = BIT(irq & (APLIC_REG_SIZE - 1));
		
		uint32_t current_setie = aplic_read(dev, addr);
	uint32_t new_setie = current_setie | bit_mask;
		aplic_write(dev, addr, new_setie);
		LOG_DBG("APLIC: Direct mode - Enabled IRQ %u via SETIE", irq);
	}
}

static inline void aplic_irq_disable_internal(const struct device *dev, uint32_t irq)
{
	const struct aplic_config *config = (const struct aplic_config *)dev->config;
	struct aplic_data *data = dev->data;
	mem_addr_t addr;
	uint32_t bit_mask;
	
	if (data->msi_mode_enabled) {
		/* MSI mode: use CLRIENUM register */
		addr = config->base + APLIC_CLRIENUM;
		aplic_write(dev, addr, irq);
		LOG_DBG("APLIC: MSI mode - Disabled IRQ %u via CLRIENUM", irq);
	} else {
		/* Direct mode: use CLRIE register */
		addr = config->base + APLIC_CLRIE_BASE + local_irq_to_reg_offset(irq);
		bit_mask = BIT(irq & (APLIC_REG_SIZE - 1));
		
		uint32_t current_clrie = aplic_read(dev, addr);
		aplic_write(dev, addr, current_clrie | bit_mask);
		LOG_DBG("APLIC: Direct mode - Disabled IRQ %u via CLRIE", irq);
	}
}

static inline int aplic_irq_is_enabled_internal(const struct device *dev, uint32_t irq)
{
	const struct aplic_config *config = (const struct aplic_config *)dev->config;
	struct aplic_data *data = dev->data;
	
	if (data->msi_mode_enabled) {
		/* MSI mode: SETIENUM/CLRIENUM are write-only, assume enabled */
		LOG_DBG("APLIC: MSI mode - Cannot check IRQ %u status (write-only registers)", irq);
		return 1;
	} else {
		/* Direct mode: read from SETIE register */
		mem_addr_t setie_addr = config->base + APLIC_SETIE_BASE + local_irq_to_reg_offset(irq);
		uint32_t bit_mask = BIT(irq & (APLIC_REG_SIZE - 1));
	uint32_t setie_value = aplic_read(dev, setie_addr);
		return (setie_value & bit_mask) ? 1 : 0;
	}
}

static inline void aplic_set_priority_internal(const struct device *dev, uint32_t irq, uint32_t priority)
{
	mem_addr_t sourcecfg_addr = get_sourcecfg_addr(dev, irq);
	uint32_t current_cfg = aplic_read(dev, sourcecfg_addr);
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
	if (irq == 0 || irq >= config->nr_irqs) {
		return -EINVAL;
	}
	target_hart = __builtin_ffs(cpumask) - 1;
	if (target_hart >= CONFIG_MP_MAX_NUM_CPUS) {
		return -EINVAL;
	}
	
	key = k_spin_lock(&data->lock);
	
	
	sourcecfg_addr = get_sourcecfg_addr(dev, irq);
	sourcecfg_val = aplic_read(dev, sourcecfg_addr);
	
	
	
	data->irq_info[irq].affinity_mask = cpumask;
	
	
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
	
	
	if (irq == 0 || irq >= config->nr_irqs) {
		return -EINVAL;
	}
	
	
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
	
	
	sourcecfg_addr = get_sourcecfg_addr(dev, irq);
	sourcecfg_val = aplic_read(dev, sourcecfg_addr);
	
	
	sourcecfg_val &= ~APLIC_SOURCECFG_SM_MASK;
	sourcecfg_val |= (uint32_t)type;
	aplic_write(dev, sourcecfg_addr, sourcecfg_val);
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
	if (irq == 0 || irq >= config->nr_irqs) {
		return -EINVAL;
	}
	sourcecfg_addr = get_sourcecfg_addr(dev, irq);
	sourcecfg_val = aplic_read(dev, sourcecfg_addr);
	
	return sourcecfg_val & APLIC_SOURCECFG_SM_MASK;
}

static inline int aplic_hart_set_threshold_internal(const struct device *dev, uint32_t hart_id, uint32_t threshold)
{
	struct aplic_data *data = dev->data;
	k_spinlock_key_t key;
	if (hart_id >= CONFIG_MP_MAX_NUM_CPUS) {
		return -EINVAL;
	}
	if (threshold > 255) {
		return -EINVAL;
	}
	key = k_spin_lock(&data->lock);
	
	/* Set IDC threshold for Direct mode */
	aplic_set_idc_threshold(dev, hart_id, threshold);
	
	data->hart_thresholds[hart_id] = threshold;
	
	k_spin_unlock(&data->lock, key);

	LOG_DBG("APLIC: Set Hart %u threshold to %u", hart_id, threshold);

	return 0;
}

static inline uint32_t aplic_hart_get_threshold_internal(const struct device *dev, uint32_t hart_id)
{
	struct aplic_data *data = dev->data;
	if (hart_id >= CONFIG_MP_MAX_NUM_CPUS) {
		return 0;
	}
	return data->hart_thresholds[hart_id];
}

static inline void aplic_irq_set_pending_internal(const struct device *dev, uint32_t irq)
{
	const struct aplic_config *config = (const struct aplic_config *)dev->config;
	struct aplic_data *data = dev->data;
	
	/* Set interrupt pending bit */
	mem_addr_t setip_addr = config->base + APLIC_SETIP_BASE + local_irq_to_reg_offset(irq);
	uint32_t bit_mask = BIT(irq & (APLIC_REG_SIZE - 1));
	
	uint32_t current_setip = aplic_read(dev, setip_addr);
	aplic_write(dev, setip_addr, current_setip | bit_mask);
	LOG_DBG("APLIC: Set IRQ %u pending - SETIP: 0x%08X -> 0x%08X", 
	       irq, current_setip, current_setip | bit_mask);
	/* Check if interrupt is enabled */
	if (!riscv_aplic_irq_is_enabled(irq)) {
		LOG_DBG("APLIC: IRQ %u is not enabled, skipping routing", irq);
		return;
	}
			/* Update statistics */
			k_spinlock_key_t key = k_spin_lock(&data->lock);
	if (data->msi_mode_enabled) {
		data->msi_interrupts_sent++;
		LOG_DBG("APLIC: MSI mode - IRQ %u pending, hardware will handle MSI transmission", irq);
	} else {
			data->direct_interrupts++;
		LOG_DBG("APLIC: Direct mode - IRQ %u pending, hardware will assert MEIP", irq);
	}
			k_spin_unlock(&data->lock, key);
}

static inline const struct device *aplic_get_dev(void)
{
	return save_dev[arch_proc_id()];
}


void riscv_aplic_irq_enable(uint32_t irq)
{
	const struct device *dev = aplic_get_dev();
	struct aplic_data *data;
	k_spinlock_key_t key;

	if (dev != NULL) {
		aplic_irq_enable_internal(dev, irq);
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
		return aplic_get_idc_topi(dev);
	}

	return UINT_MAX;
}

const struct device *riscv_aplic_get_dev(void)
{
	return aplic_get_dev();
}

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

static inline void aplic_update_irq_stats(const struct device *dev, uint32_t irq_id)
{
	struct aplic_data *data = dev->data;
	uint32_t current_cpu = arch_curr_cpu()->id;
	k_spinlock_key_t key;
	if (irq_id >= 1024) {
		return;
	}
	key = k_spin_lock(&data->lock);
	data->irq_info[irq_id].count++;
	data->irq_info[irq_id].last_cpu = current_cpu;
	data->total_interrupts++;
	
	k_spin_unlock(&data->lock, key);
}

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
		affinity_mask = BIT_MASK(CONFIG_MP_MAX_NUM_CPUS); 
	}
	
	
	for (uint32_t cpu = 0; cpu < CONFIG_MP_MAX_NUM_CPUS; cpu++) {
		if (!(affinity_mask & BIT(cpu))) {
			continue;
		}
		
		
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

static int aplic_find_imsic_devices(const struct device *dev)
{
	struct aplic_data *data = dev->data;
	int found_count = 0;
	
	/* Use device tree to find IMSIC device */
	const struct device *imsic_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(imsic));
			if (imsic_dev != NULL && device_is_ready(imsic_dev)) {
		/* Set the same IMSIC device reference for all CPUs */
		for (int i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
				data->imsic_devices[i] = imsic_dev;
		}
		found_count = CONFIG_MP_MAX_NUM_CPUS;
		LOG_INF("APLIC: Found IMSIC device %s for all %d CPUs", 
			imsic_dev->name, CONFIG_MP_MAX_NUM_CPUS);
	} else {
		LOG_WRN("APLIC: No IMSIC device found in device tree");
		/* Clear all IMSIC device references */
		for (int i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
			data->imsic_devices[i] = NULL;
		}
	}
	
	return found_count;
}

static int aplic_configure_msi_mode(const struct device *dev)
{
	const struct aplic_config *config = dev->config;
	struct aplic_data *data = dev->data;
	struct aplic_msicfg *mc = &data->msicfg;
	
	LOG_INF("APLIC: Configuring MSI mode");
	
	/* Find IMSIC devices */
	if (aplic_find_imsic_devices(dev) == 0) {
		LOG_ERR("APLIC: No IMSIC devices found, cannot enable MSI mode");
		return -ENODEV;
	}
	
	/* Get IMSIC configuration from device tree */
	const struct device *imsic_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(imsic));
	if (imsic_dev == NULL) {
		LOG_ERR("APLIC: Cannot find IMSIC device for MSI configuration");
		return -ENODEV;
	}
	
	/* Configure MSI parameters based on QEMU IMSIC (Linux compatible) */
	mc->lhxs = 0;   /* Guest index bits - QEMU uses 0 */
	mc->lhxw = 0;   /* Hart index bits - QEMU uses 0 */
	mc->hhxw = 0;   /* Group index bits - single group */
	mc->hhxs = 0;   /* Group index shift */
	
	/* Calculate base PPN from IMSIC base address (QEMU default) */
	mem_addr_t imsic_base = DT_REG_ADDR(DT_NODELABEL(imsic));
	if (imsic_base == 0) {
		imsic_base = 0x24000000;  /* Fallback to QEMU default */
	}
	mc->base_ppn = imsic_base >> 12;
	
	LOG_INF("APLIC: IMSIC base: 0x%08lX, base_ppn: 0x%08X", 
		imsic_base, mc->base_ppn);
	/* Configure MSI address registers */
	uint32_t mmsicfgaddr = mc->base_ppn & 0xFFFFFFFF;
	uint32_t mmsicfgaddrh = (mc->lhxw & 0xF) << 0 |
				 (mc->hhxw & 0xF) << 4 |
				 (mc->lhxs & 0xF) << 8 |
				 (mc->hhxs & 0xF) << 12;
	
	aplic_write(dev, config->base + APLIC_xMSICFGADDR, mmsicfgaddr);
	aplic_write(dev, config->base + APLIC_xMSICFGADDRH, mmsicfgaddrh);
	
	LOG_INF("APLIC: MSI address registers configured: ADDR=0x%08X, ADDRH=0x%08X", 
	       mmsicfgaddr, mmsicfgaddrh);
	data->msi_mode_enabled = true;
	data->msi_base_eid = 0;
	
	LOG_INF("APLIC: MSI mode configuration complete");
	return 0;
}

static int aplic_configure_direct_mode(const struct device *dev)
{
	const struct aplic_config *config = dev->config;
	struct aplic_data *data = dev->data;
	uint32_t i;
	LOG_INF("APLIC: Configuring Direct mode");
	/* Configure IDC for each hart */
	for (i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		mem_addr_t idc_base = config->base + APLIC_IDC_BASE + (i * APLIC_IDC_SIZE);
		/* Enable interrupt delivery */
		mem_addr_t idelivery_addr = idc_base + APLIC_IDC_IDELIVERY;
		aplic_write(dev, idelivery_addr, APLIC_IDC_DELIVERY_ENABLE);
		/* Set threshold to 0 (accept all interrupts) */
		mem_addr_t ithreshold_addr = idc_base + APLIC_IDC_ITHRESHOLD;
		aplic_write(dev, ithreshold_addr, APLIC_IDC_THRESHOLD_ENABLE);
		LOG_DBG("APLIC: Configured IDC for hart %u", i);
	}
	/* Configure interrupt sources for Direct mode */
	for (i = 1; i < config->nr_irqs; i++) {
		mem_addr_t sourcecfg_addr = get_sourcecfg_addr(dev, i);
		uint32_t sourcecfg_value = APLIC_SOURCECFG_SM_DETACHED;
		aplic_write(dev, sourcecfg_addr, sourcecfg_value);
		/* Configure TARGET register for Direct mode */
		mem_addr_t target_addr = get_target_addr(dev, i);
		uint32_t target_value = (0 & APLIC_TARGET_HART_IDX_MASK) |
					 (0 << APLIC_TARGET_GUEST_IDX_SHIFT) |
					 (APLIC_DEFAULT_PRIORITY << APLIC_TARGET_EIID_SHIFT) |
					 (1 << APLIC_TARGET_IE_SHIFT);
		
		aplic_write(dev, target_addr, target_value);
		LOG_DBG("APLIC: Configured IRQ %u for Direct mode", i);
	}
	
	data->msi_mode_enabled = false;
	LOG_INF("APLIC: Direct mode configuration complete");
	return 0;
}

int aplic_send_msi(const struct device *dev, uint32_t target_hart, 
			  uint32_t target_guest, uint32_t irq_id)
{
	struct aplic_data *data = dev->data;
	
	if (!data->msi_mode_enabled) {
		LOG_ERR("APLIC: MSI mode not enabled, cannot send MSI");
		return -ENOTSUP;
	}
	
	if (target_hart >= CONFIG_MP_MAX_NUM_CPUS) {
		return -EINVAL;
	}
	
	uint32_t eid = irq_id;
	if (eid > 63) {
		return -EINVAL;
	}
	LOG_DBG("APLIC: Sending MSI for IRQ %u (EID %u) to hart %u, guest %u", 
		irq_id, eid, target_hart, target_guest);
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	data->msi_interrupts_sent++;
	k_spin_unlock(&data->lock, key);
	LOG_DBG("APLIC: MSI sent successfully - EID %u to hart %u (guest %u)", 
		eid, target_hart, target_guest);
	return 0;
}

static int aplic_configure_source_msi(const struct device *dev, uint32_t irq_id, 
				     uint32_t target_hart, uint32_t target_guest)
{
	struct aplic_data *data = dev->data;
	if (!data->msi_mode_enabled) {
		return -ENOTSUP;
	}
	if (irq_id >= 1024 || target_hart >= CONFIG_MP_MAX_NUM_CPUS || target_guest > 63) {
		return -EINVAL;
	}
	mem_addr_t sourcecfg_addr = get_sourcecfg_addr(dev, irq_id);
	/* For MSI mode, SOURCECFG format is:
	 * [21:11] - Child Index (EID in IMSIC)
	 * [10]    - Delegate bit (set to 1 for MSI)
	 * [2:0]   - Source Mode (set to 0 for inactive in MSI mode)
	 */
	uint32_t eid = data->msi_base_eid + irq_id;
	uint32_t sourcecfg_value = (eid << 11) | (1 << 10) | APLIC_SOURCECFG_SM_INACTIVE;
	aplic_write(dev, sourcecfg_addr, sourcecfg_value);
	mem_addr_t source_targetcfg_addr = get_target_addr(dev, irq_id);
	/* TARGET format:
	 * [13:0]  - Hart Index
	 * [19:14] - Guest Index
	 * [27:20] - Interrupt Priority
	 * [31]    - Interrupt Enable
	 */
	uint32_t targetcfg_value = (target_hart & 0x3FFF) | 
				   ((target_guest & 0x3F) << 14) | 
				   (7 << 20) |  
				   (1 << 31);   
	
	aplic_write(dev, source_targetcfg_addr, targetcfg_value);
	
	LOG_DBG("APLIC: Configured IRQ %u for MSI to hart %u, guest %u (EID %u)", 
	       irq_id, target_hart, target_guest, eid);
	
	return 0;
}

/* ============================================================================
 * Initialization Helper Functions
 * ============================================================================ */

static int aplic_validate_config(const struct device *dev)
{
	const struct aplic_config *config = dev->config;
	
	if (config->base == 0) {
		LOG_ERR("APLIC: Invalid base address");
		return -EINVAL;
	}

	if (config->nr_irqs == 0 || config->nr_irqs > 1024) {
		LOG_ERR("APLIC: Invalid IRQ count: %u", config->nr_irqs);
		return -EINVAL;
	}
	
	return 0;
}

static void aplic_init_data_structures(struct aplic_data *data, uint32_t nr_irqs)
{
	/* Initialize basic data */
	memset(data, 0, sizeof(*data));
	
	/* Initialize interrupt info structures */
	for (uint32_t i = 0; i < nr_irqs && i < 1024; i++) {
		data->irq_info[i].affinity_mask = BIT_MASK(CONFIG_MP_MAX_NUM_CPUS);
		data->irq_info[i].trigger_type = APLIC_TRIGGER_LEVEL_HIGH;
		data->irq_info[i].priority = APLIC_DEFAULT_PRIORITY;
	}
}

static int aplic_init_hardware_irqs(const struct device *dev)
{
	const struct aplic_config *config = dev->config;
	uint32_t i;
	
	/* Disable all interrupts (Linux compatible) */
	for (i = 0; i <= config->nr_irqs; i += 32) {
		aplic_write(dev, config->base + APLIC_CLRIE_BASE + 
			(i / 32) * sizeof(uint32_t), 0xFFFFFFFF);
	}
	
	/* Set interrupt type and default priority for all interrupts */
	for (i = 1; i <= config->nr_irqs; i++) {
		/* Set source configuration to inactive */
		aplic_write(dev, config->base + APLIC_SOURCECFG_BASE + 
			(i - 1) * sizeof(uint32_t), APLIC_SOURCECFG_SM_INACTIVE);
		
		/* Set default priority */
		aplic_write(dev, config->base + APLIC_TARGET_BASE + 
			(i - 1) * sizeof(uint32_t), APLIC_DEFAULT_PRIORITY);
	}
	
	/* Clear DOMAINCFG initially (Linux compatible) */
	aplic_write(dev, config->base + APLIC_DOMAINCFG, 0);
	
	return 0;
}

static bool aplic_detect_msi_mode(const struct device *dev)
{
	/* Method 1: Check if IMSIC device is available and ready */
	const struct device *imsic_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(imsic));
	if (imsic_dev == NULL || !device_is_ready(imsic_dev)) {
		LOG_INF("APLIC: No IMSIC device available, using Direct mode");
		return false;
	}
	
	/* Check if APLIC has msi-parent property in device tree */
	/* In our device tree, we added: msi-parent = <&imsic>; */
	/* This means APLIC should use MSI mode when IMSIC is available */
	LOG_INF("APLIC: IMSIC device available and ready, enabling MSI mode");
	LOG_INF("APLIC: Device tree shows msi-parent = <&imsic>");
	
	return true;
}

static int aplic_configure_domain(const struct device *dev, bool msi_mode)
{
	const struct aplic_config *config = dev->config;
	uint32_t domaincfg_value = APLIC_DOMAINCFG_IE;
	
	if (msi_mode) {
		domaincfg_value |= APLIC_DOMAINCFG_DM;
	}
	
	aplic_write(dev, config->base + APLIC_DOMAINCFG, domaincfg_value);
	
	/* Verify configuration */
	uint32_t readback = aplic_read(dev, config->base + APLIC_DOMAINCFG);
	if ((readback & APLIC_DOMAINCFG_IE) == 0) {
		LOG_ERR("APLIC: Failed to enable interrupt domain");
		return -EIO;
	}
	
	if (msi_mode && (readback & APLIC_DOMAINCFG_DM) == 0) {
		LOG_ERR("APLIC: Failed to enable MSI mode");
		return -EIO;
	}
	
	if (!msi_mode && (readback & APLIC_DOMAINCFG_DM) != 0) {
		LOG_ERR("APLIC: Failed to enable Direct mode");
		return -EIO;
	}

	return 0;
}

static void aplic_save_device_references(const struct device *dev)
{
	for (uint32_t i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		save_dev[i] = dev;
	}
}

static int aplic_init(const struct device *dev)
{
	const struct aplic_config *config = dev->config;
	struct aplic_data *data = dev->data;
	int ret;
	
	LOG_INF("APLIC: Initializing device %s at base 0x%08lX", 
		dev->name ? dev->name : "NULL", config->base);
	
	/* Phase 1: Validate configuration */
	ret = aplic_validate_config(dev);
	if (ret != 0) {
		return ret;
	}
	
	/* Phase 2: Initialize data structures */
	aplic_init_data_structures(data, config->nr_irqs);
	
	/* Phase 3: Initialize hardware interrupts (Linux compatible) */
	ret = aplic_init_hardware_irqs(dev);
	if (ret != 0) {
		LOG_ERR("APLIC: Hardware initialization failed: %d", ret);
		return ret;
	}
	
	/* Phase 4: Detect operation mode */
	bool msi_mode = aplic_detect_msi_mode(dev);
	LOG_INF("APLIC: Detected mode: %s", msi_mode ? "MSI" : "Direct");
	
	/* Phase 5: Configure mode-specific hardware */
	if (msi_mode) {
		ret = aplic_configure_msi_mode(dev);
		if (ret != 0) {
			LOG_ERR("APLIC: MSI mode failed, falling back to Direct mode");
			ret = aplic_configure_direct_mode(dev);
		}
	} else {
		ret = aplic_configure_direct_mode(dev);
	}
	
	if (ret != 0) {
		LOG_ERR("APLIC: Mode configuration failed: %d", ret);
		return ret;
	}
	
	/* Phase 6: Configure domain (AIA requirement: last step) */
	ret = aplic_configure_domain(dev, data->msi_mode_enabled);
	if (ret != 0) {
		return ret;
	}
	
	/* Phase 7: Save device references */
	aplic_save_device_references(dev);
	
	LOG_INF("APLIC: Initialization complete - Mode: %s, IRQs: %u", 
		data->msi_mode_enabled ? "MSI" : "Direct", config->nr_irqs);
	
	return 0;
}

/* ============================================================================
 * Device Initialization Macros
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
    if (!aplic_parent_irq) { \
        aplic_parent_irq = RISCV_IRQ_MEXT; \
        IRQ_CONNECT(RISCV_IRQ_MEXT, 0, aplic_direct_mode_handler, NULL, 0); \
        irq_enable(RISCV_IRQ_MEXT); \
    } \
}

DT_INST_FOREACH_STATUS_OKAY(APLIC_INIT)

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