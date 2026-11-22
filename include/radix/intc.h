#ifndef RADIX_INTC_H
#define RADIX_INTC_H

#include <radix/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ESP32-S3 Interrupt Matrix Geometry & Peripheral Sources (TRM Chapter 8)
 */
#define INTC_NUM_SOURCES                99u
#define INTC_NUM_CPU_INTS               32u
#define INTC_DISCONNECTED_LINE          16u

/*
 * Level-1 External CPU Interrupt Lines on ESP32-S3 Xtensa LX7 (TRM Table 8-2):
 *   Lines 0..5, 8..10, 12..13, 17..18 are Level-1 external interrupts
 *   routable via the Interrupt Matrix.
 *   (Excluded: 6=Timer0, 7=SW-L1, 11=Prof-L3, 14=NMI, 15=Timer1, 16=Timer2,
 *    19..21=L2, 22..23/27=L3, 24..25/28/30=L4, 26/31=L5, 29=SW-L3)
 */
#define INTC_LEVEL1_EXTERNAL_MASK       0x0006373FUL

/* Verified ESP32-S3 TRM Table 8-1 Peripheral Interrupt Source IDs */
#define INT_SRC_UART0                   27u
#define INT_SRC_SYSTIMER_TARGET0        57u
#define INT_SRC_CPU_INTR_FROM_CPU_0     79u
#define INT_SRC_CPU_INTR_FROM_CPU_1     80u

/* Default Level-1 CPU interrupt lines assigned by RadixOS */
#define INTC_CPU_LINE_SYSTIMER          1u
#define INTC_CPU_LINE_IPI               2u
#define INTC_CPU_LINE_UART0             3u

/* Interrupt service routine callback signature */
typedef void (*radix_irq_handler_t)(uint32_t cpu_int_num, void *arg);

/* Interrupt Controller Initialization & Routing */
void intc_init(uint32_t core_id);
bool intc_route_irq(uint32_t core_id, uint32_t source_id, uint32_t cpu_int_num);
bool intc_register_handler(uint32_t cpu_int_num, radix_irq_handler_t handler, void *arg);

/* CPU Interrupt Masking (Xtensa INTENABLE SR 228 / Host Simulation) */
bool intc_enable_irq(uint32_t cpu_int_num);
bool intc_disable_irq(uint32_t cpu_int_num);
uint32_t intc_get_enabled_mask(void);

/* Level-1 Interrupt Dispatcher */
uint32_t intc_dispatch_pending(uint32_t pending_mask);

#ifdef RADIX_HOST_TEST
/* Host inspection & simulation helpers */
uint32_t intc_host_get_mapped_irq(uint32_t core_id, uint32_t source_id);
void intc_host_set_pending(uint32_t cpu_int_num);
void intc_host_clear_pending(uint32_t cpu_int_num);
uint32_t intc_host_get_pending_mask(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* RADIX_INTC_H */
