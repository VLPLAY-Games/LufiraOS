/* shell.h - простой shell для LufiraOS */

#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>

/* Максимальная длина команды */
#define MAX_COMMAND_LENGTH 64
#define MAX_ARGUMENTS 10

/* Структура команды */
typedef struct {
    const char* name;
    const char* description;
    void (*handler)(void);
} command_t;

/* Запуск shell */
void shell_start(void);

/* Выполнить команду */
void shell_execute(const char* command);

/* Команды */
void cmd_help(void);
void cmd_clear(void);
void cmd_shutdown(void);
void cmd_reboot(void);
void cmd_echo(void);
void cmd_info(void);

#endif /* SHELL_H */