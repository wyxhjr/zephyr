#ifndef ZEPHYR_INCLUDE_DRIVERS_RISCV_APLIC_H_
#define ZEPHYR_INCLUDE_DRIVERS_RISCV_APLIC_H_

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Basic interrupt control */
void riscv_aplic_irq_enable(uint32_t irq);
void riscv_aplic_irq_disable(uint32_t irq);
int riscv_aplic_irq_is_enabled(uint32_t irq);
void riscv_aplic_set_priority(uint32_t irq, uint32_t prio);
int riscv_aplic_irq_set_affinity(uint32_t irq, uint32_t cpumask);
void riscv_aplic_irq_set_pending(uint32_t irq);
unsigned int riscv_aplic_get_irq(void);
const struct device *riscv_aplic_get_dev(void);

/* Advanced interrupt management */
enum riscv_aplic_trigger_type {
	RISCV_APLIC_TRIGGER_EDGE_RISING = 4,
	RISCV_APLIC_TRIGGER_EDGE_FALLING = 5,
	RISCV_APLIC_TRIGGER_LEVEL_HIGH = 6,
	RISCV_APLIC_TRIGGER_LEVEL_LOW = 7,
};

int riscv_aplic_irq_set_trigger_type(uint32_t irq, enum riscv_aplic_trigger_type type);
int riscv_aplic_irq_get_trigger_type(uint32_t irq);
int riscv_aplic_hart_set_threshold(uint32_t hart_id, uint32_t threshold);
uint32_t riscv_aplic_hart_get_threshold(uint32_t hart_id);

/* MSI mode support */
bool riscv_aplic_is_msi_mode_enabled(void);
int riscv_aplic_configure_source_msi(uint32_t irq, uint32_t target_hart, uint32_t target_guest);
int riscv_aplic_send_msi(uint32_t target_hart, uint32_t target_guest, uint32_t irq);

/* Statistics and diagnostics */
struct riscv_aplic_irq_stats {
	uint32_t count;           /* Interrupt count */
	uint32_t last_cpu;        /* Last CPU that handled this IRQ */
	uint32_t affinity_mask;   /* CPU affinity mask */
	uint32_t trigger_type;    /* Trigger type */
	uint8_t priority;         /* Interrupt priority */
	bool enabled;             /* Enable state */
};

int riscv_aplic_get_irq_stats(uint32_t irq, struct riscv_aplic_irq_stats *stats);
uint32_t riscv_aplic_get_total_interrupts(void);
uint32_t riscv_aplic_get_msi_interrupts_sent(void);
void riscv_aplic_reset_stats(void);

/* Debug: Global variable to check if aplic_init was called */
extern volatile uint32_t aplic_init_called;

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_RISCV_APLIC_H_ */
