# RISC-V IMSIC (Incoming MSI Controller) Driver

## Overview

The RISC-V IMSIC (Incoming MSI Controller) driver provides support for the RISC-V AIA (Advanced Interrupt Architecture) specification's IMSIC component. The IMSIC handles incoming MSI (Message Signaled Interrupts) and provides per-hart interrupt management capabilities.

## Features

- **MSI Support**: Full support for Message Signaled Interrupts
- **Per-Hart Management**: Independent interrupt management for each CPU hart
- **Virtualization Support**: Guest ID support for virtualized environments
- **Priority Management**: Configurable interrupt thresholds
- **Endianness Support**: Both little-endian and big-endian register access
- **Device Tree Integration**: Full device tree binding support

## Architecture

The IMSIC is part of the RISC-V AIA specification and works in conjunction with the APLIC (Advanced Platform-Level Interrupt Controller). The IMSIC receives MSIs from the APLIC and manages them per hart.

### Key Components

- **EID (External Interrupt ID)**: 64 interrupt sources (EID 0-63)
- **Delivery Modes**: OFF, MSI, ID, and Virtual modes
- **Threshold Control**: Priority-based interrupt filtering
- **Guest Support**: Multi-guest virtualization support

## Configuration

### Kconfig Options

```kconfig
CONFIG_RISCV_IMSIC=y                    # Enable IMSIC support
CONFIG_RISCV_IMSIC_INIT_PRIORITY=40    # Initialization priority
CONFIG_RISCV_IMSIC_MAX_EID=63          # Maximum EID (0-63)
CONFIG_RISCV_IMSIC_MAX_PRIORITY=7      # Maximum priority (0-7)
CONFIG_RISCV_IMSIC_DEBUG=y             # Enable debug output
```

### Device Tree Example

```dts
imsic@24000000 {
    compatible = "riscv,imsic";
    reg = <0x0 0x24000000 0x0 0x4000>;
    riscv,hart-id = <0>;
    riscv,guest-id = <0>;
    riscv,max-eid = <63>;
    riscv,max-priority = <7>;
    riscv,big-endian = <false>;
    interrupt-controller;
    #interrupt-cells = <1>;
    interrupts = <9>;
    interrupt-parent = <&cpu0_intc>;
};
```

## API Usage

### Basic Interrupt Control

```c
#include <zephyr/drivers/interrupt_controller/riscv_imsic.h>

/* Enable/disable interrupts */
riscv_imsic_irq_enable(1);      // Enable EID 1
riscv_imsic_irq_disable(1);     // Disable EID 1

/* Check interrupt status */
if (riscv_imsic_irq_is_enabled(1)) {
    // EID 1 is enabled
}

/* Set/clear pending interrupts */
riscv_imsic_irq_set_pending(1);     // Set EID 1 pending
riscv_imsic_irq_clear_pending(1);   // Clear EID 1 pending
```

### Delivery Mode Control

```c
/* Set delivery mode */
riscv_imsic_set_delivery_mode(RISCV_IMSIC_DELIVERY_MODE_MSI);

/* Get current delivery mode */
enum riscv_imsic_delivery_mode mode = riscv_imsic_get_delivery_mode();
```

### Threshold Control

```c
/* Set interrupt threshold */
riscv_imsic_set_threshold(3);    // Only accept interrupts with priority >= 3

/* Get current threshold */
uint32_t threshold = riscv_imsic_get_threshold();
```

### Device Management

```c
/* Get IMSIC device */
const struct device *imsic_dev = riscv_imsic_get_dev();

/* Get hart and guest IDs */
int hart_id = riscv_imsic_get_hart_id(imsic_dev);
int guest_id = riscv_imsic_get_guest_id(imsic_dev);
```

## MSI Mode Integration

The IMSIC works with the APLIC in MSI mode to provide a complete MSI solution:

```c
#include <zephyr/drivers/interrupt_controller/riscv_aplic.h>

/* Check if APLIC is in MSI mode */
if (riscv_aplic_is_msi_mode_enabled()) {
    /* Configure source for MSI delivery */
    riscv_aplic_configure_source_msi(1, 0, 0);  // IRQ 1 -> hart 0, guest 0
    
    /* Send MSI */
    riscv_aplic_send_msi(0, 0, 1);  // Send IRQ 1 to hart 0, guest 0
}
```

## Testing

### Build and Run IMSIC Test

```bash
# Build with IMSIC support
west build -b qemu_riscv64 samples/drivers/aplic_test -- -DCONF_FILE=prj_imsic.conf

# Run IMSIC test
west build -t run
```

### Test Features

The IMSIC test verifies:
- Device discovery and initialization
- Interrupt enable/disable functionality
- Delivery mode configuration
- Threshold control
- MSI mode integration with APLIC
- Interrupt handling

## Error Handling

The driver provides comprehensive error handling:

```c
int result = riscv_imsic_set_delivery_mode(RISCV_IMSIC_DELIVERY_MODE_MSI);
if (result < 0) {
    switch (result) {
        case -EINVAL:
            // Invalid delivery mode
            break;
        case -ENODEV:
            // Device not found
            break;
        default:
            // Other error
            break;
    }
}
```

## Performance Considerations

- **Interrupt Latency**: IMSIC provides low-latency interrupt delivery
- **Memory Access**: Register access is optimized for minimal overhead
- **SMP Support**: Full multi-core interrupt management
- **Cache Coherency**: Proper memory barriers for SMP systems

## Limitations

- **EID Range**: Limited to 64 external interrupt IDs (0-63)
- **Priority Levels**: 8 priority levels (0-7)
- **Hardware Dependency**: Requires RISC-V AIA compliant hardware
- **Guest Support**: Limited to 64 guest IDs per hart

## Future Enhancements

- **Extended EID Support**: Support for more than 64 EIDs
- **Advanced Priority**: More granular priority control
- **Performance Counters**: Interrupt performance monitoring
- **Power Management**: Runtime power state management

## Troubleshooting

### Common Issues

1. **Device Not Found**: Check device tree binding and compatible strings
2. **MSI Mode Not Working**: Verify IMSIC devices are present and configured
3. **Interrupts Not Received**: Check delivery mode and threshold settings
4. **SMP Issues**: Ensure proper hart ID configuration

### Debug Output

Enable debug output with `CONFIG_RISCV_IMSIC_DEBUG=y` to see detailed driver operation logs.

## References

- [RISC-V AIA Specification](https://github.com/riscv/riscv-aia)
- [Zephyr RTOS Documentation](https://docs.zephyrproject.org/)
- [RISC-V Privileged Architecture](https://riscv.org/specifications/privileged-isa/)
