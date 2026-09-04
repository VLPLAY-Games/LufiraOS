#pragma once

#include <stdint.h>
#include <stddef.h>

// ========== ФАЙЛОВЫЕ ТИПЫ ==========

typedef int64_t off_t;

// ========== БУЛЕВЫЙ ТИП ==========

#ifndef __cplusplus
    #ifndef bool
        typedef _Bool bool;
    #endif

    #ifndef true
        #define true 1
    #endif

    #ifndef false
        #define false 0
    #endif
#endif

// ========== NULL ==========

#ifndef NULL
    #define NULL ((void*)0)
#endif

// ========== МАКРОСЫ ==========

#ifndef offsetof
    #define offsetof(type, member) ((size_t)(&((type*)0)->member))
#endif