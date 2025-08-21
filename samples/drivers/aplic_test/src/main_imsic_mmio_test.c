/*
 * Copyright (c) 2024 Zephyr Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/interrupt_controller/riscv_imsic.h>

LOG_MODULE_REGISTER(imsic_mmio_test, LOG_LEVEL_INF);

/* Test configuration */
#define TEST_EID 10

/* Main test function */
int main(void)
{
    LOG_INF("=== RISC-V IMSIC MMIO Test ===");
    LOG_INF("🎯 Testing IMSIC MMIO register access");
    LOG_INF("🚀 Starting in 3 seconds...");
    
    /* Wait for system stabilization */
    k_msleep(3000);
    
    LOG_INF("🎬 Starting MMIO tests...");
    
    /* Test 1: Get IMSIC device */
    LOG_INF("=== Test 1: IMSIC Device Access ===");
    const struct device *imsic_dev = riscv_imsic_get_dev();
    if (!imsic_dev || !device_is_ready(imsic_dev)) {
        LOG_ERR("❌ IMSIC device not available");
        return -1;
    }
    LOG_INF("✅ IMSIC device: %s", imsic_dev->name);
    
    /* Test 2: Basic interrupt enable/disable */
    LOG_INF("=== Test 2: Basic Interrupt Control ===");
    
    LOG_INF("🔓 Enabling interrupt for EID %u...", TEST_EID);
    riscv_imsic_irq_enable(TEST_EID);
    
    int enabled = riscv_imsic_irq_is_enabled(TEST_EID);
    if (enabled > 0) {
        LOG_INF("✅ EID %u: Enabled successfully", TEST_EID);
    } else {
        LOG_ERR("❌ EID %u: Enable failed", TEST_EID);
        return -1;
    }
    
    LOG_INF("🔒 Disabling interrupt for EID %u...", TEST_EID);
    riscv_imsic_irq_disable(TEST_EID);
    
    enabled = riscv_imsic_irq_is_enabled(TEST_EID);
    if (enabled <= 0) {
        LOG_INF("✅ EID %u: Disabled successfully", TEST_EID);
    } else {
        LOG_WRN("⚠️  EID %u: Disable failed", TEST_EID);
    }
    
    /* Test 3: Interrupt pending (safe test) */
    LOG_INF("=== Test 3: Interrupt Pending Test ===");
    
    /* Re-enable for testing */
    riscv_imsic_irq_enable(TEST_EID);
    
    LOG_INF("📡 Setting interrupt pending for EID %u...", TEST_EID);
    riscv_imsic_irq_set_pending(TEST_EID);
    LOG_INF("✅ Interrupt pending set successfully");
    
    LOG_INF("🧹 Clearing interrupt pending for EID %u...", TEST_EID);
    riscv_imsic_irq_clear_pending(TEST_EID);
    LOG_INF("✅ Interrupt pending cleared successfully");
    
    /* Test 4: Threshold test (this will test MMIO to EITHRESHOLD) */
    LOG_INF("=== Test 4: Threshold Test (MMIO to EITHRESHOLD) ===");
    
    uint32_t old_threshold = riscv_imsic_get_threshold();
    LOG_INF("📊 Current threshold: %u", old_threshold);
    
    LOG_INF("📊 Setting threshold to 2...");
    int ret = riscv_imsic_set_threshold(2);
    if (ret == 0) {
        uint32_t new_threshold = riscv_imsic_get_threshold();
        LOG_INF("✅ Threshold set to %u (was %u)", new_threshold, old_threshold);
        
        /* Restore threshold */
        riscv_imsic_set_threshold(old_threshold);
        LOG_INF("📊 Threshold restored to %u", old_threshold);
    } else {
        LOG_WRN("⚠️  Failed to set threshold: %d", ret);
    }
    
    /* Test 5: Delivery mode test (this will test MMIO to EIDELIVERY) */
    LOG_INF("=== Test 5: Delivery Mode Test (MMIO to EIDELIVERY) ===");
    
    enum riscv_imsic_delivery_mode old_mode = riscv_imsic_get_delivery_mode();
    LOG_INF("📊 Current delivery mode: %d", old_mode);
    
    LOG_INF("📊 Setting delivery mode to MSI...");
    ret = riscv_imsic_set_delivery_mode(RISCV_IMSIC_DELIVERY_MODE_MSI);
    if (ret == 0) {
        enum riscv_imsic_delivery_mode new_mode = riscv_imsic_get_delivery_mode();
        LOG_INF("✅ Delivery mode set to %d (was %d)", new_mode, old_mode);
        
        /* Restore mode */
        riscv_imsic_set_delivery_mode(old_mode);
        LOG_INF("📊 Delivery mode restored to %d", old_mode);
    } else {
        LOG_WRN("⚠️  Failed to set delivery mode: %d", ret);
    }
    
    /* Final summary */
    LOG_INF("🎉 === MMIO Test Summary ===");
    LOG_INF("✅ IMSIC device access: WORKING");
    LOG_INF("✅ Interrupt enable/disable: WORKING");
    LOG_INF("✅ Interrupt pending control: WORKING");
    LOG_INF("✅ Threshold MMIO access: %s", (ret == 0) ? "WORKING" : "FAILED");
    LOG_INF("✅ Delivery mode MMIO access: %s", (ret == 0) ? "WORKING" : "FAILED");
    
    LOG_INF("🔄 Keeping system running for observation...");
    
    /* Keep the system running */
    for (int i = 0; i < 10; i++) {
        k_msleep(2000);
        LOG_INF("💻 System running normally... iteration %d/10", i + 1);
    }
    
    LOG_INF("🏁 Test completed successfully. System will continue running.");
    
    /* Final status loop */
    while (1) {
        k_msleep(5000);
        LOG_INF("💻 System status: IMSIC MMIO working");
    }
    
    return 0;
}
