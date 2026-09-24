#pragma once

// Утилиты
int atoi(const char* str);
int hex_to_int(const char* hex);

// Разбивает raw (изменяемую строку, обычно локальную копию сырого
// input_buffer — см. command_run()/command_runbg()) по пробелам на до
// max_argv токенов, модифицируя raw НА МЕСТЕ (пробелы становятся '\0',
// тот же приём, что execute_command() в shell.c уже делает для первого
// разделения имя-команды/аргументы). argv[0] — первый токен (имя файла),
// argv[1..count-1] — остальные, argv[count]=NULL (если count<max_argv).
// Возвращает количество токенов (0, если raw пуст/только пробелы).
int split_argv(char *raw, char *argv[], int max_argv);

// Системные
void command_help(void);
void command_clear(void);
void command_reboot(void);
void command_shutdown(void);
void command_version(void);
void command_status(void);
void command_trap(void);
void command_echo(const char* args);

// Цвета
void command_color(void);
void command_colors(void);
void command_fg(void);
void command_bg(void);
void command_reset(void);

// Файловая система
void command_pwd(void);
void command_cd(const char* path);
void command_ls(const char* flags);
void command_mkdir(const char* name);
void command_rm(const char* name);
void command_touch(const char* name);
void command_cat(const char* filename);
void command_write(const char* filename);
void command_cp(const char* args);
void command_mv(const char* args);
void command_rename(const char* args);
void command_edit(const char* args);
void command_df(void);
void command_du(const char* path);
void command_devmode(const char* args);

// ELF loader
void command_run(const char* filename);
void command_runbg(const char* filename);
void command_exec(const char* filename);

// Process
void command_kill(const char* args);
void command_wait(const char* args);

// Звук
void command_beep(void);
void command_mixer(const char* args);
void command_music(void);

// Пользователи/группы
void command_whoami(void);
void command_chmod(const char* args);
void command_chown(const char* args);
void command_useradd(const char* args);
void command_groupadd(const char* args);
void command_su(const char* args);

// USB Mass Storage
void command_usbinfo(void);
void command_usbread(const char* args);
void command_usbwrite(const char* args);

// Сеть
void command_ifconfig(const char* args);
void command_ping(const char* args);
void command_wget(const char* args);