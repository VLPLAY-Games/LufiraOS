#pragma once
/* Заглушка kernel/system/devmode/devmode.h для хостовой сборки selftest:
   DLOG печатает всегда — в тестах подробный вывод и нужен. */
#include <stdio.h>
#define DLOG(...) do { if (getenv("LUFIRA_SELFTEST_QUIET") == NULL) printf(__VA_ARGS__); } while (0)
#include <stdlib.h>
