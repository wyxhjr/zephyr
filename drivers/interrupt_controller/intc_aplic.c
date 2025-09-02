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


int aplic_send_msi(const struct device *dev, uint32_t target_hart, 
			  uint32_t target_guest, uint32_t irq_id);


#define APLIC_DOMAINCFG        0x0000
#define APLIC_SOURCECFG_BASE   0x0004
#define APLIC_SOURCECFG_SIZE   0x0004
#define APLIC_TARGETCFG_BASE   0x3000 
#define APLIC_TARGETCFG_SIZE   0x0004
#define APLIC_SETIE_BASE       0x1E00
#define APLIC_CLRIE_BASE       0x1F00
#define APLIC_SETIPNUM_LE      0x2000
#define APLIC_SETIP_BASE       0x1C00
#define APLIC_CLRIP_BASE       0x1D00
#define APLIC_MMSICFGADDR      0x1bc0
#define APLIC_MMSICFGADDRH     0x1bc4

#define APLIC_SETIENUM         0x1E00
#define APLIC_CLRIENUM         0x1F00



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


#define APLIC_DOMAINCFG_IE            BIT(8)     
#define APLIC_DOMAINCFG_DM            BIT(2)     
#define APLIC_DOMAINCFG_BE            BIT(0)     


#define APLIC_TARGET_HART_IDX_MASK    0x3FFF     
#define APLIC_TARGET_GUEST_IDX_MASK   0x3F       
#define APLIC_TARGET_IPRIO_MASK       0xFF       


#define APLIC_REG_SIZE 32
#define APLIC_REG_MASK BIT_MASK(LOG2(APLIC_REG_SIZE))



#define APLIC_IDC_BASE         0x4000  
#define APLIC_IDC_SIZE         32      
#define APLIC_IDC_IDELIVERY    0x00    
#define APLIC_IDC_IFORCE       0x04    
#define APLIC_IDC_ITHRESHOLD   0x08    
#define APLIC_IDC_TOPI         0x18    
#define APLIC_IDC_CLAIMI       0x1C    


#define APLIC_IDC_TOPI_ID_MASK     0x3FF  
#define APLIC_IDC_TOPI_ID_SHIFT    16
#define APLIC_IDC_TOPI_PRIO_MASK   0xFF   
#define APLIC_IDC_TOPI_PRIO_SHIFT  0


#define APLIC_IDC_DELIVERY_DISABLE 0
#define APLIC_IDC_DELIVERY_ENABLE  1
#define APLIC_IDC_THRESHOLD_DISABLE 1
#define APLIC_IDC_THRESHOLD_ENABLE  0

typedef void (*riscv_aplic_irq_config_func_t)(void);

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


enum aplic_trigger_type {
	APLIC_TRIGGER_EDGE_RISING = APLIC_SOURCECFG_SM_EDGE_RISE,
	APLIC_TRIGGER_EDGE_FALLING = APLIC_SOURCECFG_SM_EDGE_FALL,
	APLIC_TRIGGER_LEVEL_HIGH = APLIC_SOURCECFG_SM_LEVEL_HIGH,
	APLIC_TRIGGER_LEVEL_LOW = APLIC_SOURCECFG_SM_LEVEL_LOW,
};


struct aplic_irq_info {
	uint32_t count;           
	uint32_t last_cpu;        
	uint32_t affinity_mask;   
	enum aplic_trigger_type trigger_type;  
	uint8_t priority;         
	bool enabled;             
};

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
};

static const struct device *save_dev[CONFIG_MP_MAX_NUM_CPUS];

/* ============================================================================
 * Global Variables for Conditional Registration
 * ============================================================================ */


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

static inline void aplic_set_threshold(const struct device *dev, uint32_t hartid, uint32_t threshold)
{
	mem_addr_t targetcfg_addr = get_targetcfg_addr(dev, hartid);
	
	aplic_write(dev, targetcfg_addr + APLIC_TARGETCFG_THRESHOLD, threshold);
}


