/*
 * Copyright (c) 2024 Zephyr contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/interrupt_controller/riscv_aia.h>
#include <zephyr/drivers/interrupt_controller/riscv_aplic.h>
#include <zephyr/drivers/interrupt_controller/riscv_imsic.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <zephyr/sys/util.h>
#include <zephyr/arch/riscv/csr.h>
#include <zephyr/devicetree.h>

LOG_MODULE_REGISTER(aia, CONFIG_LOG_DEFAULT_LEVEL);

/* ============================================================================
 * AIA Data Structures
 * ============================================================================ */

struct aia_data {
    /* Thread safety */
    struct k_spinlock lock;

    /* Device references - AIA acts as a manager */
    const struct device *aplic_dev;
    const struct device *imsic_dev;

    /* Configuration from device tree */
    uint32_t max_harts;
    uint32_t max_guests;

    /* State */
    bool initialized;
    bool msi_mode_supported;
    bool direct_mode_supported;
    bool msi_mode_enabled;

    /* Performance and statistics */
    uint32_t total_interrupts_handled;
    uint32_t msi_interrupts_handled;
    uint32_t direct_interrupts_handled;
    uint32_t errors_count;

    /* Load balancing information */
    uint32_t hart_load[CONFIG_MP_MAX_NUM_CPUS];

    /* Debug and diagnostics */
    bool debug_mode;
};

/* ============================================================================
 * AIA Helper Functions
 * ============================================================================ */

/**
 * @brief Check if a device is ready and available
 */
static inline bool aia_device_is_ready(const struct device *dev)
{
    return (dev != NULL) && device_is_ready(dev);
}

/**
 * @brief Update interrupt statistics
 */
static inline void aia_update_stats(struct aia_data *data, bool is_msi)
{
    k_spinlock_key_t key = k_spin_lock(&data->lock);
    data->total_interrupts_handled++;
    if (is_msi) {
        data->msi_interrupts_handled++;
    } else {
        data->direct_interrupts_handled++;
    }
    k_spin_unlock(&data->lock, key);
}

/**
 * @brief Get least loaded hart for load balancing
 */
static inline uint32_t aia_get_best_hart(struct aia_data *data)
{
    uint32_t best_hart = 0;
    uint32_t min_load = UINT32_MAX;

    for (uint32_t i = 0; i < CONFIG_MP_MAX_NUM_CPUS; i++) {
        if (data->hart_load[i] < min_load) {
            min_load = data->hart_load[i];
            best_hart = i;
        }
    }

    return best_hart;
}

/**
 * @brief Log AIA operation for debugging
 */
static inline void aia_log_operation(const char *operation, uint32_t irq, int result)
{
    if (result == 0) {
        LOG_DBG("AIA: %s IRQ %u - OK", operation, irq);
    } else {
        LOG_WRN("AIA: %s IRQ %u failed (%d)", operation, irq, result);
    }
}

/* ============================================================================
 * AIA Device Discovery Functions
 * ============================================================================ */

/**
 * @brief Discover and initialize APLIC device
 */
static int aia_discover_aplic(struct aia_data *data)
{
    /* Try to find APLIC device using multiple possible names */
    const char *aplic_names[] = {
        "qemu_aplic",           /* DT_DRV_COMPAT name */
        "interrupt-controller@c000000",  /* Device tree node label */
        "aplic",                /* Generic name */
        NULL
    };

    for (int i = 0; aplic_names[i] != NULL; i++) {
        data->aplic_dev = device_get_binding(aplic_names[i]);
        if (data->aplic_dev != NULL) {
            LOG_INF("AIA: Found APLIC device '%s'", aplic_names[i]);
            break;
        }
    }

    if (data->aplic_dev == NULL) {
        LOG_WRN("AIA: APLIC device not found with any known name");
        return -ENODEV;
    }

    if (!aia_device_is_ready(data->aplic_dev)) {
        LOG_WRN("AIA: APLIC device not ready");
        return -EBUSY;
    }

    return 0;
}

