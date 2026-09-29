#include "shell.h"
#include "commands.h"
#include "drivers/console/console.h"
#include "drivers/keyboard/keyboard.h"
#include "system/process/process.h"
#include "system/users/users.h"
#include "fs/lufirafs/lufirafs.h"

extern lufirafs_t lufirafs;

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
// PATH-подобный fallback для builtin'ов, которых нет в 52-ветвевом
// диспетчере ниже: ищет /bin/<cmd_name> (а если нет — /bin/<cmd_name>.elf,
// как называется большинство .elf-программ в этой ОС), и если файл там
// есть и у вызывающего есть право на исполнение, запускает его через
// command_run() — тот же путь, что и обычный "run <file>". Это единственный
// механизм, на который будет опираться будущий пакетный менеджер (v0.7),
// чтобы установленные пакеты вели себя неотличимо от builtin-команд; сам
// пакетный менеджер (.lpg/dlpg) тут не реализуется, только точка входа.
// cmd_name — уже NUL-терминированное имя команды (cmd_lower ДО расщепления
// args, т.е. cmd_lower и args из одного и того же массива — см. вызов
// ниже), args — уже лоуеркейснутый хвост строки в ТОМ ЖЕ массиве, так что
// (args - cmd_name) — это байтовое смещение в исходном (регистрозависимом)
// input_buffer, где реально начинаются аргументы программы.
static void run_external_command(const char *cmd_name, const char *args) {
    char bin_path[80];
    int plen = 0;
    const char *prefix = "/bin/";
    while (prefix[plen] && plen < (int)sizeof(bin_path) - 1) { bin_path[plen] = prefix[plen]; plen++; }
    int j = 0;
    while (cmd_name[j] && plen < (int)sizeof(bin_path) - 1) { bin_path[plen++] = cmd_name[j++]; }
    bin_path[plen] = '\0';

    uint32_t bin_ino;
    int found = (lufirafs_lookup(&lufirafs, lufirafs.sb.root_inode, bin_path, &bin_ino) == 0);
    if (!found) {
        const char *suffix = ".elf";
        int elen = plen;
        int k = 0;
        while (suffix[k] && elen < (int)sizeof(bin_path) - 1) { bin_path[elen++] = suffix[k++]; }
        bin_path[elen] = '\0';
        found = (lufirafs_lookup(&lufirafs, lufirafs.sb.root_inode, bin_path, &bin_ino) == 0);
    }

    if (!found) {
        printf("\nUnknown command: %s\n", input_buffer);
        printf("Type 'help' for available commands.\n");
        return;
    }

    lufirafs_inode_t inode;
    if (lufirafs_read_inode(&lufirafs, bin_ino, &inode) != 0 ||
        !lufirafs_check_access(&inode, current_process->uid, current_process->gid, 0, 0, 1)) {
        printf("\n%s: permission denied\n", input_buffer);
        return;
    }

    int arg_offset = (int)(args - cmd_name);
    const char *raw_args = input_buffer + arg_offset;

    char run_line[INPUT_BUFFER_SIZE + 16];
    int rp = 0;
    for (int m = 0; bin_path[m] && rp < (int)sizeof(run_line) - 1; m++) run_line[rp++] = bin_path[m];
    if (*raw_args) {
        run_line[rp++] = ' ';
        for (int m = 0; raw_args[m] && rp < (int)sizeof(run_line) - 1; m++) run_line[rp++] = raw_args[m];
    }
    run_line[rp] = '\0';

    command_run(run_line);
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
    } else if (strcmp(cmd_lower, "free") == 0) {
        command_free();
    } else if (strcmp(cmd_lower, "cpuload") == 0) {
        command_cpuload();
    } else if (strcmp(cmd_lower, "trap") == 0) {
        command_trap();
    } else if (strcmp(cmd_lower, "pwd") == 0) {
        printf("\n%s\n", cwd_path);
    } else if (strcmp(cmd_lower, "cd") == 0) {
        // Без аргумента — домашний каталог (см. command_cd()), а не ошибка.
        if (is_help_flag(args)) printf("\nUsage: cd [directory]\nNo argument goes to your home directory.\n");
        else command_cd(*args ? args : NULL);
    } else if (strcmp(cmd_lower, "ls") == 0) {
        if (is_help_flag(args)) printf("\nUsage: ls [-l] [path]\n -l shows permissions/owner/size\n");
        else command_ls(args);
    } else if (strcmp(cmd_lower, "mkdir") == 0) {
        if (*args == '\0' || is_help_flag(args)) printf("\nUsage: mkdir <name>\n");
        else command_mkdir(args);
    } else if (strcmp(cmd_lower, "rm") == 0) {
        if (*args == '\0' || is_help_flag(args)) printf("\nUsage: rm <name>\n");
        else command_rm(args);
    } else if (strcmp(cmd_lower, "touch") == 0) {
        if (is_help_flag(args)) printf("\nUsage: touch <filename>\n");
        else command_touch(args);
    } else if (strcmp(cmd_lower, "cat") == 0) {
        if (input_buffer_index <= 4 || is_help_flag(args)) printf("\nUsage: cat <filename>\n");
        else command_cat(input_buffer + 4);
    } else if (strcmp(cmd_lower, "run") == 0) {
        // Сырой input_buffer (не лоуеркейснутый args) — аргументы программы
        // регистрозависимы, как уже сделано для cat/write ниже.
        if (*args == '\0' || is_help_flag(args)) printf("\nUsage: run <filename> [args...]\n");
        else command_run(input_buffer + 4);
    } else if (strcmp(cmd_lower, "exec") == 0) {
        if (*args == '\0' || is_help_flag(args)) printf("\nUsage: exec <filename>\n");
        else command_exec(args);
    } else if (strcmp(cmd_lower, "write") == 0) {
        if (input_buffer_index <= 6 || is_help_flag(args)) printf("\nUsage: write <filename> <text>\n");
        else command_write(input_buffer + 6);
    } else if (strcmp(cmd_lower, "cp") == 0) {
        if (*args == '\0' || is_help_flag(args)) printf("\nUsage: cp <source> <destination>\n");
        else command_cp(args);
    } else if (strcmp(cmd_lower, "mv") == 0) {
        if (*args == '\0' || is_help_flag(args)) printf("\nUsage: mv <source> <destination>\n");
        else command_mv(args);
    } else if (strcmp(cmd_lower, "rename") == 0) {
        if (*args == '\0' || is_help_flag(args)) printf("\nUsage: rename <old> <new>\n");
        else command_rename(args);
    } else if (strcmp(cmd_lower, "df") == 0) {
        command_df();
    } else if (strcmp(cmd_lower, "du") == 0) {
        if (is_help_flag(args)) printf("\nUsage: du [path]\n");
        else command_du(args);
    } else if (strcmp(cmd_lower, "devmode") == 0) {
        // command_devmode() сама печатает Usage на любой нераспознанный
        // аргумент, включая -help — отдельная проверка тут не нужна.
        command_devmode(args);
    } else if (strcmp(cmd_lower, "edit") == 0) {
        if (*args == '\0' || is_help_flag(args)) printf("\nUsage: edit <filename> <text>\n");
        else command_edit(args);
    } else if (strcmp(cmd_lower, "beep") == 0) {
        command_beep();
    } else if (strcmp(cmd_lower, "mixer") == 0) {
        // command_mixer() сама печатает "Usage: mixer [0-100]" на любой
        // нечисловой аргумент, включая -help — отдельная проверка не нужна.
        command_mixer(args);
    } else if (strcmp(cmd_lower, "music") == 0) {
        command_music();
    } else if (strcmp(cmd_lower, "ps") == 0) {
        process_ps();
    } else if (strcmp(cmd_lower, "runbg") == 0) {
        // Сырой input_buffer — та же причина, что и у "run" выше.
        if (*args == '\0' || is_help_flag(args)) printf("\nUsage: runbg <filename> [args...]\n");
        else command_runbg(input_buffer + 6);
    } else if (strcmp(cmd_lower, "kill") == 0) {
        if (is_help_flag(args)) printf("\nUsage: kill [-SIGNAL] <pid>\nSignals: -TERM (default), -KILL, -STOP, -CONT\n");
        else command_kill(args);
    } else if (strcmp(cmd_lower, "wait") == 0) {
        if (*args == '\0' || is_help_flag(args)) printf("\nUsage: wait <pid>\n");
        else command_wait(args);
    } else if (strcmp(cmd_lower, "whoami") == 0) {
        command_whoami();
    } else if (strcmp(cmd_lower, "chmod") == 0) {
        if (*args == '\0' || is_help_flag(args)) printf("\nUsage: chmod <mode> <path>  (octal, e.g. 644)\n");
        else command_chmod(args);
    } else if (strcmp(cmd_lower, "chown") == 0) {
        // Сырой input_buffer — имя пользователя регистрозависимо (см. users.c).
        if (input_buffer_index <= 6 || is_help_flag(args)) printf("\nUsage: chown <user>[:group] <path>  (root only)\n");
        else command_chown(input_buffer + 6);
    } else if (strcmp(cmd_lower, "useradd") == 0) {
        if (input_buffer_index <= 8 || is_help_flag(args)) printf("\nUsage: useradd <username> <password> [group]\n");
        else command_useradd(input_buffer + 8);
    } else if (strcmp(cmd_lower, "groupadd") == 0) {
        if (input_buffer_index <= 9 || is_help_flag(args)) printf("\nUsage: groupadd <groupname>\n");
        else command_groupadd(input_buffer + 9);
    } else if (strcmp(cmd_lower, "su") == 0) {
        if (input_buffer_index <= 3 || is_help_flag(args)) printf("\nUsage: su <username> [password]\n");
        else command_su(input_buffer + 3);
    } else if (strcmp(cmd_lower, "passwd") == 0) {
        // Сырой input_buffer — пароль регистрозависим. "passwd " = 7 символов.
        if (input_buffer_index <= 7 || is_help_flag(args))
            printf("\nUsage: passwd <new-password>\nUsage: passwd -u <username> <new-password>  (root only)\n");
        else command_passwd(input_buffer + 7);
    } else if (strcmp(cmd_lower, "usbinfo") == 0) {
        command_usbinfo();
    } else if (strcmp(cmd_lower, "usbread") == 0) {
        if (*args == '\0' || is_help_flag(args)) printf("\nUsage: usbread <device> <lba>\n");
        else command_usbread(args);
    } else if (strcmp(cmd_lower, "usbwrite") == 0) {
        // Сырой input_buffer — текст, который пишется на диск, регистрозависим.
        // "usbwrite " = 9 символов включая пробел.
        if (input_buffer_index <= 9 || is_help_flag(args)) printf("\nUsage: usbwrite <device> <lba> <text>\n");
        else command_usbwrite(input_buffer + 9);
    } else if (strcmp(cmd_lower, "mount") == 0) {
        if (is_help_flag(args)) printf("\nUsage: mount [usb-device-index] [name]\nNo arguments lists active mounts.\n");
        else command_mount(args);
    } else if (strcmp(cmd_lower, "unmount") == 0) {
        if (is_help_flag(args)) printf("\nUsage: unmount [name]\n");
        else command_unmount(args);
    } else if (strcmp(cmd_lower, "mountls") == 0) {
        if (*args == '\0' || is_help_flag(args)) printf("\nUsage: mountls <name>\n");
        else command_mountls(args);
    } else if (strcmp(cmd_lower, "mountcat") == 0) {
        if (*args == '\0' || is_help_flag(args)) printf("\nUsage: mountcat <name> <file>\n");
        else command_mountcat(args);
    } else if (strcmp(cmd_lower, "mountwrite") == 0) {
        // Сырой input_buffer — текст регистрозависим. "mountwrite " = 11 символов.
        if (input_buffer_index <= 11 || is_help_flag(args)) printf("\nUsage: mountwrite <name> <file> <text>\n");
        else command_mountwrite(input_buffer + 11);
    } else if (strcmp(cmd_lower, "ifconfig") == 0) {
        if (is_help_flag(args)) printf("\nUsage: ifconfig [ip] [netmask] [gateway]\nNo arguments shows current configuration.\n");
        else command_ifconfig(args);
    } else if (strcmp(cmd_lower, "ping") == 0) {
        if (*args == '\0' || is_help_flag(args)) printf("\nUsage: ping <ip> [count]\n");
        else command_ping(args);
    } else if (strcmp(cmd_lower, "wget") == 0) {
        // Сырой input_buffer — путь и имя файла регистрозависимы.
        // "wget " = 5 символов включая пробел.
        if (input_buffer_index <= 5 || is_help_flag(args)) printf("\nUsage: wget <ip> <path> [output-filename]\n");
        else command_wget(input_buffer + 5);
    } else {
        run_external_command(cmd_lower, args);
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
        "echo", "history", "status", "free", "cpuload", "trap",
        "color", "colors", "fg", "bg", "reset",
        "pwd", "cd", "ls", "mkdir", "rm", "touch", "cat",
        "cp", "mv", "rename", "edit",
        "run", "runbg", "exec", "write", "beep", "mixer", "music",
        "kill", "wait", "ps", "df", "du", "devmode",
        "whoami", "chmod", "chown", "useradd", "groupadd", "su", "passwd",
        "usbinfo", "usbread", "usbwrite", "mount", "unmount",
        "mountls", "mountcat", "mountwrite",
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
