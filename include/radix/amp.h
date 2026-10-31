#ifndef RADIX_AMP_H
#define RADIX_AMP_H

#include <radix/types.h>
#include <radix/ipc.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Processor Core IDs
 */
#define RADIX_CORE_0        0u
#define RADIX_CORE_1        1u
#define RADIX_NUM_CORES     2u

/*
 * Core Operational States
 */
typedef enum {
    CORE_STATE_OFFLINE = 0,
    CORE_STATE_BOOTING,
    CORE_STATE_ONLINE,
    CORE_STATE_HALTED
} core_state_t;

/*
 * Inter-Processor Interrupt (IPI) Reason Flags (Bitmask)
 */
typedef enum {
    IPI_REASON_NONE        = 0,
    IPI_REASON_RESCHEDULE  = (1u << 0),
    IPI_REASON_IPC_NOTIFY  = (1u << 1),
    IPI_REASON_CALL_FUNC   = (1u << 2),
    IPI_REASON_PING        = (1u << 3)
} ipi_reason_t;

/*
 * Standard AMP Cross-Core Message Identifiers
 */
#define AMP_MSG_CORE1_READY    0x100
#define AMP_MSG_WORK_REQUEST   0x101
#define AMP_MSG_WORK_RESPONSE  0x102
#define AMP_MSG_PING           0x103
#define AMP_MSG_PONG           0x104

/*
 * Remote Function Call Slot
 */
typedef void (*amp_call_fn_t)(void *arg);

typedef struct {
    amp_call_fn_t fn;
    void *arg;
    volatile uint32_t done;
} amp_call_slot_t;

/*
 * Shared Multi-Core State Structure (Located in Internal SRAM)
 */
typedef struct {
    volatile uint32_t core_state[RADIX_NUM_CORES];
    volatile uint32_t core_heartbeats[RADIX_NUM_CORES];
    volatile uint32_t pending_ipi[RADIX_NUM_CORES];
    volatile uint32_t ipi_tx_count[RADIX_NUM_CORES];
    volatile uint32_t ipi_rx_count[RADIX_NUM_CORES];
    volatile uint32_t resched_requests[RADIX_NUM_CORES];
    amp_call_slot_t call_mailbox[RADIX_NUM_CORES];
    ipc_channel_t ch_core0_to_core1;
    ipc_channel_t ch_core1_to_core0;
} amp_shared_state_t;

/* Low-level trampoline entry point defined in boot_core1.S */
void _start_core1(void);

/*
 * Architectural Hardware / System Functions
 */
uint32_t arch_get_core_id(void);
void system_boot_core1(void (*entry)(void));
void system_halt_core1(void);

/*
 * Inter-Processor Interrupt (IPI) Driver API
 */
void ipi_init(void);
bool ipi_send(uint32_t target_core, uint32_t reason);
uint32_t ipi_handle_irq(void);

/*
 * AMP Kernel Subsystem API
 */
amp_shared_state_t *amp_get_state(void);
void amp_init(void);
bool amp_send_to_core(uint32_t target_core, uint32_t msg_id, const void *payload, uint16_t len);
bool amp_recv_from_peer(uint32_t *msg_id, void *payload_out, uint16_t *len_out);
bool amp_call_on_core(uint32_t target_core, amp_call_fn_t fn, void *arg);
uint32_t amp_core1_poll_once(void);
void kernel_core1_main(void);

/*
 * Host Simulation & Test Inspection Helpers
 */
#ifdef RADIX_HOST_TEST
void host_set_current_core_id(uint32_t core_id);
uint32_t system_get_core1_boot_addr(void);
uint32_t system_get_core1_ctrl0_reg(void);
uint32_t ipi_get_hardware_intr_reg(uint32_t core_id);
#endif

#ifdef __cplusplus
}
#endif

#endif /* RADIX_AMP_H */
