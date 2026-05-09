#include "shell.h"
#include "../system/commands.h"
#include "../drivers/console.h"
#include "../drivers/keyboard.h"
#include "../fs/fat.h"

extern fat_fs_t fatfs;

#define HISTORY_SIZE 20

// Текущий рабочий каталог
char cwd_path[256] = "/";
uint32_t cwd_first_cluster = 0;   // 0 = корень

static uint32_t cursor_position_in_line = 0;
static uint32_t command_start_x = 0;
static uint32_t command_start_y = 0;
static char current_line[INPUT_BUFFER_SIZE] = {0};
static uint32_t current_line_length = 0;

static char command_history[HISTORY_SIZE][INPUT_BUFFER_SIZE] = {0};
static int history_count = 0;
static int history_index = -1;
static int history_current = 0;

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}
int strncmp(const char* s1, const char* s2, size_t n) {
    if (n == 0) return 0;
    while (n-- && *s1 && (*s1 == *s2)) { s1++; s2++; }
    if (n == (size_t)-1) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}
char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}
int strcmp_case_insensitive(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        char c1 = to_lower(*s1), c2 = to_lower(*s2);
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return to_lower(*s1) - to_lower(*s2);
}
void add_to_history(const char* command) {
    if (command[0] == '\0') return;
    if (history_count > 0 && strcmp(command_history[history_count - 1], command) == 0) return;
    if (history_count >= HISTORY_SIZE) {
        for (int i = 1; i < HISTORY_SIZE; i++) strcpy(command_history[i-1], command_history[i]);
        history_count--;
    }
    strcpy(command_history[history_count], command);
    history_count++;
    history_index = -1;
    history_current = history_count;
}
const char* get_history_command(int index) {
    if (index < 0 || index >= history_count) return NULL;
    return command_history[index];
}
void strcpy(char* dest, const char* src) {
    while (*src) *dest++ = *src++;
    *dest = '\0';
}
void shell_refresh_input_line(void) {
    set_cursor_position(command_start_x, command_start_y);
    for (uint32_t i = 0; i < screen_width_chars - command_start_x; i++)
        put_char_graphic(' ', command_start_x + i, command_start_y, current_color, current_bg_color);
    for (uint32_t i = 0; i < current_line_length; i++)
        put_char_graphic(current_line[i], command_start_x + i, command_start_y, current_color, current_bg_color);
    set_cursor_position(command_start_x + cursor_position_in_line, command_start_y);
}
void load_command_from_history(int history_idx) {
    const char* command = get_history_command(history_idx);
    if (command == NULL) return;
    strcpy(current_line, command);
    current_line_length = 0;
    while (current_line[current_line_length] != '\0') current_line_length++;
    cursor_position_in_line = current_line_length;
    shell_refresh_input_line();
}
void shell_handle_char(char c) {
    if (current_line_length >= INPUT_BUFFER_SIZE - 1) return;
    history_index = -1;
    if (cursor_position_in_line < current_line_length) {
        for (uint32_t i = current_line_length; i > cursor_position_in_line; i--)
            current_line[i] = current_line[i-1];
    }
    current_line[cursor_position_in_line] = c;
    current_line_length++;
    cursor_position_in_line++;
    shell_refresh_input_line();
}
void shell_handle_backspace(void) {
    if (cursor_position_in_line == 0) return;
    history_index = -1;
    for (uint32_t i = cursor_position_in_line - 1; i < current_line_length - 1; i++)
        current_line[i] = current_line[i+1];
    current_line_length--;
    cursor_position_in_line--;
    current_line[current_line_length] = '\0';
    shell_refresh_input_line();
}
void shell_handle_enter(void) {
    for (uint32_t i = 0; i < current_line_length; i++) input_buffer[i] = current_line[i];
    input_buffer[current_line_length] = '\0';
    input_buffer_index = current_line_length;
    if (current_line_length > 0) add_to_history(input_buffer);
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
        if (current_line_length > 0) {
            if (history_current < HISTORY_SIZE)
                strcpy(command_history[history_current], current_line);
        }
        history_index = history_count - 1;
    } else if (history_index > 0) history_index--;
    load_command_from_history(history_index);
}
void shell_handle_down_arrow(void) {
    if (history_count == 0) return;
    if (history_index != -1) {
        if (history_index < history_count - 1) {
            history_index++;
            load_command_from_history(history_index);
        } else {
            history_index = -1;
            current_line[0] = '\0';
            current_line_length = 0;
            cursor_position_in_line = 0;
            if (history_current < HISTORY_SIZE && command_history[history_current][0] != '\0') {
                strcpy(current_line, command_history[history_current]);
                current_line_length = 0;
                while (current_line[current_line_length] != '\0') current_line_length++;
                cursor_position_in_line = current_line_length;
            }
            shell_refresh_input_line();
        }
    }
}
void execute_command(void) {
    if (input_buffer_index == 0) return;
    char cmd_lower[INPUT_BUFFER_SIZE];
    for (uint32_t i = 0; i < input_buffer_index; i++) cmd_lower[i] = to_lower(input_buffer[i]);
    cmd_lower[input_buffer_index] = '\0';

    char* args = cmd_lower;
    while (*args != '\0' && *args != ' ') args++;
    if (*args != '\0') { *args = '\0'; args++; while (*args == ' ') args++; }

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
        for (int i = 0; i < history_count; i++) printf("  %d: %s\n", i+1, command_history[i]);
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
        if (*args == '\0') printf("\nUsage: echo <text>\n");
        else printf("\n%s\n", args);
    } else if (strcmp(cmd_lower, "status") == 0) {
        command_status();
    } else if (strcmp(cmd_lower, "trap") == 0) {
        command_trap();
    } else if (strcmp(cmd_lower, "pwd") == 0) {
        printf("\n%s\n", cwd_path);
    } else if (strcmp(cmd_lower, "cd") == 0) {
        if (*args == '\0') {
            printf("\nUsage: cd <directory>\n");
        } else {
            command_cd(args);
        }
    } else if (strcmp(cmd_lower, "ls") == 0) {
        command_ls(args);   // передаём аргумент (-l или пусто)
    } else if (strcmp(cmd_lower, "mkdir") == 0) {
        if (*args == '\0') printf("\nUsage: mkdir <name>\n");
        else command_mkdir(args);
    } else if (strcmp(cmd_lower, "rm") == 0) {
        if (*args == '\0') printf("\nUsage: rm <name>\n");
        else command_rm(args);
    } else if (strcmp(cmd_lower, "touch") == 0) {
        command_touch(args);
    } else if (strcmp(cmd_lower, "cat") == 0) {
        if (input_buffer_index <= 4) {
            printf("\nUsage: cat <filename>\n");
        } else {
            const char *fname = input_buffer + 4;
            while (*fname == ' ') fname++;
            if (*fname == '\0') printf("\nUsage: cat <filename>\n");
            else {
                uint32_t fsize;
                if (fat_open(&fatfs, fname, &fsize) == 0) {
                    static uint8_t file_buf[4096];
                    uint32_t to_read = fsize > sizeof(file_buf) ? sizeof(file_buf) : fsize;
                    int br = fat_read_file(&fatfs, fname, file_buf, to_read);
                    if (br > 0) {
                        printf("\n--- %s (%u bytes) ---\n", fname, fsize);
                        for (int i=0; i<br; i++) put_char(file_buf[i]);
                        printf("\n--- end ---\n");
                    } else printf("\nError reading file.\n");
                } else printf("\nFile not found: %s\n", fname);
            }
        }
    } else {
        printf("\nUnknown command: %s\n", input_buffer);
        printf("Type 'help' for available commands.\n");
    }

    input_buffer_index = 0;
    input_buffer[0] = '\0';
}
void show_prompt(void) {
    printf("\n");
    set_foreground_color(COLOR_LIGHT_CYAN);
    printf("[lufiraos@kernel]");
    set_foreground_color(COLOR_WHITE);
    printf(" %s $ ", cwd_path);
    command_start_x = current_x;
    command_start_y = current_y;
    cursor_position_in_line = 0;
    current_line_length = 0;
    for (int i = 0; i < INPUT_BUFFER_SIZE; i++) { current_line[i] = 0; input_buffer[i] = 0; }
    history_index = -1;
    if (cursor_enabled && !cursor_visible) draw_cursor();
}