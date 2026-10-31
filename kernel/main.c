#include <radix/types.h>
#include <radix/hal.h>
#include <radix/kprintf.h>
#include <radix/sched.h>
#include <radix/sync.h>
#include <radix/ipc.h>
#include <radix/amp.h>

#define TASK_STACK_SIZE 2048

static uint8_t s_heartbeat_stack[TASK_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t s_worker_stack[TASK_STACK_SIZE]    __attribute__((aligned(16)));

/* Inter-Task Zero-Copy IPC Channel and Synchronization */
static ipc_channel_t s_kchannel;
static mutex_t       s_stats_mutex;

typedef struct {
    uint32_t sent;
    uint32_t recv;
} ipc_stats_t;

static ipc_stats_t s_stats;

/* Forward declaration for systimer ISR */
void systimer_isr(void);

void radix_level1_interrupt_handler(void) {
#ifndef RADIX_HOST_TEST
    systimer_isr();
#endif
}

void radix_syscall_handler(void) {
    /* Level 1 Syscall Handler - Step 5 gateway */
}

void hal_early_init(void) {
    wdt_disable_all();
    uart_early_init();
}

static void heartbeat_task(void *arg) {
    (void)arg;
    uint32_t count = 0;
    while (1) {
        /* Produce zero-copy IPC message */
        ipc_msg_t *slot = ipc_claim_slot(&s_kchannel);
        if (slot) {
            slot->msg_id = count;
            slot->len = 16;
            slot->flags = 0xAA;
            for (uint16_t i = 0; i < slot->len; i++) {
                slot->data[i] = (uint8_t)(count + i);
            }
            ipc_commit_slot(&s_kchannel);

            mutex_lock(&s_stats_mutex);
            s_stats.sent++;
            uint32_t total_sent = s_stats.sent;
            mutex_unlock(&s_stats_mutex);

            kprintf("[HEARTBEAT] Core 0 pulse=%u, sent zero-copy msg_id=%u (total_sent=%u)\n",
                    (unsigned int)count, (unsigned int)count, (unsigned int)total_sent);
        } else {
            kprintf("[HEARTBEAT] Core 0 pulse=%u, IPC channel full\n", (unsigned int)count);
        }

        /* Dispatch AMP work request to Core 1 */
        amp_send_to_core(RADIX_CORE_1, AMP_MSG_WORK_REQUEST, &count, sizeof(count));

        count++;

        for (volatile int i = 0; i < 500000; i++) {}
        sched_yield();
    }
}

static void worker_task(void *arg) {
    (void)arg;
    uint32_t iteration = 0;
    while (1) {
        /* Consume zero-copy IPC message */
        const ipc_msg_t *msg = ipc_peek_slot(&s_kchannel);
        if (msg) {
            uint32_t id = msg->msg_id;
            uint8_t b0 = (msg->len > 0) ? msg->data[0] : 0;
            ipc_release_slot(&s_kchannel);

            mutex_lock(&s_stats_mutex);
            s_stats.recv++;
            uint32_t total_recv = s_stats.recv;
            mutex_unlock(&s_stats_mutex);

            kprintf("[WORKER] Batch #%u: received zero-copy IPC msg_id=%u byte0=0x%x (total_recv=%u)\n",
                    (unsigned int)iteration, (unsigned int)id, (unsigned int)b0, (unsigned int)total_recv);
        } else {
            kprintf("[WORKER] Batch #%u: no IPC messages available\n", (unsigned int)iteration);
        }

        /* Check for incoming AMP messages from Core 1 */
        uint32_t amp_msg_id = 0;
        uint32_t amp_res = 0;
        uint16_t amp_len = 0;
        if (amp_recv_from_peer(&amp_msg_id, &amp_res, &amp_len)) {
            if (amp_msg_id == AMP_MSG_CORE1_READY) {
                kprintf("[WORKER] Core 1 boot acknowledged (READY)\n");
            } else if (amp_msg_id == AMP_MSG_WORK_RESPONSE) {
                kprintf("[WORKER] Received AMP result from Core 1: %u\n", (unsigned int)amp_res);
            }
        }

        iteration++;

        for (volatile int i = 0; i < 800000; i++) {}
        sched_yield();
    }
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
    kprintf("[BOOT] CPU architecture : Xtensa LX7 (Dual-Core + ULP)\n");
    kprintf("[BOOT] Watchdogs        : Neutralized\n");
    kprintf("[BOOT] Serial console   : Polled UART0 @ 115200 8N1\n");
    kprintf("[BOOT] Step 1 status    : SUCCESSFUL\n");
    kprintf("[BOOT] Step 2 status    : PREEMPTIVE NANO-KERNEL ACTIVE\n");
    kprintf("[BOOT] Step 3 status    : ATOMICS (S32C1I), SYNC & ZERO-COPY IPC ACTIVE\n");
    kprintf("[BOOT] Step 4 status    : ASYMMETRIC MULTIPROCESSING (AMP DUAL-CORE) ACTIVE\n");
    kputs("======================================================\n\n");

    /* Initialize Asymmetric Multiprocessing (AMP) and Core 1 bring-up */
    amp_init();

    /* Initialize the nano-kernel scheduler */
    sched_init();

    /* Initialize synchronization and zero-copy IPC */
    mutex_init(&s_stats_mutex);
    ipc_channel_init(&s_kchannel);
    s_stats.sent = 0;
    s_stats.recv = 0;

    /* Create sample workloads demonstrating IPC & Mutex */
    task_create("heartbeat", heartbeat_task, NULL, 1, s_heartbeat_stack, sizeof(s_heartbeat_stack));
    task_create("worker", worker_task, NULL, 2, s_worker_stack, sizeof(s_worker_stack));

    /* Initialize 52-bit hardware Systimer at 100 Hz (10 ms per tick) */
    systimer_init(100);

    /* Hand off execution to scheduler */
    sched_start();
}
