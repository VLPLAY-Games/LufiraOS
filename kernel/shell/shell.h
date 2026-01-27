#ifndef SHELL_H
#define SHELL_H

#include "../drivers/console.h"
#include "../drivers/keyboard.h"

// Прототипы функций
void show_prompt(void);
void execute_command(void);
int strcmp(const char* s1, const char* s2);
char to_lower(char c);
int strcmp_case_insensitive(const char* s1, const char* s2);

// Функции для обработки ввода с клавиатуры
void shell_handle_char(char c);
void shell_handle_backspace(void);
void shell_handle_enter(void);
void shell_handle_left_arrow(void);
void shell_handle_right_arrow(void);
void shell_refresh_input_line(void);

#endif