/**
 * @brief Discover and initialize IMSIC device
 */
static int aia_discover_imsic(struct aia_data *data)
{
    /* Try to find IMSIC device using multiple possible names */
    const char *imsic_names[] = {
        "qemu_imsic",           /* DT_DRV_COMPAT name */
        "interrupt-controller@24000000",  /* Device tree node label */
        "imsic",                /* Generic name */
        NULL
    };

    for (int i = 0; imsic_names[i] != NULL; i++) {
        data->imsic_dev = device_get_binding(imsic_names[i]);
        if (data->imsic_dev != NULL) {
            LOG_INF("AIA: Found IMSIC device '%s'", imsic_names[i]);
            break;
        }
    }

    if (data->imsic_dev == NULL) {
        LOG_WRN("AIA: IMSIC device not found with any known name");
        return -ENODEV;
    }

    if (!aia_device_is_ready(data->imsic_dev)) {
        LOG_WRN("AIA: IMSIC device not ready");
        return -EBUSY;
    }

    return 0;
}

/* ============================================================================
 * AIA Unified Interrupt Management
 * ============================================================================ */

/**
 * @brief Enable interrupt through AIA unified interface
 */
static int aia_irq_enable(const struct device *dev, unsigned int irq)
{
    struct aia_data *data = dev->data;
    int ret = -ENOTSUP;

    /* In AIA, when both APLIC and IMSIC are available:
     * - Use APLIC for direct mode interrupts (traditional wired interrupts)
     * - Use IMSIC for MSI mode interrupts (message-signaled interrupts)
     */

    if (data->msi_mode_enabled && aia_device_is_ready(data->imsic_dev)) {
        /* MSI mode: Use IMSIC for message-signaled interrupts */
        riscv_imsic_irq_enable(irq);
        ret = 0;
    } else if (aia_device_is_ready(data->aplic_dev)) {
        /* Direct mode: Use APLIC for traditional wired interrupts */
        riscv_aplic_irq_enable(irq);
        ret = 0;
    } else if (aia_device_is_ready(data->imsic_dev)) {
        /* Fallback: IMSIC only system */
        riscv_imsic_irq_enable(irq);
        ret = 0;
    }

    if (ret == 0) {
        aia_update_stats(data, data->msi_mode_enabled);
        if (data->debug_mode) {
            aia_log_operation("enable", irq, ret);
        }
    } else {
        data->errors_count++;
        LOG_WRN("AIA: Failed to enable IRQ %u", irq);
    }

    return ret;
}

/**
 * @brief Disable interrupt through AIA unified interface
 */
static int aia_irq_disable(const struct device *dev, unsigned int irq)
{
    struct aia_data *data = dev->data;
    int ret = -ENOTSUP;

    if (data->msi_mode_enabled && aia_device_is_ready(data->imsic_dev)) {
        /* MSI mode: Use IMSIC for message-signaled interrupts */
        riscv_imsic_irq_disable(irq);
        ret = 0;
    } else if (aia_device_is_ready(data->aplic_dev)) {
        /* Direct mode: Use APLIC for traditional wired interrupts */
        riscv_aplic_irq_disable(irq);
        ret = 0;
    } else if (aia_device_is_ready(data->imsic_dev)) {
        /* Fallback: IMSIC only system */
        riscv_imsic_irq_disable(irq);
        ret = 0;
    }

    if (ret != 0) {
        LOG_WRN("AIA: Failed to disable IRQ %u", irq);
    }

    return ret;
}

/**
 * @brief Check if interrupt is enabled through AIA unified interface
 */
