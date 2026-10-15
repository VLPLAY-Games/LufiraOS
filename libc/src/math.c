// math.c — единственный файл libc, собираемый БЕЗ -mgeneral-regs-only
// (см. compile_libc_shared() в lufira-packages/build.py) — double по
// x86-64 SysV ABI обязан ходить через XMM-регистры, что
// -mgeneral-regs-only запрещает в принципе. См. длинный комментарий в
// math.h про то, почему это безопасно (FXSAVE/FXRSTOR на каждом
// switch_to_process() уже готовы заранее в process.c).
//
// Три источника вычислений, выбраны по степени риска:
//  1. Простая битовая арифметика над представлением IEEE-754 (fabs,
//     floor/ceil/trunc/round, ldexp/frexp, copysign, isnan/isinf) — вообще
//     без FPU, самый безопасный вариант, где он в принципе применим.
//  2. Аппаратные x87-инструкции через ОДИН-ДВА простых GCC-ограничения
//     "t"/"u"/"0" (регистры %st(0)/%st(1)) для sin/cos/tan/atan2/log* —
//     у них нет SSE2-эквивалента, но сама инструкция ровно одна, и GCC
//     сам отвечает за загрузку/выгрузку стека FPU по этим ограничениям
//     (классическая идиома из glibc/musl i386 — никакого ручного
//     fld/fstp здесь нет, а значит и риска перепутать порядок стека).
//  3. Обычная double-арифметика (SSE2, компилятору отдано на откуп) для
//     exp/pow/fmod/hypot — у exp нет безопасной короткой x87-идиомы (вариант
//     через F2XM1/FSCALE требует ручной жонглировки регистрами стека без
//     поддержки "t"/"u"-ограничений), поэтому вместо неё — редукция
//     диапазона + ряд Тейлора, полностью на SSE2, легко тестируется.
#include <math.h>
#include <stdint.h>

typedef union { double d; uint64_t u; } fp_bits_t;
typedef union { float f; uint32_t u; } fp_bits32_t;

// ============================================================
// 1. Битовые операции над представлением IEEE-754 — без FPU вообще.
// ============================================================

double fabs(double x) {
    fp_bits_t b; b.d = x;
    b.u &= 0x7FFFFFFFFFFFFFFFULL;
    return b.d;
}

float fabsf(float x) {
    fp_bits32_t b; b.f = x;
    b.u &= 0x7FFFFFFFu;
    return b.f;
}

double copysign(double x, double y) {
    fp_bits_t bx, by;
    bx.d = x; by.d = y;
    bx.u = (bx.u & 0x7FFFFFFFFFFFFFFFULL) | (by.u & 0x8000000000000000ULL);
    return bx.d;
}

int isnan(double x) {
    fp_bits_t b; b.d = x;
    uint64_t e = (b.u >> 52) & 0x7FF;
    uint64_t m = b.u & 0xFFFFFFFFFFFFFULL;
    return e == 0x7FF && m != 0;
}

int isinf(double x) {
    fp_bits_t b; b.d = x;
    uint64_t e = (b.u >> 52) & 0x7FF;
    uint64_t m = b.u & 0xFFFFFFFFFFFFFULL;
    return e == 0x7FF && m == 0;
}

// Отбрасывает дробную часть (округление к нулю) — обнулением младших
// битов мантиссы, без единой FPU-инструкции. |x|>=2^52 уже целое (все
// 52 бита мантиссы — целая часть, дробных не осталось), |x|<1 даёт 0
// нужного знака.
double trunc(double x) {
    fp_bits_t b; b.d = x;
    int e = (int)((b.u >> 52) & 0x7FF) - 1023;
    if (((b.u >> 52) & 0x7FF) == 0x7FF) return x; // inf/nan — без изменений
    if (e >= 52) return x;
    if (e < 0) {
        b.u &= 0x8000000000000000ULL; // ±0
        return b.d;
    }
    uint64_t mask = 0xFFFFFFFFFFFFFULL >> e; // младшие (52-e) бит мантиссы
    b.u &= ~mask;
    return b.d;
}

double floor(double x) {
    double t = trunc(x);
    if (x < 0.0 && t != x) return t - 1.0;
    return t;
}

