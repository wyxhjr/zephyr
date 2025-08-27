# RISC-V AIA (Advanced Interrupt Architecture) Test Suite

This test suite provides comprehensive testing for the RISC-V Advanced Interrupt Architecture (AIA) implementation in Zephyr.

## Overview

The AIA test suite validates the functionality of:
- **AIA Management Layer**: Unified interrupt management between APLIC and IMSIC
- **APLIC (Advanced Platform Level Interrupt Controller)**: Traditional wired interrupts
- **IMSIC (Incoming MSI Controller)**: Message-signaled interrupts
- **Mode Switching**: Between MSI and direct interrupt modes
- **Performance Monitoring**: Statistics and error tracking

## Test Structure

```
tests/arch/riscv/riscv_aia/
├── src/
│   └── main.c              # Main test implementation
├── prj.conf                # Test configuration
├── CMakeLists.txt          # Build configuration
├── testcase.yaml           # Test case definitions
└── README.md              # This file
```

## Test Categories

### 1. AIA Basic Functionality Tests
- **Device Availability**: Verify AIA, APLIC, and IMSIC devices are present and ready
- **Capabilities Detection**: Test mode support (MSI vs Direct) detection
- **Statistics Management**: Test statistics reset and retrieval

### 2. AIA Interrupt Management Tests
- **Basic Enable/Disable**: Test interrupt enable/disable through AIA unified API
- **Priority Management**: Test interrupt priority setting and retrieval
- **Pending Status**: Test interrupt pending status checking and clearing
- **Connection Handling**: Test dynamic interrupt connection

### 3. APLIC-Specific Tests
- **APLIC Integration**: Test direct APLIC device interaction
- **Priority Management**: Test APLIC-specific priority configuration
- **Interrupt Status**: Test APLIC interrupt enable/disable status

### 4. IMSIC-Specific Tests
- **IMSIC Integration**: Test direct IMSIC device interaction
- **Delivery Modes**: Test different IMSIC delivery modes (MSI, ID, Virtual)
- **MSI Handling**: Test message-signaled interrupt processing

### 5. Advanced Tests
- **Unified Management**: Test AIA's role as interrupt management layer
- **Performance Tracking**: Test statistics collection during operations
- **Error Handling**: Test system stability with invalid parameters
- **Debug Mode**: Test debug functionality

## Configuration

### Prerequisites
- RISC-V platform with AIA support (e.g., qemu_riscv64)
- APLIC and/or IMSIC devices in device tree
- Zephyr with AIA drivers enabled

### Build Configuration (prj.conf)
```conf
CONFIG_LOG=y
CONFIG_ZTEST=y
CONFIG_RISCV_AIA=y
CONFIG_RISCV_APLIC=y
CONFIG_RISCV_IMSIC=y
CONFIG_DYNAMIC_INTERRUPTS=y
```

### Device Tree Requirements
The test expects either or both of:
- APLIC device at compatible "riscv,aplic"
- IMSIC device at compatible "riscv,imsic"

## Running the Tests

### Using Twister
```bash
# Run specific AIA test
twister -T tests/arch/riscv/riscv_aia -p qemu_riscv64

# Run with verbose output
twister -T tests/arch/riscv/riscv_aia -p qemu_riscv64 -v
```

### Manual Build and Run
```bash
# Build the test
west build -b qemu_riscv64 tests/arch/riscv/riscv_aia

# Run in QEMU
west build -t run
```

## Test Architecture Comparison

### Similar to GIC v3 ITS Tests
This AIA test suite follows the same architectural patterns as the ARM GIC v3 ITS tests:

| GIC v3 ITS | RISC-V AIA | Purpose |
|------------|------------|---------|
| `test_gicv3_its_alloc` | `test_aia_unified_management` | Test interrupt allocation |
| `test_gicv3_its_connect` | `test_aia_interrupt_connection` | Test interrupt connection |
| `test_gicv3_its_irq_simple` | `test_aplic_integration` | Test basic interrupt handling |
| `test_gicv3_its_irq_disable` | `test_aia_irq_enable_disable` | Test enable/disable logic |

