#include "shell.h"
#include "commands.h"
#include "drivers/console/console.h"
#include "drivers/keyboard/keyboard.h"
#include "system/process/process.h"
#include "system/users/users.h"

#define HISTORY_SIZE 20

static uint32_t cursor_position_in_line = 0;
static uint32_t command_start_x = 0;
static uint32_t command_start_y = 0;
static char current_line[INPUT_BUFFER_SIZE] = {0};
static uint32_t current_line_length = 0;

static char command_history[HISTORY_SIZE][INPUT_BUFFER_SIZE] = {0};
static int history_count = 0;
static int history_index = -1;
static int history_current = 0;

// см. shell_handle_enter()/shell_run_pending_command().
static volatile int shell_command_pending = 0;

// см. подробный комментарий у объявления в shell.h.
volatile int shell_ctrl_c_pending = 0;

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
// Enter приходит сюда СИНХРОННО изнутри прерывания — либо напрямую с PS/2
// IRQ1, либо (для USB HID) из timer_irq_handler() (см. input.c) — оба пути
// ISR, где IF=0 (cli на входе). Раньше отсюда сразу звался execute_command(),
// и любая команда, которая внутри блокирующе ждёт тиков PIT
// (pit_wait_ms() — например, новые сетевые ping/wget, ARP/TCP-таймауты),
// намертво зависала: таймерное прерывание, от которого зависит pit_ticks,
// не может сработать, пока сам обработчик прерывания (в котором мы уже
// находимся) не вернётся. Подозреваем, что это же и есть настоящая причина
// куда более раннего, так и не найденного зависания usbread/usbwrite (см.
// план) — тот же класс бага, просто не опознанный тогда. Поэтому теперь
// здесь только сохраняем ввод и взводим флаг — сама команда выполняется
// из shell_task() (kernel.c) сразу после hlt, ДО следующего cli, то есть
// уже с настоящими работающими прерываниями, не изнутри ISR.
void shell_handle_enter(void) {
    for (uint32_t i = 0; i < current_line_length; i++) input_buffer[i] = current_line[i];
    input_buffer[current_line_length] = '\0';
    input_buffer_index = current_line_length;
    if (current_line_length > 0) add_to_history(input_buffer);

    put_char('\n');
    shell_command_pending = 1;
}

// Вызывается из shell_task() (kernel.c) вне контекста прерывания — см.
// комментарий у shell_handle_enter().
void shell_run_pending_command(void) {
    if (!shell_command_pending) return;
    shell_command_pending = 0;

    char cmd_lower[INPUT_BUFFER_SIZE];
    for (uint32_t i = 0; i < input_buffer_index; i++) cmd_lower[i] = to_lower(input_buffer[i]);
    cmd_lower[input_buffer_index] = '\0';

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
        printf("\n");
        for (int i = 0; i < history_count; i++) {
            printf("%d  %s\n", i + 1, command_history[i]);
        }
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
        // Без аргумента — домашний каталог (см. command_cd()), а не ошибка.
        command_cd(*args ? args : NULL);
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
        // Сырой input_buffer (не лоуеркейснутый args) — аргументы программы
        // регистрозависимы, как уже сделано для cat/write ниже.
        if (*args == '\0') printf("\nUsage: run <filename> [args...]\n");
        else command_run(input_buffer + 4);
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
    } else if (strcmp(cmd_lower, "df") == 0) {
        command_df();
    } else if (strcmp(cmd_lower, "du") == 0) {
        command_du(args);
    } else if (strcmp(cmd_lower, "devmode") == 0) {
        command_devmode(args);
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
        // Сырой input_buffer — та же причина, что и у "run" выше.
        if (*args == '\0') printf("\nUsage: runbg <filename> [args...]\n");
        else command_runbg(input_buffer + 6);
    } else if (strcmp(cmd_lower, "kill") == 0) {
        command_kill(args);
    } else if (strcmp(cmd_lower, "wait") == 0) {
        if (*args == '\0') printf("\nUsage: wait <pid>\n");
        else command_wait(args);
    } else if (strcmp(cmd_lower, "whoami") == 0) {
        command_whoami();
    } else if (strcmp(cmd_lower, "chmod") == 0) {
        if (*args == '\0') printf("\nUsage: chmod <mode> <path>\n");
        else command_chmod(args);
    } else if (strcmp(cmd_lower, "chown") == 0) {
        // Сырой input_buffer — имя пользователя регистрозависимо (см. users.c).
        if (input_buffer_index <= 6) printf("\nUsage: chown <user>[:group] <path>\n");
        else command_chown(input_buffer + 6);
    } else if (strcmp(cmd_lower, "useradd") == 0) {
        if (input_buffer_index <= 8) printf("\nUsage: useradd <username> <password> [group]\n");
        else command_useradd(input_buffer + 8);
    } else if (strcmp(cmd_lower, "groupadd") == 0) {
        if (input_buffer_index <= 9) printf("\nUsage: groupadd <groupname>\n");
        else command_groupadd(input_buffer + 9);
    } else if (strcmp(cmd_lower, "su") == 0) {
        if (input_buffer_index <= 3) printf("\nUsage: su <username> [password]\n");
        else command_su(input_buffer + 3);
    } else if (strcmp(cmd_lower, "usbinfo") == 0) {
        command_usbinfo();
    } else if (strcmp(cmd_lower, "usbread") == 0) {
        if (*args == '\0') printf("\nUsage: usbread <device> <lba>\n");
        else command_usbread(args);
    } else if (strcmp(cmd_lower, "usbwrite") == 0) {
        // Сырой input_buffer — текст, который пишется на диск, регистрозависим.
        // "usbwrite " = 9 символов включая пробел.
        if (input_buffer_index <= 9) printf("\nUsage: usbwrite <device> <lba> <text>\n");
        else command_usbwrite(input_buffer + 9);
    } else if (strcmp(cmd_lower, "ifconfig") == 0) {
        command_ifconfig(args);
    } else if (strcmp(cmd_lower, "ping") == 0) {
        if (*args == '\0') printf("\nUsage: ping <ip> [count]\n");
        else command_ping(args);
    } else if (strcmp(cmd_lower, "wget") == 0) {
        // Сырой input_buffer — путь и имя файла регистрозависимы.
        // "wget " = 5 символов включая пробел.
        if (input_buffer_index <= 5) printf("\nUsage: wget <ip> <path> [output-filename]\n");
        else command_wget(input_buffer + 5);
    } else {
        printf("\nUnknown command: %s\n", input_buffer);
        printf("Type 'help' for available commands.\n");
    }

    input_buffer_index = 0;
    input_buffer[0] = '\0';
}

