# ESP32-C3 FreeRTOS Port — Architecture Documentation

This document describes the complete system architecture of the bare-metal FreeRTOS port for the ESP32-C3. It is intended to be read alongside the source code; every section references the relevant files and explains the design decisions behind them.

---

## 1. System Overview

The system is composed of four layers, each with a well-defined boundary:

```
┌─────────────────────────────────────────┐
│  Application (main.c)                    │  ← tasks, user logic
├─────────────────────────────────────────┤
│  FreeRTOS Kernel (FreeRTOS-Kernel/)      │  ← scheduler, IPC, timers, heap
├──────────────────┬──────────────────────┤
│  Port Layer      │  BSP + Drivers       │  ← arch-specific + board-specific
│  bare_metal_port/│  bsp/ drivers/       │
└──────────────────┴──────────────────────┘
```

The **port layer** (`bare_metal_port/`) answers the question: "What does FreeRTOS need from the hardware to run?" The **BSP and drivers** answer: "What does this specific board provide, and how do we talk to its peripherals?" The FreeRTOS kernel source is an untouched submodule — the port adapts the kernel to the ESP32-C3 without modifying kernel code.

### What makes this port different from the standard ESP-IDF port

ESP-IDF provides its own FreeRTOS port buried inside the SDK. This project replaces every IDF-provided component:

| Component | ESP-IDF | This project |
|-----------|---------|--------------|
| Tick source | ESP timer library → SYSTIMER | Direct SYSTIMER register manipulation |
| Interrupt dispatch | IDF interrupt allocator | Custom trap handler (`port_asm.S`) |
| Boot sequence | IDF 2nd stage bootloader | `bsp/boot.c` — single-file boot |
| Peripheral drivers | IDF HAL + LL layers | Direct MMIO register structs |
| Clock init | IDF clock tree API | `soc_init()` in `bsp/mdk.h` |
| Linker script | IDF-generated | Hand-written `bsp/linker_mdk.ld` |

The result is a system where every line of code between reset and `main()` is visible and auditable.

---

## 2. Boot Sequence

**Files**: `bsp/boot.c:3-44`, `bsp/linker_mdk.ld`, `bsp/mdk.h:107-121`

### 2.1 Reset Vector

The linker script (`bsp/linker_mdk.ld:12`) sets the entry point to `_reset`. The vector table is placed in a dedicated `.vectors` section aligned to 256 bytes — the alignment required by the RISC-V `mtvec` register in vector mode.

`bsp/boot.c:9-20` — The vector table is emitted via inline assembly. All 32 entries (one per possible exception/interrupt cause) jump to `freertos_risc_v_trap_handler`, the unified trap handler in `port_asm.S`. This is a single-vector strategy: every trap enters the same handler, which then dispatches based on `mcause`.

### 2.2 Reset handler (`_reset`)

`bsp/boot.c:23-30` — The reset sequence:
1. Load the global pointer (`gp`) from the linker-defined `__global_pointer$` symbol. The GP-relative addressing optimization reduces code size for accesses to `.sdata` and `.sbss`.
2. Set the stack pointer (`sp`) to `_eram`, the top of DRAM (defined in `bsp/linker_mdk.ld:7`). The stack grows downward.
3. Jump to `_start`.

### 2.3 C entry (`_start`)

`bsp/boot.c:32-44`:
1. **Zero BSS** — Iterates from `_sbss` to `_ebss` (linker-defined boundaries at `bsp/linker_mdk.ld:43,50`) and writes zero. This must happen before any C code with static variables runs.
2. **Set mtvec** — Writes the vector table address to the `mtvec` CSR. From this point forward, any exception or interrupt will be handled by FreeRTOS's trap handler.
3. **Disable watchdogs** — `wdt_disable()` (`bsp/mdk.h:85-99`) disables the RTC WDT, the super WDT, and both timer group WDTs. Missing this step causes the chip to reset ~5 seconds after boot.
4. **Initialize SoC clocks** — `soc_init()` (`bsp/mdk.h:107-121`) sets the CPU clock to 160 MHz via the SYSTEM registers and calls the ROM function `ets_update_cpu_frequency`.
5. **Call `main()`** — Hands control to the application.

