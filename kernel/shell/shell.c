#include "shell.h"
#include "../system/commands.h"
#include "../drivers/console.h"
#include "../drivers/keyboard.h"

// Глобальные переменные для редактирования команд
static uint32_t prompt_x = 0;
static uint32_t prompt_y = 0;
static uint32_t cursor_position_in_line = 0;
static uint32_t command_start_x = 0;
static uint32_t command_start_y = 0;
static char current_line[INPUT_BUFFER_SIZE] = {0};
static uint32_t current_line_length = 0;

// Простая реализация strcmp
int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
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

// Обновление строки ввода на экране
void shell_refresh_input_line(void) {
    // Сохраняем текущую позицию
    uint32_t saved_x = current_x;
    uint32_t saved_y = current_y;
    
    // Переходим к началу команды
    set_cursor_position(command_start_x, command_start_y);
    
    // Очищаем область команды
    for (uint32_t i = 0; i < current_line_length + 10; i++) {
        put_char_graphic(' ', command_start_x + i, command_start_y, 
                         convert_color(0xFFFFFF), convert_color(0x000000));
    }
    
    // Выводим текущую строку
    for (uint32_t i = 0; i < current_line_length; i++) {
        put_char_graphic(current_line[i], command_start_x + i, command_start_y,
                         convert_color(0xFFFFFF), convert_color(0x000000));
    }
    
    // Устанавливаем курсор в правильную позицию
    set_cursor_position(command_start_x + cursor_position_in_line, command_start_y);
    
    // Восстанавливаем цвет
    current_color = convert_color(0xFFFFFF);
}

void shell_handle_char(char c) {
    if (current_line_length >= INPUT_BUFFER_SIZE - 1) return;
    
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

void execute_command(void) {
    if (input_buffer_index == 0) return;
    
    // Используем сравнение без учета регистра для определения команды
    if (strcmp_case_insensitive(input_buffer, "help") == 0) {
        command_help();
    } else if (strcmp_case_insensitive(input_buffer, "clear") == 0) {
        command_clear();
    } else if (strcmp_case_insensitive(input_buffer, "reboot") == 0) {
        command_reboot();
    } else if (strcmp_case_insensitive(input_buffer, "shutdown") == 0) {
        command_shutdown();
    } else if (strcmp_case_insensitive(input_buffer, "version") == 0) {
        command_version();
    } else if (strcmp_case_insensitive(input_buffer, "echo") == 0) {
        printf("\nUsage: echo <text>\n");
    } else {
        // Проверим, не начинается ли команда с "echo "
        int echo_prefix = 1;
        for (int i = 0; i < 5; i++) {
            if (to_lower(input_buffer[i]) != "echo "[i]) {
                echo_prefix = 0;
                break;
            }
        }
        
        if (echo_prefix && input_buffer[4] == ' ') {
            // Это команда echo с аргументами
            printf("\n");
            // Пропускаем "echo " и выводим остальное
            for (int i = 5; i < input_buffer_index; i++) {
                put_char(input_buffer[i]);
            }
            put_char('\n');
        } else {
            printf("\nUnknown command: %s\n", input_buffer);
            printf("Type 'help' for available commands.\n");
        }
    }
    
    // Очищаем буфер
    input_buffer_index = 0;
}

void show_prompt(void) {
    current_color = convert_color(0xFFFFFF);
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
    
    // Восстанавливаем видимость курсора если он был стерт
    if (cursor_enabled && !cursor_visible) {
        draw_cursor();
    }
}