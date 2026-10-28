# RadixOS Technical Implementation Roadmap

This document outlines the architectural specifications, register-level hardware interactions, design trade-offs, and implementation milestones for **RadixOS** on the Espressif **ESP32-S3** (dual-core Xtensa LX7 + ULP RISC-V).

---

## High-Level Vision & Design Philosophy

RadixOS is built around three guiding tenets:
1. **Zero Black-Box Bloat**: Avoid proprietary OS frameworks or opaque bootloaders. The CPU begins executing instructions in bare-metal assembly at the reset vector and configures all hardware peripherals via direct memory-mapped I/O (MMIO).
2. **Predictable Determinism**: Low-jitter, preemptive scheduling with minimal context-switch latency.
3. **Progressive Hardware Isolation**: Transition from a single-privilege flat model (SASOS) to hardware-enforced spatial boundaries utilizing the ESP32-S3 **World Controller** and **Permission Management System (PMS)**.

---

## Detailed Implementation Steps

```
[ Step 1: Boot & Console ] ──> [ Step 2: Nano-Kernel ] ──> [ Step 3: Atomics & IPC ]
                                                                     │
[ Step 6: Shell & ULP ]    <── [ Step 5: World/PMS Isolation ] <─── [ Step 4: AMP Core 1 ]
```

---

### Step 1: Bare-Metal Boot, Linker Script, Exception Vectors & Early Console
- **Status**: **Implemented**
- **Core Components**:
  - `linker/esp32s3_ram.ld`: Linker script that partitions internal SRAM into Instruction RAM (`IRAM: 0x40378000`, 384KB) and Data RAM (`DRAM: 0x3FC88000`, 384KB). Ensures `.text` and vector code execute from IRAM for deterministic single-cycle access.
  - `arch/xtensa/boot.S`: Entry point `_start`. Sets up the stack pointer (`sp` / `a1`), initializes the Processor State register (`PS = 0x00040020` or non-windowed call0 conventions with Level 0 interrupts), sets `VECBASE` to `_vector_table`, clears the `.bss` section in DRAM with 32-bit stores, and jumps to `kernel_early_init()`.
  - `arch/xtensa/vectors.S`: Xtensa LX7 exception vector table aligning vectors at proper offsets:
    - `0x000`: Reset Vector
    - `0x180`: Window Overflow / Underflow handlers
    - `0x280`: Level 1 General Interrupt / Exception Vector
    - `0x3C0`: Syscall / User Exception Handler
  - `drivers/wdt.c`: Disables hardware watchdogs that would otherwise trigger a core reset:
    - **TIMG0 / TIMG1 WDT**: Unlocks write protection register with magic key `0x50D83AA1` and clears enable bits.
    - **RTC WDT**: Disables RTC-level watchdog and Super Watchdog (SWD).
  - `drivers/uart.c` & `kernel/kprintf.c`: Memory-mapped driver for UART0 (`0x60000000`). Configures 8N1 format and provides polling FIFO transmission. A reentrant, dependency-free `kprintf()` supporting `%s`, `%d`, `%u`, `%x`, `%p`, and `%c`.

---

### Step 2: Preemptive Nano-Kernel — TCB, Xtensa Context Switch & Systimer
- **Status**: **Implemented**
- **Core Components**:
  - `include/radix/task.h` & `include/radix/sched.h`:
    - **Task Control Block (`tcb_t`)**: Contains task ID, priority, execution state (`READY`, `RUNNING`, `BLOCKED`, `SUSPENDED`), stack top, stack base, stack size, and runtime execution statistics.
    - **CPU Context (`cpu_context_t`)**: Represents the saved register frame on task stack: `pc`, `ps`, and general-purpose registers `a0` through `a15`.
  - `arch/xtensa/context_switch.S`:
    - `arch_context_switch(cpu_context_t **prev_ctx, cpu_context_t *next_ctx)`:
      - Decrements stack pointer and pushes callee-saved registers and return address onto the outgoing task's stack.
      - Stores outgoing stack pointer into `*prev_ctx`.
      - Loads incoming stack pointer from `next_ctx`.
      - Pops incoming registers, restores processor status (`PS`), and returns to the new task entry point.
    - `arch_start_first_task(cpu_context_t *first_ctx)`: Hands off execution to the highest-priority initial task.
  - `drivers/systimer.c`:
    - Programs the ESP32-S3 52-bit hardware **Systimer** (`0x60023000`).
    - Configures Target 0 for periodic comparator matches running at 16 MHz (e.g. 100 Hz / 10ms OS ticks or 1000 Hz / 1ms OS ticks).
    - Clears interrupt flag in `SYSTIMER_INT_CLR_REG` and triggers `sched_tick()`.
  - `kernel/sched.c`:
    - Fixed-size circular ready queue supporting static task allocation.
    - Round-robin preemption among ready tasks at the highest priority level.
    - Dedicated `idle_task` running an infinite wait loop when no workload is ready.

---

