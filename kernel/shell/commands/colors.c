#include "lib/string.h"
#include "../commands.h"
#include "drivers/console/console.h"
#include "drivers/keyboard/keyboard.h"

// color, colors, fg, bg
void command_color(void) {
    char* args = (char*)skip_spaces(input_buffer + 6);
    if (*args == '\0') {
        printf("\nUsage: color <fg> [bg]\n");
        return;
    }
    if (token_equals(args, "reset")) {
        reset_colors(); printf("\nColors reset to default\n");
        return;
    }
    int len = token_length(args);
    if (len == 1 || len == 2) {
        int fg_index = hex_to_int(args);
        int bg_index = COLOR_BLACK;
        const char* second = skip_spaces(args + len);
        if (*second != '\0') { int len2 = token_length(second); if (len2 == 1 || len2 == 2) bg_index = hex_to_int(second); }
        if (fg_index >= 0 && fg_index <= 255 && bg_index >= 0 && bg_index <= 255) {
            set_color_by_index((ConsoleColor)fg_index, (ConsoleColor)bg_index);
            printf("\nColors set: Text=%d (%s), Background=%d (%s)\n", fg_index, get_color_name((ConsoleColor)fg_index), bg_index, get_color_name((ConsoleColor)bg_index));
        } else printf("\nError: Color indices must be between 0-255\n");
    } else if (len == 6) {
        uint32_t fg_rgb = (uint32_t)hex_to_int(args);
        uint32_t bg_rgb = 0x000000;
        const char* second = skip_spaces(args + 6);
        if (*second != '\0') { int len2 = token_length(second); if (len2 == 6) bg_rgb = (uint32_t)hex_to_int(second); }
        set_color_by_rgb(fg_rgb, bg_rgb);
        ConsoleColor closest_fg = find_closest_color(fg_rgb), closest_bg = find_closest_color(bg_rgb);
        printf("\nColors set: Text=#%06X (~%s), Background=#%06X (~%s)\n", fg_rgb, get_color_name(closest_fg), bg_rgb, get_color_name(closest_bg));
    } else printf("\nError: Invalid color format\n");
}

void command_colors(void) {
    printf("\nColor Commands:\nUsage examples:\n");
    printf(" color 1F - Blue background, White text\n");
    printf(" color FF0000 - Red text on current background\n");
    printf(" color 0000FF 00FF00 - Blue text on Green background\n");
    printf(" color reset - Reset to default colors\n");
    printf(" fg 4 - Red text\n");
    printf(" bg 1 - Blue background\n\n");
    print_color_table_16();
}

void command_fg(void) {
    char* args = (char*)skip_spaces(input_buffer + 3);
    if (*args == '\0') { printf("\nUsage: fg <color>\n"); return; }
    int len = token_length(args);
    if (len == 1 || len == 2) {
        int color_index = hex_to_int(args);
        if (color_index >= 0 && color_index <= 255) {
            set_foreground_color((ConsoleColor)color_index);
            printf("\nForeground color set to %d (%s)\n", color_index, get_color_name((ConsoleColor)color_index));
        } else printf("\nError: Color index must be between 0-255\n");
    } else if (len == 6) {
        uint32_t rgb = (uint32_t)hex_to_int(args);
        set_foreground_rgb(rgb);
        ConsoleColor closest = find_closest_color(rgb);
        printf("\nForeground color set to #%06X (~%s)\n", rgb, get_color_name(closest));
    } else printf("\nError: Invalid color format\n");
}

void command_bg(void) {
    char* args = (char*)skip_spaces(input_buffer + 3);
    if (*args == '\0') { printf("\nUsage: bg <color>\n"); return; }
    int len = token_length(args);
    if (len == 1 || len == 2) {
        int color_index = hex_to_int(args);
        if (color_index >= 0 && color_index <= 255) {
            set_background_color((ConsoleColor)color_index);
            printf("\nBackground color set to %d (%s)\n", color_index, get_color_name((ConsoleColor)color_index));
        } else printf("\nError: Color index must be between 0-255\n");
    } else if (len == 6) {
        uint32_t rgb = (uint32_t)hex_to_int(args);
        set_background_rgb(rgb);
        ConsoleColor closest = find_closest_color(rgb);
        printf("\nBackground color set to #%06X (~%s)\n", rgb, get_color_name(closest));
    } else printf("\nError: Invalid color format\n");
}