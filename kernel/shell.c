/* shell.c - простой shell для LufiraOS */

#include "shell.h"
#include "keyboard.h"
#include "string.h"
#include <stddef.h>

/* Внешние функции из kernel.c */
extern void terminal_clear(void);
extern void terminal_writestring(const char* str);
extern void terminal_putchar(char c);
extern uint32_t terminal_get_y(void);
extern void terminal_set_cursor(uint32_t x, uint32_t y);

/* Буфер для ввода */
static char input_buffer[MAX_COMMAND_LENGTH];

/* Список команд */
static command_t commands[] = {
    {"help", "Display this help message", cmd_help},
    {"clear", "Clear the screen", cmd_clear},
    {"shutdown", "Shutdown the system", cmd_shutdown},
    {"reboot", "Reboot the system", cmd_reboot},
    {"echo", "Echo arguments", cmd_echo},
    {"info", "Display system information", cmd_info},
    {NULL, NULL, NULL}
};

/* Вывести приглашение */
static void print_prompt(void) {
    terminal_writestring("lufira> ");
}

/* Команда help */
void cmd_help(void) {
    terminal_writestring("\nAvailable commands:\n");
    terminal_writestring("===================\n\n");
    
    for (int i = 0; commands[i].name != NULL; i++) {
        terminal_writestring("  ");
        terminal_writestring(commands[i].name);
        
        // Выравнивание
        int spaces = 12 - strlen(commands[i].name);
        for (int j = 0; j < spaces; j++) {
            terminal_putchar(' ');
        }
        
        terminal_writestring("- ");
        terminal_writestring(commands[i].description);
        terminal_putchar('\n');
    }
    terminal_putchar('\n');
}

/* Команда clear */
void cmd_clear(void) {
    terminal_clear();
}

/* Команда shutdown */
void cmd_shutdown(void) {
    terminal_writestring("\nSystem shutting down...\n");
    
    // Попытка выключения через ACPI (в QEMU)
    __asm__ volatile(
        "movw $0x2000, %%ax\n"
        "movw $0x604, %%dx\n"
        "outw %%ax, %%dx\n"
        :
        :
        : "ax", "dx"
    );
    
    // Если не сработало, просто останавливаем процессор
    __asm__ volatile("cli");
    while (1) {
        __asm__ volatile("hlt");
    }
}

/* Команда reboot */
void cmd_reboot(void) {
    terminal_writestring("\nRebooting system...\n");
    
    // Отправка команды перезагрузки контроллеру клавиатуры
    uint8_t temp;
    do {
        __asm__ volatile("inb $0x64, %0" : "=a"(temp));
    } while (temp & 0x02);
    
    __asm__ volatile("movb $0xFE, %al\n"
                     "outb %al, $0x64");
    
    // Если не сработало, тройная ошибка
    __asm__ volatile("int $0x19");
}

/* Команда echo */
void cmd_echo(void) {
    // Пропускаем "echo " в буфере
    char* args = input_buffer + 5;
    
    // Пропускаем начальные пробелы
    while (*args == ' ') args++;
    
    if (*args != '\0') {
        terminal_writestring(args);
    }
    terminal_putchar('\n');
}

/* Команда info */
void cmd_info(void) {
    terminal_writestring("\nLufiraOS System Information\n");
    terminal_writestring("===========================\n");
    terminal_writestring("* Version: 0.1.0\n");
    terminal_writestring("* Architecture: i386\n");
    terminal_writestring("* Kernel: 32-bit protected mode\n");
    terminal_writestring("* Memory: 1MB conventional\n");
    terminal_writestring("* Features: VGA text, Keyboard, Shell\n");
    terminal_writestring("* Build: " __DATE__ " " __TIME__ "\n\n");
}

/* Разбить строку на аргументы */
static int parse_arguments(char* str, char* argv[], int max_args) {
    int argc = 0;
    int in_arg = 0;
    
    while (*str && argc < max_args) {
        if (*str == ' ' || *str == '\t') {
            if (in_arg) {
                *str = '\0';
                in_arg = 0;
            }
        } else {
            if (!in_arg) {
                argv[argc++] = str;
                in_arg = 1;
            }
        }
        str++;
    }
    
    return argc;
}

/* Выполнить команду */
void shell_execute(const char* command) {
    // Копируем команду в буфер для обработки
    strcpy(input_buffer, command);
    
    // Разбиваем на аргументы
    char* argv[MAX_ARGUMENTS];
    int argc = parse_arguments(input_buffer, argv, MAX_ARGUMENTS);
    
    if (argc == 0) {
        return;
    }
    
    // Ищем команду
    for (int i = 0; commands[i].name != NULL; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            commands[i].handler();
            return;
        }
    }
    
    // Команда не найдена
    terminal_writestring("Command not found: ");
    terminal_writestring(argv[0]);
    terminal_writestring("\nType 'help' for available commands.\n");
}

/* Запуск shell */
void shell_start(void) {
    terminal_writestring("LufiraOS Shell v0.1\n");
    terminal_writestring("Type 'help' for available commands.\n\n");
    
    while (1) {
        print_prompt();
        
        // Получаем ввод от пользователя
        keyboard_getline(input_buffer, MAX_COMMAND_LENGTH);
        
        // Выполняем команду
        if (strlen(input_buffer) > 0) {
            shell_execute(input_buffer);
        }
    }
}