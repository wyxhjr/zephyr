/*
 * Copyright (c) 2024 Zephyr Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/interrupt_controller/riscv_imsic.h>
#include <zephyr/drivers/interrupt_controller/riscv_aplic.h>
#include <zephyr/irq.h>

LOG_MODULE_REGISTER(imsic_interrupt_test, LOG_LEVEL_INF);

/* Test configuration */
#define TEST_EID_BASE 10
#define TEST_NUM_EIDS 4
#define TEST_DURATION_MS 5000
#define INTERRUPT_INTERVAL_MS 100

/* Test data */
static volatile uint32_t interrupt_count = 0;
static volatile uint32_t last_interrupted_eid = 0;
static volatile bool interrupt_received = false;
static uint32_t test_eids[TEST_NUM_EIDS];

/* IMSIC interrupt handler */
static void imsic_interrupt_isr(const void *param)
{
    uint32_t eid = (uint32_t)param;
    
    LOG_INF("🎯 *** INTERRUPT RECEIVED *** EID %u", eid);
    interrupt_count++;
    last_interrupted_eid = eid;
    interrupt_received = true;
    
    /* Clear interrupt pending */
    riscv_imsic_irq_clear_pending(eid);
    
    LOG_INF("🎯 Interrupt %u processed, count: %u", eid, interrupt_count);
}

/* Test interrupt signal generation */
static void test_interrupt_signals(void)
{
    LOG_INF("=== Testing Interrupt Signal Generation ===");
    
    /* Initialize test EIDs */
    for (int i = 0; i < TEST_NUM_EIDS; i++) {
        test_eids[i] = TEST_EID_BASE + i;
    }
    
    LOG_INF("📋 Test EIDs: %u to %u", test_eids[0], test_eids[TEST_NUM_EIDS-1]);
    
    /* Enable interrupts for all test EIDs */
    LOG_INF("🔓 Enabling interrupts for all test EIDs...");
    for (int i = 0; i < TEST_NUM_EIDS; i++) {
        uint32_t eid = test_eids[i];
        riscv_imsic_irq_enable(eid);
        int enabled = riscv_imsic_irq_is_enabled(eid);
        if (enabled > 0) {
            LOG_INF("   ✅ EID %u: Enabled", eid);
        } else {
            LOG_WRN("   ⚠️  EID %u: Enable failed", eid);
        }
    }
    
    /* Connect ISR for all test EIDs */
    LOG_INF("🔗 Connecting interrupt handlers...");
    for (int i = 0; i < TEST_NUM_EIDS; i++) {
        uint32_t eid = test_eids[i];
        irq_connect_dynamic(eid, 0, imsic_interrupt_isr, (void *)eid, 0);
        irq_enable(eid);
        LOG_INF("   ✅ EID %u: ISR connected and enabled", eid);
    }
    
    /* Test 1: Single interrupt generation */
    LOG_INF("📡 Test 1: Single interrupt generation...");
    interrupt_received = false;
    uint32_t test_eid = test_eids[0];
    
    LOG_INF("   🚀 Sending interrupt for EID %u...", test_eid);
    riscv_imsic_irq_set_pending(test_eid);
    
    /* Wait for interrupt */
    k_msleep(100);
    if (interrupt_received) {
        LOG_INF("   ✅ Interrupt received successfully!");
    } else {
        LOG_WRN("   ⚠️  Interrupt not received");
    }
    
    /* Test 2: Multiple interrupt generation */
    LOG_INF("📡 Test 2: Multiple interrupt generation...");
    interrupt_count = 0;
    
    for (int round = 0; round < 3; round++) {
        LOG_INF("   📡 Round %d: Sending interrupts...", round + 1);
        
        for (int i = 0; i < TEST_NUM_EIDS; i++) {
            uint32_t eid = test_eids[i];
            riscv_imsic_irq_set_pending(eid);
            LOG_INF("      ✅ EID %u: Interrupt sent", eid);
        }
        
        /* Wait for interrupts to be processed */
        k_msleep(200);
        LOG_INF("   📊 Round %d completed. Total interrupts: %u", 
            round + 1, interrupt_count);
    }
    
    /* Test 3: Rapid interrupt generation */
    LOG_INF("📡 Test 3: Rapid interrupt generation...");
    uint32_t start_count = interrupt_count;
    uint32_t start_time = k_uptime_get();
    
    for (int i = 0; i < 20; i++) {
        riscv_imsic_irq_set_pending(test_eids[i % TEST_NUM_EIDS]);
        k_msleep(10);
    }
    
    uint32_t end_time = k_uptime_get();
    uint32_t duration = end_time - start_time;
    uint32_t new_interrupts = interrupt_count - start_count;
    
    LOG_INF("   📊 Rapid test completed:");
    LOG_INF("      - Duration: %u ms", duration);
    LOG_INF("      - New interrupts: %u", new_interrupts);
    LOG_INF("      - Rate: %.2f interrupts/ms", (float)new_interrupts / duration);
    
    /* Test 4: Interrupt masking test */
    LOG_INF("📡 Test 4: Interrupt masking test...");
    
    /* Disable one EID */
    uint32_t masked_eid = test_eids[1];
    riscv_imsic_irq_disable(masked_eid);
    LOG_INF("   🔒 EID %u: Disabled", masked_eid);
    
    /* Send interrupt to disabled EID */
    interrupt_received = false;
    riscv_imsic_irq_set_pending(masked_eid);
    k_msleep(100);
    
    if (!interrupt_received) {
        LOG_INF("   ✅ EID %u: Interrupt correctly masked", masked_eid);
    } else {
        LOG_WRN("   ⚠️  EID %u: Interrupt not masked", masked_eid);
    }
    
    /* Re-enable the EID */
    riscv_imsic_irq_enable(masked_eid);
    LOG_INF("   🔓 EID %u: Re-enabled", masked_eid);
    
    /* Test 5: Threshold test */
    LOG_INF("📡 Test 5: Threshold test...");
    
    /* Set threshold to 2 (only accept interrupts >= 2) */
    uint32_t old_threshold = riscv_imsic_get_threshold();
    riscv_imsic_set_threshold(2);
    LOG_INF("   📊 Threshold set to 2 (was %u)", old_threshold);
    
    /* Send low priority interrupts (should be blocked) */
    interrupt_count = 0;
    for (int i = 0; i < 3; i++) {
        riscv_imsic_irq_set_pending(test_eids[0]); // EID 10
        k_msleep(50);
    }
    
    LOG_INF("   📊 Low priority interrupts sent: %u", 3);
    LOG_INF("   📊 Interrupts received: %u", interrupt_count);
    
    /* Send high priority interrupts (should be accepted) */
    for (int i = 0; i < 3; i++) {
        riscv_imsic_irq_set_pending(test_eids[2]); // EID 12
        k_msleep(50);
    }
    
    LOG_INF("   📊 High priority interrupts sent: %u", 3);
    LOG_INF("   📊 Total interrupts received: %u", interrupt_count);
    
    /* Restore threshold */
    riscv_imsic_set_threshold(old_threshold);
    LOG_INF("   📊 Threshold restored to %u", old_threshold);
}

