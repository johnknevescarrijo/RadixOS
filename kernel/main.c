#include <radix/types.h>
#include <radix/hal.h>
#include <radix/kprintf.h>
#include <radix/sched.h>

#define TASK_STACK_SIZE 2048

static uint8_t s_heartbeat_stack[TASK_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t s_worker_stack[TASK_STACK_SIZE]    __attribute__((aligned(16)));

/* Forward declaration for systimer ISR */
void systimer_isr(void);

void radix_level1_interrupt_handler(void) {
#ifndef RADIX_HOST_TEST
    systimer_isr();
#endif
}

void radix_syscall_handler(void) {
    /* Level 1 Syscall Handler - Step 5 gateway */
}

void hal_early_init(void) {
    wdt_disable_all();
    uart_early_init();
}

static void heartbeat_task(void *arg) {
    (void)arg;
    uint32_t count = 0;
    while (1) {
        kprintf("[HEARTBEAT] Core 0 alive, pulse=%u, task_ticks=%u\n",
                (unsigned int)count++, (unsigned int)sched_get_current_task()->ticks_run);
        for (volatile int i = 0; i < 500000; i++) {}
        sched_yield();
    }
}

static void worker_task(void *arg) {
    (void)arg;
    uint32_t iteration = 0;
    while (1) {
        kprintf("[WORKER] Computing batch #%u on Xtensa LX7\n", (unsigned int)iteration++);
        for (volatile int i = 0; i < 800000; i++) {}
        sched_yield();
    }
}

void kernel_early_init(void) {
    hal_early_init();

    kputs("\n======================================================\n");
    kputs("  ____            _ _       ___  ____  \n");
    kputs(" |  _ \\ __ _   __| (_)_  __/ _ \\/ ___| \n");
    kputs(" | |_) / _` | / _` | \\ \\/ / | | \\___ \\ \n");
    kputs(" |  _ < (_| || (_| | |>  <| |_| |___) |\n");
    kputs(" |_| \\_\\__,_| \\__,_|_/_/\\_\\\\___/|____/ \n");
    kputs(" Minimalist Bare-Metal OS for ESP32-S3\n");
    kputs("======================================================\n");
    kprintf("[BOOT] CPU architecture : Xtensa LX7 (Dual-Core + ULP)\n");
    kprintf("[BOOT] Watchdogs        : Neutralized\n");
    kprintf("[BOOT] Serial console   : Polled UART0 @ 115200 8N1\n");
    kprintf("[BOOT] Step 1 status    : SUCCESSFUL\n");
    kprintf("[BOOT] Step 2 status    : INITIALIZING PREEMPTIVE NANO-KERNEL\n");
    kputs("======================================================\n\n");

    /* Initialize the nano-kernel scheduler */
    sched_init();

    /* Create sample workloads */
    task_create("heartbeat", heartbeat_task, NULL, 1, s_heartbeat_stack, sizeof(s_heartbeat_stack));
    task_create("worker", worker_task, NULL, 2, s_worker_stack, sizeof(s_worker_stack));

    /* Initialize 52-bit hardware Systimer at 100 Hz (10 ms per tick) */
    systimer_init(100);

    /* Hand off execution to scheduler */
    sched_start();
}