### Step 3: Atomic Synchronization (`S32C1I`) & Zero-Copy IPC Channels
- **Status**: *Planned*
- **Target Specifications**:
  - **Hardware Atomics**: Implement atomic operations utilizing the Xtensa LX7 `S32C1I` (Conditional Store 32-bit) instruction.
    - Register `SCOMPARE1` holds the expected value.
    - Atomic compare-and-swap (CAS) primitive enables lock-free algorithms without disabling global interrupts.
  - **Kernel Spinlocks**: Ticket-based spinlocks for critical section protection across multiple cores.
  - **Zero-Copy IPC Channels**:
    - Single-Producer Single-Consumer (SPSC) circular ring buffers.
    - Fixed-size message queues with cache-line-aligned boundaries (avoiding false sharing on ESP32-S3 unified bus).
    - Event flags and notification bitmasks for lightweight inter-task signaling.

---

### Step 4: Asymmetric Multiprocessing (AMP) — Core 1 Bring-Up & Lock-Free IPIs
- **Status**: *Planned*
- **Target Specifications**:
  - **Core 1 (APP_CPU) Bootstrap**:
    - PRO_CPU releases APP_CPU from reset via System Register `SYSTEM_CORE_1_CONTROL_0_REG` (`0x600C0000`).
    - Configures APP_CPU reset vector to a designated trampoline address.
    - APP_CPU initializes its local `VECBASE`, enables local interrupts, and enters a dedicated execution loop.
  - **Workload Partitioning**:
    - **Core 0 (PRO_CPU)**: Supervisor services, timer ticks, interrupt distribution, peripheral I/O (UART, SPI, Wi-Fi/BT baseband in future), and general scheduling.
    - **Core 1 (APP_CPU)**: Pure real-time worker execution with deterministic latency, minimal interrupt jitter, and zero background I/O interference.
  - **Inter-Processor Interrupts (IPI)**:
    - Triggered via hardware interrupt lines (`SYSTEM_CPU_INTR_FROM_CPU_X_REG`).
    - Memory synchronization enforced using Xtensa `MEMW` (Memory Wait / Barrier) instruction.

---

### Step 5: Hardware Isolation — World Controller (`World 0`/`1`), PMS MPU & Syscalls
- **Status**: *Planned*
- **Target Specifications**:
  - **ESP32-S3 World Controller**:
    - The hardware supports two operational "Worlds":
      - **World 0**: Privileged / Supervisor mode. Complete access to all physical address spaces and peripheral MMIO ranges.
      - **World 1**: Unprivileged / User mode. Restricted access controlled by hardware filter matrices.
  - **Permission Management System (PMS)**:
    - Programs PMS registers to enforce read, write, and execute permissions on:
      - Internal SRAM blocks (preventing user tasks from tampering with kernel data structures or code).
      - Peripherals (blocking user tasks from directly writing to hardware registers like WDT, Systimer, or Interrupt Matrix).
  - **Syscall Gateway**:
    - User tasks execute the `syscall` instruction to enter Level 1 Exception Vector in `World 0`.
    - Syscall dispatcher validates user parameters, executes the requested kernel operation, and returns back to `World 1`.

---

### Step 6: Interactive Kernel Shell & RISC-V ULP Co-Processor Offloading
- **Status**: *Planned*
- **Target Specifications**:
  - **Interactive Kernel Shell (Radix-CLI)**:
    - Non-blocking UART serial command parser.
    - System commands:
      - `ps`: Dumps task list, state, stack high-water mark, and CPU utilization.
      - `mem`: Displays free and allocated IRAM/DRAM regions.
      - `uptime`: Queries hardware 52-bit Systimer.
      - `reboot`: Safely resets the SoC via RTC software reset registers.
  - **Ultra-Low Power (ULP) Offloading**:
    - The ESP32-S3 contains an internal 32-bit RISC-V ULP co-processor running at ~20 MHz from the RTC slow clock.
    - Load minimal RISC-V binary into RTC Slow Memory (`0x50000000`).
    - Core 0 transitions main cores into Light/Deep Sleep while the RISC-V ULP monitors sensors/GPIOs, waking the Xtensa cores only on specific threshold events.

---

## Memory Map (ESP32-S3 Internal SRAM)

```
0x40378000 ┌─────────────────────────────────────────┐  IRAM (384 KB)
           │ Vector Table (_vector_table)            │
           │ .iram0.text (Boot, ISRs, Context Switch)│
           │ .text (Kernel code, Drivers)            │
0x403DF000 ├─────────────────────────────────────────┤
           │ Reserved / Unused IRAM                  │
0x3FC88000 ├─────────────────────────────────────────┤  DRAM (384 KB)
           │ .data (Initialized globals)             │
           │ .bss (Zero-initialized globals)         │
           │ Kernel Heap & Stacks                    │
           │ Task Stacks (Idle, Heartbeat, Worker)   │
0x3FCE8000 └─────────────────────────────────────────┘
```

---

## Verification & Quality Assurance Strategy

1. **Host-Level Unit Testing (`make host-check`)**:
   - Isolates hardware-specific register operations behind mock HAL interfaces.
   - Evaluates scheduler task state transitions, FIFO priority sorting, yield operations, and time-slice expiration.
   - Zero external library dependencies; builds cleanly with `-Wall -Wextra -Werror -std=c11`.
2. **Target Assembly & Linker Validation (`make`)**:
   - Asserts proper vector alignment (Xtensa vector offsets must match hardware specifications).
   - Generates `.map` file to audit symbol placement across IRAM and DRAM regions.
   - Flat binary generation for direct flashing via `esptool.py`.