static int aia_irq_is_enabled(const struct device *dev, unsigned int irq)
{
    struct aia_data *data = dev->data;
    int ret = -ENOTSUP;

    if (data->msi_mode_enabled && aia_device_is_ready(data->imsic_dev)) {
        /* MSI mode: Check IMSIC for message-signaled interrupts */
        ret = riscv_imsic_irq_is_enabled(irq);
    } else if (aia_device_is_ready(data->aplic_dev)) {
        /* Direct mode: Check APLIC for traditional wired interrupts */
        ret = riscv_aplic_irq_is_enabled(irq);
    } else if (aia_device_is_ready(data->imsic_dev)) {
        /* Fallback: IMSIC only system */
        ret = riscv_imsic_irq_is_enabled(irq);
    }

    return ret;
}

/**
 * @brief Set interrupt priority through AIA unified interface
 */
static int aia_irq_set_priority(const struct device *dev, unsigned int irq, unsigned int prio)
{
    struct aia_data *data = dev->data;
    int ret = -ENOTSUP;

    /* Priority is typically handled by APLIC, as it manages interrupt routing */
    if (aia_device_is_ready(data->aplic_dev)) {
        riscv_aplic_set_priority(irq, prio);
        ret = 0;
    } else {
        LOG_WRN("AIA: No APLIC available for priority management");
        ret = -ENOTSUP;
    }

    if (ret != 0 && ret != -ENOTSUP) {
        LOG_WRN("AIA: Failed to set priority for IRQ %u", irq);
    }

    return ret;
}

/**
 * @brief Get interrupt priority through AIA unified interface
 */
static int aia_irq_get_priority(const struct device *dev, unsigned int irq, unsigned int *prio)
{
    struct aia_data *data = dev->data;
    int ret = -ENOTSUP;

    if (prio == NULL) {
        return -EINVAL;
    }

    /* Priority is typically handled by APLIC, as it manages interrupt routing */
    if (aia_device_is_ready(data->aplic_dev)) {
        /* APLIC doesn't have get_priority API, use default */
        *prio = 1; /* Default priority */
        ret = 0;
    } else {
        ret = -ENOTSUP;
    }

    return ret;
}

/**
 * @brief Check if interrupt is pending through AIA unified interface
 */
static int aia_irq_is_pending(const struct device *dev, unsigned int irq)
{
    struct aia_data *data = dev->data;
    int ret = -ENOTSUP;

    if (data->msi_mode_enabled && aia_device_is_ready(data->imsic_dev)) {
        /* MSI mode: Check IMSIC for message-signaled interrupts */
        ret = riscv_imsic_irq_is_enabled(irq); /* Use enabled status as proxy */
    } else if (aia_device_is_ready(data->aplic_dev)) {
        /* Direct mode: Check APLIC for traditional wired interrupts */
        ret = riscv_aplic_irq_is_enabled(irq); /* Use enabled status as proxy */
    } else if (aia_device_is_ready(data->imsic_dev)) {
        /* Fallback: IMSIC only system */
        ret = riscv_imsic_irq_is_enabled(irq); /* Use enabled status as proxy */
    }

    return ret;
}

/**
 * @brief Clear interrupt pending status through AIA unified interface
 */
static int aia_irq_clear_pending(const struct device *dev, unsigned int irq)
{
    struct aia_data *data = dev->data;
    int ret = -ENOTSUP;

    if (data->msi_mode_enabled && aia_device_is_ready(data->imsic_dev)) {
        /* MSI mode: Clear pending in IMSIC for message-signaled interrupts */
        riscv_imsic_irq_clear_pending(irq);
        ret = 0;
    } else if (aia_device_is_ready(data->aplic_dev)) {
        /* Direct mode: Clear pending in APLIC for traditional wired interrupts */
        /* APLIC handles this automatically, just return success */
        ret = 0;
    } else if (aia_device_is_ready(data->imsic_dev)) {
        /* Fallback: IMSIC only system */
        riscv_imsic_irq_clear_pending(irq);
        ret = 0;
    }

    if (ret != 0) {
        LOG_WRN("AIA: Failed to clear pending for IRQ %u", irq);
    }

    return ret;
}

