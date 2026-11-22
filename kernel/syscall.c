/*
 * kernel/syscall.c - RadixOS Syscall Gateway & Validation
 *
 * Implements kernel-space syscall dispatcher, pointer access validation
 * against PMS permissions, and user-space syscall invocation wrappers.
 */

#include <radix/types.h>
#include <radix/syscall.h>
#include <radix/pms.h>
#include <radix/sched.h>
#include <radix/ipc.h>
#include <radix/hal.h>
#include <radix/kprintf.h>
#include <radix/panic.h>
#include <radix/atomic.h>
#include <xtensa_regs.h>

static uint32_t s_total_syscalls = 0;

uint32_t syscall_get_total_count(void) {
    return s_total_syscalls;
}

int32_t radix_syscall_dispatch(uint32_t syscall_id, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4) {
    s_total_syscalls++;
    tcb_t *curr = sched_get_current_task();

    switch ((syscall_id_t)syscall_id) {
    case SYS_YIELD:
        sched_yield();
        return RADIX_SYSCALL_OK;

    case SYS_TASK_EXIT:
        task_exit();
        return RADIX_SYSCALL_OK;

    case SYS_IPC_SEND: {
        ipc_channel_t *ch = (ipc_channel_t *)arg1;
        uint32_t msg_id = (uint32_t)arg2;
        const void *payload = (const void *)arg3;
        uint16_t len = (uint16_t)arg4;

        if (ch == NULL || (len > 0 && payload == NULL)) {
            return RADIX_SYSCALL_ERR_INVAL;
        }

        if (curr && curr->world == RADIX_WORLD_1_USER) {
            if (!pms_check_access(RADIX_WORLD_1_USER, (uintptr_t)ch, sizeof(ipc_channel_t), PMS_PERM_RW)) {
                return RADIX_SYSCALL_ERR_ACCESS;
            }
        }

        /* Enforce user read permission on source payload against active task PMS */
        if (len > 0) {
            if (!pms_check_access(RADIX_WORLD_1_USER, (uintptr_t)payload, len, PMS_PERM_R)) {
                return RADIX_SYSCALL_ERR_ACCESS;
            }
        }

        bool sent = ipc_send_noblock(ch, msg_id, payload, len);
        return sent ? RADIX_SYSCALL_OK : RADIX_SYSCALL_ERR_AGAIN;
    }

    case SYS_IPC_RECV: {
        ipc_channel_t *ch = (ipc_channel_t *)arg1;
        uint32_t *msg_id_out = (uint32_t *)arg2;
        void *payload_out = (void *)arg3;
        uint16_t *len_out = (uint16_t *)arg4;

        if (ch == NULL || msg_id_out == NULL || payload_out == NULL || len_out == NULL) {
            return RADIX_SYSCALL_ERR_INVAL;
        }

        if (curr && curr->world == RADIX_WORLD_1_USER) {
            if (!pms_check_access(RADIX_WORLD_1_USER, (uintptr_t)ch, sizeof(ipc_channel_t), PMS_PERM_RW)) {
                return RADIX_SYSCALL_ERR_ACCESS;
            }
        }

        /* Enforce user write permissions on destination pointers against active task PMS */
        if (!pms_check_access(RADIX_WORLD_1_USER, (uintptr_t)msg_id_out, sizeof(uint32_t), PMS_PERM_W)) {
            return RADIX_SYSCALL_ERR_ACCESS;
        }
        if (!pms_check_access(RADIX_WORLD_1_USER, (uintptr_t)len_out, sizeof(uint16_t), PMS_PERM_W)) {
            return RADIX_SYSCALL_ERR_ACCESS;
        }
        if (!pms_check_access(RADIX_WORLD_1_USER, (uintptr_t)payload_out, IPC_SLOT_PAYLOAD_SIZE, PMS_PERM_W)) {
            return RADIX_SYSCALL_ERR_ACCESS;
        }

        bool recvd = ipc_recv_noblock(ch, msg_id_out, payload_out, len_out);
        return recvd ? RADIX_SYSCALL_OK : RADIX_SYSCALL_ERR_AGAIN;
    }

    case SYS_GET_TIME: {
        uint64_t *ticks_out = (uint64_t *)arg1;
        if (ticks_out == NULL) {
            return RADIX_SYSCALL_ERR_INVAL;
        }

        /* Enforce user write permission on destination ticks memory */
        if (!pms_check_access(RADIX_WORLD_1_USER, (uintptr_t)ticks_out, sizeof(uint64_t), PMS_PERM_W)) {
            return RADIX_SYSCALL_ERR_ACCESS;
        }

        *ticks_out = systimer_get_ticks();
        return RADIX_SYSCALL_OK;
    }

    case SYS_UART_WRITE: {
        const char *buf = (const char *)arg1;
        size_t len = (size_t)arg2;

        if (len == 0) {
            return 0;
        }
        if (buf == NULL) {
            return RADIX_SYSCALL_ERR_INVAL;
        }

        /* Enforce user read permission on source string buffer */
        if (!pms_check_access(RADIX_WORLD_1_USER, (uintptr_t)buf, len, PMS_PERM_R)) {
            return RADIX_SYSCALL_ERR_ACCESS;
        }

        for (size_t i = 0; i < len; i++) {
            uart_putc(buf[i]);
        }
        return (int32_t)len;
    }

    default:
        return RADIX_SYSCALL_ERR_NOSYS;
    }
}

