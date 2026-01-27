#include "commands.h"
#include "../drivers/console.h"
#include "../shell/shell.h"

// Простая реализация atoi
int atoi(const char* str) {
    int result = 0;
    int sign = 1;
    
    if (*str == '-') {
        sign = -1;
        str++;
    }
    
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return sign * result;
}

// Конвертация шестнадцатеричной строки в число
int hex_to_int(const char* hex) {
    int result = 0;
    
    while (*hex) {
        char c = *hex;
        int digit = 0;
        
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            break;
        }
        
        result = result * 16 + digit;
        hex++;
    }
    
    return result;
}

void command_help(void) {
    printf("\nAvailable commands:\n");
    printf("  help           - Show this help\n");
    printf("  clear          - Clear screen\n");
    printf("  reboot         - Reboot system\n");
    printf("  shutdown       - Shutdown system\n");
    printf("  version        - Show kernel version\n");
    printf("  echo           - Echo text back\n");
    printf("  history        - Show command history\n");
    printf("  color          - Set console colors or reset\n");
    printf("  colors         - Show available colors\n");
    printf("  fg <color>     - Set foreground color\n");
    printf("  bg <color>     - Set background color\n");
}

void command_clear(void) {
    clear_screen();
    show_prompt();
}

void command_reboot(void) {
    printf("\nRebooting system...\n");
    // Перезагрузка через 8042 контроллер
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    
    // Запасной метод через ACPI
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    
    // Если все еще работает
    printf("Reboot failed. Please restart manually.\n");
}

void command_shutdown(void) {
    printf("\nShutting down system...\n");
    
    // Попытка выключения через ACPI (QEMU и современные системы)
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    
    // Запасной метод для QEMU (более старый)
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x3400), "Nd"((uint16_t)0x604));
    
    // Метод для Bochs и старых версий QEMU
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004));
    
    // Если все еще работает
    printf("Shutdown command sent. System may require manual power off.\n");
}

void command_version(void) {
    printf("\nLufiraOS Kernel v0.1\n");
    printf("Built: %s %s\n", __DATE__, __TIME__);
    printf("Architecture: x86_64\n");
    printf("Color Support: 256 colors\n");
}

void command_colors(void) {
    printf("\nColor Commands:\n");
    printf("---------------\n");
    printf("Usage examples:\n");
    printf("  color 1F        - Blue background, White text (like BIOS)\n");
    printf("  color FF0000    - Red text on current background\n");
    printf("  color 0000FF 00FF00 - Blue text on Green background\n");
    printf("  color reset     - Reset to default colors\n");
    printf("  fg 4            - Red text\n");
    printf("  bg 1            - Blue background\n\n");
    
    print_color_table_16();
}

