/*
 * RadixOS ESP32-S3 RISC-V ULP Coprocessor Linker Script
 * Target: RV32IMC in RTC Slow Memory (8 KB at 0x50000000)
 */

ENTRY(_ulp_start)

MEMORY
{
    rtc_slow_ram (rwx) : ORIGIN = 0x50000000, LENGTH = 0x1F00
    rtc_mailbox  (rw)  : ORIGIN = 0x50001F00, LENGTH = 0x0100
}

SECTIONS
{
    .text :
    {
        *(.entry)
        *(.text .text.*)
    } > rtc_slow_ram

    .rodata :
    {
        *(.rodata .rodata.*)
    } > rtc_slow_ram

    .data :
    {
        *(.data .data.*)
    } > rtc_slow_ram

    .bss (NOLOAD) :
    {
        *(.bss .bss.*)
        *(COMMON)
    } > rtc_slow_ram

    .rtc.mailbox (NOLOAD) :
    {
        KEEP(*(.rtc.mailbox))
    } > rtc_mailbox
}
