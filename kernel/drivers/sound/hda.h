#pragma once

#include "lib/types.h"

// Intel HD Audio (HDA) — второй звуковой драйвер после AC'97: многие
// современные чипсеты не имеют AC'97-эмуляции, только HDA. В отличие от
// AC'97 (I/O BAR'ы, простой 8-байтный BDL, настройка через прямые NAM-
// регистры), HDA — MMIO-контроллер с отдельным командным интерфейсом к
// кодеку (CORB/RIRB — кольца verb-команд/ответов) и 16-байтным BDL с
// 64-битными адресами. Полностью polled: прерывания контроллера не
// используются ни для завершения DMA (SDnSTS.BCIS), ни для ответов кодека
// (RIRBWP).
//
// Топология кодека определяется в hda_init() обходом дерева узлов через
// verb'ы Get Parameter/Get Connection List Entry — см. hda.c (поддержано
// прямое соединение Pin->ЦАП и до двух хопов через Mixer/Selector; более
// сложные топологии — отказ, а не угадывание).

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
