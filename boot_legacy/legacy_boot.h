#pragma once
#include <stdint.h>

// LegacyHandoff — данные, которые stage2.asm собирает в реальном режиме/
// до входа в long mode и передаёт в legacy_main() через RDI.
//
// packed обязателен: смещения полей продублированы вручную в stage2.asm
// (HANDOFF_OFF_* константы) как сумма размеров предыдущих полей. Без
// packed компилятор вставил бы выравнивающий padding перед полями u64
// (например, между fb_red_field_pos и kernel_base), и asm писал бы не туда.
// Невыровненный доступ к u64 на x86-64 не фолтит, лишь чуть медленнее —
// несущественно, структура читается один раз.
typedef struct __attribute__((packed)) {
    uint64_t e820_addr;      // физический адрес сырых E820-записей (по 24 байта)
    uint32_t e820_count;

    uint64_t fb_base;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;          // байт на строку (BytesPerScanLine из VESA)
    uint32_t fb_bpp;
    uint32_t fb_red_field_pos;  // VESA RedFieldPosition — 16 типично для 32bpp режимов с B,G,R,X в памяти

    uint64_t kernel_base;
    uint64_t kernel_size;
    uint64_t fatimage_base;
    uint64_t fatimage_size;
} LegacyHandoff;

_Static_assert(sizeof(LegacyHandoff) == 72, "LegacyHandoff size changed — пересчитай HANDOFF_OFF_* в stage2.asm");

// Смещения полей (для синхронизации с HANDOFF_OFF_* в stage2.asm):
//   e820_addr=0  e820_count=8  fb_base=12  fb_width=20  fb_height=24
//   fb_pitch=28  fb_bpp=32  fb_red_field_pos=36  kernel_base=40
//   kernel_size=48  fatimage_base=56  fatimage_size=64  (конец: 72)
