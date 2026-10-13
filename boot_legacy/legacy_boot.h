#pragma once
#include <stdint.h>

// LegacyHandoff — то, что stage2.asm (16/32-битная часть) успевает собрать
// в реальном режиме/до входа в long mode, и передаёт в legacy_main() через
// RDI (один указатель, как и kernel_entry(BootInfo*) ниже по цепочке).
//
// __attribute__((packed)) — ОБЯЗАТЕЛЬНО: смещения полей продублированы
// руками в stage2.asm (HANDOFF_OFF_* константы) как простая сумма размеров
// предыдущих полей. Без packed компилятор вставил бы выравнивающий padding
// перед полями u64 (например, между fb_red_field_pos и kernel_base) —
// asm писал бы не в то место, а legacy_main() читал бы мусор. Невыровненный
// доступ к u64 на x86-64 не фолтит, просто не самый быстрый — тут это не
// имеет значения (структура читается один раз на старте).
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
