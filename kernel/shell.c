/*
 * kernel/shell.c - Radix-Shell Interactive Kernel CLI
 *
 * Provides a non-blocking UART command line interface with VT100 ANSI formatting,
 * task inspection and termination, AMP status, and ULP coprocessor control.
 */

#include <radix/shell.h>
#include <radix/hal.h>
#include <radix/kprintf.h>
#include <radix/sched.h>
#include <radix/pms.h>
#include <radix/syscall.h>
#include <radix/amp.h>
#include <radix/ulp.h>

#ifndef RADIX_HOST_TEST
#include <esp32s3_soc.h>
#endif

/* Local buffer and state */
static char     s_line_buf[SHELL_MAX_LINE_LEN];
static size_t   s_line_len = 0;
static uint32_t s_executed_cmd_count = 0;
static bool     s_reboot_requested = false;

/* String helper functions (freestanding) */
static int shell_strcmp(const char *s1, const char *s2) {
    if (!s1 || !s2) return (s1 == s2) ? 0 : (s1 ? 1 : -1);
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static int shell_atoi(const char *s) {
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    int res = 0;
    while (*s >= '0' && *s <= '9') {
        res = res * 10 + (*s - '0');
        s++;
    }
    return sign * res;
}

/* Command forward declarations */
static int cmd_help(int argc, char *argv[]);
static int cmd_info(int argc, char *argv[]);
static int cmd_ps(int argc, char *argv[]);
static int cmd_kill(int argc, char *argv[]);
static int cmd_amp(int argc, char *argv[]);
static int cmd_ulp(int argc, char *argv[]);
static int cmd_reboot(int argc, char *argv[]);

static const shell_cmd_t g_shell_cmds[] = {
    { "help",   "Display available kernel commands",     cmd_help },
    { "info",   "Show system, PMS, syscall and ULP info", cmd_info },
    { "ps",     "List all active kernel tasks",          cmd_ps },
    { "kill",   "Terminate a task: kill <id>",           cmd_kill },
    { "amp",    "Show dual-core AMP and IPI status",     cmd_amp },
    { "ulp",    "Inspect & control RISC-V ULP",          cmd_ulp },
    { "reboot", "Reset the ESP32-S3 system",             cmd_reboot }
};

static const size_t g_num_shell_cmds = sizeof(g_shell_cmds) / sizeof(g_shell_cmds[0]);

static int cmd_help(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    kprintf(SHELL_VT100_BOLD "\nRadixOS Kernel Shell Commands:\n" SHELL_VT100_RESET);
    for (size_t i = 0; i < g_num_shell_cmds; i++) {
        kprintf("  " SHELL_VT100_CYAN "%-8s" SHELL_VT100_RESET " - %s\n",
                g_shell_cmds[i].name, g_shell_cmds[i].usage);
    }
    return SHELL_OK;
}

static int cmd_info(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    kprintf(SHELL_VT100_BOLD "\n=== RadixOS System Information ===\n" SHELL_VT100_RESET);
    kprintf("  OS Version    : RadixOS v1.0 (ESP32-S3 Xtensa LX7 dual-core)\n");
    kprintf("  Uptime Ticks  : %u\n", (unsigned int)systimer_get_ticks());

    const world_pms_stats_t *pms = pms_get_stats();
    if (pms) {
        kprintf("  World Switches: W0->W1: %u, W1->W0: %u\n",
                (unsigned int)pms->world0_to_world1_switches,
                (unsigned int)pms->world1_to_world0_traps);
        kprintf("  PMS Violations: %u\n", (unsigned int)pms->pms_violations);
    }

    kprintf("  Syscalls Count: %u\n", (unsigned int)syscall_get_total_count());

    volatile ulp_rtc_mailbox_t *mb = ulp_get_mailbox();
    if (mb) {
        const char *state_str = "OFF";
        if (mb->state == ULP_STATE_LOADED) state_str = "LOADED";
        else if (mb->state == ULP_STATE_RUNNING) state_str = "RUNNING";
        else if (mb->state == ULP_STATE_HALTED) state_str = "HALTED";

        kprintf("  ULP RISC-V    : %s (wakeups: %u, sensor: %u)\n",
                state_str, (unsigned int)mb->wakeup_count, (unsigned int)mb->sensor_sample);
    }
    return SHELL_OK;
}

static int cmd_ps(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    kprintf(SHELL_VT100_BOLD "\n%-4s %-16s %-6s %-10s %-6s\n" SHELL_VT100_RESET,
            "ID", "NAME", "PRIO", "STATE", "SLICE");
    kprintf("-----------------------------------------------\n");

    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        const tcb_t *t = sched_get_task_by_index(i);
        if (t && t->state != TASK_STATE_UNUSED) {
            const char *st = "UNKNOWN";
            switch (t->state) {
                case TASK_STATE_READY:     st = "READY"; break;
                case TASK_STATE_RUNNING:   st = "RUNNING"; break;
                case TASK_STATE_BLOCKED:   st = "BLOCKED"; break;
                case TASK_STATE_SUSPENDED: st = "SUSPENDED"; break;
                default: break;
            }
            kprintf("%-4u %-16s %-6u %-10s %-6u\n",
                    (unsigned int)t->task_id, t->name, (unsigned int)t->priority,
                    st, (unsigned int)t->slice_remaining);
        }
    }
    return SHELL_OK;
}

