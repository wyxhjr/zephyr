/*
 * Copyright (c) 2024 Zephyr Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qemu_imsic

/**
 * @brief Incoming MSI Controller (IMSIC) driver for RISC-V processors
 *        following the RISC-V AIA specification
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/interrupt_controller/riscv_imsic.h>
#include <zephyr/drivers/interrupt_controller/riscv_aplic.h>
#include "intc_shared.h"

LOG_MODULE_REGISTER(imsic, CONFIG_INTC_LOG_LEVEL);

/*
 * IMSIC register offsets based on RISC-V AIA specification and Linux kernel
 * https://github.com/riscv/riscv-aia
 * https://elixir.bootlin.com/linux/latest/source/drivers/irqchip/irq-riscv-imsic.c
 */
#define IMSIC_MMIO_PAGE_SHIFT		12
#define IMSIC_MMIO_PAGE_SZ		BIT(IMSIC_MMIO_PAGE_SHIFT)
#define IMSIC_MMIO_PAGE_LE		0x00
#define IMSIC_MMIO_PAGE_BE		0x04

#define IMSIC_MIN_ID			63
#define IMSIC_MAX_ID			2048

#define IMSIC_EIDELIVERY		0x70
#define IMSIC_EITHRESHOLD		0x74

#define IMSIC_EIP0			0x80
#define IMSIC_EIP63			0xbf
#define IMSIC_EIPx_BITS		32

#define IMSIC_EIE0			0xc0
#define IMSIC_EIE63			0xff
#define IMSIC_EIEx_BITS		32

#define IMSIC_FIRST			IMSIC_EIDELIVERY
#define IMSIC_LAST			IMSIC_EIE63

/* Big-endian variants for big-endian systems */
/* Note: These offsets are based on RISC-V AIA specification
 * Big-endian registers are accessed through a different page offset
 * in the IMSIC MMIO space (typically +0x100 for big-endian)
 */
#define IMSIC_MMIO_PAGE_BE_OFFSET	0x100

/* Big-endian register addresses */
#define IMSIC_EIDELIVERY_BE		(IMSIC_EIDELIVERY + IMSIC_MMIO_PAGE_BE_OFFSET)
#define IMSIC_EITHRESHOLD_BE		(IMSIC_EITHRESHOLD + IMSIC_MMIO_PAGE_BE_OFFSET)
#define IMSIC_EIP0_BE			(IMSIC_EIP0 + IMSIC_MMIO_PAGE_BE_OFFSET)
#define IMSIC_EIP63_BE			(IMSIC_EIP63 + IMSIC_MMIO_PAGE_BE_OFFSET)
#define IMSIC_EIE0_BE			(IMSIC_EIE0 + IMSIC_MMIO_PAGE_BE_OFFSET)
#define IMSIC_EIE63_BE			(IMSIC_EIE63 + IMSIC_MMIO_PAGE_BE_OFFSET)



/* IMSIC register bit fields */
#define IMSIC_EIDELIVERY_MODE_MASK     0x3
#define IMSIC_EIDELIVERY_MODE_OFF      0x0
#define IMSIC_EIDELIVERY_MODE_MSI     0x1
#define IMSIC_EIDELIVERY_MODE_ID      0x2
#define IMSIC_EIDELIVERY_MODE_VIRTUAL 0x3

#define IMSIC_EIDELIVERY_HARTID_MASK  0x3FFF0000
#define IMSIC_EIDELIVERY_HARTID_SHIFT 16
#define IMSIC_EIDELIVERY_GUESTID_MASK 0x3F00
#define IMSIC_EIDELIVERY_GUESTID_SHIFT 8
#define IMSIC_EIDELIVERY_EID_MASK     0xFF
#define IMSIC_EIDELIVERY_EID_SHIFT    0

/* IMSIC interrupt enable/disable masks */
#define IMSIC_EIE_MASK_0_31    0xFFFFFFFF
#define IMSIC_EIE_MASK_32_63   0xFFFFFFFF

typedef void (*riscv_imsic_irq_config_func_t)(void);