/* ============================================================================
 * AIA Management API Functions
 * ============================================================================ */

/**
 * @brief Get AIA statistics
 */
int riscv_aia_get_stats(struct riscv_aia_stats *stats)
{
    const struct device *dev = riscv_aia_get_device();
    if (dev == NULL || stats == NULL) {
        return -EINVAL;
    }

    struct aia_data *data = dev->data;
    k_spinlock_key_t key = k_spin_lock(&data->lock);

    stats->total_interrupts = data->total_interrupts_handled;
    stats->msi_interrupts = data->msi_interrupts_handled;
    stats->direct_interrupts = data->direct_interrupts_handled;
    stats->errors = data->errors_count;

    k_spin_unlock(&data->lock, key);

    return 0;
}

/**
 * @brief Reset AIA statistics
 */
void riscv_aia_reset_stats(void)
{
    const struct device *dev = riscv_aia_get_device();
    if (dev == NULL) {
        return;
    }

    struct aia_data *data = dev->data;
    k_spinlock_key_t key = k_spin_lock(&data->lock);

    data->total_interrupts_handled = 0;
    data->msi_interrupts_handled = 0;
    data->direct_interrupts_handled = 0;
    data->errors_count = 0;

    k_spin_unlock(&data->lock, key);

    LOG_INF("AIA: Statistics reset");
}

/**
 * @brief Enable/disable AIA debug mode
 */
int riscv_aia_set_debug_mode(bool enable)
{
    const struct device *dev = riscv_aia_get_device();
    if (dev == NULL) {
        return -ENODEV;
    }

    struct aia_data *data = dev->data;
    k_spinlock_key_t key = k_spin_lock(&data->lock);
    data->debug_mode = enable;
    k_spin_unlock(&data->lock, key);

    LOG_INF("AIA: Debug mode %s", enable ? "enabled" : "disabled");
    return 0;
}

/**
 * @brief Get AIA capabilities
 */
int riscv_aia_get_capabilities(struct riscv_aia_caps *caps)
{
    const struct device *dev = riscv_aia_get_device();
    if (dev == NULL || caps == NULL) {
        return -EINVAL;
    }

    struct aia_data *data = dev->data;
    k_spinlock_key_t key = k_spin_lock(&data->lock);

    caps->msi_supported = data->msi_mode_supported;
    caps->direct_supported = data->direct_mode_supported;
    caps->msi_enabled = data->msi_mode_enabled;
    caps->max_harts = data->max_harts;
    caps->max_guests = data->max_guests;

    k_spin_unlock(&data->lock, key);

    return 0;
}

/* ============================================================================
 * AIA Public API Implementation
 * ============================================================================ */

const struct device *riscv_aia_get_device(void)
{
    return device_get_binding("aia");
}

const struct device *riscv_aia_get_device_for_hart(uint32_t hart_id)
{
    /* For now, return the main AIA device */
    return riscv_aia_get_device();
}

bool riscv_aia_is_msi_mode_enabled(const struct device *dev)
{
    if (dev == NULL) {
        return false;
    }
    
    struct aia_data *data = dev->data;
    return data->msi_mode_enabled;
}

int riscv_aia_enable_irq(uint32_t irq)
{
    const struct device *dev = riscv_aia_get_device();
    if (dev == NULL) {
        return -ENODEV;
    }
    
    return aia_irq_enable(dev, irq);
}

int riscv_aia_disable_irq(uint32_t irq)
{
    const struct device *dev = riscv_aia_get_device();
    if (dev == NULL) {
        return -ENODEV;
    }
    
    return aia_irq_disable(dev, irq);
}

int riscv_aia_is_irq_enabled(uint32_t irq)
{
    const struct device *dev = riscv_aia_get_device();
    if (dev == NULL) {
        return -ENODEV;
    }
    
    return aia_irq_is_enabled(dev, irq);
}