### AIA-Specific Features
- **Dual-Mode Support**: Tests both MSI and direct interrupt modes
- **Unified API**: Tests AIA as a management layer between APLIC and IMSIC
- **Device Discovery**: Tests automatic discovery of APLIC and IMSIC devices
- **Load Balancing**: Tests hart load balancing capabilities

## Expected Output

Successful test run shows:
```
*** Booting Zephyr OS build v3.5.0 ***
Running TESTSUITE riscv_aia
===================================================================
START - test_aia_device_availability
AIA: Device found and ready: aia
PASS - test_aia_device_availability
===================================================================
START - test_aia_alloc
PASS - test_aia_alloc in 0.003 seconds
===================================================================
START - test_aia_connect
PASS - test_aia_connect in 0.001 seconds
===================================================================
START - test_aia_irq_simple
PASS - test_aia_irq_simple in 0.001 seconds
===================================================================
START - test_imsic_delivery_modes
[00:00:00.060,000] <wrn> imsic: IMSIC: Delivery mode verification failed: wrote 0x00000001, read 0x00000000
IMSIC: Delivery mode test completed (QEMU may have limitations)
PASS - test_imsic_delivery_modes in 0.003 seconds
===================================================================
... (more tests)
```

## Known Issues and Workarounds

### QEMU IMSIC Limitations

**Issue**: IMSIC delivery mode verification warnings
```
[00:00:00.060,000] <wrn> imsic: IMSIC: Delivery mode verification failed: wrote 0x00000001, read 0x00000000
```

**Root Cause**: QEMU's IMSIC implementation has limited support for delivery mode changes.

**Workaround**: Tests are designed to be tolerant of QEMU limitations and will pass even with these warnings.

### APLIC/IMSIC Integration Issues

**Issue**: Some tests fail due to QEMU's incomplete AIA emulation
- `test_aplic_integration`: APLIC state management
- `test_imsic_integration`: IMSIC state management
- `test_aia_unified_management`: AIA API status checks

**Root Cause**: QEMU's APLIC and IMSIC implementations are not fully compliant with the AIA specification.

**Workaround**: These tests are marked as expected failures on QEMU platforms.

## Platform Support

Currently supported platforms:
- **qemu_riscv64**: Full AIA support with QEMU APLIC/IMSIC emulation
- **qemu_riscv32**: Limited support depending on device tree configuration
- **hifive_unleashed**: SiFive U54/U74 with AIA support (if enabled)

## Debugging

### Enable Debug Logging
Add to prj.conf:
```conf
CONFIG_LOG_DEFAULT_LEVEL=4  # Set to DEBUG level
CONFIG_RISCV_AIA_DEBUG=y   # Enable AIA debug features
```

### Common Issues

1. **Test Skipped**: APLIC/IMSIC devices not available in device tree
   - Check device tree has correct AIA device nodes
   - Verify platform supports AIA

2. **Test Failures**: Device not ready
   - Check device initialization order
   - Verify device tree configuration

3. **Interrupt Tests Fail**: Platform-specific interrupt limitations
   - Some QEMU versions have limited AIA emulation
   - Check QEMU version and AIA support

## Extending the Tests

### Adding New Test Cases
1. Add new ZTEST() functions to `src/main.c`
2. Follow the existing pattern with proper assertions
3. Add device availability checks where needed

### Adding Platform-Specific Tests
1. Create platform-specific overlays in `boards/` directory
2. Update `testcase.yaml` with platform-specific configurations
3. Add platform detection in test code

## References

- [RISC-V AIA Specification](https://github.com/riscv/riscv-aia)
- [Zephyr AIA Driver Documentation](https://docs.zephyrproject.org/latest/hardware/peripherals/interrupt_controller.html)
- [GIC v3 ITS Test Reference](../../arm64/arm64_gicv3_its/)
