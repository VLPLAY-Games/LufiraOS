#include "x25519.h"

/*
 * Представление элемента поля GF(2^255-19): 16 "конечностей" по 16 бит в
 * int64_t каждая, младшая первой (значение = sum t[i] * 2^(16*i)). Запас
 * до 64 бит нужен, чтобы школьное умножение 16x16 конечностей не
 * переполнялось до приведения переносов (fe_carry()). Промежуточные
 * значения НЕ нормализованы и могут быть отрицательными — приводятся
 * только в fe_carry()/fe_pack().
 */
typedef int64_t fe[16];

// Константа (A-2)/4 = 121665 из лестницы Монтгомери (RFC 7748 §5).
static const fe FE_121665 = { 0xDB41, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static void fe_copy(fe out, const fe in) {
    for (int i = 0; i < 16; i++) out[i] = in[i];
}

static void fe_set_zero(fe out) {
    for (int i = 0; i < 16; i++) out[i] = 0;
}

static void fe_set_one(fe out) {
    fe_set_zero(out);
    out[0] = 1;
}

static void fe_add(fe out, const fe a, const fe b) {
    for (int i = 0; i < 16; i++) out[i] = a[i] + b[i];
}

static void fe_sub(fe out, const fe a, const fe b) {
    for (int i = 0; i < 16; i++) out[i] = a[i] - b[i];
}

// Приведение переносов: каждая конечность загоняется в [0, 2^16), перенос
// уходит в следующую, а перенос из САМОЙ СТАРШЕЙ умножается на 38 и
// добавляется к младшей — потому что 2^256 = 2 * 2^255 = 2 * 19 = 38
// (mod 2^255-19). Сдвиг вправо здесь арифметический, поэтому отрицательные
// конечности тоже обрабатываются корректно (перенос получается
// отрицательным). Один проход может снова "раздуть" out[0] переносом с
// верха, поэтому вызывающие делают два прохода подряд.
static void fe_carry(fe out) {
    for (int i = 0; i < 16; i++) {
        int64_t c = out[i] >> 16;
        // Именно умножение, а не "c << 16": перенос бывает отрицательным,
        // а сдвиг влево отрицательного значения — неопределённое поведение
        // по стандарту C (ловится -fsanitize=undefined в selftest/).
        // Компилятор всё равно разворачивает это в тот же сдвиг.
        out[i] -= c * 65536;
        if (i < 15) out[i + 1] += c;
        else out[0] += 38 * c;
    }
}

static void fe_mul(fe out, const fe a, const fe b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    }
    // Свёртка верхней половины: вес 2^(16*(i+16)) = 2^256 * 2^(16*i),
    // а 2^256 == 38 (mod p).
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) out[i] = t[i];
    fe_carry(out);
    fe_carry(out);
}

static void fe_sq(fe out, const fe a) {
    fe_mul(out, a, a);
}

// out = 1/in (mod p) = in^(p-2) = in^(2^255-21). Цепочка возведений в
// квадрат с умножением на всех позициях, кроме бит 2 и 4 — именно так
// выглядит двоичная запись 2^255-21 (единицы везде, кроме этих двух бит).
static void fe_invert(fe out, const fe in) {
    fe c;
    fe_copy(c, in);
    for (int a = 253; a >= 0; a--) {
        fe_sq(c, c);
        if (a != 2 && a != 4) fe_mul(c, c, in);
    }
    fe_copy(out, c);
}