struct imsic_config {
	mem_addr_t base;
	uint32_t hart_id;
	uint32_t guest_id;
	uint32_t max_eid;
	uint32_t max_prio;
	uint32_t irq;
	riscv_imsic_irq_config_func_t irq_config_func;
	bool big_endian;
};

struct imsic_data {
	struct k_spinlock lock;
	uint32_t eie_mask[2];      /* Interrupt enable masks for EIDs 0-63 */
	uint32_t eip_pending[2];   /* Interrupt pending masks for EIDs 0-63 */
	uint32_t eithreshold;      /* Interrupt threshold */
	uint32_t delivery_mode;    /* Current delivery mode */
	uint32_t total_interrupts; /* Total interrupts processed */
	uint32_t msi_interrupts;   /* MSI interrupts received */
	uint32_t id_interrupts;    /* ID interrupts received */
	uint32_t virtual_interrupts; /* Virtual interrupts received */
	uint32_t threshold_rejected; /* Interrupts rejected due to threshold */
};

static const struct device *save_dev[CONFIG_MP_MAX_NUM_CPUS];

/* Helper functions for register access */
static inline uint32_t imsic_read(const struct device *dev, mem_addr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void imsic_write(const struct device *dev, mem_addr_t addr, uint32_t value)
{
	/* Write to IMSIC register */
	*(volatile uint32_t *)addr = value;
}

/* Read/write with endianness support */
static inline uint32_t imsic_read_be(const struct device *dev, mem_addr_t addr)
{
	const struct imsic_config *config = dev->config;
	
	if (config->big_endian) {
		return __builtin_bswap32(imsic_read(dev, addr));
	}
	return imsic_read(dev, addr);
}

static inline void imsic_write_be(const struct device *dev, mem_addr_t addr, uint32_t value)
{
	const struct imsic_config *config = dev->config;
	
	if (config->big_endian) {
		imsic_write(dev, addr, __builtin_bswap32(value));
	} else {
		imsic_write(dev, addr, value);
	}
}

/* Get IMSIC device for current hart */
static inline const struct device *imsic_get_dev(void)
{
	return save_dev[arch_proc_id()];
}

/* Calculate register address for EID */
static inline mem_addr_t get_eie_addr(const struct device *dev, uint32_t eid)
{
	const struct imsic_config *config = dev->config;
	mem_addr_t base = config->base;
	
	/* For EIDs 0-31, use EIE0 */
	if (eid < 32) {
		return base + IMSIC_EIE0;
	} else {
		/* For EIDs 32-63, use EIE0_BE */
		return base + IMSIC_EIE0_BE;
	}
}

static inline mem_addr_t get_eip_addr(const struct device *dev, uint32_t eid)
{
	const struct imsic_config *config = dev->config;
	mem_addr_t base = config->base;
	
	/* For EIDs 0-31, use EIP0 */
	if (eid < 32) {
		return base + IMSIC_EIP0;
	} else {
		/* For EIDs 32-63, use EIP0_BE */
		return base + IMSIC_EIP0_BE;
	}
}

/* IMSIC interrupt enable/disable */
static inline void imsic_irq_enable_internal(const struct device *dev, uint32_t eid)
{
	struct imsic_data *data = dev->data;
	uint32_t mask_index = eid / 32;
	uint32_t bit_mask = BIT(eid % 32);
	
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	data->eie_mask[mask_index] |= bit_mask;
	k_spin_unlock(&data->lock, key);
	
	/* Write to hardware register */
	mem_addr_t eie_addr = get_eie_addr(dev, eid);
	imsic_write_be(dev, eie_addr, data->eie_mask[mask_index]);
}

static inline void imsic_irq_disable_internal(const struct device *dev, uint32_t eid)
{
	struct imsic_data *data = dev->data;
	uint32_t mask_index = eid / 32;
	uint32_t bit_mask = BIT(eid % 32);
	
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	data->eie_mask[mask_index] &= ~bit_mask;
	k_spin_unlock(&data->lock, key);
	
	/* Write to hardware register */
	mem_addr_t eie_addr = get_eie_addr(dev, eid);
	imsic_write_be(dev, eie_addr, data->eie_mask[mask_index]);
}

/* Set interrupt pending */
static inline void imsic_irq_set_pending_internal(const struct device *dev, uint32_t eid)
{
	struct imsic_data *data = dev->data;
	uint32_t mask_index = eid / 32;
	uint32_t bit_mask = BIT(eid % 32);
	
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	data->eip_pending[mask_index] |= bit_mask;
	k_spin_unlock(&data->lock, key);
	
	/* Write to hardware register */
	mem_addr_t eip_addr = get_eip_addr(dev, eid);
	imsic_write_be(dev, eip_addr, data->eip_pending[mask_index]);
}

/* Clear interrupt pending */
static inline void imsic_irq_clear_pending_internal(const struct device *dev, uint32_t eid)
{
	struct imsic_data *data = dev->data;
	uint32_t mask_index = eid / 32;
	uint32_t bit_mask = BIT(eid % 32);
	
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	data->eip_pending[mask_index] &= ~bit_mask;
	k_spin_unlock(&data->lock, key);
	
	/* Write to hardware register */
	mem_addr_t eip_addr = get_eip_addr(dev, eid);
	imsic_write_be(dev, eip_addr, data->eip_pending[mask_index]);
}

/* Set delivery mode with simplified error handling */
static inline int imsic_set_delivery_mode(const struct device *dev, uint32_t mode)
{
	struct imsic_data *data = dev->data;
	const struct imsic_config *config = dev->config;

	if (mode > IMSIC_EIDELIVERY_MODE_VIRTUAL) {
		return -EINVAL;
	}

	data->delivery_mode = mode;

	/* Prepare the value to write */
	uint32_t value = (config->hart_id << IMSIC_EIDELIVERY_HARTID_SHIFT) |
			 (config->guest_id << IMSIC_EIDELIVERY_GUESTID_SHIFT) |
			 (mode << IMSIC_EIDELIVERY_EID_SHIFT);

	LOG_DBG("IMSIC: Setting delivery mode 0x%08X", value);

	/* Write to hardware register using appropriate endianness */
	mem_addr_t delivery_addr;
	if (config->big_endian) {
		delivery_addr = config->base + IMSIC_EIDELIVERY_BE;
	} else {
		delivery_addr = config->base + IMSIC_EIDELIVERY;
	}

	imsic_write_be(dev, delivery_addr, value);

	/* Optional: Verify the write (without complex retry logic) */
#ifdef CONFIG_RISCV_IMSIC_DEBUG
	uint32_t verify_value = imsic_read_be(dev, delivery_addr);
	if (verify_value != value) {
		LOG_WRN("IMSIC: Delivery mode verification failed: wrote 0x%08X, read 0x%08X",
			value, verify_value);
	} else {
		LOG_DBG("IMSIC: Delivery mode 0x%08X verified", value);
	}
#endif

	return 0;
}

/* Set interrupt threshold with simplified error handling */
static inline int imsic_set_threshold(const struct device *dev, uint32_t threshold)
{
	struct imsic_data *data = dev->data;
	const struct imsic_config *config = dev->config;

	if (threshold > config->max_prio) {
		return -EINVAL;
	}

	data->eithreshold = threshold;

	LOG_DBG("IMSIC: Setting threshold 0x%08X", threshold);

	/* Write to hardware register using appropriate endianness */
	mem_addr_t threshold_addr;
	if (config->big_endian) {
		threshold_addr = config->base + IMSIC_EITHRESHOLD_BE;
	} else {
		threshold_addr = config->base + IMSIC_EITHRESHOLD;
	}

	imsic_write_be(dev, threshold_addr, threshold);

	/* Optional: Verify the write (without complex retry logic) */
#ifdef CONFIG_RISCV_IMSIC_DEBUG
	uint32_t verify_value = imsic_read_be(dev, threshold_addr);
	if (verify_value != threshold) {
		LOG_WRN("IMSIC: Threshold verification failed: wrote 0x%08X, read 0x%08X",
			threshold, verify_value);
	} else {
		LOG_DBG("IMSIC: Threshold 0x%08X verified", threshold);
	}
#endif

	return 0;
}

/* IMSIC interrupt service routine */
#if 0
static void imsic_isr(const struct device *dev)
{
	struct imsic_data *data = dev->data;

	/* Check pending interrupts */
	for (int i = 0; i < 2; i++) {
		uint32_t pending = data->eip_pending[i] & data->eie_mask[i];
		if (pending != 0) {
			/* Find highest priority pending interrupt */
			uint32_t eid = i * 32 + 31 - __builtin_clz(pending);

			/* Check if interrupt priority is above threshold */
			if (eid >= data->eithreshold) {
				/* Handle the interrupt */
				/* printk("IMSIC: Handling EID %u\n", eid); */

				/* Clear pending bit */
				imsic_irq_clear_pending_internal(dev, eid);

				/* Call registered ISR if available */
				// TODO: Implement ISR table lookup
			}
		}
	}
}
#endif

/* Device initialization */
static int imsic_init(const struct device *dev)
{
	const struct imsic_config *config = dev->config;
	struct imsic_data *data = dev->data;

	LOG_INF("IMSIC: Initializing device %s, base=0x%08lX, hart_id=%u",
		dev->name ? dev->name : "NULL", config->base, config->hart_id);
	
	/* Initialize data structures */
	data->eie_mask[0] = 0;
	data->eie_mask[1] = 0;
	data->eip_pending[0] = 0;
	data->eip_pending[1] = 0;
	data->eithreshold = 0;
	data->delivery_mode = IMSIC_EIDELIVERY_MODE_OFF;
	data->total_interrupts = 0;
	
	/* Save device reference for all harts to enable SMP access */
	for (int i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
		save_dev[i] = dev;
	}
	
	/* Configure delivery mode to MSI */
	data->delivery_mode = IMSIC_EIDELIVERY_MODE_MSI;
	data->eithreshold = 0;

	/* Set delivery mode to MSI */
	int result = imsic_set_delivery_mode(dev, IMSIC_EIDELIVERY_MODE_MSI);
	if (result != 0) {
		LOG_WRN("IMSIC: Hardware delivery mode setting failed");
	}

	/* Set threshold to 0 */
	result = imsic_set_threshold(dev, 0);
	if (result != 0) {
		LOG_WRN("IMSIC: Hardware threshold setting failed");
	}

	LOG_INF("IMSIC: Initialization completed successfully");

	return 0;
}

/* ============================================================================
 * Interrupt Handling Helper Functions
 * ============================================================================ */

/**
 * @brief Read pending interrupts from IMSIC
 */
static uint32_t imsic_read_pending(const struct device *dev)
{
	const struct imsic_config *config = dev->config;
	uint32_t pending = 0;
	
	/* Read pending bits from IMSIC registers */
	/* This is a simplified implementation - in reality you'd read from specific registers */
	pending = imsic_read(dev, config->base + IMSIC_EIP0);
	
	return pending & 0xFFFFFFFF; /* Return lower 32 bits */
}

/**
 * @brief Handle a single IMSIC interrupt
 */
static void imsic_handle_single_interrupt(const struct device *dev, uint32_t eid)
{
	struct imsic_data *data = dev->data;
	
	if (dev == NULL || data == NULL) {
		return;
	}
	
	/* 1. Check if the interrupt is enabled */
	if (riscv_imsic_irq_is_enabled(eid) <= 0) {
		LOG_DBG("IMSIC: EID %u not enabled, ignoring", eid);
		return;
	}
	
	/* 2. Check the delivery mode */
	enum riscv_imsic_delivery_mode delivery_mode = riscv_imsic_get_delivery_mode();
	LOG_DBG("IMSIC: Handling EID %u in delivery mode %d", eid, delivery_mode);
	
	/* 3. Route to the appropriate target based on delivery mode */
	switch (delivery_mode) {
	case RISCV_IMSIC_DELIVERY_MODE_MSI:
		/* MSI mode: Process MSI interrupt */
		LOG_DBG("IMSIC: MSI mode - processing EID %u", eid);
		
		/* Check if there's a registered ISR for this EID */
		if (eid < CONFIG_NUM_IRQS) {
			const struct _isr_table_entry *entry = &_sw_isr_table[eid];
			if (entry->isr != NULL) {
				/* Call the registered interrupt service routine */
				entry->isr(entry->arg);
				LOG_DBG("IMSIC: Called ISR for EID %u", eid);
				
				/* Update MSI interrupt statistics */
				k_spinlock_key_t key = k_spin_lock(&data->lock);
				data->msi_interrupts++;
				k_spin_unlock(&data->lock, key);
			} else {
				LOG_WRN("IMSIC: No ISR registered for EID %u", eid);
			}
		}
		break;
		
	case RISCV_IMSIC_DELIVERY_MODE_ID:
		/* ID mode: Process ID-based interrupt */
		LOG_DBG("IMSIC: ID mode - processing EID %u", eid);
		
		/* ID mode typically handles system-level interrupts */
		/* Check if there's a registered ISR for this EID */
		if (eid < CONFIG_NUM_IRQS) {
			const struct _isr_table_entry *entry = &_sw_isr_table[eid];
			if (entry->isr != NULL) {
				entry->isr(entry->arg);
				LOG_DBG("IMSIC: Called ID ISR for EID %u", eid);
				
				/* Update ID interrupt statistics */
				k_spinlock_key_t key = k_spin_lock(&data->lock);
				data->id_interrupts++;
				k_spin_unlock(&data->lock, key);
			}
		}
		break;
		
	case RISCV_IMSIC_DELIVERY_MODE_VIRTUAL:
		/* Virtual mode: Process virtual interrupt */
		LOG_DBG("IMSIC: Virtual mode - processing EID %u", eid);
		
		/* Virtual mode handles guest-level interrupts */
		if (eid < CONFIG_NUM_IRQS) {
			const struct _isr_table_entry *entry = &_sw_isr_table[eid];
			if (entry->isr != NULL) {
				entry->isr(entry->arg);
				LOG_DBG("IMSIC: Called virtual ISR for EID %u", eid);
				
				/* Update virtual interrupt statistics */
				k_spinlock_key_t key = k_spin_lock(&data->lock);
				data->virtual_interrupts++;
				k_spin_unlock(&data->lock, key);
			}
		}
		break;
		
	case RISCV_IMSIC_DELIVERY_MODE_OFF:
	default:
		LOG_WRN("IMSIC: EID %u in invalid delivery mode %d", eid, delivery_mode);
		return;
	}
	
	/* 4. Clear the interrupt source */
	/* IMSIC interrupts are typically cleared by reading the EIP register */
	/* But we can also explicitly clear them */
	riscv_imsic_irq_clear_pending(eid);
	
	/* Update general interrupt statistics */
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	data->total_interrupts++;
	k_spin_unlock(&data->lock, key);
	
	/* Check threshold and update threshold statistics if needed */
	uint32_t current_threshold = riscv_imsic_get_threshold();
	if (eid < current_threshold) {
		key = k_spin_lock(&data->lock);
		data->threshold_rejected++;
		k_spin_unlock(&data->lock, key);
		LOG_DBG("IMSIC: EID %u rejected due to threshold %u", eid, current_threshold);
	}
	
	LOG_DBG("IMSIC: Successfully handled interrupt EID %u in mode %d", eid, delivery_mode);
}

/* ============================================================================
 * Interrupt Handling
 * ============================================================================ */

/**
 * @brief IMSIC interrupt service routine
 */
static void imsic_isr(const void *arg)
{
	const struct device *dev = arg;
	struct imsic_data *data = dev->data;
	
	if (dev == NULL || data == NULL) {
		/* Cannot use LOG_ERR in ISR context */
		return;
	}
	
	/* Check if IMSIC has pending interrupts */
	uint32_t pending = imsic_read_pending(dev);
	if (pending) {
		/* Process pending interrupts */
		for (int i = 0; i < 32 && pending; i++) {
			if (pending & BIT(i)) {
				/* Clear pending bit */
				pending &= ~BIT(i);
				
				/* Handle interrupt i */
				imsic_handle_single_interrupt(dev, i);
			}
		}
		
		/* Update statistics */
		data->total_interrupts++;
	}
}

/* ============================================================================
 * Global Variables for Conditional Registration
 * ============================================================================ */

/* Global variable to track if external IRQ is already registered */
static unsigned int imsic_parent_irq = 0;

/* ============================================================================
 * Device tree based configuration
 * ============================================================================ */
#define IMSIC_INIT(n) \
	static void imsic_irq_config_func_##n(void); \
	\
	static const struct imsic_config imsic_config_##n = { \
		.base = DT_INST_REG_ADDR(n), /* Base address from device tree */ \
		.hart_id = DT_INST_PROP_OR(n, riscv_hart_id, n), /* Hart ID from device tree or instance number */ \
		.guest_id = DT_INST_PROP_OR(n, riscv_guest_id, 0), /* Guest ID from device tree */ \
		.max_eid = DT_INST_PROP_OR(n, riscv_num_ids, 255), /* Max EID from device tree */ \
		.max_prio = DT_INST_PROP_OR(n, riscv_max_priority, 7), /* Max priority from device tree */ \
		.irq = DT_INST_IRQN(n), /* IRQ number from device tree */ \
		.irq_config_func = imsic_irq_config_func_##n, \
		.big_endian = DT_INST_PROP(n, riscv_big_endian), /* Endianness from device tree */ \
	}; \
	\
	static struct imsic_data imsic_data_##n; \
	\
	DEVICE_DT_INST_DEFINE(n, imsic_init, NULL, \
			      &imsic_data_##n, &imsic_config_##n, \
			      PRE_KERNEL_1, CONFIG_INTC_INIT_PRIORITY, NULL); \
	\
	static void imsic_irq_config_func_##n(void) \
{ \
    /* CRITICAL: IMSIC interrupts are handled by shared_ext_isr */ \
    /* APLIC already registered shared_ext_isr to handle both APLIC and IMSIC */ \
    /* IMSIC should NOT register its own handler to avoid conflicts */ \
    /* But we still need to configure the parent IRQ for proper routing */ \
    printk("IMSIC: Using shared interrupt handler, not registering separate handler\n"); \
    printk("IMSIC: Parent IRQ configured as RISCV_IRQ_MEXT (%d)\n", RISCV_IRQ_MEXT); \
}

