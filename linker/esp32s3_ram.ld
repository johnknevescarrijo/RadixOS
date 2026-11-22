ENTRY(_start)

MEMORY
{
  /* ESP32-S3 Internal SRAM */
  iram0_seg (rx) : ORIGIN = 0x40378000, LENGTH = 0x60000 /* 384 KB Instruction RAM */
  dram0_seg (rw) : ORIGIN = 0x3FC88000, LENGTH = 0x60000 /* 384 KB Data RAM */
}

SECTIONS
{
  _iram_start = ORIGIN(iram0_seg);

  /* Exception Vector Table at base of IRAM */
  .vectors : ALIGN(1024)
  {
    _vector_table = .;
    KEEP(*(.vectors))
    KEEP(*(.vectors.*))
    . = ALIGN(1024);
  } > iram0_seg

  /* Executable text section */
  .text : ALIGN(4)
  {
    _text_start = .;
    *(.literal .text .literal.* .text.*)
    *(.iram0 .iram0.*)
    *(.iram1 .iram1.*)
    . = ALIGN(4);
    _text_end = .;
  } > iram0_seg

  _iram_end = ALIGN(_text_end, 256);
  _iram_seg_end = ORIGIN(iram0_seg) + LENGTH(iram0_seg);

  _dram_start = ORIGIN(dram0_seg);

  /* Read-only data */
  .rodata : ALIGN(4)
  {
    _rodata_start = .;
    *(.rodata .rodata.*)
    *(.srodata .srodata.*)
    . = ALIGN(4);
    _rodata_end = .;
  } > dram0_seg

  /* Initialized data */
  .data : ALIGN(4)
  {
    _data_start = .;
    *(.data .data.*)
    *(.sdata .sdata.*)
    . = ALIGN(4);
    _data_end = .;
  } > dram0_seg

  /* Zero-initialized BSS section */
  .bss : ALIGN(4)
  {
    _bss_start = .;
    *(.bss .bss.*)
    *(.sbss .sbss.*)
    *(COMMON)
    . = ALIGN(4);
    _bss_end = .;
  } > dram0_seg

  /* Boot stack space in DRAM (8 KB) */
  .stack : ALIGN(16)
  {
    _stack_bottom = .;
    . += 0x2000;
    _stack_top = .;
  } > dram0_seg

  _dram_end = ALIGN(_stack_top, 256);

  /* Unused heap begins right after kernel DRAM (256-byte aligned for PMS split line) */
  _heap_start = _dram_end;
  _heap_end = ORIGIN(dram0_seg) + LENGTH(dram0_seg);
}