double ceil(double x) {
    double t = trunc(x);
    if (x > 0.0 && t != x) return t + 1.0;
    return t;
}

// round-half-away-from-zero (стандартная семантика C99 round(), в
// отличие от round-half-to-even у rint()/nearbyint() — последних здесь нет).
double round(double x) {
    double t = trunc(x);
    double diff = x - t;
    if (diff >= 0.5) return t + 1.0;
    if (diff <= -0.5) return t - 1.0;
    return t;
}

// x = m * 2^e, 0.5 <= |m| < 1 (m сохраняет знак x). Денормалы сначала
// домножаются на 2^64 — для ЛЮБОГО денормала результат уже не денормал
// (минимальный денормал 2^-1074 * 2^64 = 2^-1010, граница денормалов —
// 2^-1022), после чего обычная поправка экспоненты работает как для
// нормального числа.
double frexp(double x, int *exp) {
    if (x == 0.0 || isnan(x) || isinf(x)) { *exp = 0; return x; }
    fp_bits_t b; b.d = x;
    uint64_t e = (b.u >> 52) & 0x7FF;
    int shift = 0;
    if (e == 0) {
        b.d = x * 18446744073709551616.0; // 2^64
        shift = -64;
        e = (b.u >> 52) & 0x7FF;
    }
    *exp = (int)e - 1022 + shift;
    b.u = (b.u & ~(0x7FFULL << 52)) | (1022ULL << 52);
    return b.d;
}

// x * 2^n. Быстрый путь — прямая правка поля экспоненты; переполнение/
// антипереполнение и денормальный результат (редкие в реальном
// использовании случаи) уходят в медленный, но безопасный путь обычным
// умножением на 2.0/0.5 в цикле — n заранее ограничен, так что цикл
// гарантированно конечен и короткий относительно любого реалистичного
// double.
double ldexp(double x, int n) {
    if (x == 0.0 || isnan(x) || isinf(x) || n == 0) return x;
    if (n > 2100) n = 2100;
    if (n < -2200) n = -2200;

    fp_bits_t b; b.d = x;
    int e = (int)((b.u >> 52) & 0x7FF);
    if (e == 0) goto slow_path; // денормальный вход

    e += n;
    if (e >= 2047) return copysign(HUGE_VAL, x);
    if (e <= 0) goto slow_path;

    b.u = (b.u & ~(0x7FFULL << 52)) | ((uint64_t)e << 52);
    return b.d;

slow_path:
    {
        double r = x;
        if (n > 0) { while (n-- > 0) r *= 2.0; }
        else       { while (n++ < 0) r *= 0.5; }
        return r;
    }
}

double fmod(double x, double y) {
    if (y == 0.0 || isnan(x) || isnan(y) || isinf(x)) return NAN;
    if (isinf(y)) return x;
    double q = trunc(x / y);
    double r = x - q * y;
    // Поправка на редкий случай, когда x/y округлилось так, что r
    // "перескочил" через y по модулю — тот же приём, что в большинстве
    // простых (не FPREM-точных) реализаций fmod().
    if (r != 0.0 && ((r < 0.0) != (x < 0.0))) r += y;
    return r;
}

double hypot(double x, double y) {
    x = fabs(x); y = fabs(y);
    if (x < y) { double t = x; x = y; y = t; }
    if (x == 0.0) return 0.0;
    double r = y / x;
    return x * sqrt(1.0 + r * r);
}

// ============================================================
// 2. Аппаратные x87-инструкции через ограничения "t"/"u"/"0" — GCC сам
//    генерирует fld/fstp по месту, здесь нет ручной работы со стеком FPU.
// ============================================================

double sqrt(double x) {
    double result;
    asm volatile ("sqrtsd %1, %0" : "=x"(result) : "x"(x));
    return result;
}

float sqrtf(float x) {
    float result;
    asm volatile ("sqrtss %1, %0" : "=x"(result) : "x"(x));
    return result;
}

double sin(double x) {
    double result;
    asm volatile ("fsin" : "=t"(result) : "0"(x));
    return result;
}

double cos(double x) {
    double result;
    asm volatile ("fcos" : "=t"(result) : "0"(x));
    return result;
}

