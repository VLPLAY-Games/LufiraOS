#pragma once

#include "lib/types.h"

// Intel HD Audio (HDA) — v0.9, фаза 1, пункт 6. Второй (после AC'97, см.
// ac97.h) звуковой драйвер — многие современные чипсеты (и QEMU-профили
// без AC'97-эмуляции) имеют только HDA. В отличие от AC'97 (I/O BAR'ы,
// простой 8-байтный BDL, настройки кодека через прямые NAM-регистры), HDA —
// MMIO-контроллер с отдельным командным интерфейсом к кодеку (CORB/RIRB —
// кольца "verb"-команд/ответов) и 16-байтным BDL с 64-битными адресами.
// Тот же полностью polled подход, что и везде в этом ядре: прерывания
// контроллера не используются вовсе (см. разбор в hda.c) — ни для
// завершения DMA (опрашиваем SDnSTS.BCIS), ни для ответов кодека
// (опрашиваем RIRBWP).
//
// Топология кодека (какой виджет — ЦАП, какой — выходной Pin, как они
// соединены) определяется во время hda_init() обходом дерева узлов через
// verb'ы Get Parameter/Get Connection List Entry — см. подробный разбор
// поддержанного подмножества в hda.c (прямое соединение Pin->ЦАП и до двух
// хопов через Mixer/Selector; более сложные топологии — отказ, не угадывание).

int hda_init(void);
int hda_is_available(void);

void hda_set_volume(uint8_t volume);
uint8_t hda_get_volume(void);

// Фактическая частота дискретизации HDA квантуется (BASE/MULT/DIV поля
// формата, см. hda.c) — в отличие от AC'97 (непрерывный регистр через VRA),
// запрошенное значение округляется до ближайшей ИЗ ПОДДЕРЖАННОГО НАБОРА
// (8000/11025/16000/22050/32000/44100/48000/88200/96000/176400/192000 Гц).
void hda_set_sample_rate(uint32_t rate);
uint32_t hda_get_sample_rate(void);

int hda_play_pcm_stereo(const int16_t *samples, uint32_t frames);
int hda_play_tone(uint32_t frequency, uint32_t duration_ms);

void hda_stop(void);
