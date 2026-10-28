#include <radix/types.h>
#include <radix/hal.h>
#include <radix/kprintf.h>

void radix_level1_interrupt_handler(void) {
    /* Step 1 interrupt stub */
}

void radix_syscall_handler(void) {
    /* Step 1 syscall stub */
}

void hal_early_init(void) {
    wdt_disable_all();
    uart_early_init();
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
    kprintf("[BOOT] CPU architecture : Xtensa LX7 (Call0 ABI)\n");
    kprintf("[BOOT] Watchdogs        : Disabled (TIMG0, TIMG1, RTC, SWD)\n");
    kprintf("[BOOT] Serial console   : Polled UART0 @ 115200 8N1\n");
    kprintf("[BOOT] Step 1 status    : SUCCESSFUL\n");
    kputs("======================================================\n\n");
}