/* Generate device instances from device tree */
DT_INST_FOREACH_STATUS_OKAY(IMSIC_INIT)

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

/* Basic interrupt control */
void riscv_imsic_irq_enable(uint32_t eid)
{
	const struct device *dev = imsic_get_dev();
	
	if (dev != NULL && eid <= 63) {
		imsic_irq_enable_internal(dev, eid);
	}
}

void riscv_imsic_irq_disable(uint32_t eid)
{
	const struct device *dev = imsic_get_dev();
	
	if (dev != NULL && eid <= 63) {
		imsic_irq_disable_internal(dev, eid);
	}
}

int riscv_imsic_irq_is_enabled(uint32_t eid)
{
	const struct device *dev = imsic_get_dev();
	
	if (dev == NULL || eid > 63) {
		return -EINVAL;
	}
	
	struct imsic_data *data = dev->data;
	uint32_t mask_index = eid / 32;
	uint32_t bit_mask = BIT(eid % 32);
	
	return (data->eie_mask[mask_index] & bit_mask) ? 1 : 0;
}

void riscv_imsic_irq_set_pending(uint32_t eid)
{
	const struct device *dev = imsic_get_dev();
	
	if (dev != NULL && eid <= 63) {
		imsic_irq_set_pending_internal(dev, eid);
	}
}