void show_prompt(void) {
    printf("\n");

    // "[user@lufiraos]" всегда рисуем cyan, но остальную часть строки —
    // ТЕКУЩИМ цветом текста, а не жёстко белым: раньше это затирало цвет,
    // который пользователь настроил командой fg (bg при этом не трогалась,
    // поэтому казалось, что fg "не работает", а bg работает).
    ConsoleColor saved_fg_index = current_colors.fg_index;
    uint32_t saved_fg_color = current_color;

    user_entry_t prompt_user;
    int have_user = (users_lookup_by_uid(current_process->uid, &prompt_user) == 0);

    set_foreground_color(COLOR_LIGHT_CYAN);
    printf("[%s@lufiraos]", have_user ? prompt_user.username : "?");

    current_colors.fg_index = saved_fg_index;
    current_colors.fg_color = saved_fg_color;
    current_color = saved_fg_color;

    // Домашний каталог показываем как "~" (и "~/остаток"), как в
    // большинстве Unix-шеллов — кроме root'а, чей home == "/": там "~" было
    // бы неотличимо от корня и только запутывало бы.
    const char *display_path = cwd_path;
    char tilde_buf[256];
    if (have_user && prompt_user.home[0] == '/' && prompt_user.home[1] != '\0') {
        int home_len = 0;
        while (prompt_user.home[home_len]) home_len++;
        if (prompt_user.home[home_len - 1] == '/') home_len--;

        int match = 1;
        for (int i = 0; i < home_len; i++) {
            if (cwd_path[i] != prompt_user.home[i]) { match = 0; break; }
        }
        if (match && (cwd_path[home_len] == '\0' || cwd_path[home_len] == '/')) {
            int pos = 0;
            tilde_buf[pos++] = '~';
            int i = home_len;
            while (cwd_path[i] && pos < (int)sizeof(tilde_buf) - 1) tilde_buf[pos++] = cwd_path[i++];
            tilde_buf[pos] = '\0';
            display_path = tilde_buf;
        }
    }

    printf(" %s $ ", display_path);
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
        "cp", "mv", "rename", "edit",
        "run", "runbg", "exec", "write", "beep", "mixer", "music",
        "kill", "wait", "ps", "df", "du", "devmode",
        "whoami", "chmod", "chown", "useradd", "groupadd", "su",
        "usbinfo", "usbread", "usbwrite",
        "ifconfig", "ping", "wget",
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
