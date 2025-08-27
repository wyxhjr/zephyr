/*
 * Copyright (c) 2024 Zephyr contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_INTERRUPT_CONTROLLER_RISCV_AIA_H_
#define ZEPHYR_DRIVERS_INTERRUPT_CONTROLLER_RISCV_AIA_H_

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * AIA Configuration Structure
 * ============================================================================ */

struct aia_config {
    /* APLIC configuration */
    uintptr_t aplic_base;
    uint32_t aplic_max_prio;
    uint32_t aplic_nr_irqs;
    uint32_t aplic_irq;
    
    /* IMSIC configuration */
    uintptr_t imsic_base;
    uint32_t imsic_max_eid;
    uint32_t imsic_max_priority;
    bool imsic_big_endian;
    
    /* Common configuration */
    uint32_t max_harts;
    uint32_t max_guests;
    bool msi_mode_supported;
    bool direct_mode_supported;
};

/* ============================================================================
 * AIA Configuration Structure
 * ============================================================================ */

/**
 * @brief AIA configuration structure (for future device tree integration)
 * Note: AIA acts as a management layer and doesn't directly access hardware
 */
struct aia_dt_config {
    uint32_t max_harts;              /* Maximum number of harts supported */
    uint32_t max_guests;             /* Maximum number of guest contexts */
    bool msi_mode_supported;         /* Whether MSI mode is supported */
    bool direct_mode_supported;      /* Whether direct mode is supported */
};

/* ============================================================================
 * Device Management Functions
 * ============================================================================ */

/**
 * @brief Get the main AIA device
 *
 * @return Pointer to AIA device, or NULL if not found
 */
const struct device *riscv_aia_get_device(void);

/**
 * @brief Get AIA device for specific hart
 *
 * @param hart_id Hart ID
 * @return Pointer to AIA device for hart, or NULL if not found
 */
const struct device *riscv_aia_get_device_for_hart(uint32_t hart_id);

/* ============================================================================
 * Mode Detection Functions
 * ============================================================================ */

/**
 * @brief Check if MSI mode is enabled
 *
 * @param dev AIA device
 * @return true if MSI mode is enabled, false otherwise
 */
bool riscv_aia_is_msi_mode_enabled(const struct device *dev);

/* ============================================================================
 * Unified Interrupt Control Functions
 * ============================================================================ */

/**
 * @brief Enable an interrupt
 *
 * @param irq Interrupt number
 * @return 0 on success, negative error code on failure
 */
int riscv_aia_enable_irq(uint32_t irq);

/**
 * @brief Disable an interrupt
 *
 * @param irq Interrupt number
 * @return 0 on success, negative error code on failure
 */
int riscv_aia_disable_irq(uint32_t irq);

/**
 * @brief Check if an interrupt is enabled
 *
 * @param irq Interrupt number
 * @return 1 if enabled, 0 if disabled, negative error code on failure
 */
int riscv_aia_is_irq_enabled(uint32_t irq);

/**
 * @brief Set interrupt priority
 *
 * @param irq Interrupt number
 * @param priority Priority level
 * @return 0 on success, negative error code on failure
 */
int riscv_aia_set_irq_priority(uint32_t irq, uint32_t priority);

/**
 * @brief Get interrupt priority
 *
 * @param irq Interrupt number
 * @param priority Pointer to store priority
 * @return 0 on success, negative error code on failure
 */
int riscv_aia_get_irq_priority(uint32_t irq, uint32_t *priority);

/**
 * @brief Check if an interrupt is pending
 *
 * @param irq Interrupt number
 * @return 1 if pending, 0 if not pending, negative error code on failure
 */
int riscv_aia_is_irq_pending(uint32_t irq);

/**
 * @brief Clear interrupt pending status
 *
 * @param irq Interrupt number
 * @return 0 on success, negative error code on failure
 */
int riscv_aia_clear_irq_pending(uint32_t irq);

/* ============================================================================
 * AIA Management and Statistics API
 * ============================================================================ */

/* AIA statistics structure */
struct riscv_aia_stats {
    uint32_t total_interrupts;    /* Total interrupts handled */
    uint32_t msi_interrupts;      /* MSI interrupts handled */
    uint32_t direct_interrupts;   /* Direct interrupts handled */
    uint32_t errors;              /* Error count */
};

/* AIA capabilities structure */
struct riscv_aia_caps {
    bool msi_supported;           /* MSI mode supported */
    bool direct_supported;        /* Direct mode supported */
    bool msi_enabled;             /* MSI mode currently enabled */
    uint32_t max_harts;           /* Maximum harts supported */
    uint32_t max_guests;          /* Maximum guests supported */
};

/**
 * @brief Get AIA statistics
 *
 * @param stats Pointer to statistics structure
 * @return 0 on success, negative error code on failure
 */
int riscv_aia_get_stats(struct riscv_aia_stats *stats);

/**
 * @brief Reset AIA statistics
 */
void riscv_aia_reset_stats(void);

/**
 * @brief Enable/disable AIA debug mode
 *
 * @param enable true to enable debug mode, false to disable
 * @return 0 on success, negative error code on failure
 */
int riscv_aia_set_debug_mode(bool enable);

/**
 * @brief Get AIA capabilities
 *
 * @param caps Pointer to capabilities structure
 * @return 0 on success, negative error code on failure
 */
int riscv_aia_get_capabilities(struct riscv_aia_caps *caps);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_INTERRUPT_CONTROLLER_RISCV_AIA_H_ */