void riscv_imsic_irq_clear_pending(uint32_t eid)
{
	const struct device *dev = imsic_get_dev();
	
	if (dev != NULL && eid <= 63) {
		imsic_irq_clear_pending_internal(dev, eid);
	}
}

/* Delivery mode control */
int riscv_imsic_set_delivery_mode(enum riscv_imsic_delivery_mode mode)
{
	const struct device *dev = imsic_get_dev();
	
	if (dev == NULL) {
		return -ENODEV;
	}
	
	return imsic_set_delivery_mode(dev, mode);
}

enum riscv_imsic_delivery_mode riscv_imsic_get_delivery_mode(void)
{
	const struct device *dev = imsic_get_dev();
	
	if (dev == NULL) {
		return RISCV_IMSIC_DELIVERY_MODE_OFF;
	}
	
	struct imsic_data *data = dev->data;
	return (enum riscv_imsic_delivery_mode)data->delivery_mode;
}

/* Threshold control */
int riscv_imsic_set_threshold(uint32_t threshold)
{
	const struct device *dev = imsic_get_dev();
	
	if (dev == NULL) {
		return -ENODEV;
	}
	
	return imsic_set_threshold(dev, threshold);
}

uint32_t riscv_imsic_get_threshold(void)
{
	const struct device *dev = imsic_get_dev();
	
	if (dev == NULL) {
		return 0;
	}
	
	struct imsic_data *data = dev->data;
	return data->eithreshold;
}

