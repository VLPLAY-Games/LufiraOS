#include "shell.h"
#include "../system/commands.h"
#include "../drivers/console.h"
#include "../drivers/keyboard.h"

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

void execute_command(void) {
    input_buffer[input_buffer_index] = '\0';
    
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
    
    // Сбрасываем состояние ввода
    input_buffer_index = 0;
    for (int i = 0; i < INPUT_BUFFER_SIZE; i++) {
        input_buffer[i] = 0;
    }
    
    // Восстанавливаем видимость курсора если он был стерт
    if (cursor_enabled && !cursor_visible) {
        draw_cursor();
    }
}