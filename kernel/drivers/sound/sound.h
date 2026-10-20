#pragma once

#include "lib/types.h"

// Диспетчер звуковых драйверов — та же архитектура, что disk.c поверх
// ahci.c/nvme.c: пробует HDA (hda.c, более современный, предпочтителен) и,
// если контроллера/кодека/подходящего Pin+ЦАП не нашлось, откатывается на
// AC'97 (ac97.c).
//
// sound_play_tone() возвращает 0/-1 (как и весь остальной код этого ядра),
// а НЕ "0=ошибка" стиль самого ac97_play_tone() — несоответствие гасится
// прямо здесь, в sound.c, так что вызывающему (shell/commands/sound.c)
// не нужно помнить, какой бэкенд сейчас активен и в какую сторону у него
// развёрнут возврат.

int sound_init(void);
int sound_is_available(void);

// "HDA" / "AC'97" / "none" — для вывода в mixer, чтобы команда не врала
// про конкретное устройство, когда активен не ac97.
const char *sound_driver_name(void);

void sound_set_volume(uint8_t volume);
uint8_t sound_get_volume(void);

void sound_set_sample_rate(uint32_t rate);
uint32_t sound_get_sample_rate(void);

// 0 — успех, -1 — ошибка или нет активного драйвера.
int sound_play_tone(uint32_t frequency, uint32_t duration_ms);

// Сырой PCM-стерео-буфер (int16_t, interleaved L/R, frames фреймов) —
// для doomgeneric (SYS_SOUND_PLAY_PCM, syscall.h); оба бэкенда уже умеют
// это (ac97_play_pcm_stereo()/hda_play_pcm_stereo()), sound_play_tone()
// выше просто синтезирует такой же буфер внутри себя. 0 — успех, -1 —
// ошибка или нет активного драйвера.
int sound_play_pcm_stereo(const int16_t *samples, uint32_t frames);

void sound_stop(void);
