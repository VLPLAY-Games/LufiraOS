#include "shell.h"
#include "commands.h"
#include "drivers/console/console.h"
#include "drivers/keyboard/keyboard.h"
#include "fs/fat/fat.h"
#include "system/process/process.h"

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
    
    char cmd_lower[INPUT_BUFFER_SIZE];
    for (uint32_t i = 0; i < input_buffer_index; i++) cmd_lower[i] = to_lower(input_buffer[i]);
    cmd_lower[input_buffer_index] = '\0';
    
    put_char('\n');
    execute_command();

    if (strcmp(cmd_lower, "clear") != 0) {
        show_prompt();
    }
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
        // ...
    } else if (strcmp(cmd_lower, "colors") == 0) {
        command_colors();
    } else if (strcmp(cmd_lower, "reset") == 0) {
        command_reset();
    } else if (strcmp(cmd_lower, "color") == 0) {
        command_color();
    } else if (strcmp(cmd_lower, "fg") == 0) {
        command_fg();
    } else if (strcmp(cmd_lower, "bg") == 0) {
        command_bg();
    } else if (strcmp(cmd_lower, "echo") == 0) {
        command_echo(args);
    } else if (strcmp(cmd_lower, "status") == 0) {
        command_status();
    } else if (strcmp(cmd_lower, "trap") == 0) {
        command_trap();
    } else if (strcmp(cmd_lower, "pwd") == 0) {
        printf("\n%s\n", cwd_path);
    } else if (strcmp(cmd_lower, "cd") == 0) {
        if (*args == '\0') printf("\nUsage: cd <directory>\n");
        else command_cd(args);
    } else if (strcmp(cmd_lower, "ls") == 0) {
        command_ls(args);
    } else if (strcmp(cmd_lower, "mkdir") == 0) {
        if (*args == '\0') printf("\nUsage: mkdir <name>\n");
        else command_mkdir(args);
    } else if (strcmp(cmd_lower, "rm") == 0) {
        if (*args == '\0') printf("\nUsage: rm <name>\n");
        else command_rm(args);
    } else if (strcmp(cmd_lower, "touch") == 0) {
        command_touch(args);
    } else if (strcmp(cmd_lower, "cat") == 0) {
        if (input_buffer_index <= 4) printf("\nUsage: cat <filename>\n");
        else command_cat(input_buffer + 4);
    } else if (strcmp(cmd_lower, "run") == 0) {
        if (*args == '\0') printf("\nUsage: run <filename>\n");
        else command_run(args);
    } else if (strcmp(cmd_lower, "exec") == 0) {
        if (*args == '\0') printf("\nUsage: exec <filename>\n");
        else command_exec(args);
    } else if (strcmp(cmd_lower, "write") == 0) {
        if (input_buffer_index <= 6) printf("\nUsage: write <filename> <text>\n");
        else command_write(input_buffer + 6);
    } else if (strcmp(cmd_lower, "cp") == 0) {
        if (*args == '\0') printf("\nUsage: cp <source> <destination>\n");
        else command_cp(args);
    } else if (strcmp(cmd_lower, "mv") == 0) {
        if (*args == '\0') printf("\nUsage: mv <source> <destination>\n");
        else command_mv(args);
    } else if (strcmp(cmd_lower, "rename") == 0) {
        if (*args == '\0') printf("\nUsage: rename <old> <new>\n");
        else command_rename(args);
    } else if (strcmp(cmd_lower, "edit") == 0) {
        if (*args == '\0') printf("\nUsage: edit <filename> <text>\n");
        else command_edit(args);
    } else if (strcmp(cmd_lower, "beep") == 0) {
        command_beep();
    } else if (strcmp(cmd_lower, "mixer") == 0) {
        command_mixer(args); 
    } else if (strcmp(cmd_lower, "music") == 0) {
        command_music();
    } else if (strcmp(cmd_lower, "ps") == 0) {
        process_ps();
    } else if (strcmp(cmd_lower, "runbg") == 0) {
        if (*args == '\0') printf("\nUsage: runbg <filename>\n");
        else command_runbg(args);
    } else if (strcmp(cmd_lower, "kill") == 0) {
        command_kill(args);
    } else if (strcmp(cmd_lower, "wait") == 0) {
        if (*args == '\0') printf("\nUsage: wait <pid>\n");
        else command_wait(args);
    } else if (strcmp(cmd_lower, "exec") == 0) {
        if (*args == '\0') printf("\nUsage: exec <filename>\n");
        else command_exec(args);
    } else {
        printf("\nUnknown command: %s\n", input_buffer);
        printf("Type 'help' for available commands.\n");
    }

    input_buffer_index = 0;
    input_buffer[0] = '\0';
}