void command_color(void) {
    char* args = input_buffer + 6; // Пропускаем "color "
    
    // Пропускаем начальные пробелы
    while (*args == ' ') args++;
    
    if (*args == '\0') {
        printf("\nUsage: color <fg> [bg]\n");
        printf("       color <RRGGBB> [RRGGBB]\n");
        printf("       color reset\n");
        printf("Examples:\n");
        printf("  color 1F           - Blue bg, White text\n");
        printf("  color FF0000       - Red text\n");
        printf("  color 0000FF 00FF00 - Blue on Green\n");
        printf("  color reset        - Reset to default colors\n");
        return;
    }
    
    // Проверяем, не является ли аргумент "reset"
    if (strncmp(args, "reset", 5) == 0 && (args[5] == ' ' || args[5] == '\0')) {
        reset_colors();
        printf("\nColors reset to default (white on black)\n");
        return;
    }
    
    // Определяем тип аргументов (шестнадцатеричные цвета или индексы)
    int len = 0;
    char* temp = args;
    while (*temp != ' ' && *temp != '\0') {
        len++;
        temp++;
    }
    
    if (len == 1 || len == 2) {
        // Это индекс (1-2 символа)
        int fg_index = hex_to_int(args);
        int bg_index = COLOR_BLACK;
        
        // Ищем второй аргумент
        temp = args + len;
        while (*temp == ' ') temp++;
        
        if (*temp != '\0') {
            int len2 = 0;
            char* temp2 = temp;
            while (*temp2 != ' ' && *temp2 != '\0') {
                len2++;
                temp2++;
            }
            
            if (len2 == 1 || len2 == 2) {
                bg_index = hex_to_int(temp);
            }
        }
        
        if (fg_index >= 0 && fg_index <= 255 && 
            bg_index >= 0 && bg_index <= 255) {
            set_color_by_index(fg_index, bg_index);
            printf("\nColors set: Text=%d (%s), Background=%d (%s)\n", 
                   fg_index, get_color_name(fg_index), 
                   bg_index, get_color_name(bg_index));
        } else {
            printf("\nError: Color indices must be between 0-255\n");
        }
    } else if (len == 6) {
        // Это RGB цвет (6 символов)
        uint32_t fg_rgb = hex_to_int(args);
        uint32_t bg_rgb = 0x000000; // Черный по умолчанию
        
        // Ищем второй аргумент
        temp = args + 6;
        while (*temp == ' ') temp++;
        
        if (*temp != '\0') {
            int len2 = 0;
            char* temp2 = temp;
            while (*temp2 != ' ' && *temp2 != '\0') {
                len2++;
                temp2++;
            }
            
            if (len2 == 6) {
                bg_rgb = hex_to_int(temp);
            }
        }
        
        set_color_by_rgb(fg_rgb, bg_rgb);
        ConsoleColor closest_fg = find_closest_color(fg_rgb);
        ConsoleColor closest_bg = find_closest_color(bg_rgb);
        printf("\nColors set: Text=#%06X (~%s), Background=#%06X (~%s)\n", 
               fg_rgb, get_color_name(closest_fg), 
               bg_rgb, get_color_name(closest_bg));
    } else {
        printf("\nError: Invalid color format. Use:\n");
        printf("  - 1-2 digit index (0-255)\n");
        printf("  - 6 digit RGB (RRGGBB)\n");
        printf("  - 'reset' to reset colors\n");
    }
}

void command_fg(void) {
    char* args = input_buffer + 3; // Пропускаем "fg "
    
    // Пропускаем начальные пробелы
    while (*args == ' ') args++;
    
    if (*args == '\0') {
        printf("\nUsage: fg <color>\n");
        printf("       fg <RRGGBB>\n");
        printf("Examples:\n");
        printf("  fg 4     - Red text\n");
        printf("  fg FF0000 - Bright red text\n");
        return;
    }
    
    int len = 0;
    char* temp = args;
    while (*temp != ' ' && *temp != '\0') {
        len++;
        temp++;
    }
    
    if (len == 1 || len == 2) {
        // Индекс цвета
        int color_index = hex_to_int(args);
        if (color_index >= 0 && color_index <= 255) {
            set_foreground_color(color_index);
            printf("\nForeground color set to %d (%s)\n", 
                   color_index, get_color_name(color_index));
        } else {
            printf("\nError: Color index must be between 0-255\n");
        }
    } else if (len == 6) {
        // RGB цвет
        uint32_t rgb = hex_to_int(args);
        set_foreground_rgb(rgb);
        ConsoleColor closest = find_closest_color(rgb);
        printf("\nForeground color set to #%06X (~%s)\n", rgb, get_color_name(closest));
    } else {
        printf("\nError: Invalid color format\n");
    }
}

void command_bg(void) {
    char* args = input_buffer + 3; // Пропускаем "bg "
    
    // Пропускаем начальные пробелы
    while (*args == ' ') args++;
    
    if (*args == '\0') {
        printf("\nUsage: bg <color>\n");
        printf("       bg <RRGGBB>\n");
        printf("Examples:\n");
        printf("  bg 1     - Blue background\n");
        printf("  bg 0000FF - Blue background\n");
        return;
    }
    
    int len = 0;
    char* temp = args;
    while (*temp != ' ' && *temp != '\0') {
        len++;
        temp++;
    }
    
    if (len == 1 || len == 2) {
        // Индекс цвета
        int color_index = hex_to_int(args);
        if (color_index >= 0 && color_index <= 255) {
            set_background_color(color_index);
            printf("\nBackground color set to %d (%s)\n", 
                   color_index, get_color_name(color_index));
        } else {
            printf("\nError: Color index must be between 0-255\n");
        }
    } else if (len == 6) {
        // RGB цвет
        uint32_t rgb = hex_to_int(args);
        set_background_rgb(rgb);
        ConsoleColor closest = find_closest_color(rgb);
        printf("\nBackground color set to #%06X (~%s)\n", rgb, get_color_name(closest));
    } else {
        printf("\nError: Invalid color format\n");
    }
}