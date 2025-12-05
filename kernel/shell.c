/* shell.c - простой shell для LufiraOS */

#include "shell.h"
#include "keyboard.h"
#include "string.h"
#include <stddef.h>

/* Константы VGA */
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000
#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA 0x3D5

/* Текущий цвет терминала */
static uint8_t terminal_color = 0x0F; /* Белый на черном */

/* VGA буфер */
static uint16_t* vga_buffer = (uint16_t*)VGA_MEMORY;

/* Функция для отправки команды в порт */
static inline void outb(uint16_t port, uint8_t value) {
    asm volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* Функция для чтения из порта */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

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

/* Очистка экрана */
void terminal_clear(void) {
    uint16_t blank = make_vgaentry(' ', terminal_color);

    for (uint32_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = blank;
    }

    cursor_x = 0;
    cursor_y = 0;
    update_hardware_cursor();
}

/* Поместить символ в определенную позицию */
static void terminal_putentryat(char c, uint8_t color, uint32_t x, uint32_t y) {
    uint32_t index = y * VGA_WIDTH + x;
    vga_buffer[index] = make_vgaentry(c, color);
}

/* Получить символ из определенной позиции */
static uint16_t terminal_getentryat(uint32_t x, uint32_t y) {
    uint32_t index = y * VGA_WIDTH + x;
    return vga_buffer[index];
}

/* Прокрутка экрана */
static void terminal_scroll(void) {
    /* Копируем строки на одну вверх */
    for (uint32_t y = 1; y < VGA_HEIGHT; y++) {
        for (uint32_t x = 0; x < VGA_WIDTH; x++) {
            uint32_t src_index = y * VGA_WIDTH + x;
            uint32_t dst_index = (y - 1) * VGA_WIDTH + x;
            vga_buffer[dst_index] = vga_buffer[src_index];
        }
    }

    /* Очищаем последнюю строку */
    uint16_t blank = make_vgaentry(' ', terminal_color);
    uint32_t last_line_start = (VGA_HEIGHT - 1) * VGA_WIDTH;
    for (uint32_t x = 0; x < VGA_WIDTH; x++) {
        vga_buffer[last_line_start + x] = blank;
    }
}

/* Обновить аппаратный курсор */
void update_hardware_cursor(void) {
    uint16_t cursor_location = (uint16_t)(cursor_y * VGA_WIDTH + cursor_x);

    outb(VGA_CRTC_INDEX, 14);                    /* старший байт */
    outb(VGA_CRTC_DATA, (uint8_t)(cursor_location >> 8));
    outb(VGA_CRTC_INDEX, 15);                    /* младший байт */
    outb(VGA_CRTC_DATA, (uint8_t)(cursor_location & 0xFF));
}

/* Спрятать аппаратный курсор */
void disable_cursor(void) {
    outb(VGA_CRTC_INDEX, 10);
    outb(VGA_CRTC_DATA, 32); /* Устанавливаем сканирующую линию курсора вне экрана */
}

/* Вывести символ */
void terminal_putchar(char c) {
    /* Обработка backspace */
    if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
        } else if (cursor_y > 0) {
            cursor_y--;
            cursor_x = VGA_WIDTH - 1;
        } else {
            /* В начале экрана, ничего не делаем */
            return;
        }
        
        /* Стираем символ на экране */
        terminal_putentryat(' ', terminal_color, cursor_x, cursor_y);
        update_hardware_cursor();
        return;
    }

    /* Обработка перевода строки */
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        
        if (cursor_y >= VGA_HEIGHT) {
            terminal_scroll();
            cursor_y = VGA_HEIGHT - 1;
        }
        update_hardware_cursor();
        return;
    }

    /* Обработка возврата каретки */
    if (c == '\r') {
        cursor_x = 0;
        update_hardware_cursor();
        return;
    }

    /* Обработка табуляции */
    if (c == '\t') {
        cursor_x = (cursor_x + 8) & ~(8 - 1);
        if (cursor_x >= VGA_WIDTH) {
            cursor_x = 0;
            cursor_y++;
        }
        
        if (cursor_y >= VGA_HEIGHT) {
            terminal_scroll();
            cursor_y = VGA_HEIGHT - 1;
        }
        
        update_hardware_cursor();
        return;
    }

    /* Вывод обычного символа */
    terminal_putentryat(c, terminal_color, cursor_x, cursor_y);

    cursor_x++;
    if (cursor_x >= VGA_WIDTH) {
        cursor_x = 0;
        cursor_y++;
        
        if (cursor_y >= VGA_HEIGHT) {
            terminal_scroll();
            cursor_y = VGA_HEIGHT - 1;
        }
    }

    /* Обновляем аппаратный курсор после каждого символа */
    update_hardware_cursor();
}