void show_prompt(void) {
    printf("\n");

    // "[lufiraos@kernel]" всегда рисуем cyan, но остальную часть строки —
    // ТЕКУЩИМ цветом текста, а не жёстко белым: раньше это затирало цвет,
    // который пользователь настроил командой fg (bg при этом не трогалась,
    // поэтому казалось, что fg "не работает", а bg работает).
    ConsoleColor saved_fg_index = current_colors.fg_index;
    uint32_t saved_fg_color = current_color;

    set_foreground_color(COLOR_LIGHT_CYAN);
    printf("[lufiraos@kernel]");

    current_colors.fg_index = saved_fg_index;
    current_colors.fg_color = saved_fg_color;
    current_color = saved_fg_color;

    printf(" %s $ ", cwd_path);
    command_start_x = current_x;
    command_start_y = current_y;
    cursor_position_in_line = 0;
    current_line_length = 0;
    for (int i = 0; i < INPUT_BUFFER_SIZE; i++) { current_line[i] = 0; input_buffer[i] = 0; }
    history_index = -1;
    if (cursor_enabled && !cursor_visible) draw_cursor();
}

void shell_handle_tab(void) {
    static const char *commands[] = {
        "help", "clear", "reboot", "shutdown", "version",
        "echo", "history", "status", "trap",
        "color", "colors", "fg", "bg", "reset",
        "pwd", "cd", "ls", "mkdir", "rm", "touch", "cat",
        "run", "runbg", "exec", "write", "beep", "mixer", "music",
        "kill", "wait",
        NULL
    };
    
    int matches[32];
    int match_count = 0;
    
    for (int i = 0; commands[i] != NULL; i++) {
        int match = 1;
        for (int j = 0; j < (int)current_line_length; j++) {
            if (commands[i][j] == '\0' || 
                to_lower(commands[i][j]) != to_lower(current_line[j])) {
                match = 0;
                break;
            }
        }
        if (match) {
            matches[match_count++] = i;
            if (match_count >= 32) break;
        }
    }
    
    if (match_count == 0) return;
    
    if (match_count == 1) {
        // Автодополняем
        const char *cmd = commands[matches[0]];
        while (current_line_length < INPUT_BUFFER_SIZE - 1 && cmd[current_line_length]) {
            current_line[current_line_length] = cmd[current_line_length];
            current_line_length++;
            cursor_position_in_line++;
        }
        current_line[current_line_length] = '\0';
        
        // Добавляем пробел
        if (current_line_length < INPUT_BUFFER_SIZE - 1) {
            current_line[current_line_length] = ' ';
            current_line_length++;
            cursor_position_in_line++;
            current_line[current_line_length] = '\0';
        }
        
        shell_refresh_input_line();
    } else {
        // Показываем варианты
        put_char('\n');
        for (int i = 0; i < match_count; i++) {
            printf("%s  ", commands[matches[i]]);
        }
        put_char('\n');
        show_prompt();
        
        // Восстанавливаем ввод
        for (uint32_t i = 0; i < current_line_length; i++) {
            put_char(current_line[i]);
        }
    }
}

// Ctrl+C — принудительно прерывает то, что сейчас "на переднем плане"
// (run/exec), как в настоящем шелле. runbg сюда не попадает вовсе —
// foreground_pid для фоновых процессов никогда не выставляется (см.
// process.h/elf.c), поэтому Ctrl+C их не трогает.
void shell_handle_ctrl_c(void) {
    uint32_t pid = foreground_pid;

    if (pid != 0) {
        process_signal(pid, SIGINT);

        // Если сигнал застал процесс НЕ текущим (обычный случай — он спал
        // в sys_sleep()), process_signal() вернул управление сюда как
        // всегда, и зомби можно сразу забрать. Если же процесс оказался
        // ровно текущим (current_process), process_signal() увёл нас через
        // schedule() и до этой строки в ЭТОМ вызове мы уже не дойдём —
        // foreground_pid к этому моменту уже обнулён самим
        // terminate_process_by_signal() (см. process.c), так что застрять
        // с "висящим" foreground_pid на мёртвом PID мы не можем; шелл сам
        // восстановится, когда планировщик в следующий раз его выберет.
        process_wait(pid, NULL);
    }

    printf("^C\n");

    current_line[0] = '\0';
    current_line_length = 0;
    cursor_position_in_line = 0;
    history_index = -1;

    show_prompt();
}
