#ifndef RADIX_SHELL_H
#define RADIX_SHELL_H

#include <radix/types.h>

/* VT100 ANSI Escape Sequences */
#define SHELL_VT100_RESET   "\033[0m"
#define SHELL_VT100_BOLD    "\033[1m"
#define SHELL_VT100_GREEN   "\033[32m"
#define SHELL_VT100_CYAN    "\033[36m"
#define SHELL_VT100_YELLOW  "\033[33m"
#define SHELL_VT100_RED     "\033[31m"
#define SHELL_VT100_CLEAR   "\033[2J\033[H"

#define SHELL_MAX_LINE_LEN  64
#define SHELL_MAX_ARGS      8

/* Shell Execution Return Codes */
#define SHELL_OK              0
#define SHELL_ERR_EMPTY       1
#define SHELL_ERR_UNKNOWN_CMD (-1)
#define SHELL_ERR_BAD_ARG     (-2)
#define SHELL_ERR_NOT_FOUND   (-3)

typedef struct {
    const char *name;
    const char *usage;
    int (*handler)(int argc, char *argv[]);
} shell_cmd_t;

void shell_init(void);
void shell_print_prompt(void);
bool shell_poll_char(int ch);
void shell_poll_uart(void);
int shell_execute_line(const char *line);
void shell_task(void *arg);
uint32_t shell_get_executed_cmd_count(void);
bool shell_was_reboot_requested(void);

#endif /* RADIX_SHELL_H */