---

## 3. Memory Layout

**File**: `bsp/linker_mdk.ld`

The ESP32-C3 has separate instruction and data buses. The linker script defines three regions:

| Region | Physical Start | Size | Contents |
|--------|---------------|------|----------|
| `iache` | `0x4037C000` | 16 KB | Not used in this project |
| `iram` | `0x4037C000 + 16K + 256` = `0x40380400` | 32 KB | `.vectors` (256B aligned) + `.text` + `.text.*` |
| `dram` | `0x3FC80000 + 32K` = `0x3FC88000` | 128 KB | `.data`, `.sdata`, `.rodata`, `.bss`, `.sbss` |

Key linker symbols exported to the code:
- `_eram` (`bsp/linker_mdk.ld:7`) — Top of DRAM; used as the initial stack pointer and ISR stack top.
- `__freertos_irq_stack_top` (`bsp/linker_mdk.ld:9-10`) — Set equal to `_eram`. After the scheduler starts, the boot stack becomes the ISR stack. See `port.c:64-66` where `xISRStackTop` is initialized from this symbol.
- `__global_pointer$` (`bsp/linker_mdk.ld:25`) — GP base at `.data + 0x800`, giving ±2 KB GP-relative addressing range.
- `_sbss`, `_ebss` — BSS boundaries used by the boot zeroing loop.

The `.vectors` section is placed at the beginning of IRAM with `KEEP(*(.vectors))` to prevent the linker from garbage-collecting it.

---

## 4. The Port Layer — RISC-V Specific Implementation

This is the heart of the project. These files implement everything FreeRTOS needs from the hardware to schedule tasks.

### 4.1 Type System and Critical Sections

**File**: `bare_metal_port/portmacro.h`

**Stack and base types** (`portmacro.h:49-57`): All port types are 32-bit (`uint32_t`/`int32_t`), matching the RV32 architecture. `TickType_t` is 32-bit, and `portTICK_TYPE_IS_ATOMIC` is set to 1 — on a 32-bit core, reads of the tick count don't need critical section protection.

**Critical sections** (`portmacro.h:100-121`): The ESP32-C3 runs all code in Machine mode (M-mode). Interrupts are globally enabled/disabled through the MIE bit (bit 3) of the `mstatus` CSR:
- `portDISABLE_INTERRUPTS()` → `csrc mstatus, 8` (clear bit 3)
- `portENABLE_INTERRUPTS()` → `csrs mstatus, 8` (set bit 3)

Critical sections use a nesting counter (`xCriticalNesting`, defined in `port.c:85`). Interrupts are only re-enabled when the counter reaches zero. The counter is initialized to `0xaaaaaaaa` — deliberately non-zero — so interrupts cannot be accidentally enabled before the scheduler starts.

**portYIELD** (`portmacro.h:83`): Emits the `ecall` instruction, which raises an environment-call exception (mcause = 11). The trap handler in `port_asm.S:307-309` catches this specific cause code and calls `vTaskSwitchContext()`.

**Optimised task selection** (`portmacro.h:129-142`): With 5 priority levels, the ready list fits in a single 32-bit bitmap. `portGET_HIGHEST_PRIORITY` uses `__builtin_clz()` (count leading zeros) for O(1) selection.

**Stack** (`portmacro.h:72-78`): `portSTACK_GROWTH = -1` (downward). Alignment is 16 bytes for RV32I. The port supports the RV32E ABI (reduced register set) with 8-byte alignment if `__riscv_32e` is defined.

### 4.2 Scheduler Lifecycle

**File**: `bare_metal_port/port.c`

**Starting the scheduler** (`port.c:115-145`):
1. Assert ISR stack alignment (`portBYTE_ALIGNMENT_MASK`).
2. If `configISR_STACK_SIZE_WORDS` is defined, fill the ISR stack with `0xee` as a sentinel for overflow detection. (This project uses the linker-script method, so this path is not compiled.)
3. Call `portSetupSystimerInterrupt()` — configure the hardware tick source (see §5).
4. Call `xPortStartFirstTask()` (assembly, in `port_asm.S:172-216`) — this never returns.

