#include "shell.h"
#include "../system/commands.h"
#include "../drivers/console.h"
#include "../drivers/keyboard.h"

// Размер истории команд
#define HISTORY_SIZE 20

// Глобальные переменные для редактирования команд
static uint32_t prompt_x = 0;
static uint32_t prompt_y = 0;
static uint32_t cursor_position_in_line = 0;
static uint32_t command_start_x = 0;
static uint32_t command_start_y = 0;
static char current_line[INPUT_BUFFER_SIZE] = {0};
static uint32_t current_line_length = 0;

// Переменные для истории команд
static char command_history[HISTORY_SIZE][INPUT_BUFFER_SIZE] = {0};
static int history_count = 0;
static int history_index = -1; // -1 означает, что мы не в истории
static int history_current = 0; // Текущая позиция в истории

// Простая реализация strcmp
int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

// Реализация strncmp
int strncmp(const char* s1, const char* s2, size_t n) {
    if (n == 0) return 0;
    
    while (n-- && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    
    if (n == (size_t)-1) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

// Функция приведения символа к нижнему регистру
char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

// Функция сравнения строк без учета регистра
int strcmp_case_insensitive(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        char c1 = to_lower(*s1);
        char c2 = to_lower(*s2);
        
        if (c1 != c2) {
            return c1 - c2;
        }
        
        s1++;
        s2++;
    }
    
    // Если одна строка закончилась, а другая нет
    return to_lower(*s1) - to_lower(*s2);
}

// Добавление команды в историю
void add_to_history(const char* command) {
    // Не добавляем пустые команды
    if (command[0] == '\0') return;
    
    // Не добавляем команду, если она такая же, как последняя в истории
    if (history_count > 0 && strcmp(command_history[history_count - 1], command) == 0) {
        return;
    }
    
    // Если история полна, сдвигаем все команды вверх
    if (history_count >= HISTORY_SIZE) {
        for (int i = 1; i < HISTORY_SIZE; i++) {
            strcpy(command_history[i - 1], command_history[i]);
        }
        history_count--;
    }
    
    // Добавляем новую команду в конец истории
    strcpy(command_history[history_count], command);
    history_count++;
    
    // Сбрасываем индекс истории
    history_index = -1;
    history_current = history_count;
}

// Получение команды из истории по индексу
const char* get_history_command(int index) {
    if (index < 0 || index >= history_count) {
        return NULL;
    }
    return command_history[index];
}

// Простая реализация strcpy
void strcpy(char* dest, const char* src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

// Обновление строки ввода на экране
void shell_refresh_input_line(void) {
    // Сохраняем текущую позицию
    uint32_t saved_x = current_x;
    uint32_t saved_y = current_y;
    
    // Переходим к началу команды
    set_cursor_position(command_start_x, command_start_y);
    
    // Очищаем область команды
    for (uint32_t i = 0; i < screen_width_chars - command_start_x; i++) {
        put_char_graphic(' ', command_start_x + i, command_start_y, 
                         current_color, current_bg_color);
    }
    
    // Выводим текущую строку
    for (uint32_t i = 0; i < current_line_length; i++) {
        put_char_graphic(current_line[i], command_start_x + i, command_start_y,
                         current_color, current_bg_color);
    }
    
    // Устанавливаем курсор в правильную позицию
    set_cursor_position(command_start_x + cursor_position_in_line, command_start_y);
}

// Загрузка команды из истории в текущую строку
void load_command_from_history(int history_idx) {
    const char* command = get_history_command(history_idx);
    if (command == NULL) return;
    
    // Копируем команду в текущую строку
    strcpy(current_line, command);
    current_line_length = 0;
    while (current_line[current_line_length] != '\0') {
        current_line_length++;
    }
    
    cursor_position_in_line = current_line_length;
    
    // Обновляем отображение
    shell_refresh_input_line();
}

void shell_handle_char(char c) {
    if (current_line_length >= INPUT_BUFFER_SIZE - 1) return;
    
    // Выходим из режима истории при вводе нового символа
    history_index = -1;
    
    // Если курсор не в конце строки, сдвигаем символы вправо
    if (cursor_position_in_line < current_line_length) {
        for (uint32_t i = current_line_length; i > cursor_position_in_line; i--) {
            current_line[i] = current_line[i - 1];
        }
    }
    
    // Вставляем новый символ
    current_line[cursor_position_in_line] = c;
    current_line_length++;
    cursor_position_in_line++;
    
    // Обновляем отображение
    shell_refresh_input_line();
}

void shell_handle_backspace(void) {
    if (cursor_position_in_line == 0) return;
    
    // Выходим из режима истории при редактировании
    history_index = -1;
    
    // Сдвигаем символы влево
    for (uint32_t i = cursor_position_in_line - 1; i < current_line_length - 1; i++) {
        current_line[i] = current_line[i + 1];
    }
    
    current_line_length--;
    cursor_position_in_line--;
    current_line[current_line_length] = '\0';
    
    // Обновляем отображение
    shell_refresh_input_line();
}

void shell_handle_enter(void) {
    // Копируем текущую строку в input_buffer
    for (uint32_t i = 0; i < current_line_length; i++) {
        input_buffer[i] = current_line[i];
    }
    input_buffer[current_line_length] = '\0';
    input_buffer_index = current_line_length;
    
    // Добавляем команду в историю (если она не пустая)
    if (current_line_length > 0) {
        add_to_history(input_buffer);
    }
    
    put_char('\n');
    execute_command();
    show_prompt();
}

void shell_handle_left_arrow(void) {
    if (cursor_position_in_line > 0) {
        cursor_position_in_line--;
        set_cursor_position(command_start_x + cursor_position_in_line, command_start_y);
    }
}

void shell_handle_right_arrow(void) {
    if (cursor_position_in_line < current_line_length) {
        cursor_position_in_line++;
        set_cursor_position(command_start_x + cursor_position_in_line, command_start_y);
    }
}

void shell_handle_up_arrow(void) {
    if (history_count == 0) return;
    
    if (history_index == -1) {
        // Сохраняем текущую команду как "черновик" если она не пустая
        if (current_line_length > 0) {
            // Сохраняем в history_current (за пределами истории)
            if (history_current < HISTORY_SIZE) {
                strcpy(command_history[history_current], current_line);
            }
        }
        history_index = history_count - 1;
    } else if (history_index > 0) {
        history_index--;
    }
    
    load_command_from_history(history_index);
}

void shell_handle_down_arrow(void) {
    if (history_count == 0) return;
    
    if (history_index != -1) {
        if (history_index < history_count - 1) {
            history_index++;
            load_command_from_history(history_index);
        } else {
            // Достигли конца истории - возвращаем "черновик"
            history_index = -1;
            
            // Очищаем текущую строку
            current_line[0] = '\0';
            current_line_length = 0;
            cursor_position_in_line = 0;
            
            // Проверяем, есть ли "черновик" в history_current
            if (history_current < HISTORY_SIZE && command_history[history_current][0] != '\0') {
                strcpy(current_line, command_history[history_current]);
                current_line_length = 0;
                while (current_line[current_line_length] != '\0') {
                    current_line_length++;
                }
                cursor_position_in_line = current_line_length;
            }
            
            shell_refresh_input_line();
        }
    }
}

void execute_command(void) {
    if (input_buffer_index == 0) return;

    // Делаем копию команды в нижнем регистре
    char cmd_lower[INPUT_BUFFER_SIZE];

    for (uint32_t i = 0; i < input_buffer_index; i++) {
        cmd_lower[i] = to_lower(input_buffer[i]);
    }

    cmd_lower[input_buffer_index] = '\0';

    // Ищем аргументы
    char* args = cmd_lower;

    while (*args != '\0' && *args != ' ') {
        args++;
    }

    if (*args != '\0') {
        *args = '\0';
        args++;

        while (*args == ' ') {
            args++;
        }
    }

    // ================= COMMAND DISPATCHER =================

    if (strcmp(cmd_lower, "help") == 0) {

        command_help();

    } else if (strcmp(cmd_lower, "clear") == 0) {

        command_clear();

    } else if (strcmp(cmd_lower, "reboot") == 0) {

        command_reboot();

    } else if (strcmp(cmd_lower, "shutdown") == 0) {

        command_shutdown();

    } else if (strcmp(cmd_lower, "version") == 0) {

        command_version();

    } else if (strcmp(cmd_lower, "history") == 0) {

        printf("\nCommand History (last %d commands):\n", history_count);

        for (int i = 0; i < history_count; i++) {
            printf("  %d: %s\n", i + 1, command_history[i]);
        }

    } else if (strcmp(cmd_lower, "colors") == 0) {

        command_colors();

    } else if (strcmp(cmd_lower, "reset") == 0) {

        reset_colors();
        printf("\nColors reset to default (white on black)\n");

    } else if (strcmp(cmd_lower, "color") == 0) {

        command_color();

    } else if (strcmp(cmd_lower, "fg") == 0) {

        command_fg();

    } else if (strcmp(cmd_lower, "bg") == 0) {

        command_bg();

    } else if (strcmp(cmd_lower, "echo") == 0) {

        if (*args == '\0') {
            printf("\nUsage: echo <text>\n");
        } else {
            printf("\n%s\n", args);
        }

    } else if (strcmp(cmd_lower, "status") == 0) {

        command_status();

    } else if (strcmp(cmd_lower, "trap") == 0) {

        command_trap();

    } else {

        printf("\nUnknown command: %s\n", input_buffer);
        printf("Type 'help' for available commands.\n");
    }

    // Очистка буфера
    input_buffer_index = 0;
    input_buffer[0] = '\0';
}

void show_prompt(void) {
    printf("\n[lufiraos@kernel] $ ");
    
    // Запоминаем позицию начала ввода команды
    command_start_x = current_x;
    command_start_y = current_y;
    
    // Сбрасываем состояние редактирования
    cursor_position_in_line = 0;
    current_line_length = 0;
    for (int i = 0; i < INPUT_BUFFER_SIZE; i++) {
        current_line[i] = 0;
        input_buffer[i] = 0;
    }
    
    // Сбрасываем индекс истории
    history_index = -1;
    
    // Восстанавливаем видимость курсора если он был стерт
    if (cursor_enabled && !cursor_visible) {
        draw_cursor();
    }
}