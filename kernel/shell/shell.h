#ifndef SHELL_H
#define SHELL_H

#include "../drivers/console/console.h"
#include "../drivers/keyboard/keyboard.h"

// Прототипы функций
void show_prompt(void);
void execute_command(void);
int strcmp(const char* s1, const char* s2);
char to_lower(char c);
int strcmp_case_insensitive(const char* s1, const char* s2);
void strcpy(char* dest, const char* src);

// Функции для обработки ввода с клавиатуры
void shell_handle_char(char c);
void shell_handle_backspace(void);
void shell_handle_enter(void);
void shell_handle_left_arrow(void);
void shell_handle_right_arrow(void);
void shell_handle_up_arrow(void);
void shell_handle_down_arrow(void);
void shell_refresh_input_line(void);

// Функции для работы с историей
void add_to_history(const char* command);
const char* get_history_command(int index);
void load_command_from_history(int history_idx);

// Текущий путь и кластер
extern char cwd_path[256];
extern uint32_t cwd_first_cluster;

#endif