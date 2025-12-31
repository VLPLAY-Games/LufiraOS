#ifndef SHELL_H
#define SHELL_H

#include "../drivers/console.h"

// Прототипы функций
void show_prompt(void);
void execute_command(void);
int strcmp(const char* s1, const char* s2);
char to_lower(char c);
int strcmp_case_insensitive(const char* s1, const char* s2);

#endif