**xPortStartFirstTask** (`port_asm.S:172-216`):
1. Load the current TCB's stack pointer.
2. Restore all RISC-V registers from the task's stack frame (x1, x5-x31).
3. Set the MIE bit in `mstatus` before returning — the first task starts with interrupts enabled. The comment at line 179 notes "returns with ret not eret" — this is deliberate; the first task is entered via a normal function return, not an exception return.
4. Restore `xCriticalNesting` for the task.
5. `ret` — pops the "return address" (initially set to the task function) from the stack and jumps to it.

**Ending the scheduler** (`port.c:148-154`): `vPortEndScheduler()` is a no-op infinite loop. There's no OS to return to in a bare-metal system.

### 4.3 Context Management

**File**: `bare_metal_port/portContext.h`

This header defines macros (not functions) for saving and restoring the RISC-V context. Being macros, they are inlined into every trap handler, avoiding function call overhead during context switches.

**portcontextSAVE_CONTEXT_INTERNAL** (`portContext.h:50-90`):
1. Allocates `portCONTEXT_SIZE` (32 × 4 = 128 bytes) on the stack.
2. Saves registers x1, x5-x31 to known offsets. x1 (ra) goes to offset 2, mstatus to offset 1. The register at offset 0 is `mepc` (the exception return address), written by the specific exception/interrupt macros.
3. Saves `xCriticalNesting` to offset 30 — this is per-task critical nesting, allowing nested critical sections to survive context switches.
4. Saves `mstatus` via `csrr t0, mstatus`.
5. Stores the new stack pointer into `pxCurrentTCB->pxTopOfStack`.

**Exception vs. interrupt save** (`portContext.h:93-109`): The only difference is how `mepc` is handled:
- Exception: `mepc + 4` is saved (so the return skips the faulting instruction).
- Interrupt: `mepc` is saved unmodified (so the interrupted instruction is re-executed).

After saving, both macros switch to the ISR stack (`xISRStackTop`) — interrupts are never serviced on a task's stack.

**portcontextRESTORE_CONTEXT** (`portContext.h:112-162`):
1. Loads the next task's stack pointer from `pxCurrentTCB`.
2. Restores `mepc` from the stack frame (this is the address `mret` will jump to).
3. Restores `mstatus` to register t3 first (not t0 — note the comment at lines 122-123 warns that t3 is used because other restore macros may clobber t0).
4. Restores `xCriticalNesting` for the incoming task.
5. Restores all GP registers.
6. Deallocates the frame and executes `mret` — the instruction that simultaneously jumps to `mepc` and restores the previous privilege mode (always M-mode in this system).

### 4.4 Assembly Trap Handling

**File**: `bare_metal_port/port_asm.S`

**Stack initialization** (`port_asm.S:154-169` — `pxPortInitialiseStack`):
Creates the initial stack frame for a new task. The frame layout (documented in the block comment at lines 99-153):
```
Offset 0:  pxCode (task function pointer)
Offset 1:  mstatus (initial value: 0x188 << 4 = 0x1880 → MPIE set)
Offset 2:  xTaskReturnAddress (default 0, catches tasks that return)
...
Offset 8:  pvParameters (a0 = first argument)
...
Offset 30: 0 (xCriticalNesting)
```

The mstatus value `0x1880` has the MPIE bit (bit 7) set, so when `mret` is executed, MIE becomes 1 and the task runs with interrupts enabled.

**Unified trap handler** (`port_asm.S:267-318` — `freertos_risc_v_trap_handler`):
This is the single entry point for all exceptions and interrupts. The flow:

```
Trap → save context → read mcause
                          │
            ┌─────────────┴─────────────┐
            │ msb set?                   │
            ▼ (yes, interrupt)           ▼ (no, exception)
    asynchronous_interrupt          synchronous_exception
            │                              │
    ┌───────┴────────┐              ┌──────┴──────┐
    │ mcause ==      │              │ mcause ==   │
    │ 0x80000001?    │              │ 11 (ecall)? │
    ├───yes──┬───no──┤              ├──yes──┬─no──┤
    │        │        │              │       │      │
    ▼        ▼        ▼              ▼       ▼      ▼
  SYSTIMER  app     app          context   app    (none)
  handler   int     int          switch    exc
            hndlr   hndlr
```

