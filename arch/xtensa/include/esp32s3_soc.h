#ifndef RADIX_ESP32S3_SOC_H
#define RADIX_ESP32S3_SOC_H

#include <radix/types.h>

/*
 * ESP32-S3 Base Peripheral MMIO Addresses
 */
#define DR_REG_UART0_BASE            0x60000000
#define DR_REG_UART1_BASE            0x60001000
#define DR_REG_UART2_BASE            0x60002000

#define DR_REG_RTCCNTL_BASE          0x60008000
#define DR_REG_TIMG0_BASE            0x6001F000
#define DR_REG_TIMG1_BASE            0x60020000
#define DR_REG_SYSTIMER_BASE         0x60023000
#define DR_REG_SYSTEM_BASE           0x600C0000
#define DR_REG_INTERRUPT_CORE0_BASE  0x600C2000
#define DR_REG_WORLD_CTRL_BASE       0x600D5000

/*
 * UART0 Registers (Base: 0x60000000)
 */
#define UART_FIFO_REG(i)             ((i) + 0x0000)
#define UART_INT_ST_REG(i)           ((i) + 0x0008)
#define UART_INT_ENA_REG(i)          ((i) + 0x000C)
#define UART_INT_CLR_REG(i)          ((i) + 0x0010)
#define UART_CLKDIV_SYNC_REG(i)      ((i) + 0x0014)
#define UART_STATUS_REG(i)           ((i) + 0x001C)
#define UART_CONF0_SYNC_REG(i)       ((i) + 0x0020)

/* UART_STATUS_REG Bitfields */
#define UART_TXFIFO_CNT_MASK         0x000003FF
#define UART_TXFIFO_CNT_SHIFT        0
#define UART_RXFIFO_CNT_MASK         0x003FF000
#define UART_RXFIFO_CNT_SHIFT        12

/*
 * Timer Group Watchdog Registers (TIMG0: 0x6001F000, TIMG1: 0x60020000)
 */
#define TIMG_WDTCONFIG0_REG(i)       ((i) + 0x0048)
#define TIMG_WDTCONFIG1_REG(i)       ((i) + 0x004C)
#define TIMG_WDTCONFIG2_REG(i)       ((i) + 0x0050)
#define TIMG_WDTCONFIG3_REG(i)       ((i) + 0x0054)
#define TIMG_WDTCONFIG4_REG(i)       ((i) + 0x0058)
#define TIMG_WDTCONFIG5_REG(i)       ((i) + 0x005C)
#define TIMG_WDTFEED_REG(i)          ((i) + 0x0060)
#define TIMG_WDTWPROTECT_REG(i)      ((i) + 0x0064)

#define TIMG_WDT_WKEY_VALUE          0x50D83AA1
#define TIMG_WDT_EN_BIT              BIT(31)

/*
 * RTC Controller Watchdog & Super WDT Registers (Base: 0x60008000)
 */
#define RTC_CNTL_OPTIONS0_REG        (DR_REG_RTCCNTL_BASE + 0x0000)
#define RTC_CNTL_WDTCONFIG0_REG      (DR_REG_RTCCNTL_BASE + 0x0098)
#define RTC_CNTL_WDTCONFIG1_REG      (DR_REG_RTCCNTL_BASE + 0x009C)
#define RTC_CNTL_WDTFEED_REG         (DR_REG_RTCCNTL_BASE + 0x00A8)
#define RTC_CNTL_WDTWPROTECT_REG     (DR_REG_RTCCNTL_BASE + 0x00AC)
#define RTC_CNTL_SWD_CONF_REG        (DR_REG_RTCCNTL_BASE + 0x00B0)
#define RTC_CNTL_SWD_WPROTECT_REG    (DR_REG_RTCCNTL_BASE + 0x00B4)

#define RTC_CNTL_WDT_WKEY_VALUE      0x50D83AA1
#define RTC_CNTL_WDT_EN_BIT          BIT(31)
#define RTC_CNTL_SWD_AUTO_FEED_EN    BIT(31)
#define RTC_CNTL_SWD_DISABLE         BIT(30)

/*
 * Systimer Registers (Base: 0x60023000)
 * 52-bit hardware counter with 16 MHz reference clock.
 */
#define SYSTIMER_CONF_REG            (DR_REG_SYSTIMER_BASE + 0x0000)
#define SYSTIMER_UNIT0_OP_REG        (DR_REG_SYSTIMER_BASE + 0x0004)
#define SYSTIMER_UNIT0_LOAD_HI_REG   (DR_REG_SYSTIMER_BASE + 0x0008)
#define SYSTIMER_UNIT0_LOAD_LO_REG   (DR_REG_SYSTIMER_BASE + 0x000C)
#define SYSTIMER_UNIT0_VALUE_HI_REG  (DR_REG_SYSTIMER_BASE + 0x0040)
#define SYSTIMER_UNIT0_VALUE_LO_REG  (DR_REG_SYSTIMER_BASE + 0x0044)

#define SYSTIMER_TARGET0_HI_REG      (DR_REG_SYSTIMER_BASE + 0x0018)
#define SYSTIMER_TARGET0_LO_REG      (DR_REG_SYSTIMER_BASE + 0x001C)
#define SYSTIMER_TARGET0_CONF_REG    (DR_REG_SYSTIMER_BASE + 0x0028)

#define SYSTIMER_INT_ENA_REG         (DR_REG_SYSTIMER_BASE + 0x006C)
#define SYSTIMER_INT_RAW_REG         (DR_REG_SYSTIMER_BASE + 0x0070)
#define SYSTIMER_INT_CLR_REG         (DR_REG_SYSTIMER_BASE + 0x0074)

#define SYSTIMER_CLK_EN_BIT          BIT(24)
#define SYSTIMER_TARGET0_PERIOD_EN   BIT(30)
#define SYSTIMER_TARGET0_WORK_EN     BIT(31)
#define SYSTIMER_INT_TARGET0_BIT     BIT(0)

/*
 * System Control Registers for Dual-Core & IPIs (Base: 0x600C0000)
 */
#define SYSTEM_CORE_1_CONTROL_0_REG    (DR_REG_SYSTEM_BASE + 0x0014)
#define SYSTEM_CONTROL_CORE_1_RUNSTALL BIT(0)
#define SYSTEM_CONTROL_CORE_1_CLKGATE_EN BIT(1)
#define SYSTEM_CONTROL_CORE_1_RESETING BIT(2)

#define SYSTEM_CORE_1_CONTROL_1_REG    (DR_REG_SYSTEM_BASE + 0x0018)
#define SYSTEM_CORE_1_BOOT_ADDR_REG    (DR_REG_SYSTEM_BASE + 0x001C)

#define SYSTEM_CPU_INTR_FROM_CPU_0_REG (DR_REG_SYSTEM_BASE + 0x0030)
#define SYSTEM_CPU_INTR_FROM_CPU_1_REG (DR_REG_SYSTEM_BASE + 0x0034)
#define SYSTEM_CPU_INTR_FROM_CPU_0     BIT(0)
#define SYSTEM_CPU_INTR_FROM_CPU_1     BIT(0)

/*
 * Memory Map Boundaries (Internal SRAM)
 */
#define ESP32S3_IRAM_START           0x40378000
#define ESP32S3_IRAM_SIZE            (384 * 1024)
#define ESP32S3_DRAM_START           0x3FC88000
#define ESP32S3_DRAM_SIZE            (384 * 1024)

#endif /* RADIX_ESP32S3_SOC_H */