// FPTAN кладёт на стек ДВА значения (ST(0)=1.0, ST(1)=тангенс) — лишняя
// константа снимается "fstp %%st" (поп без сохранения куда-либо).
double tan(double x) {
    double result;
    asm volatile ("fptan\n\tfstp %%st" : "=t"(result) : "0"(x));
    return result;
}

// FPATAN: ST(1) <- arctan(ST(1)/ST(0)), поп ST(0) — т.е. при ST(0)=x,
// ST(1)=y считает ровно atan2(y,x) (с учётом знаков по всем четвертям,
// это аппаратно полный atan2, не "голый" atan).
double atan2(double y, double x) {
    double result;
    asm volatile ("fpatan" : "=t"(result) : "0"(x), "u"(y));
    return result;
}

double atan(double x) {
    return atan2(x, 1.0);
}

// asin/acos через atan2 — классические тождества (устойчивее, чем
// atan(x/sqrt(1-x^2)) в лоб, не делят на ноль на границах x=±1 за счёт
// формы atan2(0, ...)).
double asin(double x) {
    return atan2(x, sqrt((1.0 - x) * (1.0 + x)));
}

double acos(double x) {
    return atan2(sqrt((1.0 - x) * (1.0 + x)), x);
}

// FYL2X: ST(1) <- ST(1)*log2(ST(0)), поп ST(0). log2(x)=1*log2(x),
// ln(x)=ln2*log2(x), log10(x)=log10(2)*log2(x) — три одинаковые по форме
// идиомы, разница только в константе ST(1).
double log2(double x) {
    double result;
    asm volatile ("fyl2x" : "=t"(result) : "0"(x), "u"(1.0));
    return result;
}

double log(double x) {
    double result;
    asm volatile ("fyl2x" : "=t"(result) : "0"(x), "u"(M_LN2));
    return result;
}

double log10(double x) {
    double result;
    asm volatile ("fyl2x" : "=t"(result) : "0"(x), "u"(0.30102999566398119521));
    return result;
}

// ============================================================
// 3. Обычная double-арифметика (SSE2) — редукция диапазона + ряд Тейлора.
// ============================================================

double exp(double x) {
    if (isnan(x)) return x;
    if (x > 709.0) return HUGE_VAL;     // переполнение double (e^709.78 ≈ DBL_MAX)
    if (x < -745.0) return 0.0;         // антипереполнение (e^-745.13 ≈ самый мелкий денормал)

    double nd = round(x / M_LN2);
    int n = (int)nd;
    double r = x - nd * M_LN2;          // |r| <= ln2/2 ≈ 0.3466

    // Ряд Тейлора e^r — на таком узком диапазоне сходится с огромным
    // запасом (член ~0.3466^17/17! < 1e-22) раньше, чем кончается
    // фиксированный, заранее ограниченный цикл.
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k <= 17; k++) {
        term *= r / (double)k;
        sum += term;
    }
    return ldexp(sum, n);
}

double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (isnan(x) || isnan(y)) return NAN;
    if (x == 0.0) return (y > 0.0) ? 0.0 : HUGE_VAL;

    double iy = trunc(y);
    if (iy == y && fabs(y) <= 1024.0) {
        // Целая степень — возведение в степень умножением (двоичное
        // разложение показателя): точно работает и для x<0, в отличие от
        // общей ветки через exp(y*log(x)), которая для x<0 не определена
        // в вещественных числах.
        int n = (int)iy;
        int neg = n < 0;
        if (neg) n = -n;
        double base = x, result = 1.0;
        while (n > 0) {
            if (n & 1) result *= base;
            base *= base;
            n >>= 1;
        }
        return neg ? 1.0 / result : result;
    }

    if (x < 0.0) return NAN; // дробная степень отрицательного числа — не определено в R
    return exp(y * log(x));
}

// ============================================================
// float-обёртки — через double-версии (небольшая потеря скорости
// совершенно не критична для некритичных к производительности мест,
// где нужен именно float; свои float-алгоритмы не нужны).
// ============================================================

float sinf(float x) { return (float)sin((double)x); }
float cosf(float x) { return (float)cos((double)x); }
float tanf(float x) { return (float)tan((double)x); }
float powf(float x, float y) { return (float)pow((double)x, (double)y); }
float expf(float x) { return (float)exp((double)x); }
float logf(float x) { return (float)log((double)x); }
