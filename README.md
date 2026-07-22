# ESP32-C3 FreeRTOS Bare-Metal Port

A from-scratch FreeRTOS port for the ESP32-C3 (RISC-V) built entirely without the ESP-IDF framework. **This project was created to understand the internal structure of FreeRTOS and to learn, step by step, what it means to port a real-time kernel to a new microcontroller.** By writing every line of the port layer, the boot code, and the peripheral drivers directly against the hardware registers, the project exposes the interfaces between the kernel, the architecture-specific port, and the board support layer — the exact boundaries that every FreeRTOS port must implement.

## Motivation

Porting FreeRTOS to a new chip is usually hidden behind vendor SDKs that provide a ready-made port. This project reverses that: it starts from the ESP32-C3 Technical Reference Manual, the RISC-V privileged specification, and the stock FreeRTOS kernel source, and builds the port from scratch. The goal is to make the porting process transparent and document the decisions every porter faces:

- How does FreeRTOS expect the hardware to deliver a tick interrupt?
- What happens between `vTaskStartScheduler()` and the first task running?
- How is a task's context saved and restored on a RISC-V core?
- What replaces the standard RISC-V CLINT/MTIME when the SoC doesn't have one?
- How does `portYIELD()` actually trigger a context switch in hardware?

The result is a working, flashable firmware that prints "Hello world!" over UART — not as the end goal, but as proof that the port correctly handles boot, interrupts, scheduling, and peripheral I/O.

## Hardware

- **Board**: any ESP32-C3 module (tested on a Beetle C3 and a generic C3 dev board)
- **Debug probe**: built-in USB-JTAG (VID:PID `0x303a:0x1001`) or external JTAG adapter + OpenOCD

## Prerequisites

| Tool | Version | Purpose |
|------|---------|---------|
| xPack RISC-V GCC | 15.2+ | Cross-compiler (`riscv-none-elf-gcc`) |
| esptool.py | ≥ 4.x | Flashing and image generation |
| Python | ≥ 3.8 | For esptool (virtualenv in `venv_esptool/`) |
| OpenOCD | ≥ 0.12 | Debugging (optional) |
| GNU Screen | any | Serial monitor (optional) |

### Installing the toolchain

```bash
# macOS (Homebrew)
brew install xpack-dev-tools/riscv-none-elf-gcc

# Or download manually from:
# https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases
```

Set `TOOLCHAIN_BIN` in the Makefile to point to your toolchain `bin/` directory.

### Python environment

```bash
python3 -m venv venv_esptool
source venv_esptool/bin/activate
pip install esptool
```

## Build & Run

```bash
# Clone and init the FreeRTOS kernel submodule (first time only)
git submodule update --init

# Build the firmware → build/freertos_esp32c3.bin
make all

# Load into RAM via esptool (no flash, fastest iteration cycle)
make run

# Flash the binary to the internal flash
make flash

# Open serial monitor (115200 8N1)
make monitor

# Clean all build artifacts
make clean
```

The default `PORT` is `/dev/cu.usbmodem*` — adjust in the Makefile if your device enumerates differently.

## Debugging

Two OpenOCD configuration files are provided in `debug/`:

```bash
# Terminal 1: start OpenOCD
openocd -f debug/esp_usb_jtag.cfg -f debug/esp32c3.cfg

# Terminal 2: connect GDB
make debug
```

`esp_usb_jtag.cfg` configures the built-in USB-JTAG adapter on the C3. `esp32c3.cfg` declares the RISC-V target with TAP ID `0x00005c25` and sets up a 16 KB work area at `0x40380000` (IRAM) for fast flash breakpoints.

## Project Structure

```
.
├── bare_metal_port/       ← FreeRTOS port layer (the core of this project)
│   ├── portmacro.h        → Critical sections, yield, type definitions
│   ├── port.c             → Scheduler start/end, ISR stack setup
│   ├── portContext.h      → Context save/restore macros (assembly)
│   ├── port_asm.S         → Trap handler, tick ISR, stack init, vector table entry
│   ├── port_systimer.c    → SYSTIMER periodic timer setup + interrupt matrix routing
│   ├── port_systimer.h    → SYSTIMER init declaration
│   ├── port_systimer_regs.h → SYSTIMER and interrupt matrix register map
│   └── freertos_risc_v_chip_specific_extensions.h → Chip capability flags (no CLINT, no MTIME)
├── bsp/                   ← Board Support Package
│   ├── boot.c             → Reset handler, BSS zeroing, WDT disable, SoC clock init
│   ├── mdk.h              → SoC base addresses, GPIO/SPI/WS2812 helpers, delay functions
│   └── linker_mdk.ld      → Linker script (iram 32K, dram 128K, vector alignment)
├── drivers/               ← Peripheral drivers (register-level, no HAL abstraction)
│   └── uart/
│       ├── uart_hal.h     → UART register struct, config types, baud rate divisor
│       ├── uart_hal.c     → Clock gating, baud setup, TX/RX with FIFO management
│       └── gpio.h         → IO_MUX register struct and pin configuration bits
├── common/
│   └── utils.h            → REG() and BIT() macros for MMIO access
├── FreeRTOS-Kernel/       → Stock FreeRTOS kernel (git submodule)
├── FreeRTOSConfig.h       → Kernel configuration (160 MHz, 1000 Hz tick, 40 KB heap)
├── main.c                 → Application entry point (UART hello world task)
├── debug/                 → OpenOCD configuration files
├── Documentation/         → ESP32-C3 TRM, RISC-V privileged spec, reference books
├── Makefile               → Build system
└── CLAUDE.md              → AI coding assistant guidance
```

## FreeRTOS Configuration

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| `configCPU_CLOCK_HZ` | 160 MHz | ESP32-C3 default after `soc_init()` |
| `configTICK_RATE_HZ` | 1000 Hz | 1 ms tick — good balance for responsiveness vs. overhead |
| `configMAX_PRIORITIES` | 5 | Keeps the ready-list bitmap within 32 bits for CLZ-based selection |
| `configTOTAL_HEAP_SIZE` | 40 KB | heap_4 allocator; sufficient for moderate task + queue workloads |
| `configMINIMAL_STACK_SIZE` | 100 words | 400 bytes per task minimum |
| `configUSE_PREEMPTION` | 1 | Enabled |
| `configUSE_TIME_SLICING` | 1 | Enabled |
| Memory allocator | `heap_4.c` | Standard FreeRTOS heap, supports free() coalescing |

## Further Reading

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — Detailed walkthrough of every subsystem with code references
- [`CLAUDE.md`](CLAUDE.md) — Instructions for AI coding assistants working in this repo
- [ESP32-C3 Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32-c3_technical_reference_manual_en.pdf) — All peripheral registers
- [RISC-V Privileged Specification](https://github.com/riscv/riscv-isa-manual/releases) — M-mode CSRs, trap handling, interrupt model
- [FreeRTOS Kernel Docs](https://www.freertos.org/FreeRTOS-kernel-detailed-documentation.html) — Kernel API and porting guide

## License

This project's original code is MIT-licensed. The FreeRTOS kernel submodule (`FreeRTOS-Kernel/`) is distributed under the MIT license by Amazon.com, Inc. The OpenOCD configuration files in `debug/` are derived from Espressif's GPL-2.0-licensed configurations.