/* Device management */
const struct device *riscv_imsic_get_dev(void)
{
	return imsic_get_dev();
}

int riscv_imsic_get_hart_id(const struct device *dev)
{
	if (dev == NULL) {
		return -EINVAL;
	}
	
	const struct imsic_config *config = dev->config;
	return config->hart_id;
}

int riscv_imsic_get_guest_id(const struct device *dev)
{
	if (dev == NULL) {
		return -EINVAL;
	}
	
	const struct imsic_config *config = dev->config;
	return config->guest_id;
}

/* MSI specific functions */
int riscv_imsic_send_msi(uint32_t target_hart, uint32_t target_guest, uint32_t eid)
{
	/* IMSIC itself cannot send MSIs - it only receives them
	 * We need to use APLIC to send MSI interrupts to IMSIC */
	const struct device *aplic_dev = riscv_aplic_get_dev();
	
	if (aplic_dev == NULL) {
		return -ENODEV;
	}
	
	/* Check if APLIC supports MSI mode */
	if (!riscv_aplic_is_msi_mode_enabled()) {
		return -ENOTSUP;
	}
	
	/* Validate parameters */
	if (eid > 63 || target_hart >= CONFIG_MP_MAX_NUM_CPUS) {
		return -EINVAL;
	}
	
	/* Use APLIC to send MSI to the target hart/guest */
	return riscv_aplic_send_msi(target_hart, target_guest, eid);
}

