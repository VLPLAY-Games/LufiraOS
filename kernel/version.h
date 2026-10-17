// version.h — единственное место, где зашита версия ядра. Используется
// и самим ядром (SYS_OS_VERSION, syscall.c — для "О системе" в
// lufira-packages/apps/settings.c), и lufira-packages/build.py (простым
// regex по этому файлу, без компиляции — см. его komментарий у
// KERNEL_VERSION), и LufiraOS-Builder (та же проверка при сборке образа,
// до запуска ядра). Поднимать эти три числа нужно вручную при релизе —
// не из git-тегов и не из README.md (тот — маркетинговый бейдж, не
// источник истины для проверки совместимости).
#pragma once

#define LUFIRA_VERSION_MAJOR 0
#define LUFIRA_VERSION_MINOR 9
#define LUFIRA_VERSION_PATCH 0
