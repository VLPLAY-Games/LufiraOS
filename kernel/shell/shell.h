#pragma once

#include "drivers/console/console.h"
#include "drivers/keyboard/keyboard.h"
#include "lib/string.h"
#include "system/process/process.h"

// Прототипы функций
void show_prompt(void);
void execute_command(void);

// Функции для обработки ввода с клавиатуры
void shell_handle_char(char c);
void shell_handle_backspace(void);
void shell_handle_enter(void);
void shell_run_pending_command(void);
void shell_handle_left_arrow(void);
void shell_handle_right_arrow(void);
void shell_handle_up_arrow(void);
void shell_handle_down_arrow(void);
void shell_refresh_input_line(void);

// Функции для работы с историей
void add_to_history(const char* command);
const char* get_history_command(int index);
void load_command_from_history(int history_idx);
void shell_handle_tab(void);
void shell_handle_ctrl_c(void);

// Текущий путь и inode текущего каталога (LufiraFS) — раньше отдельные
// шелл-глобалы, теперь макросы поверх полей current_process: шелл — тоже
// просто процесс, и должен видеть/менять ТУ ЖЕ cwd, что видят системные
// вызовы SYS_CHDIR/SYS_GETCWD (kernel/system/syscall/syscall.c), а не
// независимую копию. current_process гарантированно не NULL здесь — этот
// заголовок используется только кодом, исполняющимся как процесс "shell".
#define cwd_path (current_process->cwd_path)
#define cwd_inode (current_process->cwd_inode)