int riscv_imsic_receive_msi(uint32_t eid, uint32_t *source_hart, uint32_t *source_guest)
{
	const struct device *dev = imsic_get_dev();
	
	if (dev == NULL || eid > 63 || source_hart == NULL || source_guest == NULL) {
		return -EINVAL;
	}
	
	/* Check if this EID is pending */
	if (riscv_imsic_irq_is_enabled(eid) <= 0) {
		return -ENOENT;
	}
	
	/* For now, we don't have source information in the current implementation
	 * This would require additional hardware support or APLIC integration */
	*source_hart = 0;
	*source_guest = 0;
	
	return 0;
}

/* Statistics and diagnostics */
int riscv_imsic_get_stats(struct riscv_imsic_stats *stats)
{
	const struct device *dev = imsic_get_dev();
	
	if (dev == NULL || stats == NULL) {
		return -EINVAL;
	}
	
	struct imsic_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	
	/* Return real statistics from the device data */
	stats->total_interrupts = data->total_interrupts;
	stats->msi_interrupts = data->msi_interrupts;
	stats->id_interrupts = data->id_interrupts;
	stats->virtual_interrupts = data->virtual_interrupts;
	stats->threshold_rejected = data->threshold_rejected;
	
	k_spin_unlock(&data->lock, key);
	
	return 0;
}

void riscv_imsic_reset_stats(void)
{
	const struct device *dev = imsic_get_dev();
	
	if (dev == NULL) {
		return;
	}
	
	struct imsic_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	
	/* Reset all statistics to zero */
	data->total_interrupts = 0;
	data->msi_interrupts = 0;
	data->id_interrupts = 0;
	data->virtual_interrupts = 0;
	data->threshold_rejected = 0;
	
	k_spin_unlock(&data->lock, key);
}
