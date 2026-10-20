// version.h — единственное место с версией ядра. Читается ядром
// (SYS_OS_VERSION), lufira-packages/build.py (regex'ом, без компиляции) и
// LufiraOS-Builder при сборке образа. Числа поднимаются вручную при
// релизе — не из git-тегов и не из README.md (это лишь бейдж, не
// источник истины).
#pragma once

#define LUFIRA_VERSION_MAJOR 0
#define LUFIRA_VERSION_MINOR 9
#define LUFIRA_VERSION_PATCH 0