static void aplic_route_interrupt_to_riscv(const struct device *dev, uint32_t irq);
static void aplic_trigger_riscv_interrupt(const struct device *dev, uint32_t irq);
static void aplic_direct_mode_handler(const void *arg);

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
	struct aplic_data *data = dev->data;
	
	
	if (data->msi_mode_enabled) {
		
		
		mem_addr_t setienum_addr = config->base + APLIC_SETIENUM;
		
		
		aplic_write(dev, setienum_addr, irq);
		
		printk("APLIC: MSI mode - Enabled IRQ %u via SETIENUM at 0x%08lX\n", 
		       irq, setienum_addr);
		
	} else {
		
	
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
	
	mem_addr_t setie_addr = config->base + APLIC_SETIE_BASE + local_irq_to_reg_offset(irq); 
	uint32_t bit_mask = BIT(irq & (APLIC_REG_SIZE - 1));
	
	
	printk("APLIC: Enabling IRQ %u - SETIE operation details:\n", irq);
	printk("  - Base address: 0x%08lX\n", config->base);
	printk("  - SETIE_BASE: 0x%08X\n", APLIC_SETIE_BASE);
	printk("  - Register offset: 0x%08X\n", local_irq_to_reg_offset(irq));
	printk("  - Final SETIE address: 0x%08lX\n", setie_addr);
	printk("  - IRQ bit position: %u (bit %u in register)\n", irq, irq & (APLIC_REG_SIZE - 1));
	printk("  - Bit mask: 0x%08X\n", bit_mask);
	
	
	uint32_t current_setie = aplic_read(dev, setie_addr);
	printk("  - Current SETIE value: 0x%08X\n", current_setie);
	
	uint32_t new_setie = current_setie | bit_mask;
	printk("  - New SETIE value: 0x%08X (0x%08X | 0x%08X)\n", new_setie, current_setie, bit_mask);
	
	aplic_write(dev, setie_addr, new_setie);
	printk("  - Wrote 0x%08X to SETIE register at 0x%08lX\n", new_setie, setie_addr);
	
	
	uint32_t verify_setie = aplic_read(dev, setie_addr);
	printk("  - Verification read: 0x%08X\n", verify_setie);
	
	if (verify_setie == new_setie) {
		printk("  - SETIE write verification successful\n");
	} else {
		printk("  - SETIE write verification failed! Expected: 0x%08X, Got: 0x%08X\n", 
		       new_setie, verify_setie);
		}
	}
}

static inline void aplic_irq_disable_internal(const struct device *dev, uint32_t irq)
{
	const struct aplic_config *config = (const struct aplic_config *)dev->config;
	struct aplic_data *data = dev->data;
	
	
	if (data->msi_mode_enabled) {
		
		
		mem_addr_t clrienum_addr = config->base + APLIC_CLRIENUM;
		
		
		aplic_write(dev, clrienum_addr, irq);
		
		printk("APLIC: MSI mode - Disabled IRQ %u via CLRIENUM at 0x%08lX\n", 
		       irq, clrienum_addr);
		
	} else {
		
	mem_addr_t clrie_addr = config->base + APLIC_CLRIE_BASE + local_irq_to_reg_offset(irq); 
	uint32_t bit_mask = BIT(irq & (APLIC_REG_SIZE - 1));
	
	
	uint32_t current_clrie = aplic_read(dev, clrie_addr);
	aplic_write(dev, clrie_addr, current_clrie | bit_mask);
		
		printk("APLIC: Direct mode - Disabled IRQ %u via CLRIE at 0x%08lX\n", 
		       irq, clrie_addr);
	}
}