The key dispatch logic is at `port_asm.S:288-292`:
```asm
addi t0, x0, 1
slli t0, t0, __riscv_xlen - 1    → 0x80000000 (MSB set mask)
addi t1, t0, 1                    → 0x80000001 (SYSTIMER interrupt ID)
bne a0, t1, application_interrupt_handler
```

This checks if `mcause` is exactly `0x80000001` — the value produced when interrupt line 1 fires (the line the SYSTIMER is routed to via the interrupt matrix). If it matches, the SYSTIMER interrupt is cleared and the tick is incremented.

**Tick interrupt handler** (`port_asm.S:255-263` — `freertos_risc_v_mtimer_interrupt_handler`):
A separate entry point that also services the tick. This exists for code that may want to call the timer handler directly (it's declared global). It uses the `CLEAR_SYSTIMER_INTERRUPT` macro from `portContext.h:81-86`.

**Weak application handlers** (`port_asm.S:219-231`): `freertos_risc_v_application_exception_handler` and `freertos_risc_v_application_interrupt_handler` are declared `.weak`. By default, they copy CSRs into registers for debugger inspection and spin forever (`j .`). Applications can override them to handle custom interrupts.

### 4.5 Chip-Specific Extensions

**File**: `bare_metal_port/freertos_risc_v_chip_specific_extensions.h`

This file declares what the SoC does (or does not) provide:

```c
#define portasmHAS_SIFIVE_CLINT   0   // No SiFive CLINT
#define portasmHAS_MTIME          0   // No standard mtime CSR
#define portasmADDITIONAL_CONTEXT_SIZE 0  // No FPU/VPU registers to save
```

All three save/restore/update macros are empty. The absence of MTIME is the reason `port_systimer.c` exists.

---

## 5. The Tick Source: SYSTIMER

**Files**: `bare_metal_port/port_systimer.c`, `bare_metal_port/port_systimer.h`, `bare_metal_port/port_systimer_regs.h`

This is the most hardware-specific part of the port. The ESP32-C3 lacks the standard RISC-V CLINT with `mtime`/`mtimecmp` registers. Instead, it has a SYSTIMER peripheral — a 52-bit counter running at 40 MHz with two independent timer units, each capable of driving three compare targets.

### 5.1 Why SYSTIMER?

Standard RISC-V chips provide `mtime` (a 64-bit counter incremented at a constant frequency) and `mtimecmp` (a compare register that triggers an interrupt when `mtime ≥ mtimecmp`). FreeRTOS's generic RISC-V port expects these. The ESP32-C3 implements neither. The SYSTIMER provides equivalent functionality through a different register interface.

### 5.2 Registers

**File**: `bare_metal_port/port_systimer_regs.h`

Three peripheral blocks are used:
- **SYSTIMER** at `0x60023000` — Counter, compare, interrupt control registers.
- **Interrupt Matrix** at `0x600C2000` — Routes peripheral interrupt signals to CPU interrupt lines.
- **System Control** at `0x600C0000` — Clock gating and reset for the SYSTIMER peripheral.

The register map is documented with TRM references in the file header comments.

### 5.3 Initialization sequence

`port_systimer.c:23-75` — `portSetupSystimerInterrupt()`:

**Phase 1: SYSTIMER peripheral setup** (lines 27-55):
1. Enable SYSTIMER clock via `SYSTEM_PERIP_CLK_EN0_REG`, toggle reset.
2. Configure Unit 0: enable counter, enable stall on CPU0 halt (for debug), set counter to 0.
3. Configure Target 0: use Unit 0 as source, set period to `40,000,000 / configTICK_RATE_HZ` (e.g., 40,000 for a 1 ms tick), enable periodic mode.
4. Load the configuration, clear pending interrupts, enable interrupt output.

**Phase 2: Interrupt matrix routing** (lines 57-74):
1. Disable global interrupts (clear MIE in mstatus) while remapping.
2. Write `SYSTIMER_CPU_LINE` (value 1) to the SYSTIMER Target 0 interrupt map register — this routes the signal to CPU line 1.
3. Set priority for CPU line 1 to `SYSTIMER_PRIORITY` (2).
4. Unmask CPU line 1 in the CPU interrupt enable register.
5. Memory fence, then re-enable global interrupts.

After this function returns, the SYSTIMER fires every tick period on CPU interrupt line 1. The trap handler detects `mcause == 0x80000001` and calls `xTaskIncrementTick()`.

---

## 6. BSP: Board Support Package

### 6.1 SoC Hardware Abstraction

**File**: `bsp/mdk.h`

This header provides:
- **Base addresses** for all ESP32-C3 peripherals (lines 16-57) — GPIO, UART, SPI, SYSTIMER, timers, AES, SHA, etc.
- **BIT and REG macros** (lines 13-14) — `BIT(x)` creates a bitmask, `REG(x)` casts an address to a `volatile uint32_t*` for MMIO. (These are also defined in `common/utils.h`; the duplication exists because `mdk.h` was originally a standalone BSP.)
- **Timing utilities** (`systick()`, `uptime_us()`, `delay_us()`, `delay_ms()`) — Read the 52-bit SYSTIMER counter. `systick()` triggers a latch (bit 30 of the LOAD register, TRM §10.5), then reads the 52-bit value as two 32-bit halves.
- **GPIO API** (`gpio_output()`, `gpio_write()`, `gpio_toggle()`, `gpio_input()`, `gpio_read()`) — Direct IO_MUX and GPIO register manipulation.
- **SPI bit-banging API** and **WS2812 LED driver** — Included for convenience.
- **Watchdog disable** (`wdt_disable()`) — Disables all four watchdogs on the ESP32-C3.
- **SoC clock init** (`soc_init()`) — Configures the CPU PLL for 160 MHz operation and calls the ROM function to update frequency-dependent timing.
- **WiFi MAC address** reading from eFUSE.

### 6.2 Boot File

**File**: `bsp/boot.c` — Described in detail in §2 (Boot Sequence).

---

## 7. Peripheral Drivers

### 7.1 Design Pattern

All drivers follow the same pattern:
1. A register layout struct (`typedef struct { volatile uint32_t reg0; ... }`) that mirrors the peripheral's address space.
2. A base address cast: `#define PERIPH (*(volatile periph_reg_t *)PERIPH_BASE_ADDRESS)`.
3. Configuration bit definitions as preprocessor constants.
4. Functions that read-modify-write the struct fields directly.

No dynamic allocation, no interrupt registration, no callback system — drivers are synchronous and blocking.

### 7.2 UART Driver

**Files**: `drivers/uart/uart_hal.h`, `drivers/uart/uart_hal.c`, `drivers/uart/gpio.h`

**Register map** (`uart_hal.h:42-76`): A complete struct mapping all UART registers from `0x0000` (FIFO) to `0x0080` (ID register), verified against the ESP32-C3 TRM v1.4. The struct is overlaid at `0x60000000` (UART0 base).

**Configuration types** (`uart_hal.h:8-31`):
- `uart_port_t` — UART0 or UART1.
- `uart_sclk_t` — Clock source selection (APB 80 MHz, XTAL 40 MHz, or RTC ~17.5 MHz). Despite the enum labels, `get_clk_freq()` in `uart_hal.c:16` currently returns 40 MHz for both APB and XTAL — the APB frequency was adjusted from the typical 80 MHz to match the actual clock configuration.
- `uart_config_t` — User-facing config struct (clock source, baud rate, parity, data/stop bits).
- `divisor_t` — Internal baud rate divisor (integer + 4-bit fractional part).

**Initialization** (`uart_hal.c:49-104` — `hal_uart_init()`):
1. Enable UART memory clock gating.
2. Toggle the peripheral clock and reset bits for the selected UART port.
3. Assert and deassert the UART core reset bit.
4. Wait for register synchronization (poll `UART_REG_UPDATE` bit).
5. Set clock source selector, compute baud rate divisor, write `clkdiv` and `conf0` registers.
6. Reset RX and TX FIFOs, set RX FIFO threshold to 10 bytes.
7. Trigger register update and wait for completion.

**Baud rate calculation** (`uart_hal.c:29-35` — `get_clk_div()`): Uses the formula from TRM §12.3.3:
```
div = (freq << 4) / baud
integral = div >> 4
fractional = div & 0xF
```
The `freq << 4` provides 4 fractional bits of precision. The `+ (baud / 2)` term rounds to nearest.

**TX** (`uart_hal.c:131-136` — `hal_uart_write_byte()`): Blocks while the TX FIFO has ≥ 128 bytes (reading the FIFO occupancy from `status[23:16]`). Then writes the byte to the FIFO data register.

**RX** (`uart_hal.c:148-157` — `hal_uart_read_byte()`): Non-blocking. Reads the RX FIFO count from `status[7:0]`; returns 0 if empty.

**GPIO/IO_MUX setup** (`gpio.h` + `uart_hal.c:115-120`): 
The IO_MUX register struct (`gpio.h:13-36`) maps each GPIO pin's configuration register. For UART0, GPIO21 (TX) is set to function 1 (MCU select), GPIO20 (RX) is set to function 1 with input buffer enabled. These are ESP32-C3-specific pin assignments — changing boards means changing these values.

### 7.3 Driver Conventions

- TX functions are blocking (wait for FIFO space).
- RX functions are non-blocking (poll and return immediately).
- All register access is `volatile` — no caching or reordering assumptions.
- Configuration is passed via const-correct struct pointers.
- No interrupt-driven I/O is implemented (the UART interrupt enable register exists in the struct but is unused).

---

## 8. Common Utilities

**File**: `common/utils.h`

Two macros used throughout the codebase:
- `REG(addr)` — Casts a literal address to `volatile uint32_t*` and dereferences it. In assembly context (`#ifdef __ASSEMBLER__`), it just passes the address through unchanged.
- `BIT(x)` — Returns `1 << x` as a `uint32_t`.

These two macros are the entire abstraction layer for MMIO. All driver and port code uses them directly.

---

## 9. Application Entry Point

**File**: `main.c`

The current application is a minimal smoke test:
1. Create a `uart_config_t` with 115200 8N1, APB clock source.
2. Create a single FreeRTOS task (`uart_task`).
3. Start the scheduler.

The task initializes the UART hardware, transmits "Hello world!\n", and deletes itself. All other tasks (idle, timer service if configured) run inside the kernel.

---

## 10. Build System

**File**: `Makefile`

**Compiler flags** (`Makefile:47-50`):
- `-march=rv32im_zicsr` — RISC-V 32-bit integer + multiply + CSR instructions.
- `-mabi=ilp32` — 32-bit integer ABI (no floating point in registers).
- `-g3 -O2` — Full debug info, optimize for speed.
- `-ffunction-sections -fdata-sections` — One section per function/data item; unused sections are GC'd at link time (`--gc-sections`).
- `-msmall-data-limit=8 -fno-common` — Objects ≤ 8 bytes go in `.sdata`/`.sbss` for GP-relative access.

**Sources**: All `.c` files are compiled from their relative paths. `port_asm.S` is assembled separately (`.S` suffix → preprocessed assembly). Object files mirror the source tree under `build/`.

**Linking** (`Makefile:53-58`):
- Uses `bsp/linker_mdk.ld` as the memory map.
- `--gc-sections` removes unreferenced code and data.
- `-nostartfiles` — No standard CRT; boot sequence is entirely in `bsp/boot.c`.
- `--specs=nosys.specs` — Provides stubs for system calls (no OS underneath).
- Explicitly links `-lc -lgcc` for standard library and compiler intrinsics.

**Image generation** (`Makefile:72-74`): `esptool.py elf2image` converts the ELF to an ESP32-C3 flash binary with DIO flash mode, 40 MHz flash frequency, 4 MB flash size.

---

## 11. FreeRTOS Configuration

**File**: `FreeRTOSConfig.h`

The configuration choices are driven by the port's constraints:

- `configCPU_CLOCK_HZ = 160000000` — Must match the actual clock set in `soc_init()`. A mismatch silently breaks all timing (tick period, delays, timeouts).
- `configTICK_RATE_HZ = 1000` — 1 ms tick. The SYSTIMER period is computed as `40,000,000 / 1000 = 40,000` cycles.
- `SYSTIMER_PRIORITY = 2` — Interrupt priority for CPU line 1. Must be between 1 and 7 (ESP32-C3 supports 4 priority bits). Priority 0 is reserved.
- `configMAX_PRIORITIES = 5` — Kept small so the ready-list bitmap fits in 32 bits, enabling CLZ-based O(1) task selection.
- `configTOTAL_HEAP_SIZE = 40 * 1024` — 40 KB from the 128 KB DRAM. heap_4 coalesces adjacent free blocks.
- `configMINIMAL_STACK_SIZE = 100` — 400 bytes. The port context frame alone is 128 bytes; the rest accommodates function call depth.

---

## 12. Debugging Infrastructure

**Files**: `debug/esp_usb_jtag.cfg`, `debug/esp32c3.cfg`

Two OpenOCD configs work together:
- `esp_usb_jtag.cfg` — Adapter layer: selects the `esp_usb_jtag` driver with the known VID:PID pair and capability descriptor value. Speed is set to 4 MHz.
- `esp32c3.cfg` — Target layer: declares the RISC-V CPU with TAP ID `0x00005c25` (ESP32-C3 silicon ID), 5-bit IR length, and a 16 KB work area at `0x40380000` in IRAM. The work area enables hardware breakpoints and fast memory access during debug.

Start OpenOCD with both configs, then `make debug` launches GDB and connects to `:3333`.

---

## 13. Porting Lessons

This section documents the non-obvious issues encountered while developing this port, as a reference for others doing similar work.

### 13.1 The MTIME trap

The most common mistake when porting FreeRTOS to a new RISC-V chip is assuming `mtime` and `mtimecmp` exist. The ESP32-C3's RISC-V core does not implement these CSRs. Reading them returns zero. The fix is to:
1. Set `portasmHAS_MTIME 0` in `freertos_risc_v_chip_specific_extensions.h`.
2. Provide a custom `portSetupTimerInterrupt()` — in this project, `portSetupSystimerInterrupt()`.
3. Route the tick interrupt through the SoC's interrupt controller — here, the ESP32-C3's interrupt matrix.

### 13.2 Vector table alignment

RISC-V requires the vector table to be 256-byte aligned when using vectored mode (`mtvec[0] = 1`). The project uses direct mode (`mtvec[1:0] = 0`) so 4-byte alignment suffices, but 256-byte alignment is maintained for compatibility.

### 13.3 Task return address

`pxPortInitialiseStack` stores `portTASK_RETURN_ADDRESS` (default 0) as the initial `ra` (x1). If a task function returns, execution jumps to address 0, causing an exception. The default application exception handler spins forever with CSRs loaded into registers for debugger inspection. This catches the common bug of a task missing its infinite loop or `vTaskDelete(NULL)`.

### 13.4 Critical section initialization

`xCriticalNesting` is initialized to `0xaaaaaaaa` in `port.c:85`. It must be non-zero before the scheduler starts, because if an interrupt fires during boot and calls a FreeRTOS API that enters a critical section, a value of zero would cause `portEXIT_CRITICAL()` to re-enable interrupts prematurely. The scheduler start routine zeroes it for the first task.

### 13.5 Clock frequency assumptions

The UART driver's `get_clk_freq()` returns 40 MHz for both `UART_SCLK_APB` and `UART_SCLK_XTAL`. The standard APB clock is 80 MHz, but this project's `soc_init()` configures a divider that results in 40 MHz. If the clock tree is modified, the UART baud rate calculation will be wrong.

### 13.6 SYSTIMER interrupt clearance

The SYSTIMER interrupt must be explicitly cleared by writing 1 to `SYSTIMER_INT_CLR_REG` (`port_asm.S:294`, using the macro from `portContext.h:81-86`). Without this, the interrupt remains pending and the core re-enters the handler immediately after `mret`, creating an infinite loop. The `fence io, io` after the write ensures the store is visible before continuing.
