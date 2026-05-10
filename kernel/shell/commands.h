#pragma once

// Утилиты
int atoi(const char* str);
int hex_to_int(const char* hex);

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
void command_write(const char *filename);

// ELF loader
void command_run(const char* filename);