#include "bignum.h"

/*
 * Внутреннее представление: массив 32-битных конечностей, МЛАДШАЯ ПЕРВОЙ.
 * Конечностей на одну больше, чем нужно модулю: промежуточные значения
 * внутри модульного умножения доходят до 2n (сдвиг влево или прибавление
 * второго слагаемого ДО приведения), а 2n уже может не влезть в столько
 * же бит, сколько n.
 *
 * Все операции получают nl — сколько конечностей РЕАЛЬНО задействовано под
 * текущий модуль (а не весь потолок BN_LIMBS). Для RSA-2048 это 65 вместо
 * 129, то есть вдвое меньше работы на каждое сравнение/сдвиг/вычитание;
 * ради этого стоит протащить лишний параметр.
 */
#define BN_LIMBS (BIGNUM_MAX_BYTES / 4 + 1)

typedef struct {
    uint32_t v[BN_LIMBS];
} bn_t;

static void bn_zero(bn_t *a) {
    for (int i = 0; i < BN_LIMBS; i++) a->v[i] = 0;
}

static void bn_copy(bn_t *dst, const bn_t *src, int nl) {
    for (int i = 0; i < nl; i++) dst->v[i] = src->v[i];
}

// bytes — big-endian; младший байт входа попадает в младший байт v[0].
// Байты, не влезающие в nl конечностей, отбрасывать нельзя — вызывающий
// обязан это проверить заранее (см. bignum_modexp()).
static void bn_from_bytes(bn_t *a, const uint8_t *bytes, size_t len) {
    bn_zero(a);
    for (size_t i = 0; i < len; i++) {
        size_t rev = len - 1 - i; // индекс байта, считая от младшего
        if (rev / 4 >= BN_LIMBS) continue;
        a->v[rev / 4] |= (uint32_t)bytes[i] << (8 * (rev % 4));
    }
}

// Выгружает a в out_len байт big-endian с нулями слева. -1, если значащих
// байт больше, чем out_len (то есть результат не влезает).
static int bn_to_bytes(const bn_t *a, uint8_t *out, size_t out_len, int nl) {
    for (size_t rev = out_len; rev < (size_t)nl * 4; rev++) {
        if (((a->v[rev / 4] >> (8 * (rev % 4))) & 0xFFu) != 0) return -1;
    }
    for (size_t i = 0; i < out_len; i++) {
        size_t rev = out_len - 1 - i;
        out[i] = (rev / 4 < (size_t)nl)
                     ? (uint8_t)((a->v[rev / 4] >> (8 * (rev % 4))) & 0xFFu)
                     : (uint8_t)0;
    }
    return 0;
}

static int bn_is_zero(const bn_t *a, int nl) {
    uint32_t acc = 0;
    for (int i = 0; i < nl; i++) acc |= a->v[i];
    return acc == 0;
}

static int bn_cmp(const bn_t *a, const bn_t *b, int nl) {
    for (int i = nl - 1; i >= 0; i--) {
        if (a->v[i] != b->v[i]) return (a->v[i] > b->v[i]) ? 1 : -1;
    }
    return 0;
}

// a -= b; вызывается только когда a >= b, поэтому заём из-за верхней
// конечности не уходит.
static void bn_sub(bn_t *a, const bn_t *b, int nl) {
    uint64_t borrow = 0;
    for (int i = 0; i < nl; i++) {
        uint64_t d = (uint64_t)a->v[i] - (uint64_t)b->v[i] - borrow;
        a->v[i] = (uint32_t)d;
        borrow = (d >> 63) & 1u; // старший бит 64-битной разности == был заём
    }
}

static void bn_add(bn_t *a, const bn_t *b, int nl) {
    uint64_t carry = 0;
    for (int i = 0; i < nl; i++) {
        uint64_t s = (uint64_t)a->v[i] + (uint64_t)b->v[i] + carry;
        a->v[i] = (uint32_t)s;
        carry = s >> 32;
    }
}

static void bn_shl1(bn_t *a, int nl) {
    uint32_t carry = 0;
    for (int i = 0; i < nl; i++) {
        uint32_t next = a->v[i] >> 31;
        a->v[i] = (a->v[i] << 1) | carry;
        carry = next;
    }
}

// Номер старшего единичного бита + 1 (0 для нуля).
static int bn_bitlen(const bn_t *a, int nl) {
    for (int i = nl - 1; i >= 0; i--) {
        if (a->v[i] == 0) continue;
        for (int bit = 31; bit >= 0; bit--) {
            if ((a->v[i] >> bit) & 1u) return i * 32 + bit + 1;
        }
    }
    return 0;
}

