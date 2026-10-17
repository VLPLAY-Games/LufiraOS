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

// Ctrl+C может завершить именно ТЕКУЩИЙ процесс, и тогда
// terminate_process_by_signal() уходит через переключение контекста и не
// возвращается в исходный вызов. Поэтому звать shell_handle_ctrl_c()
// напрямую из input_keyboard_event() опасно: для USB это происходит изнутри
// usb_poll() (см. timer_irq_handler() в pit.c), и такой уход посреди
// разбора HID-отчёта оставит клавиатуру без подготовленной следующей
// interrupt-передачи — USB-клавиатура замолкнет навсегда. Поэтому
// input_keyboard_event() только взводит флаг, а shell_handle_ctrl_c()
// вызывается из timer_irq_handler() сразу после usb_poll()/net_poll() —
// задержка не больше одного тика PIT (10мс).
extern volatile int shell_ctrl_c_pending;

// Текущий путь и inode текущего каталога (LufiraFS) — макросы поверх полей
// current_process: шелл — тоже процесс и должен видеть/менять ту же cwd,
// что и SYS_CHDIR/SYS_GETCWD (kernel/system/syscall/syscall.c), а не
// отдельную копию. current_process здесь гарантированно не NULL — заголовок
// используется только кодом процесса "shell".
#define cwd_path (current_process->cwd_path)
#define cwd_inode (current_process->cwd_inode)
