// math.h — IEEE-754 double-математика через аппаратный FPU (x87 для
// sin/cos/tan/log*/atan2 — у них нет SSE2-эквивалента, SSE2 sqrtsd для
// sqrt, обычная double-арифметика для всего остального).
//
// ВАЖНО: math.c — единственный файл libc, собираемый БЕЗ
// -mgeneral-regs-only (см. SHARED_LIBC_SOURCES/compile_libc_shared() в
// lufira-packages/build.py) — именно эта функция под x86-64 SysV ABI
// обязана передавать/возвращать double через XMM-регистры, что
// -mgeneral-regs-only запрещает в принципе. Это касается и ЛЮБОГО
// другого .c-файла (ядра или пакета), который захочет напрямую объявить
// double/float или вызвать что-то отсюда — ему тоже придётся собираться
// без -mgeneral-regs-only (см. пример в build.py). Корректность этого при
// переключении процессов обеспечена заранее: kernel/system/process/
// process.c уже сохраняет/восстанавливает FXSAVE-область (fpu_save()/
// fpu_restore()) на каждом switch_to_process() — см. комментарий там же.
#pragma once

#define M_E        2.71828182845904523536
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

#define HUGE_VAL   (__builtin_huge_val())
#define INFINITY   (__builtin_inff())
#define NAN        (__builtin_nanf(""))

double fabs(double x);
float  fabsf(float x);

double sqrt(double x);
float  sqrtf(float x);

double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);

double fmod(double x, double y);
double hypot(double x, double y);

double ldexp(double x, int exp);
double frexp(double x, int *exp);
double copysign(double x, double y);

int isnan(double x);
int isinf(double x);

double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

float sinf(float x);
float cosf(float x);
float tanf(float x);
float powf(float x, float y);
float expf(float x);
float logf(float x);
