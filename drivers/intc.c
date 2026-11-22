/*
 * drivers/intc.c - ESP32-S3 Interrupt Matrix & Level-1 IRQ Dispatcher
 *
 * Routes peripheral interrupt sources (0..98) to Xtensa LX7 Level-1 external
 * CPU interrupt lines, manages INTENABLE masking, and dispatches registered
 * handlers per core.
 */

#include <radix/intc.h>
#include <radix/amp.h>
#include <radix/atomic.h>

#ifndef RADIX_HOST_TEST
#include <esp32s3_soc.h>
#endif

typedef struct {
    radix_irq_handler_t fn;
    void *arg;
} intc_handler_entry_t;

static intc_handler_entry_t s_handlers[RADIX_NUM_CORES][INTC_NUM_CPU_INTS];

#ifdef RADIX_HOST_TEST
static uint32_t s_host_irq_map[RADIX_NUM_CORES][INTC_NUM_SOURCES];
static uint32_t s_host_intenable[RADIX_NUM_CORES];
static uint32_t s_host_interrupt[RADIX_NUM_CORES];
#endif

static inline uint32_t intc_current_core(void) {
    uint32_t core_id = arch_get_core_id();
    return (core_id < RADIX_NUM_CORES) ? core_id : 0u;
}

static inline bool intc_is_valid_level1_line(uint32_t cpu_int_num) {
    if (cpu_int_num >= INTC_NUM_CPU_INTS) {
        return false;
    }
    return ((INTC_LEVEL1_EXTERNAL_MASK & (1UL << cpu_int_num)) != 0u);
}

static inline uint32_t intc_hw_read_intenable(uint32_t core_id) {
#ifdef RADIX_HOST_TEST
    return s_host_intenable[core_id];
#else
    (void)core_id;
    uint32_t val;
    __asm__ volatile ("rsr %0, 228" : "=r"(val));
    return val;
#endif
}

static inline void intc_hw_write_intenable(uint32_t core_id, uint32_t val) {
#ifdef RADIX_HOST_TEST
    s_host_intenable[core_id] = val;
#else
    (void)core_id;
    __asm__ volatile ("wsr %0, 228\n\trsync" :: "r"(val) : "memory");
#endif
}

static inline void intc_hw_write_map(uint32_t core_id, uint32_t source_id, uint32_t cpu_int_num) {
#ifdef RADIX_HOST_TEST
    s_host_irq_map[core_id][source_id] = cpu_int_num;
#else
    uintptr_t reg = (core_id == RADIX_CORE_0) ?
                    INTERRUPT_CORE0_MAP_REG(source_id) :
                    INTERRUPT_CORE1_MAP_REG(source_id);
    REG_WRITE(reg, cpu_int_num);
#endif
}

void intc_init(uint32_t core_id) {
    if (core_id >= RADIX_NUM_CORES) {
        return;
    }

    for (uint32_t src = 0; src < INTC_NUM_SOURCES; src++) {
        intc_hw_write_map(core_id, src, INTC_DISCONNECTED_LINE);
    }

    for (uint32_t line = 0; line < INTC_NUM_CPU_INTS; line++) {
        s_handlers[core_id][line].fn = NULL;
        s_handlers[core_id][line].arg = NULL;
    }

#ifdef RADIX_HOST_TEST
    s_host_intenable[core_id] = 0u;
    s_host_interrupt[core_id] = 0u;
#else
    if (intc_current_core() == core_id) {
        intc_hw_write_intenable(core_id, 0u);
    }
#endif
}

bool intc_route_irq(uint32_t core_id, uint32_t source_id, uint32_t cpu_int_num) {
    if (core_id >= RADIX_NUM_CORES ||
        source_id >= INTC_NUM_SOURCES ||
        !intc_is_valid_level1_line(cpu_int_num)) {
        return false;
    }

    intc_hw_write_map(core_id, source_id, cpu_int_num);
    return true;
}

bool intc_register_handler(uint32_t cpu_int_num, radix_irq_handler_t handler, void *arg) {
    if (!intc_is_valid_level1_line(cpu_int_num)) {
        return false;
    }

    uint32_t core_id = intc_current_core();
    uint32_t flags = arch_irq_save();
    s_handlers[core_id][cpu_int_num].fn = handler;
    s_handlers[core_id][cpu_int_num].arg = (handler != NULL) ? arg : NULL;
    arch_irq_restore(flags);
    return true;
}

bool intc_enable_irq(uint32_t cpu_int_num) {
    if (!intc_is_valid_level1_line(cpu_int_num)) {
        return false;
    }

    uint32_t core_id = intc_current_core();
    uint32_t flags = arch_irq_save();
    uint32_t mask = intc_hw_read_intenable(core_id);
    mask |= (1UL << cpu_int_num);
    intc_hw_write_intenable(core_id, mask);
    arch_irq_restore(flags);
    return true;
}

bool intc_disable_irq(uint32_t cpu_int_num) {
    if (!intc_is_valid_level1_line(cpu_int_num)) {
        return false;
    }

    uint32_t core_id = intc_current_core();
    uint32_t flags = arch_irq_save();
    uint32_t mask = intc_hw_read_intenable(core_id);
    mask &= ~(1UL << cpu_int_num);
    intc_hw_write_intenable(core_id, mask);
    arch_irq_restore(flags);
    return true;
}

uint32_t intc_get_enabled_mask(void) {
    return intc_hw_read_intenable(intc_current_core());
}

uint32_t intc_dispatch_pending(uint32_t pending_mask) {
    uint32_t core_id = intc_current_core();
    uint32_t dispatched = 0u;
    uint32_t remaining = pending_mask;

    while (remaining != 0u) {
        uint32_t bit = (uint32_t)__builtin_ctz(remaining);
        remaining &= (remaining - 1u);

        if (bit < INTC_NUM_CPU_INTS) {
            radix_irq_handler_t fn = s_handlers[core_id][bit].fn;
            void *arg = s_handlers[core_id][bit].arg;
            if (fn != NULL) {
                fn(bit, arg);
                dispatched++;
            }
        }
    }

    return dispatched;
}

#ifdef RADIX_HOST_TEST
uint32_t intc_host_get_mapped_irq(uint32_t core_id, uint32_t source_id) {
    if (core_id >= RADIX_NUM_CORES || source_id >= INTC_NUM_SOURCES) {
        return INTC_DISCONNECTED_LINE;
    }
    return s_host_irq_map[core_id][source_id];
}

void intc_host_set_pending(uint32_t cpu_int_num) {
    if (cpu_int_num < INTC_NUM_CPU_INTS) {
        s_host_interrupt[intc_current_core()] |= (1UL << cpu_int_num);
    }
}

void intc_host_clear_pending(uint32_t cpu_int_num) {
    if (cpu_int_num < INTC_NUM_CPU_INTS) {
        s_host_interrupt[intc_current_core()] &= ~(1UL << cpu_int_num);
    }
}

uint32_t intc_host_get_pending_mask(void) {
    uint32_t core_id = intc_current_core();
    return s_host_interrupt[core_id] & s_host_intenable[core_id];
}
#endif