static int cmd_kill(int argc, char *argv[]) {
    if (argc < 2) {
        kprintf(SHELL_VT100_RED "Usage: kill <task_id>\n" SHELL_VT100_RESET);
        return SHELL_ERR_BAD_ARG;
    }

    int id = shell_atoi(argv[1]);
    if (id <= 0) {
        kprintf(SHELL_VT100_RED "Error: cannot kill idle task (0) or invalid ID.\n" SHELL_VT100_RESET);
        return SHELL_ERR_BAD_ARG;
    }

    if (sched_kill_task((uint32_t)id)) {
        kprintf(SHELL_VT100_GREEN "Task %d terminated.\n" SHELL_VT100_RESET, id);
        return SHELL_OK;
    } else {
        kprintf(SHELL_VT100_RED "Error: task %d not found or protected.\n" SHELL_VT100_RESET, id);
        return SHELL_ERR_NOT_FOUND;
    }
}

static int cmd_amp(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    kprintf(SHELL_VT100_BOLD "\n=== Asymmetric Multiprocessing (AMP) Status ===\n" SHELL_VT100_RESET);
    amp_shared_state_t *st = amp_get_state();
    if (!st) {
        kprintf("AMP subsystem uninitialized.\n");
        return SHELL_OK;
    }

    const char *core0_st = (st->core_state[0] == CORE_STATE_ONLINE) ? "ONLINE" : "OFFLINE";
    const char *core1_st = "OFFLINE";
    if (st->core_state[1] == CORE_STATE_ONLINE) core1_st = "ONLINE";
    else if (st->core_state[1] == CORE_STATE_BOOTING) core1_st = "BOOTING";
    else if (st->core_state[1] == CORE_STATE_HALTED) core1_st = "HALTED";

    kprintf("  Core 0 (PRO_CPU): %s | Heartbeat: %u | IPI TX: %u | IPI RX: %u\n",
            core0_st, (unsigned int)st->core_heartbeats[0],
            (unsigned int)st->ipi_tx_count[0], (unsigned int)st->ipi_rx_count[0]);
    kprintf("  Core 1 (APP_CPU): %s | Heartbeat: %u | IPI TX: %u | IPI RX: %u\n",
            core1_st, (unsigned int)st->core_heartbeats[1],
            (unsigned int)st->ipi_tx_count[1], (unsigned int)st->ipi_rx_count[1]);
    return SHELL_OK;
}

static int cmd_ulp(int argc, char *argv[]) {
    volatile ulp_rtc_mailbox_t *mb = ulp_get_mailbox();
    if (!mb) {
        kprintf("ULP mailbox not available.\n");
        return SHELL_OK;
    }

    if (argc >= 2) {
        if (shell_strcmp(argv[1], "sample") == 0) {
            ulp_send_command(ULP_CMD_SAMPLE_NOW);
            kprintf("Sent ULP_CMD_SAMPLE_NOW to coprocessor.\n");
        } else if (shell_strcmp(argv[1], "reset") == 0) {
            ulp_send_command(ULP_CMD_RESET_STATS);
            kprintf("Sent ULP_CMD_RESET_STATS to coprocessor.\n");
        } else if (shell_strcmp(argv[1], "start") == 0) {
            ulp_start(50000);
            kprintf("Started ULP coprocessor (50 ms wakeup interval).\n");
        } else if (shell_strcmp(argv[1], "stop") == 0) {
            ulp_stop();
            kprintf("Stopped ULP coprocessor.\n");
        } else {
            kprintf(SHELL_VT100_RED "Usage: ulp [sample|reset|start|stop]\n" SHELL_VT100_RESET);
            return SHELL_ERR_BAD_ARG;
        }
    }

    const char *state_str = "OFF";
    if (mb->state == ULP_STATE_LOADED) state_str = "LOADED";
    else if (mb->state == ULP_STATE_RUNNING) state_str = "RUNNING";
    else if (mb->state == ULP_STATE_HALTED) state_str = "HALTED";

    kprintf(SHELL_VT100_BOLD "\n=== RISC-V ULP Coprocessor Status ===\n" SHELL_VT100_RESET);
    kprintf("  State        : %s\n", state_str);
    kprintf("  Firmware Size: %u bytes\n", (unsigned int)mb->fw_size_bytes);
    kprintf("  Wakeup Count : %u\n", (unsigned int)mb->wakeup_count);
    kprintf("  Sensor Sample: %u\n", (unsigned int)mb->sensor_sample);
    kprintf("  Cmd Ack Count: %u\n", (unsigned int)mb->cmd_ack_count);
    return SHELL_OK;
}

