#pragma once

// Строковые утилиты для парсинга команд
const char* skip_spaces(const char* s);
int token_length(const char* s);
int token_equals(const char* s, const char* word);
int atoi(const char* str);
int hex_to_int(const char* hex);