// r = (a * b) mod n, при условии a < n и b < n. Побитовый "сдвиг-и-
// сложение": r пробегает по битам b от старшего к младшему, на каждом
// шаге удваиваясь (и, если бит единичный, прибавляя a). Так как и 2r, и
// r+a не превосходят 2n, после каждой операции достаточно ОДНОГО условного
// вычитания модуля — честного деления с остатком не требуется (см.
// обоснование выбора такого подхода в bignum.h).
static void bn_mod_mul(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *n, int nl) {
    bn_t acc;
    bn_zero(&acc);

    for (int bit = bn_bitlen(b, nl) - 1; bit >= 0; bit--) {
        bn_shl1(&acc, nl);
        if (bn_cmp(&acc, n, nl) >= 0) bn_sub(&acc, n, nl);

        if ((b->v[bit / 32] >> (bit % 32)) & 1u) {
            bn_add(&acc, a, nl);
            if (bn_cmp(&acc, n, nl) >= 0) bn_sub(&acc, n, nl);
        }
    }
    bn_copy(r, &acc, nl);
}

// r = (big-endian число из bytes) mod n — тот же побитовый проход, только
// биты берутся прямо из входного потока байт, поэтому приводимое число
// может быть любой длины и вообще не обязано влезать в bn_t.
static void bn_mod_from_bytes(bn_t *r, const uint8_t *bytes, size_t len,
                              const bn_t *n, int nl) {
    bn_zero(r);
    for (size_t i = 0; i < len; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            bn_shl1(r, nl);
            if ((bytes[i] >> bit) & 1u) r->v[0] |= 1u; // младший бит свободен после сдвига
            if (bn_cmp(r, n, nl) >= 0) bn_sub(r, n, nl);
        }
    }
}

int bignum_modexp(uint8_t *out, size_t out_len,
                  const uint8_t *base, size_t base_len,
                  const uint8_t *exp, size_t exp_len,
                  const uint8_t *mod, size_t mod_len) {
    if (!out || !base || !exp || !mod) return -1;
    if (base_len > BIGNUM_MAX_BYTES || exp_len > BIGNUM_MAX_BYTES ||
        mod_len > BIGNUM_MAX_BYTES || out_len > BIGNUM_MAX_BYTES) {
        return -1;
    }

    // Модуль занимает ceil(mod_len/4) конечностей, плюс одна запасная под
    // промежуточные значения до 2n (см. комментарий к BN_LIMBS).
    int nl = (int)((mod_len + 3) / 4);
    int out_limbs = (int)((out_len + 3) / 4);
    if (out_limbs > nl) nl = out_limbs; // чтобы bn_to_bytes() не читал за пределами nl
    nl += 1;
    if (nl > BN_LIMBS) return -1;

    bn_t n;
    bn_from_bytes(&n, mod, mod_len);
    if (bn_is_zero(&n, nl)) return -1;

    bn_t b, r;
    bn_mod_from_bytes(&b, base, base_len, &n, nl);

    // r = 1 mod n (для n == 1 это ноль — вырожденный случай, но считать
    // его отдельно незачем, приведение само даёт правильный ответ).
    bn_zero(&r);
    r.v[0] = 1;
    if (bn_cmp(&r, &n, nl) >= 0) bn_sub(&r, &n, nl);

    // Старший значащий бит показателя — до него итерации смысла не имеют
    // (возведение единицы в квадрат). Нулевой показатель даёт r == 1 mod n.
    size_t first = 0;
    while (first < exp_len && exp[first] == 0) first++;
    if (first == exp_len) {
        return bn_to_bytes(&r, out, out_len, nl);
    }
    int top_bit = 7;
    while (((exp[first] >> top_bit) & 1u) == 0) top_bit--;

    for (size_t i = first; i < exp_len; i++) {
        int start = (i == first) ? top_bit : 7;
        for (int bit = start; bit >= 0; bit--) {
            bn_t tmp;
            bn_mod_mul(&tmp, &r, &r, &n, nl);
            bn_copy(&r, &tmp, nl);
            if ((exp[i] >> bit) & 1u) {
                bn_mod_mul(&tmp, &r, &b, &n, nl);
                bn_copy(&r, &tmp, nl);
            }
        }
    }

    return bn_to_bytes(&r, out, out_len, nl);
}