static int cmd_reboot(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    kprintf(SHELL_VT100_YELLOW "\nRebooting RadixOS ESP32-S3 system...\n" SHELL_VT100_RESET);
    s_reboot_requested = true;

#ifndef RADIX_HOST_TEST
    REG_SET_BITS(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);
    while (1) {}
#endif
    return SHELL_OK;
}

void shell_print_prompt(void) {
    kprintf(SHELL_VT100_GREEN "radixos> " SHELL_VT100_RESET);
}

void shell_init(void) {
    s_line_len = 0;
    s_line_buf[0] = '\0';
    s_executed_cmd_count = 0;
    s_reboot_requested = false;

    kprintf(SHELL_VT100_BOLD SHELL_VT100_CYAN "\n=== Welcome to Radix-Shell (v1.0) ===\n" SHELL_VT100_RESET);
    kprintf("Type '" SHELL_VT100_GREEN "help" SHELL_VT100_RESET "' to see available kernel commands.\n\n");
    shell_print_prompt();
}

int shell_execute_line(const char *line) {
    if (!line) return SHELL_ERR_EMPTY;

    /* Skip leading whitespace */
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (*line == '\0') {
        return SHELL_ERR_EMPTY;
    }

    /* Make a copy for tokenization */
    char buf[SHELL_MAX_LINE_LEN];
    size_t i = 0;
    while (line[i] != '\0' && i < SHELL_MAX_LINE_LEN - 1) {
        buf[i] = line[i];
        i++;
    }
    buf[i] = '\0';

    /* Tokenize into argc and argv */
    int argc = 0;
    char *argv[SHELL_MAX_ARGS];
    char *ptr = buf;

    while (*ptr != '\0' && argc < SHELL_MAX_ARGS) {
        while (*ptr == ' ' || *ptr == '\t') {
            ptr++;
        }
        if (*ptr == '\0') break;

        argv[argc++] = ptr;
        while (*ptr != '\0' && *ptr != ' ' && *ptr != '\t') {
            ptr++;
        }
        if (*ptr != '\0') {
            *ptr++ = '\0';
        }
    }

    if (argc == 0) {
        return SHELL_ERR_EMPTY;
    }

    /* Search built-in command table */
    for (size_t c = 0; c < g_num_shell_cmds; c++) {
        if (shell_strcmp(argv[0], g_shell_cmds[c].name) == 0) {
            s_executed_cmd_count++;
            return g_shell_cmds[c].handler(argc, argv);
        }
    }

    kprintf(SHELL_VT100_RED "Unknown command: '%s'. Type 'help' for available commands.\n" SHELL_VT100_RESET, argv[0]);
    return SHELL_ERR_UNKNOWN_CMD;
}

bool shell_poll_char(int ch) {
    if (ch < 0) {
        return false;
    }

    /* Handle Backspace */
    if (ch == '\b' || ch == 0x7F || ch == 8) {
        if (s_line_len > 0) {
            s_line_len--;
            s_line_buf[s_line_len] = '\0';
            uart_putc('\b');
            uart_putc(' ');
            uart_putc('\b');
        }
        return false;
    }

    /* Handle Enter / Carriage Return */
    if (ch == '\r' || ch == '\n') {
        uart_putc('\r');
        uart_putc('\n');
        s_line_buf[s_line_len] = '\0';

        if (s_line_len > 0) {
            shell_execute_line(s_line_buf);
        }

        s_line_len = 0;
        s_line_buf[0] = '\0';
        shell_print_prompt();
        return true;
    }

    /* Handle printable ASCII characters */
    if (ch >= 32 && ch <= 126) {
        if (s_line_len < SHELL_MAX_LINE_LEN - 1) {
            s_line_buf[s_line_len++] = (char)ch;
            s_line_buf[s_line_len] = '\0';
            uart_putc((char)ch);
        }
    }

    return false;
}

void shell_poll_uart(void) {
    int ch;
    while ((ch = uart_getc_nonblock()) >= 0) {
        shell_poll_char(ch);
    }
}

void shell_task(void *arg) {
    (void)arg;
    while (1) {
        shell_poll_uart();
        sched_yield();
    }
}

uint32_t shell_get_executed_cmd_count(void) {
    return s_executed_cmd_count;
}

bool shell_was_reboot_requested(void) {
    return s_reboot_requested;
}
