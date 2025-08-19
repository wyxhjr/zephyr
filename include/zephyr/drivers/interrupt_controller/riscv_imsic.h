#ifndef ZEPHYR_INCLUDE_DRIVERS_RISCV_IMSIC_H_
#define ZEPHYR_INCLUDE_DRIVERS_RISCV_IMSIC_H_

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IMSIC delivery modes */
enum riscv_imsic_delivery_mode {
	RISCV_IMSIC_DELIVERY_MODE_OFF = 0,      /* Interrupts disabled */
	RISCV_IMSIC_DELIVERY_MODE_MSI = 1,      /* MSI mode */
	RISCV_IMSIC_DELIVERY_MODE_ID = 2,       /* ID mode */
	RISCV_IMSIC_DELIVERY_MODE_VIRTUAL = 3,  /* Virtual mode */
};

/* Basic interrupt control */
void riscv_imsic_irq_enable(uint32_t eid);
void riscv_imsic_irq_disable(uint32_t eid);
int riscv_imsic_irq_is_enabled(uint32_t eid);
void riscv_imsic_irq_set_pending(uint32_t eid);
void riscv_imsic_irq_clear_pending(uint32_t eid);

/* Delivery mode control */
int riscv_imsic_set_delivery_mode(enum riscv_imsic_delivery_mode mode);
enum riscv_imsic_delivery_mode riscv_imsic_get_delivery_mode(void);

/* Threshold control */
int riscv_imsic_set_threshold(uint32_t threshold);
uint32_t riscv_imsic_get_threshold(void);

/* Device management */
const struct device *riscv_imsic_get_dev(void);
int riscv_imsic_get_hart_id(const struct device *dev);
int riscv_imsic_get_guest_id(const struct device *dev);

/* MSI specific functions */
int riscv_imsic_send_msi(uint32_t target_hart, uint32_t target_guest, uint32_t eid);
int riscv_imsic_receive_msi(uint32_t eid, uint32_t *source_hart, uint32_t *source_guest);

/* Statistics and diagnostics */
struct riscv_imsic_stats {
	uint32_t total_interrupts;    /* Total interrupts received */
	uint32_t msi_interrupts;      /* MSI interrupts received */
	uint32_t id_interrupts;       /* ID interrupts received */
	uint32_t virtual_interrupts;  /* Virtual interrupts received */
	uint32_t threshold_rejected;  /* Interrupts rejected due to threshold */
};

int riscv_imsic_get_stats(struct riscv_imsic_stats *stats);
void riscv_imsic_reset_stats(void);

/* Debug: Global variable to check if imsic_init was called */
extern volatile uint32_t imsic_init_called;

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_RISCV_IMSIC_H_ */