/* Вывести строку */
void terminal_writestring(const char* str) {
    while (str && *str) {
        terminal_putchar(*str++);
    }
}

/* Получить текущую позицию Y */
uint32_t terminal_get_y(void) {
    return cursor_y;
}

/* Установить позицию курсора */
void terminal_set_cursor(uint32_t x, uint32_t y) {
    cursor_x = x;
    cursor_y = y;

    /* Проверяем границы */
    if (cursor_x >= VGA_WIDTH) cursor_x = VGA_WIDTH - 1;
    if (cursor_y >= VGA_HEIGHT) cursor_y = VGA_HEIGHT - 1;

    update_hardware_cursor();
}

/* Инициализация терминала */
void terminal_init(void) {
    terminal_clear();
}

/* Установить цвет текста */
void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

/* Вывести приглашение */
static void print_prompt(void) {
    /* Проверяем, не в начале ли строки мы */
    if (cursor_x != 0) {
        terminal_putchar('\n');
    }
    terminal_writestring("lufira> ");
}

/* Команда help */
void cmd_help(void) {
    terminal_writestring("\nAvailable commands:\n");
    terminal_writestring("===================\n\n");

    for (int i = 0; commands[i].name != NULL; i++) {
        terminal_writestring("  ");
        terminal_writestring(commands[i].name);

        /* Выравнивание */
        int spaces = 12 - (int)strlen(commands[i].name);
        if (spaces < 1) spaces = 1;
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

    /* Попытка выключения через ACPI (в QEMU) */
    __asm__ volatile(
        "movw $0x2000, %%ax\n"
        "movw $0x604, %%dx\n"
        "outw %%ax, %%dx\n"
        :
        :
        : "ax", "dx"
    );

    /* Если не сработало, просто останавливаем процессор */
    __asm__ volatile("cli");
    while (1) {
        __asm__ volatile("hlt");
    }
}

/* Команда reboot */
void cmd_reboot(void) {
    terminal_writestring("\nRebooting system...\n");

    /* Отправка команды перезагрузки контроллеру клавиатуры */
    uint8_t temp;
    do {
        __asm__ volatile("inb $0x64, %0" : "=a"(temp));
    } while (temp & 0x02);

    __asm__ volatile("movb $0xFE, %al\n"
                     "outb %al, $0x64");

    /* Если не сработало, тройная ошибка */
    __asm__ volatile("int $0x19");
}

/* Команда echo */
void cmd_echo(void) {
    /* Пропускаем "echo " в буфере (если есть) */
    char* args = input_buffer + 4; /* "echo" длина 4 */
    /* Пропускаем начальные пробелы */
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
    terminal_writestring(" Version: 0.1.0\n");
    terminal_writestring("* Architecture: i386\n");
    terminal_writestring("* Kernel: 32-bit protected mode\n");
    terminal_writestring("* Memory: 1MB conventional\n");
    terminal_writestring("* Features: VGA text, Keyboard, Shell\n");

#ifdef __DATE__
    terminal_writestring("* Build: ");
    terminal_writestring(__DATE__);
    terminal_writestring(" ");
#endif
#ifdef __TIME__
    terminal_writestring(__TIME__);
    terminal_writestring("\n\n");
#else
    terminal_writestring("\n\n");
#endif
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
    /* Копируем команду в буфер для обработки */
    strncpy(input_buffer, command, MAX_COMMAND_LENGTH - 1);
    input_buffer[MAX_COMMAND_LENGTH - 1] = '\0';

    /* Разбиваем на аргументы */
    char* argv[MAX_ARGUMENTS];
    int argc = parse_arguments(input_buffer, argv, MAX_ARGUMENTS);

    if (argc == 0) {
        return;
    }

    /* Ищем команду */
    for (int i = 0; commands[i].name != NULL; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            commands[i].handler();
            return;
        }
    }

    /* Команда не найдена */
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

        /* Получаем ввод от пользователя */
        keyboard_getline(input_buffer, MAX_COMMAND_LENGTH);

        /* Выполняем команду */
        if (strlen(input_buffer) > 0) {
            shell_execute(input_buffer);
        }
    }
}