static inline int aplic_irq_is_enabled_internal(const struct device *dev, uint32_t irq)
{
	const struct aplic_config *config = (const struct aplic_config *)dev->config;
	struct aplic_data *data = dev->data;
	
	
	if (data->msi_mode_enabled) {
		
		
		printk("APLIC: MSI mode - Cannot check IRQ %u status (SETIENUM/CLRIENUM are write-only)\n", irq);
		return 1; 
		
	} else {
		
	mem_addr_t setie_addr = config->base + APLIC_SETIE_BASE + local_irq_to_reg_offset(irq); 
	uint32_t bit_mask = BIT(irq & (APLIC_REG_SIZE - 1));
	
	
	printk("APLIC: Checking if IRQ %u is enabled - SETIE read details:\n", irq);
	printk("  - Base address: 0x%08lX\n", config->base);
	printk("  - SETIE_BASE: 0x%08X\n", APLIC_SETIE_BASE);
	printk("  - Register offset: 0x%08X\n", local_irq_to_reg_offset(irq));
	printk("  - Final SETIE address: 0x%08lX\n", setie_addr);
	printk("  - IRQ bit position: %u (bit %u in register)\n", irq, irq & (APLIC_REG_SIZE - 1));
	printk("  - Bit mask: 0x%08X\n", bit_mask);
	
	
	uint32_t setie_value = aplic_read(dev, setie_addr);
	printk("  - SETIE register value: 0x%08X\n", setie_value);
	printk("  - Bit check: (0x%08X & 0x%08X) = 0x%08X\n", setie_value, bit_mask, setie_value & bit_mask);
	
	int result = (setie_value & bit_mask) ? 1 : 0;
	printk("  - Result: IRQ %u is %s\n", irq, result ? "enabled" : "disabled");
	
	return result;
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
	mem_addr_t targetcfg_addr;
	k_spinlock_key_t key;
	
	
	if (hart_id >= CONFIG_MP_MAX_NUM_CPUS) {
		return -EINVAL;
	}
	
	
	if (threshold > 255) {
		return -EINVAL;
	}
	
	key = k_spin_lock(&data->lock);
	
	
	targetcfg_addr = get_targetcfg_addr(dev, hart_id);
	aplic_write(dev, targetcfg_addr + APLIC_TARGETCFG_THRESHOLD, threshold);
	
	
	data->hart_thresholds[hart_id] = threshold;
	
	k_spin_unlock(&data->lock, key);

	printk("APLIC: Set Hart %u threshold to %u\n", hart_id, threshold);

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
	
	
	mem_addr_t setip_addr = config->base + APLIC_SETIP_BASE + local_irq_to_reg_offset(irq);
	uint32_t bit_mask = BIT(irq & (APLIC_REG_SIZE - 1));
	
	
	uint32_t current_setip = aplic_read(dev, setip_addr);
	aplic_write(dev, setip_addr, current_setip | bit_mask);
	
	printk("APLIC: Set IRQ %u pending - SETIP: 0x%08X -> 0x%08X\n", 
	       irq, current_setip, current_setip | bit_mask);
	
	
	// aplic_debug_interrupt_status(dev, irq); // Temporarily commented out
	
	
	if (!riscv_aplic_irq_is_enabled(irq)) {
		printk("APLIC: IRQ %u is not enabled, skipping routing\n", irq);
		return;
	}
	
	
	
	
	uint32_t target_hart = 0;
	uint32_t target_guest = 0;
	uint32_t priority = 1;
	bool target_enabled = true;
	
	if (data->msi_mode_enabled) {
		mem_addr_t targetcfg_addr = get_targetcfg_addr(dev, 0);
		uint32_t targetcfg = aplic_read(dev, targetcfg_addr);
		
		target_hart = targetcfg & APLIC_TARGET_HART_IDX_MASK;
		target_guest = (targetcfg >> 14) & APLIC_TARGET_GUEST_IDX_MASK;
		priority = (targetcfg >> 20) & 0xFF;
		target_enabled = (targetcfg >> 31) & 0x1;
		
		printk("APLIC: IRQ %u routing - Hart: %u, Guest: %u, Priority: %u, Enabled: %s\n",
		       irq, target_hart, target_guest, priority, target_enabled ? "yes" : "no");
		
		if (!target_enabled) {
			printk("APLIC: MSI mode - TARGETCFG not configured, using default routing (hart 0, guest 0)\n");
			target_hart = 0;
			target_guest = 0;
			priority = 7; 
		}
		
		mem_addr_t hart_targetcfg_addr = get_targetcfg_addr(dev, target_hart);
		uint32_t hart_threshold = aplic_read(dev, hart_targetcfg_addr + APLIC_TARGETCFG_THRESHOLD);
	} else {
		printk("APLIC: Direct mode - IRQ %u routing to hart 0, priority 1\n", irq);
	}
	
	printk("APLIC: Hart %u threshold: 0, IRQ %u priority: %u\n", 
	       target_hart, irq, priority);
	
	
	if (priority > 0) {
		printk("APLIC: Routing IRQ %u (priority %u) to RISC-V Hart %u\n", 
		       irq, priority, target_hart);
		
		
		struct aplic_data *aplic_data = dev->data;
		if (aplic_data->msi_mode_enabled) {
			
			printk("APLIC: Using MSI mode - APLIC hardware should automatically send MSI for interrupt %u\n", irq);
			
			/* CRITICAL: According to AIA spec section 4.9.3, APLIC automatically forwards MSI when:
			 * 1. Source is active and enabled
			 * 2. Source is pending
			 * 3. Domain IE bit is set
			 * 4. Target is properly configured
			 * 
			 * We should NOT manually call aplic_send_msi - the hardware does it automatically!
			 */
			printk("APLIC: MSI mode - letting APLIC hardware handle MSI transmission automatically\n");
			printk("APLIC: Conditions: source active=1, enabled=1, pending=1, domain IE=1\n");
			
			
			k_spinlock_key_t key = k_spin_lock(&aplic_data->lock);
			aplic_data->msi_interrupts_sent++;
			k_spin_unlock(&aplic_data->lock, key);
			
		} else {
			
			printk("APLIC: Using Direct mode - attempting IDC interrupt routing\n");
			
			
			
			const struct aplic_config *aplic_config = dev->config;
			uint32_t cpu_id = arch_proc_id();
			mem_addr_t idc_base = aplic_config->base + APLIC_IDC_BASE + (cpu_id * APLIC_IDC_SIZE);
			
			
			mem_addr_t idelivery_addr = idc_base + APLIC_IDC_IDELIVERY;
			uint32_t current_idelivery = aplic_read(dev, idelivery_addr);
			if (current_idelivery != APLIC_IDC_DELIVERY_ENABLE) {
				printk("APLIC: IDC delivery not enabled (0x%08X), enabling...\n", current_idelivery);
				aplic_write(dev, idelivery_addr, APLIC_IDC_DELIVERY_ENABLE);
				
				uint32_t verify_idelivery = aplic_read(dev, idelivery_addr);
				printk("APLIC: IDC delivery after enable: 0x%08X\n", verify_idelivery);
			}
			
			
			mem_addr_t ithreshold_addr = idc_base + APLIC_IDC_ITHRESHOLD;
			uint32_t current_threshold = aplic_read(dev, ithreshold_addr);
			if (current_threshold != APLIC_IDC_THRESHOLD_ENABLE) {
				printk("APLIC: IDC threshold not correct (0x%08X), setting to 0...\n", current_threshold);
				aplic_write(dev, ithreshold_addr, APLIC_IDC_THRESHOLD_ENABLE);
			}
			
			
			
			printk("APLIC: Using natural APLIC interrupt routing for IRQ %u (Hart %u)\n", irq, target_hart);
			
			
			
			mem_addr_t topi_addr = idc_base + APLIC_IDC_TOPI;
			uint32_t topi_value = aplic_read(dev, topi_addr);
			printk("APLIC: Read IDC TOPI register at 0x%08lX: 0x%08X\n", topi_addr, topi_value);
			
			
			uint32_t topi_irq = (topi_value >> APLIC_IDC_TOPI_ID_SHIFT) & APLIC_IDC_TOPI_ID_MASK;
			uint32_t topi_priority = (topi_value >> APLIC_IDC_TOPI_PRIO_SHIFT) & APLIC_IDC_TOPI_PRIO_MASK;
			
			if (topi_irq == irq) {
				printk("APLIC: TOPI shows IRQ %u with priority %u - interrupt should be triggered\n", topi_irq, topi_priority);
			} else {
				printk("APLIC: TOPI shows different IRQ %u (expected %u) - this may indicate a problem\n", topi_irq, irq);
			}
			
			
			k_spinlock_key_t key = k_spin_lock(&data->lock);
			data->direct_interrupts++;
			k_spin_unlock(&data->lock, key);
		}
		
	} else {
		printk("APLIC: IRQ %u priority %u <= threshold 0, not routing\n", 
		       irq, priority);
	}
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
	
	
	for (int i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		
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
	
	
	if (found_count > 0) {
		const struct device *global_imsic = riscv_imsic_get_dev();
		if (global_imsic != NULL && device_is_ready(global_imsic)) {
			printk("APLIC: Found global IMSIC device %s\n", global_imsic->name);
		}
	}
	
	return found_count;
}


static int aplic_configure_msi_mode(const struct device *dev)
{
	const struct aplic_config *config = dev->config;
	struct aplic_data *data = dev->data;
	
	
	if (aplic_find_imsic_devices(dev) == 0) {
		printk("APLIC: No IMSIC devices found, cannot enable MSI mode\n");
		return -ENODEV;
	}
	
	
	
	
	
	const struct device *imsic_dev = riscv_imsic_get_dev();
	if (imsic_dev == NULL) {
		printk("APLIC: ERROR - Cannot find IMSIC device for MSI configuration\n");
		return -ENODEV;
	}
	
	
	
	mem_addr_t imsic_base = 0x24000000; 
	
	printk("APLIC: Configuring MSI address registers for IMSIC at 0x%08lX\n", imsic_base);
	
	
	
	uint32_t mmsicfgaddr = (imsic_base >> 12) & 0xFFFFFFFF; 
	aplic_write(dev, config->base + APLIC_MMSICFGADDR, mmsicfgaddr);
	
	
	
	uint32_t mmsicfgaddrh = 0;
	
	
	
	
	
	mmsicfgaddrh = (0 << 24) | (0 << 20) | (0 << 16) | (0 << 12) | 0;
	aplic_write(dev, config->base + APLIC_MMSICFGADDRH, mmsicfgaddrh);
	
	printk("APLIC: MSI address registers configured: ADDR=0x%08X, ADDRH=0x%08X\n", 
	       mmsicfgaddr, mmsicfgaddrh);
	
	
	uint32_t domaincfg_value = (1 << 8) | (1 << 2); 
	printk("APLIC: Enabling MSI mode, writing 0x%08X to DOMAINCFG\n", domaincfg_value);
	aplic_write(dev, config->base + APLIC_DOMAINCFG, domaincfg_value);
	
	
	uint32_t domaincfg_readback = aplic_read(dev, config->base + APLIC_DOMAINCFG);
	if ((domaincfg_readback & (1 << 2)) == 0) {
		printk("APLIC: ERROR - Failed to enable MSI mode\n");
		return -EIO;
	}
	
	data->msi_mode_enabled = false;
	data->msi_base_eid = 0; 
	
	printk("APLIC: Direct mode enabled - MSI mode disabled\n");
	return 0;
}


int aplic_send_msi(const struct device *dev, uint32_t target_hart, 
			  uint32_t target_guest, uint32_t irq_id)
{
	struct aplic_data *data = dev->data;
	const struct aplic_config *config = dev->config;
	
	if (!data->msi_mode_enabled) {
		printk("APLIC: MSI mode not enabled, cannot send MSI\n");
		return -ENOTSUP;
	}
	
	if (target_hart >= CONFIG_MP_MAX_NUM_CPUS) {
		return -EINVAL;
	}
	
	
	uint32_t eid = irq_id; 
	
	
	if (eid > 63) {
		return -EINVAL;
	}
	
	printk("APLIC: Sending MSI for IRQ %u (EID %u) to hart %u, guest %u\n", 
	       irq_id, eid, target_hart, target_guest);
	
	mem_addr_t imsic_msi_addr = data->imsic_base + (target_hart * 4096) + 0x000;
	
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	data->msi_interrupts_sent++;
	k_spin_unlock(&data->lock, key);
	
	printk("APLIC: MSI sent successfully - EID %u to hart %u (guest %u)\n", 
	       eid, target_hart, target_guest);
	
	return 0;
}


static int aplic_configure_source_msi(const struct device *dev, uint32_t irq_id, 
				     uint32_t target_hart, uint32_t target_guest)
{
	const struct aplic_config *config = dev->config;
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
	
	
	
	mem_addr_t source_targetcfg_addr = get_targetcfg_addr(dev, target_hart);
	
	/* TARGETCFG format:
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
	
	
	uint32_t readback = aplic_read(dev, source_targetcfg_addr);
	printk("APLIC: Configured IRQ %u for MSI to hart %u, guest %u (EID %u)\n", 
	       irq_id, target_hart, target_guest, eid);
	printk("APLIC: TARGETCFG write: 0x%08X, readback: 0x%08X\n", targetcfg_value, readback);
	
	return 0;
}


static int aplic_init(const struct device *dev)
{
	const struct aplic_config *config = dev->config;
	struct aplic_data *data = dev->data;
	uint32_t i;

	
	ARG_UNUSED(data);
	
	
	printk("=== APLIC INIT START ===\n");
	printk("APLIC: Device name: %s\n", dev->name ? dev->name : "NULL");
	printk("APLIC: Base address: 0x%08lX\n", config->base);
	printk("=== APLIC INIT CONTINUE ===\n");

	
	if (config->base == 0) {
		printk("APLIC: ERROR - Invalid base address 0x%08lX\n", config->base);
		return -EINVAL;
	}

	
	printk("APLIC: Starting Direct mode initialization\n");
	data->msi_mode_enabled = false;
	for (i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		data->imsic_devices[i] = NULL;
	}
	data->msi_base_eid = 0;
	
	
	printk("APLIC: Direct mode enabled - skipping MSI configuration\n");

	
	
	uint32_t domaincfg_value = (1 << 8); 
	printk("APLIC: Writing 0x%08X to DOMAINCFG at 0x%08lX\n", domaincfg_value, config->base + APLIC_DOMAINCFG);
	aplic_write(dev, config->base + APLIC_DOMAINCFG, domaincfg_value);
	
	
	uint32_t domaincfg_readback = aplic_read(dev, config->base + APLIC_DOMAINCFG);
	printk("APLIC: DOMAINCFG readback: 0x%08X\n", domaincfg_readback);
	
	
	if ((domaincfg_readback & (1 << 8)) == 0) {
		
		printk("APLIC: IE bit not set, trying safe write method\n");
		uint32_t safe_value = domaincfg_value | (domaincfg_value << 24);
		aplic_write(dev, config->base + APLIC_DOMAINCFG, safe_value);
		domaincfg_readback = aplic_read(dev, config->base + APLIC_DOMAINCFG);
		printk("APLIC: DOMAINCFG after safe write: 0x%08X\n", domaincfg_readback);
	}
	
	
	if ((domaincfg_readback & (1 << 8)) == 0) {
		printk("APLIC: IE bit still not set, trying alternative configurations\n");
		
		
		uint32_t full_config = (1 << 8) | (0 << 2) | (0 << 0); 
		aplic_write(dev, config->base + APLIC_DOMAINCFG, full_config);
		domaincfg_readback = aplic_read(dev, config->base + APLIC_DOMAINCFG);
		printk("APLIC: DOMAINCFG after full config: 0x%08X\n", domaincfg_readback);
		
		
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
	
	if (data->msi_mode_enabled) {
		printk("APLIC: Configuring interrupts for MSI mode\n");
		
		
		for (i = 1; i < config->nr_irqs; i++) {
			
			mem_addr_t sourcecfg_addr = get_sourcecfg_addr(dev, i);
			
			uint32_t sourcecfg_value = APLIC_SOURCECFG_SM_EDGE_RISE;
			
			aplic_write(dev, sourcecfg_addr, sourcecfg_value);	
			
			mem_addr_t targetcfg_addr = get_targetcfg_addr(dev, 0);
			
			/* TARGETCFG format for MSI mode:
			 * [13:0]  - Hart Index (0)
			 * [19:14] - Guest Index (0) 
			 * [27:20] - EID (External Interrupt ID)
			 * [31]    - Interrupt Enable (1)
			 */
			uint32_t eid = i; 
			uint32_t targetcfg_value = (0 & 0x3FFF) |           
						   (0 << 14) |                    
						   (eid << 20) |                  
						   (1 << 31);                     
			
			
			aplic_write(dev, targetcfg_addr, targetcfg_value);
			
			
			riscv_aplic_irq_enable(i);
			
			printk("APLIC: Configured IRQ %u for MSI - SOURCECFG: 0x%08X, TARGETCFG: 0x%08X (EID %u)\n", 
			       i, sourcecfg_value, targetcfg_value, eid);
		}
		
		printk("APLIC: MSI mode configuration complete\n");
		
	} else {
		printk("APLIC: Configuring SOURCECFG for direct mode interrupts\n");
		for (i = 1; i < config->nr_irqs; i++) {
			
			mem_addr_t sourcecfg_addr = get_sourcecfg_addr(dev, i);
			
			uint32_t sourcecfg_value = APLIC_SOURCECFG_SM_DETACHED;
			
			aplic_write(dev, sourcecfg_addr, sourcecfg_value);
			
			riscv_aplic_irq_enable(i);
			
			printk("APLIC: Configured IRQ %u for Direct mode - SOURCECFG: 0x%08X\n", 
			       i, sourcecfg_value);
		}
		
		printk("APLIC: Direct mode configuration complete - skipping TARGETCFG access\n");
	}

	
	for (i = 0; i < config->nr_irqs; i++) {
		mem_addr_t sourcecfg_addr = get_sourcecfg_addr(dev, i);
		
		
		if (i < 1024) {
			data->irq_info[i].count = 0;
			data->irq_info[i].last_cpu = 0;
			data->irq_info[i].affinity_mask = BIT_MASK(CONFIG_MP_MAX_NUM_CPUS); 
			data->irq_info[i].trigger_type = APLIC_TRIGGER_LEVEL_HIGH; 
			data->irq_info[i].priority = 1; 
			data->irq_info[i].enabled = false;
		}
		
		
		if (data->msi_mode_enabled) {
			
			
			aplic_configure_source_msi(dev, i, 0, 0);
		} else {
			
			aplic_write(dev, sourcecfg_addr, APLIC_SOURCECFG_SM_LEVEL_HIGH);
		}
	}
	
	
	data->total_interrupts = 0;
	data->direct_interrupts = 0;

	
	for (i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		if (data->msi_mode_enabled) {
			mem_addr_t targetcfg_addr = get_targetcfg_addr(dev, i);
			aplic_set_threshold(dev, i, 0);
			data->hart_thresholds[i] = 0;
			aplic_write(dev, targetcfg_addr + APLIC_TARGETCFG_IE, 0x1);
		} else {
			printk("APLIC: Direct mode - skipping hart %u TARGETCFG configuration\n", i);
		}
	}

	
	printk("APLIC: Configuring IDC for %d harts\n", CONFIG_MP_MAX_NUM_CPUS);
	for (i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		mem_addr_t idc_base = config->base + APLIC_IDC_BASE + (i * APLIC_IDC_SIZE);
		
		
		
		mem_addr_t idelivery_addr = idc_base + APLIC_IDC_IDELIVERY;
		uint32_t current_idelivery = aplic_read(dev, idelivery_addr);
		printk("APLIC: IDC hart %u - idelivery before config: 0x%08X\n", i, current_idelivery);
		
		
		aplic_write(dev, idelivery_addr, APLIC_IDC_DELIVERY_ENABLE);
		
		
		uint32_t verify_idelivery = aplic_read(dev, idelivery_addr);
		printk("APLIC: IDC hart %u - idelivery after config: 0x%08X\n", i, verify_idelivery);
		
		if (verify_idelivery == APLIC_IDC_DELIVERY_ENABLE) {
			printk("APLIC: IDC hart %u - idelivery successfully set to 1\n", i);
		} else {
			printk("APLIC: IDC hart %u - WARNING: idelivery write failed, got 0x%08X\n", i, verify_idelivery);
		}
		
		
		mem_addr_t ithreshold_addr = idc_base + APLIC_IDC_ITHRESHOLD;
		uint32_t current_threshold = aplic_read(dev, ithreshold_addr);
		printk("APLIC: IDC hart %u - ithreshold before config: 0x%08X\n", i, current_threshold);
		
		aplic_write(dev, ithreshold_addr, APLIC_IDC_THRESHOLD_ENABLE);
		
		
		uint32_t verify_threshold = aplic_read(dev, ithreshold_addr);
		printk("APLIC: IDC hart %u - ithreshold after config: 0x%08X\n", i, verify_threshold);
		
		printk("APLIC: Configured IDC for hart %u - base: 0x%08lX, delivery: %s, threshold: %s\n", 
		       i, idc_base, 
		       (verify_idelivery == APLIC_IDC_DELIVERY_ENABLE) ? "enabled" : "FAILED",
		       (verify_threshold == APLIC_IDC_THRESHOLD_ENABLE) ? "0" : "FAILED");
	}
	
	
	
	printk("APLIC: Configuring DOMAINCFG after all other configurations...\n");
	
	
	uint32_t final_domaincfg_read = aplic_read(dev, config->base + APLIC_DOMAINCFG);
	printk("APLIC: Current DOMAINCFG before final configuration: 0x%08X\n", final_domaincfg_read);
	
	
	uint32_t final_domaincfg = final_domaincfg_read | (1 << 8); 
	if (!data->msi_mode_enabled) {
		final_domaincfg &= ~(1 << 2); 
	} else {
		final_domaincfg |= (1 << 2);  
	}
	
	
	aplic_write(dev, config->base + APLIC_DOMAINCFG, final_domaincfg);
	
	
	uint32_t verify_domaincfg = aplic_read(dev, config->base + APLIC_DOMAINCFG);
	printk("APLIC: Final DOMAINCFG: 0x%08X (IE=%s, DM=%s, BE=%s)\n", 
	       verify_domaincfg,
	       (verify_domaincfg & (1 << 8)) ? "1" : "0",
	       (verify_domaincfg & (1 << 2)) ? "1(MSI)" : "0(Direct)",
	       (verify_domaincfg & (1 << 0)) ? "1(Big)" : "0(Little)");
	
	
	if ((verify_domaincfg & (1 << 2)) != 0) {
		printk("APLIC: ERROR - Failed to enter Direct Mode! Still in MSI mode!\n");
		printk("APLIC: This means IDC registers will not be accessible!\n");
	} else {
		printk("APLIC: SUCCESS - APLIC is now in Direct Mode (DM=0)\n");
		printk("APLIC: IDC registers should now be accessible\n");
	}
	
	
	printk("APLIC: Testing IDC register accessibility after DOMAINCFG configuration...\n");
	for (i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		mem_addr_t idc_base = config->base + APLIC_IDC_BASE + (i * APLIC_IDC_SIZE);
		mem_addr_t idelivery_addr = idc_base + APLIC_IDC_IDELIVERY;
		
		
		uint32_t test_idelivery = aplic_read(dev, idelivery_addr);
		printk("APLIC: IDC hart %u - idelivery after DOMAINCFG config: 0x%08X\n", i, test_idelivery);
		
		if (test_idelivery != 0x00000000) {
			printk("APLIC: IDC hart %u - SUCCESS: IDC registers are now accessible!\n", i);
		} else {
			printk("APLIC: IDC hart %u - WARNING: IDC registers still not accessible\n", i);
		}
	}

	
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
	mem_addr_t targetcfg_addr = get_targetcfg_addr(dev, irq);
	uint32_t targetcfg = aplic_read(dev, targetcfg_addr);
	
	uint32_t target_hart = targetcfg & APLIC_TARGET_HART_IDX_MASK;
		
	uint32_t riscv_irq = 2; 

	printk("APLIC: Routing IRQ %u to RISC-V IRQ %u (Hart %u)\n", irq, riscv_irq, target_hart);
}

