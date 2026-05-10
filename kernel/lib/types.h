#pragma once

// ========== БЕЗЗНАКОВЫЕ ЦЕЛЫЕ ==========
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;

// ========== ЗНАКОВЫЕ ЦЕЛЫЕ ==========
typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;

// ========== РАЗМЕРНЫЕ ТИПЫ (x86_64) ==========
typedef uint64_t            size_t;
typedef int64_t             ssize_t;
typedef int64_t             ptrdiff_t;
typedef uint64_t            uintptr_t;
typedef int64_t             intptr_t;

// ========== ФАЙЛОВЫЕ ТИПЫ ==========
typedef int64_t             off_t;      // Для seek

// ========== БУЛЕВЫЙ ТИП ==========
#ifndef __cplusplus
    #ifndef bool
        typedef _Bool       bool;
    #endif
    #ifndef true
        #define true        1
    #endif
    #ifndef false
        #define false       0
    #endif
#endif

// ========== NULL ==========
#ifndef NULL
    #define NULL            ((void*)0)
#endif

// ========== МАКРОСЫ ==========
#define offsetof(type, member)   ((size_t)(&((type*)0)->member))

// ========== ПРЕДЕЛЫ ==========
#define UINT8_MAX   0xFF
#define UINT16_MAX  0xFFFF
#define UINT32_MAX  0xFFFFFFFF
#define UINT64_MAX  0xFFFFFFFFFFFFFFFFULL
#define INT8_MAX    0x7F
#define INT16_MAX   0x7FFF
#define INT32_MAX   0x7FFFFFFF
#define INT64_MAX   0x7FFFFFFFFFFFFFFFLL
#define INT8_MIN    (-0x80)
#define INT16_MIN   (-0x8000)
#define INT32_MIN   (-0x80000000)
#define INT64_MIN   (-0x8000000000000000LL)