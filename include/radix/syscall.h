#ifndef RADIX_SYSCALL_H
#define RADIX_SYSCALL_H

#include <radix/types.h>

/*
 * Syscall Identification Numbers
 */
typedef enum {
    SYS_YIELD       = 0,
    SYS_TASK_EXIT   = 1,
    SYS_IPC_SEND    = 2,
    SYS_IPC_RECV    = 3,
    SYS_GET_TIME    = 4,
    SYS_UART_WRITE  = 5,
    SYS_MAX_SYSCALLS = 6
} syscall_id_t;

/*
 * Syscall Return Status Codes
 */
#define RADIX_SYSCALL_OK          0
#define RADIX_SYSCALL_ERR_INVAL  -1
#define RADIX_SYSCALL_ERR_PERM   -2
#define RADIX_SYSCALL_ERR_ACCESS -3
#define RADIX_SYSCALL_ERR_NOSYS  -4
#define RADIX_SYSCALL_ERR_AGAIN  -5

/*
 * Kernel Dispatcher & Trap Interface
 */
int32_t radix_syscall_dispatch(uint32_t syscall_id, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4);
void radix_exception_handler_c(uint32_t exccause, uintptr_t epc1, uintptr_t excvaddr, uint32_t *regs);
uint32_t syscall_get_total_count(void);

/*
 * User-space wrappers
 */
int32_t sys_yield(void);
int32_t sys_task_exit(void);
int32_t sys_ipc_send(void *ch, uint32_t msg_id, const void *payload, uint16_t len);
int32_t sys_ipc_recv(void *ch, uint32_t *msg_id_out, void *payload_out, uint16_t *len_out);
int32_t sys_get_time(uint64_t *ticks_out);
int32_t sys_uart_write(const char *buf, size_t len);

#endif /* RADIX_SYSCALL_H */