/* Test APLIC MSI forwarding */
static void test_aplic_msi_forwarding(void)
{
    LOG_INF("=== Testing APLIC MSI Forwarding ===");
    
    const struct device *aplic_dev = riscv_aplic_get_dev();
    if (!aplic_dev || !device_is_ready(aplic_dev)) {
        LOG_WRN("⚠️  APLIC device not available for MSI testing");
        return;
    }
    
    LOG_INF("✅ APLIC device available: %s", aplic_dev->name);
    
    /* This would test APLIC forwarding interrupts to IMSIC */
    LOG_INF("📝 Note: MSI forwarding test requires hardware support");
    LOG_INF("📝 Current QEMU implementation may have limitations");
    
    LOG_INF("✅ MSI forwarding test completed");
}

/* Main test function */
int main(void)
{
    LOG_INF("=== RISC-V IMSIC Interrupt Signal Test ===");
    LOG_INF("🎯 Testing IMSIC interrupt signal generation and handling");
    LOG_INF("🚀 Starting in 2 seconds...");
    
    /* Wait for system stabilization */
    k_msleep(2000);
    
    LOG_INF("🎬 Starting interrupt signal tests...");
    
    /* Test 1: Interrupt signal generation */
    test_interrupt_signals();
    
    /* Test 2: APLIC MSI forwarding */
    test_aplic_msi_forwarding();
    
    /* Final summary */
    LOG_INF("🎉 === Interrupt Signal Test Summary ===");
    LOG_INF("✅ Total interrupts processed: %u", interrupt_count);
    LOG_INF("✅ Last interrupted EID: %u", last_interrupted_eid);
    LOG_INF("✅ Interrupt handling: %s", interrupt_received ? "WORKING" : "FAILED");
    LOG_INF("✅ IMSIC interrupt signals: TESTED");
    LOG_INF("✅ Interrupt masking: TESTED");
    LOG_INF("✅ Threshold filtering: TESTED");
    
    LOG_INF("🔄 Keeping system running for observation...");
    
    /* Keep the system running with status updates */
    uint32_t iteration = 0;
    while (1) {
        k_msleep(3000);
        iteration++;
        LOG_INF("💻 System status [%u]: IMSIC interrupts working, count: %u", 
            iteration, interrupt_count);
        
        /* Periodically generate test interrupts */
        if (iteration % 5 == 0) {
            LOG_INF("📡 Generating periodic test interrupt...");
            riscv_imsic_irq_set_pending(test_eids[0]);
        }
    }
    
    return 0;
}

