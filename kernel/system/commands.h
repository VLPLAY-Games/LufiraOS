#ifndef COMMANDS_H
#define COMMANDS_H

#include <stdint.h>

// Вспомогательные функции
int atoi(const char* str);
int hex_to_int(const char* hex);

// Прототипы функций команд
void command_help(void);
void command_clear(void);
void command_reboot(void);
void command_shutdown(void);
void command_version(void);
void command_color(void);
void command_colors(void);
void command_fg(void);
void command_bg(void);

#endif