// Условный обмен p <-> q при b == 1, без ветвления по секретному биту
// (бит скаляра — секрет, ветвление по нему дало бы разное время/следы в
// кеше). Маска -b даёт либо все нули, либо все единицы.
static void fe_cswap(fe p, fe q, int64_t b) {
    int64_t mask = -b;
    for (int i = 0; i < 16; i++) {
        int64_t t = mask & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void fe_unpack(fe out, const uint8_t in[32]) {
    for (int i = 0; i < 16; i++) {
        out[i] = (int64_t)in[2 * i] + ((int64_t)in[2 * i + 1] << 8);
    }
    out[15] &= 0x7FFF; // старший бит u-координаты игнорируется (RFC 7748 §5)
}

// Полное приведение в [0, p) и выгрузка 32 байт little-endian. Вычитание
// p выполняется трижды, потому что после fe_carry() значение гарантированно
// меньше 2p, но "меньше p" нужно получить без ветвлений: каждая итерация
// вычитает p и оставляет результат только если он не стал отрицательным.
static void fe_pack(uint8_t out[32], const fe in) {
    fe t, m;
    fe_copy(t, in);
    fe_carry(t);
    fe_carry(t);
    fe_carry(t);

    for (int round = 0; round < 3; round++) {
        m[0] = t[0] - 0xFFED; // p = 2^255-19 в этом представлении
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xFFFF - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xFFFF;
        }
        m[15] = t[15] - 0x7FFF - ((m[14] >> 16) & 1);
        int64_t borrow = (m[15] >> 16) & 1; // 1, если t < p (вычитание ушло в минус)
        m[14] &= 0xFFFF;
        fe_cswap(t, m, 1 - borrow);
    }

    for (int i = 0; i < 16; i++) {
        out[2 * i]     = (uint8_t)(t[i] & 0xFF);
        out[2 * i + 1] = (uint8_t)((t[i] >> 8) & 0xFF);
    }
}

void x25519_scalarmult(uint8_t out[X25519_KEY_SIZE],
                       const uint8_t scalar[X25519_KEY_SIZE],
                       const uint8_t point[X25519_KEY_SIZE]) {
    uint8_t e[32];
    for (int i = 0; i < 32; i++) e[i] = scalar[i];
    // Clamping скаляра (RFC 7748 §5): младшие три бита в 0 (кратность
    // кофактору 8), старший бит в 0, следующий в 1 (фиксированная длина
    // скаляра — лестница всегда одинаковой длины).
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    fe x1, x2, z2, x3, z3, a, b, c, d, t;
    fe_unpack(x1, point);

    // Лестница Монтгомери (RFC 7748 §5): (x2,z2) = 0*P, (x3,z3) = 1*P.
    fe_set_one(x2);
    fe_set_zero(z2);
    fe_copy(x3, x1);
    fe_set_one(z3);

    int64_t swap = 0;
    for (int pos = 254; pos >= 0; pos--) {
        int64_t bit = (e[pos >> 3] >> (pos & 7)) & 1;
        // Вместо явной перестановки "по биту" на каждом шаге обмениваем
        // только когда бит ИЗМЕНИЛСЯ по сравнению с предыдущим — ровно
        // тот же результат при вдвое меньшем числе обменов.
        fe_cswap(x2, x3, swap ^ bit);
        fe_cswap(z2, z3, swap ^ bit);
        swap = bit;

        fe_sub(a, x2, z2);
        fe_add(b, x2, z2);
        fe_sub(c, x3, z3);
        fe_add(d, x3, z3);

        fe_sq(t, b);        // t = (x2+z2)^2
        fe_sq(x2, a);       // x2 = (x2-z2)^2
        fe_mul(a, a, d);    // a = (x2-z2)*(x3+z3)
        fe_mul(c, c, b);    // c = (x3-z3)*(x2+z2)

        fe_add(b, a, c);
        fe_sub(a, a, c);
        fe_sq(x3, b);       // x3 = (a+c)^2
        fe_sq(b, a);        // b = (a-c)^2
        fe_mul(z3, b, x1);  // z3 = x1 * (a-c)^2

        fe_sub(c, t, x2);   // c = (x2+z2)^2 - (x2-z2)^2 = 4*x2*z2
        fe_mul(z2, c, FE_121665);
        fe_add(z2, z2, t);
        fe_mul(z2, z2, c);
        fe_mul(x2, t, x2);  // x2 = (x2+z2)^2 * (x2-z2)^2
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_pack(out, x2);
}

void x25519_base(uint8_t out[X25519_KEY_SIZE], const uint8_t scalar[X25519_KEY_SIZE]) {
    uint8_t basepoint[32];
    for (int i = 0; i < 32; i++) basepoint[i] = 0;
    basepoint[0] = 9; // u = 9 (RFC 7748 §4.1)
    x25519_scalarmult(out, scalar, basepoint);
}
