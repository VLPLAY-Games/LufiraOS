#include "commands.h"
#include "../drivers/console.h"
#include "../shell/shell.h"

// --- Вспомогательные функции ---
static const char* skip_spaces(const char* s) {
    while (*s == ' ' || *s == '\t') { s++; }
    return s;
}
static int token_length(const char* s) {
    int len = 0;
    while (s[len] != '\0' && s[len] != ' ' && s[len] != '\t') { len++; }
    return len;
}
static int token_equals(const char* s, const char* word) {
    int i = 0;
    while (word[i] != '\0' && s[i] == word[i]) { i++; }
    return word[i] == '\0' && (s[i] == '\0' || s[i] == ' ' || s[i] == '\t');
}
static int interrupts_enabled(void) {
    uint64_t rflags;
    asm volatile ("pushfq; pop %0" : "=r"(rflags));
    return (int)((rflags >> 9) & 1ULL);
}
int atoi(const char* str) {
    int result = 0, sign = 1;
    if (*str == '-') { sign = -1; str++; }
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return sign * result;
}
int hex_to_int(const char* hex) {
    int result = 0;
    while (*hex) {
        char c = *hex;
        int digit = 0;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        result = result * 16 + digit;
        hex++;
    }
    return result;
}

void command_help(void) {
    printf("\nAvailable commands:\n");
    printf(" help - Show this help\n");
    printf(" clear - Clear screen\n");
    printf(" reboot - Reboot system\n");
    printf(" shutdown - Shutdown system\n");
    printf(" version - Show kernel version\n");
    printf(" echo - Echo text back\n");
    printf(" history - Show command history\n");
    printf(" status - Show interrupt/CPU status\n");
    printf(" trap - Trigger test exceptions\n");
    printf(" color - Set console colors or reset\n");
    printf(" colors - Show available colors\n");
    printf(" fg <color> - Set foreground color\n");
    printf(" bg <color> - Set background color\n");
    printf("\nTrap examples:\n");
    printf(" trap int3 - Breakpoint exception\n");
    printf(" trap ud2 - Invalid opcode exception\n");
    printf(" trap pf - Page fault exception\n");
    printf(" trap sti - Enable interrupts\n");
    printf(" trap cli - Disable interrupts\n");
    printf(" trap hlt - Halt CPU until interrupt\n");
}
void command_clear(void) {
    clear_screen();
    show_prompt();
}
void command_reboot(void) {
    printf("\nRebooting system...\n");
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    printf("Reboot failed. Please restart manually.\n");
}
void command_shutdown(void) {
    printf("\nShutting down system...\n");
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x3400), "Nd"((uint16_t)0x604));
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004));
    printf("Shutdown command sent. System may require manual power off.\n");
}
void command_version(void) {
    printf("\nLufiraOS Kernel v0.1\n");
    printf("Built: %s %s\n", __DATE__, __TIME__);
    printf("Architecture: x86_64\n");
    printf("Color Support: 256 colors\n");
}
void command_status(void) {
    printf("\nSYSTEM STATUS:\n");
    printf("--------------\n");
    printf(" Interrupt Flag: %s\n", interrupts_enabled() ? "SET" : "CLEAR");
    printf(" Interrupts: %s\n", interrupts_enabled() ? "ENABLED" : "DISABLED");
    printf(" CPU Test: trap int3 / ud2 / pf\n");
    printf(" Hint: INT3 and UD2 work even when IF=0\n");
}
void command_colors(void) {
    printf("\nColor Commands:\n");
    printf("---------------\n");
    printf("Usage examples:\n");
    printf(" color 1F - Blue background, White text (like BIOS)\n");
    printf(" color FF0000 - Red text on current background\n");
    printf(" color 0000FF 00FF00 - Blue text on Green background\n");
    printf(" color reset - Reset to default colors\n");
    printf(" fg 4 - Red text\n");
    printf(" bg 1 - Blue background\n\n");
    print_color_table_16();
}
void command_color(void) {
    char* args = (char*)skip_spaces(input_buffer + 6);
    if (*args == '\0') {
        printf("\nUsage: color <fg> [bg]\n");
        printf(" color <RRGGBB> [RRGGBB]\n");
        printf(" color reset\n");
        printf("Examples:\n");
        printf(" color 1F - Blue bg, White text\n");
        printf(" color FF0000 - Red text\n");
        printf(" color 0000FF 00FF00 - Blue on Green\n");
        printf(" color reset - Reset to default colors\n");
        return;
    }
    if (token_equals(args, "reset")) {
        reset_colors();
        printf("\nColors reset to default (white on black)\n");
        return;
    }
    int len = token_length(args);
    if (len == 1 || len == 2) {
        int fg_index = hex_to_int(args);
        int bg_index = COLOR_BLACK;
        const char* second = skip_spaces(args + len);
        if (*second != '\0') {
            int len2 = token_length(second);
            if (len2 == 1 || len2 == 2) bg_index = hex_to_int(second);
        }
        if (fg_index >= 0 && fg_index <= 255 && bg_index >= 0 && bg_index <= 255) {
            set_color_by_index((ConsoleColor)fg_index, (ConsoleColor)bg_index);
            printf("\nColors set: Text=%d (%s), Background=%d (%s)\n",
                   fg_index, get_color_name((ConsoleColor)fg_index),
                   bg_index, get_color_name((ConsoleColor)bg_index));
        } else {
            printf("\nError: Color indices must be between 0-255\n");
        }
    } else if (len == 6) {
        uint32_t fg_rgb = (uint32_t)hex_to_int(args);
        uint32_t bg_rgb = 0x000000;
        const char* second = skip_spaces(args + 6);
        if (*second != '\0') {
            int len2 = token_length(second);
            if (len2 == 6) bg_rgb = (uint32_t)hex_to_int(second);
        }
        set_color_by_rgb(fg_rgb, bg_rgb);
        ConsoleColor closest_fg = find_closest_color(fg_rgb);
        ConsoleColor closest_bg = find_closest_color(bg_rgb);
        printf("\nColors set: Text=#%06X (~%s), Background=#%06X (~%s)\n",
               fg_rgb, get_color_name(closest_fg),
               bg_rgb, get_color_name(closest_bg));
    } else {
        printf("\nError: Invalid color format. Use:\n");
        printf(" - 1-2 digit index (0-255)\n");
        printf(" - 6 digit RGB (RRGGBB)\n");
        printf(" - 'reset' to reset colors\n");
    }
}
void command_fg(void) {
    char* args = (char*)skip_spaces(input_buffer + 3);
    if (*args == '\0') {
        printf("\nUsage: fg <color>\n");
        printf(" fg <RRGGBB>\n");
        printf("Examples:\n");
        printf(" fg 4 - Red text\n");
        printf(" fg FF0000 - Bright red text\n");
        return;
    }
    int len = token_length(args);
    if (len == 1 || len == 2) {
        int color_index = hex_to_int(args);
        if (color_index >= 0 && color_index <= 255) {
            set_foreground_color((ConsoleColor)color_index);
            printf("\nForeground color set to %d (%s)\n",
                   color_index, get_color_name((ConsoleColor)color_index));
        } else {
            printf("\nError: Color index must be between 0-255\n");
        }
    } else if (len == 6) {
        uint32_t rgb = (uint32_t)hex_to_int(args);
        set_foreground_rgb(rgb);
        ConsoleColor closest = find_closest_color(rgb);
        printf("\nForeground color set to #%06X (~%s)\n", rgb, get_color_name(closest));
    } else {
        printf("\nError: Invalid color format\n");
    }
}
void command_bg(void) {
    char* args = (char*)skip_spaces(input_buffer + 3);
    if (*args == '\0') {
        printf("\nUsage: bg <color>\n");
        printf(" bg <RRGGBB>\n");
        printf("Examples:\n");
        printf(" bg 1 - Blue background\n");
        printf(" bg 0000FF - Blue background\n");
        return;
    }
    int len = token_length(args);
    if (len == 1 || len == 2) {
        int color_index = hex_to_int(args);
        if (color_index >= 0 && color_index <= 255) {
            set_background_color((ConsoleColor)color_index);
            printf("\nBackground color set to %d (%s)\n",
                   color_index, get_color_name((ConsoleColor)color_index));
        } else {
            printf("\nError: Color index must be between 0-255\n");
        }
    } else if (len == 6) {
        uint32_t rgb = (uint32_t)hex_to_int(args);
        set_background_rgb(rgb);
        ConsoleColor closest = find_closest_color(rgb);
        printf("\nBackground color set to #%06X (~%s)\n", rgb, get_color_name(closest));
    } else {
        printf("\nError: Invalid color format\n");
    }
}
void command_trap(void) {
    char* args = (char*)skip_spaces(input_buffer + 5);
    if (*args == '\0') {
        printf("\nUsage: trap <int3|ud2|pf|cli|sti|hlt>\n");
        printf("Examples:\n");
        printf(" trap int3\n");
        printf(" trap ud2\n");
        printf(" trap pf\n");
        printf(" trap sti\n");
        return;
    }
    if (token_equals(args, "int3")) {
        printf("\nTriggering breakpoint exception...\n");
        asm volatile ("int3");
    } else if (token_equals(args, "ud2")) {
        printf("\nTriggering invalid opcode exception...\n");
        asm volatile ("ud2");
    } else if (token_equals(args, "pf")) {
        printf("\nTriggering page fault exception...\n");
        volatile uint64_t* bad = (volatile uint64_t*)0x0;
        *bad = 0xDEADBEEF;
    } else if (token_equals(args, "cli")) {
        asm volatile ("cli");
        printf("\nInterrupt Flag cleared.\n");
    } else if (token_equals(args, "sti")) {
        asm volatile ("sti");
        printf("\nInterrupt Flag set.\n");
        printf("(All IRQs are masked by PIC - no external interrupts will be received.)\n");
    } else if (token_equals(args, "hlt")) {
        printf("\nHalting CPU. (IRQs are masked, so only a reset/NMI can wake it.)\n");
        asm volatile ("hlt");
    } else {
        printf("\nUnknown trap: %s\n", args);
        printf("Available: int3, ud2, pf, cli, sti, hlt\n");
    }
}