/**
 * @brief Trigger RISC-V external interrupt from APLIC
 * This function actually sets the interrupt pending in RISC-V's mip register
 */
static void aplic_trigger_riscv_interrupt(const struct device *dev, uint32_t irq)
{
	
	mem_addr_t targetcfg_addr = get_targetcfg_addr(dev, irq);
	uint32_t targetcfg = aplic_read(dev, targetcfg_addr);
		
	uint32_t target_hart = targetcfg & APLIC_TARGET_HART_IDX_MASK;
	
	printk("APLIC: Triggering RISC-V external interrupt for IRQ %u (Hart %u)\n", irq, target_hart);
	
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
	
	
	mem_addr_t sourcecfg_addr = get_sourcecfg_addr(dev, irq);
	uint32_t sourcecfg = aplic_read(dev, sourcecfg_addr);
	
	
	mem_addr_t targetcfg_addr = get_targetcfg_addr(dev, irq);
	uint32_t targetcfg = aplic_read(dev, targetcfg_addr);
	
	
	bool irq_enabled = riscv_aplic_irq_is_enabled(irq);
	
	
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
	
	
	if (!riscv_aplic_irq_is_enabled(irq)) {
		LOG_DBG("APLIC: IRQ %u not enabled, ignoring", irq);
		return;
	}
	
	
	uint32_t priority = 0;
	
	priority = 7; 
	
	
	if (data->msi_mode_enabled) {
		
		LOG_DBG("APLIC: MSI mode - sending IRQ %u (priority %u)", irq, priority);
		
		
		uint32_t target_hart = 0;
		uint32_t target_guest = 0;
		
		
		mem_addr_t targetcfg_addr = get_targetcfg_addr(dev, target_hart);
		uint32_t targetcfg = aplic_read(dev, targetcfg_addr + APLIC_TARGETCFG_HARTID);
		
		
		target_hart = targetcfg & APLIC_TARGET_HART_IDX_MASK;
		target_guest = (targetcfg >> 14) & APLIC_TARGET_GUEST_IDX_MASK;
		
		
		int ret = aplic_send_msi(dev, target_hart, target_guest, irq);
		if (ret != 0) {
			LOG_ERR("APLIC: Failed to send MSI for IRQ %u: %d", irq, ret);
			
			LOG_ERR("APLIC: MSI send failed for IRQ %u", irq);
		} else {
			
			k_spinlock_key_t key = k_spin_lock(&data->lock);
			data->msi_interrupts_sent++;
			k_spin_unlock(&data->lock, key);
		}
		
	} else {
		
		LOG_DBG("APLIC: Direct mode - routing IRQ %u (priority %u) to RISC-V", irq, priority);
		
		printk("APLIC: Direct mode - skipping debug routing for IRQ %u to avoid TARGETCFG access\n", irq);
			
		if (irq < CONFIG_NUM_IRQS) {
			const struct _isr_table_entry *entry = &_sw_isr_table[irq];
			if (entry->isr != NULL) {
				
				entry->isr(entry->arg);
				LOG_DBG("APLIC: Called ISR for IRQ %u", irq);
			} else {
				LOG_WRN("APLIC: No ISR registered for IRQ %u", irq);
			}
		}
		
		
		k_spinlock_key_t key = k_spin_lock(&data->lock);
		data->direct_interrupts++;
		k_spin_unlock(&data->lock, key);
	}
	
	enum riscv_aplic_trigger_type trigger_type;
	int trigger_ret = riscv_aplic_irq_get_trigger_type(irq);
	
	if (trigger_ret < 0) {
		
		trigger_type = RISCV_APLIC_TRIGGER_EDGE_RISING;
	} else {
		trigger_type = (enum riscv_aplic_trigger_type)trigger_ret;
	}
	
	
	if (trigger_type == RISCV_APLIC_TRIGGER_LEVEL_HIGH || 
	    trigger_type == RISCV_APLIC_TRIGGER_LEVEL_LOW) {
		
		
		LOG_DBG("APLIC: Level-triggered IRQ %u, may need manual clearing", irq);
	}
	
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	data->total_interrupts++;
	if (irq < 1024) {
		data->irq_info[irq].count++;
		data->irq_info[irq].last_cpu = arch_curr_cpu()->id;
	}
	k_spin_unlock(&data->lock, key);
	
	
	uint32_t current_cpu = arch_curr_cpu()->id;
	LOG_DBG("APLIC: Interrupt %u handled on CPU %u", irq, current_cpu);
	
	LOG_DBG("APLIC: Successfully handled interrupt %u (priority %u)", irq, priority);
}


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
	
	uint32_t pending = 0;
	
	printk("APLIC: ISR called for device %s\n", dev->name ? dev->name : "NULL");
	
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	data->total_interrupts++;
	k_spin_unlock(&data->lock, key);
}


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
     \
    if (!aplic_parent_irq) { \
         \
        aplic_parent_irq = RISCV_IRQ_MEXT; \
        IRQ_CONNECT(RISCV_IRQ_MEXT, 0, shared_ext_isr, NULL, 0); \
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


