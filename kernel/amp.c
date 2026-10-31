/*
 * kernel/amp.c - Asymmetric Multiprocessing (AMP) Cross-Core Dispatch
 *
 * Implements dual-core bring-up, shared SRAM state, zero-copy cross-core IPC,
 * and lock-free hardware IPI signaling for the ESP32-S3 (Xtensa LX7).
 */

#include <radix/amp.h>
#include <radix/atomic.h>
#include <radix/kprintf.h>

/* Shared state in internal SRAM */
static amp_shared_state_t g_amp_state;

amp_shared_state_t *amp_get_state(void) {
    return &g_amp_state;
}

void amp_init(void) {
    for (uint32_t i = 0; i < RADIX_NUM_CORES; i++) {
        g_amp_state.core_state[i] = CORE_STATE_OFFLINE;
        g_amp_state.core_heartbeats[i] = 0;
        g_amp_state.pending_ipi[i] = 0;
        g_amp_state.ipi_tx_count[i] = 0;
        g_amp_state.ipi_rx_count[i] = 0;
        g_amp_state.resched_requests[i] = 0;
        g_amp_state.call_mailbox[i].fn = NULL;
        g_amp_state.call_mailbox[i].arg = NULL;
        g_amp_state.call_mailbox[i].done = 0;
    }

    /* Initialize unidirectional zero-copy SPSC channels in shared SRAM */
    ipc_channel_init(&g_amp_state.ch_core0_to_core1);
    ipc_channel_init(&g_amp_state.ch_core1_to_core0);

    /* Core 0 is currently executing the kernel boot */
    g_amp_state.core_state[RADIX_CORE_0] = CORE_STATE_ONLINE;
    g_amp_state.core_state[RADIX_CORE_1] = CORE_STATE_BOOTING;

    /* Initialize hardware IPI subsystem */
    ipi_init();

    /* Bring Core 1 out of reset and branch to _start_core1 */
    system_boot_core1(_start_core1);

    kprintf("[AMP] Initialized dual-core subsystem. Core 0 ONLINE, Core 1 booting...\n");
}

bool amp_send_to_core(uint32_t target_core, uint32_t msg_id, const void *payload, uint16_t len) {
    if (target_core >= RADIX_NUM_CORES) {
        return false;
    }

    ipc_channel_t *ch = (target_core == RADIX_CORE_1) ?
                        &g_amp_state.ch_core0_to_core1 :
                        &g_amp_state.ch_core1_to_core0;

    ipc_msg_t *slot = ipc_claim_slot(ch);
    if (!slot) {
        return false;
    }

    slot->msg_id = msg_id;
    if (len > IPC_SLOT_PAYLOAD_SIZE) {
        len = IPC_SLOT_PAYLOAD_SIZE;
    }
    slot->len = len;
    slot->flags = 0;

    if (payload && len > 0) {
        const uint8_t *src = (const uint8_t *)payload;
        for (uint16_t i = 0; i < len; i++) {
            slot->data[i] = src[i];
        }
    }

    ipc_commit_slot(ch);

    /* Signal peer core via hardware IPI */
    ipi_send(target_core, IPI_REASON_IPC_NOTIFY);
    return true;
}

bool amp_recv_from_peer(uint32_t *msg_id, void *payload_out, uint16_t *len_out) {
    uint32_t core_id = arch_get_core_id();
    ipc_channel_t *ch = (core_id == RADIX_CORE_0) ?
                        &g_amp_state.ch_core1_to_core0 :
                        &g_amp_state.ch_core0_to_core1;

    const ipc_msg_t *msg = ipc_peek_slot(ch);
    if (!msg) {
        return false;
    }

    if (msg_id) {
        *msg_id = msg->msg_id;
    }
    if (len_out) {
        *len_out = msg->len;
    }
    if (payload_out && msg->len > 0) {
        uint8_t *dst = (uint8_t *)payload_out;
        for (uint16_t i = 0; i < msg->len; i++) {
            dst[i] = msg->data[i];
        }
    }

    ipc_release_slot(ch);
    return true;
}

bool amp_call_on_core(uint32_t target_core, amp_call_fn_t fn, void *arg) {
    if (target_core >= RADIX_NUM_CORES || !fn) {
        return false;
    }

    amp_call_slot_t *slot = &g_amp_state.call_mailbox[target_core];
    slot->fn = fn;
    slot->arg = arg;
    slot->done = 0;

    arch_memory_barrier();

    /* Trigger function call IPI */
    return ipi_send(target_core, IPI_REASON_CALL_FUNC);
}

uint32_t amp_core1_poll_once(void) {
    arch_atomic_fetch_add(&g_amp_state.core_heartbeats[RADIX_CORE_1], 1);

    /* Process pending hardware IPIs */
    uint32_t reasons = ipi_handle_irq();

    /* Process pending messages arriving from Core 0 */
    uint32_t msg_id = 0;
    uint8_t buf[IPC_SLOT_PAYLOAD_SIZE];
    uint16_t len = 0;

    while (amp_recv_from_peer(&msg_id, buf, &len)) {
        if (msg_id == AMP_MSG_PING) {
            amp_send_to_core(RADIX_CORE_0, AMP_MSG_PONG, NULL, 0);
        } else if (msg_id == AMP_MSG_WORK_REQUEST) {
            uint32_t in_val = 0;
            if (len >= sizeof(uint32_t)) {
                for (size_t i = 0; i < sizeof(uint32_t); i++) {
                    ((uint8_t *)&in_val)[i] = buf[i];
                }
            }
            uint32_t out_val = in_val * 2u + 1u;
            amp_send_to_core(RADIX_CORE_0, AMP_MSG_WORK_RESPONSE, &out_val, sizeof(out_val));
        }
    }

    return reasons;
}

void kernel_core1_main(void) {
    g_amp_state.core_state[RADIX_CORE_1] = CORE_STATE_ONLINE;
    arch_memory_barrier();

    /* Acknowledge readiness to Core 0 */
    amp_send_to_core(RADIX_CORE_0, AMP_MSG_CORE1_READY, NULL, 0);

    /* Main processing loop for Core 1 (APP_CPU) */
    while (g_amp_state.core_state[RADIX_CORE_1] == CORE_STATE_ONLINE) {
        amp_core1_poll_once();
#ifndef RADIX_HOST_TEST
        __asm__ volatile ("waiti 0");
#endif
    }
}