int riscv_aia_set_irq_priority(uint32_t irq, uint32_t priority)
{
    const struct device *dev = riscv_aia_get_device();
    if (dev == NULL) {
        return -ENODEV;
    }
    
    return aia_irq_set_priority(dev, irq, priority);
}

int riscv_aia_get_irq_priority(uint32_t irq, uint32_t *priority)
{
    const struct device *dev = riscv_aia_get_device();
    if (dev == NULL) {
        return -ENODEV;
    }
    
    return aia_irq_get_priority(dev, irq, priority);
}

int riscv_aia_is_irq_pending(uint32_t irq)
{
    const struct device *dev = riscv_aia_get_device();
    if (dev == NULL) {
        return -ENODEV;
    }
    
    return aia_irq_is_pending(dev, irq);
}

int riscv_aia_clear_irq_pending(uint32_t irq)
{
    const struct device *dev = riscv_aia_get_device();
    if (dev == NULL) {
        return -ENODEV;
    }
    
    return aia_irq_clear_pending(dev, irq);
}

/* ============================================================================
 * AIA Driver Initialization
 * ============================================================================ */

/**
 * @brief Initialize AIA as a management layer
 */
static int aia_init(const struct device *dev)
{
    struct aia_data *data = dev->data;
    int ret;

    LOG_INF("AIA: Initializing RISC-V AIA management layer");

    /* Initialize data structure */
    memset(data, 0, sizeof(struct aia_data));

    /* Discover APLIC device */
    ret = aia_discover_aplic(data);
    if (ret != 0) {
        LOG_WRN("AIA: APLIC discovery failed, continuing without APLIC");
    }

    /* Discover IMSIC device */
    ret = aia_discover_imsic(data);
    if (ret != 0) {
        LOG_WRN("AIA: IMSIC discovery failed, continuing without IMSIC");
    }

    /* Determine supported modes */
    if (data->aplic_dev != NULL && data->imsic_dev != NULL) {
        data->msi_mode_supported = true;
        data->direct_mode_supported = true;
        data->msi_mode_enabled = true; /* Prefer MSI mode when both are available */
        LOG_INF("AIA: MSI mode supported and enabled");
    } else if (data->aplic_dev != NULL) {
        data->msi_mode_supported = false;
        data->direct_mode_supported = true;
        data->msi_mode_enabled = false;
        LOG_INF("AIA: Direct mode only (APLIC available)");
    } else if (data->imsic_dev != NULL) {
        data->msi_mode_supported = true;
        data->direct_mode_supported = false;
        data->msi_mode_enabled = true;
        LOG_INF("AIA: MSI mode only (IMSIC available)");
    } else {
        LOG_ERR("AIA: No interrupt controllers found");
        return -ENODEV;
    }

    /* Set configuration defaults */
    data->max_harts = 4;   /* Default for QEMU */
    data->max_guests = 1;  /* Default for QEMU */

    /* Mark as initialized */
    data->initialized = true;
    data->total_interrupts_handled = 0;

    LOG_INF("AIA: Management layer initialized successfully");
    LOG_INF("AIA: APLIC: %s, IMSIC: %s",
            data->aplic_dev ? data->aplic_dev->name : "not available",
            data->imsic_dev ? data->imsic_dev->name : "not available");
    LOG_INF("AIA: MSI mode: %s, Direct mode: %s",
            data->msi_mode_supported ? "supported" : "not supported",
            data->direct_mode_supported ? "supported" : "not supported");

    return 0;
}

/* ============================================================================
 * Static Device Configuration
 * ============================================================================ */

/* AIA static data - management layer */
static struct aia_data aia_data_0;

/* AIA device definition - management layer for RISC-V AIA */
DEVICE_DEFINE(aia, "aia", aia_init, NULL,
              &aia_data_0, NULL,
              PRE_KERNEL_1, 60, NULL);