static bool syscall_is_violation_cause(uint32_t exccause) {
    return (exccause == EXCCAUSE_ILLEGAL_INSTRUCTION ||
            exccause == EXCCAUSE_INSTR_FETCH_ERROR ||
            exccause == EXCCAUSE_LOAD_STORE_ERROR ||
            exccause == EXCCAUSE_PRIVILEGED ||
            exccause == EXCCAUSE_LOAD_PROHIBITED ||
            exccause == EXCCAUSE_STORE_PROHIBITED);
}

void radix_exception_handler_c(uint32_t exccause, uintptr_t epc1, uintptr_t excvaddr, uint32_t *regs) {
    tcb_t *curr = sched_get_current_task();
    const trap_frame_t *tf = (const trap_frame_t *)regs;

    panic_dump_exception(exccause, (uint32_t)epc1, (uint32_t)excvaddr, tf);

    if (curr && curr->world == RADIX_WORLD_1_USER) {
        if (syscall_is_violation_cause(exccause)) {
            pms_handle_violation_exception(exccause, excvaddr);
            return;
        }
        kprintf("[USER-TRAP] Exception cause=%u in task '%s' (id=%u) at epc=0x%x, vaddr=0x%x -> terminating task\n",
                (unsigned int)exccause, curr->name, (unsigned int)curr->task_id,
                (unsigned int)epc1, (unsigned int)excvaddr);
        task_exit();
        return;
    }

    kprintf("[KERNEL-TRAP] Exception cause=%u (%s) at epc=0x%x, vaddr=0x%x\n",
            (unsigned int)exccause, panic_exccause_str(exccause),
            (unsigned int)epc1, (unsigned int)excvaddr);
#ifndef RADIX_HOST_TEST
    /* Fatal World 0 kernel fault: halt safely */
    arch_irq_save();
    while (1) {
        __asm__ volatile ("waiti 0");
    }
#else
    if (curr && curr->task_id != 0u) {
        task_exit();
    }
#endif
}

/* User-Space Invocation Gate */
static inline int32_t arch_invoke_syscall(uint32_t id, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
#if defined(RADIX_HOST_TEST)
    tcb_t *before = sched_get_current_task();
    radix_world_t caller_world = world_capture_and_enter_kernel();
    int32_t ret = radix_syscall_dispatch(id, a1, a2, a3, a4);
    tcb_t *after = sched_get_current_task();
    if ((after == before || after == NULL) && caller_world == RADIX_WORLD_1_USER) {
        world_switch_to_user(0);
    }
    return ret;
#else
    register uint32_t r_id __asm__("a2") = id;
    register uintptr_t r_a1 __asm__("a3") = a1;
    register uintptr_t r_a2 __asm__("a4") = a2;
    register uintptr_t r_a3 __asm__("a5") = a3;
    register uintptr_t r_a4 __asm__("a6") = a4;
    __asm__ volatile (
        "syscall\n"
        : "+r"(r_id)
        : "r"(r_a1), "r"(r_a2), "r"(r_a3), "r"(r_a4)
        : "memory"
    );
    return (int32_t)r_id;
#endif
}

int32_t sys_yield(void) {
    return arch_invoke_syscall(SYS_YIELD, 0, 0, 0, 0);
}

int32_t sys_task_exit(void) {
    return arch_invoke_syscall(SYS_TASK_EXIT, 0, 0, 0, 0);
}

int32_t sys_ipc_send(void *ch, uint32_t msg_id, const void *payload, uint16_t len) {
    return arch_invoke_syscall(SYS_IPC_SEND, (uintptr_t)ch, (uintptr_t)msg_id, (uintptr_t)payload, (uintptr_t)len);
}

int32_t sys_ipc_recv(void *ch, uint32_t *msg_id_out, void *payload_out, uint16_t *len_out) {
    return arch_invoke_syscall(SYS_IPC_RECV, (uintptr_t)ch, (uintptr_t)msg_id_out, (uintptr_t)payload_out, (uintptr_t)len_out);
}

int32_t sys_get_time(uint64_t *ticks_out) {
    return arch_invoke_syscall(SYS_GET_TIME, (uintptr_t)ticks_out, 0, 0, 0);
}

int32_t sys_uart_write(const char *buf, size_t len) {
    return arch_invoke_syscall(SYS_UART_WRITE, (uintptr_t)buf, (uintptr_t)len, 0, 0);
}
