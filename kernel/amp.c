/*
 * kernel/amp.c - Asymmetric Multiprocessing (AMP) Cross-Core Dispatch
 *
 * Implements dual-core bring-up, shared SRAM state, zero-copy cross-core IPC,
 * and lock-free hardware IPI signaling for the ESP32-S3 (Xtensa LX7).
 */

#include <radix/amp.h>
#include <radix/atomic.h>
#include <radix/intc.h>
#include <radix/kprintf.h>
#include <radix/sched.h>
#include <radix/sync.h>

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
        g_amp_state.call_mailbox[i].done_flag = NULL;

        spinlock_init(&g_amp_state.call_queue[i].lock);
        g_amp_state.call_queue[i].head = 0u;
        g_amp_state.call_queue[i].tail = 0u;
        g_amp_state.call_queue[i].count = 0u;
        for (uint32_t s = 0u; s < AMP_CALL_QUEUE_SIZE; s++) {
            g_amp_state.call_queue[i].slots[s].fn = NULL;
            g_amp_state.call_queue[i].slots[s].arg = NULL;
            g_amp_state.call_queue[i].slots[s].done = 0u;
            g_amp_state.call_queue[i].slots[s].done_flag = NULL;
        }
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

static bool amp_enqueue_call_internal(uint32_t target_core,
                                      amp_call_fn_t fn,
                                      void *arg,
                                      volatile uint32_t *done_flag) {
    if (target_core >= RADIX_NUM_CORES || !fn) {
        return false;
    }

    amp_call_queue_t *q = &g_amp_state.call_queue[target_core];
    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&q->lock, &flags);

    if (q->count >= AMP_CALL_QUEUE_SIZE) {
        spinlock_release_irqrestore(&q->lock, flags);
        return false;
    }

    amp_call_slot_t *slot = &q->slots[q->tail];
    slot->fn = fn;
    slot->arg = arg;
    slot->done = 0u;
    slot->done_flag = done_flag;
    q->tail = (q->tail + 1u) % AMP_CALL_QUEUE_SIZE;
    q->count++;

    /* Maintain compatibility mirror for single-slot inspection */
    g_amp_state.call_mailbox[target_core].fn = fn;
    g_amp_state.call_mailbox[target_core].arg = arg;
    g_amp_state.call_mailbox[target_core].done = 0u;
    g_amp_state.call_mailbox[target_core].done_flag = done_flag;

    spinlock_release_irqrestore(&q->lock, flags);
    arch_memory_barrier();

    return ipi_send(target_core, IPI_REASON_CALL_FUNC);
}

bool amp_enqueue_call(uint32_t target_core, amp_call_fn_t fn, void *arg) {
    return amp_enqueue_call_internal(target_core, fn, arg, NULL);
}

bool amp_call_on_core(uint32_t target_core, amp_call_fn_t fn, void *arg) {
    if (target_core >= RADIX_NUM_CORES || !fn) {
        return false;
    }

    if (target_core == arch_get_core_id()) {
        fn(arg);
        g_amp_state.call_mailbox[target_core].fn = fn;
        g_amp_state.call_mailbox[target_core].arg = arg;
        g_amp_state.call_mailbox[target_core].done = 1u;
        return true;
    }

#ifndef RADIX_HOST_TEST
    if (g_amp_state.core_state[target_core] == CORE_STATE_ONLINE) {
        volatile uint32_t local_done = 0u;
        if (!amp_enqueue_call_internal(target_core, fn, arg, &local_done)) {
            return false;
        }
        while (local_done == 0u) {
            __asm__ volatile ("" ::: "memory");
        }
        return true;
    }
#endif

    return amp_enqueue_call_internal(target_core, fn, arg, NULL);
}

uint32_t amp_drain_call_queue(uint32_t core_id) {
    if (core_id >= RADIX_NUM_CORES) {
        return 0u;
    }

    amp_call_queue_t *q = &g_amp_state.call_queue[core_id];
    uint32_t executed = 0u;

    while (1) {
        uint32_t flags = 0u;
        spinlock_acquire_irqsave(&q->lock, &flags);

        if (q->count == 0u) {
            spinlock_release_irqrestore(&q->lock, flags);
            break;
        }

        amp_call_slot_t item = q->slots[q->head];
        q->slots[q->head].fn = NULL;
        q->slots[q->head].arg = NULL;
        q->slots[q->head].done = 1u;
        q->slots[q->head].done_flag = NULL;
        q->head = (q->head + 1u) % AMP_CALL_QUEUE_SIZE;
        q->count--;

        spinlock_release_irqrestore(&q->lock, flags);

        if (item.fn != NULL) {
            item.fn(item.arg);
            arch_memory_barrier();
            if (item.done_flag != NULL) {
                *item.done_flag = 1u;
                arch_memory_barrier();
            }
            spinlock_acquire_irqsave(&q->lock, &flags);
            if (g_amp_state.call_mailbox[core_id].fn == item.fn &&
                g_amp_state.call_mailbox[core_id].arg == item.arg) {
                g_amp_state.call_mailbox[core_id].done = 1u;
            }
            spinlock_release_irqrestore(&q->lock, flags);
            executed++;
        }
    }

    return executed;
}

static void core1_ipi_irq_handler(uint32_t cpu_int_num, void *arg) {
    (void)cpu_int_num;
    (void)arg;
    uint32_t reasons = ipi_handle_irq();
    if (reasons & IPI_REASON_RESCHEDULE) {
        sched_set_need_resched();
    }
}

uint32_t amp_core1_poll_once(void) {
    arch_atomic_fetch_add(&g_amp_state.core_heartbeats[RADIX_CORE_1], 1);

    /* Process pending hardware IPIs (also drains call_queue on IPI_REASON_CALL_FUNC) */
    uint32_t reasons = ipi_handle_irq();

    /* Ensure any queued remote calls for Core 1 are drained in FIFO order */
    amp_drain_call_queue(RADIX_CORE_1);

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
    intc_init(RADIX_CORE_1);
    intc_register_handler(INTC_CPU_LINE_IPI, core1_ipi_irq_handler, NULL);
    intc_route_irq(RADIX_CORE_1, INT_SRC_CPU_INTR_FROM_CPU_1, INTC_CPU_LINE_IPI);
    intc_enable_irq(INTC_CPU_LINE_IPI);

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
