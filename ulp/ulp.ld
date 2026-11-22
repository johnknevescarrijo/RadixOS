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
    .text : ALIGN(4)
    {
        *(.entry)
        *(.text .text.*)
    } > rtc_slow_ram

    .rodata : ALIGN(4)
    {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } > rtc_slow_ram

    .data : ALIGN(4)
    {
        *(.data .data.*)
        *(.sdata .sdata.*)
    } > rtc_slow_ram

    _ulp_image_end = .;
    ASSERT((_ulp_image_end - ORIGIN(rtc_slow_ram)) <= LENGTH(rtc_slow_ram),
           "ULP firmware binary overlaps RTC mailbox at 0x50001F00")

    .bss (NOLOAD) : ALIGN(4)
    {
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
    } > rtc_slow_ram

    .rtc.mailbox (NOLOAD) :
    {
        KEEP(*(.rtc.mailbox))
    } > rtc_mailbox
}
