#include "sound.h"
#include "hda.h"
#include "ac97.h"
#include "drivers/console/console.h"
#include "system/devmode/devmode.h"

typedef enum { SOUND_BACKEND_NONE, SOUND_BACKEND_HDA, SOUND_BACKEND_AC97 } sound_backend_t;

static sound_backend_t g_backend = SOUND_BACKEND_NONE;

int sound_init(void) {
    if (hda_init() == 0) {
        g_backend = SOUND_BACKEND_HDA;
        DLOG("[SOUND] Using HDA\n");
        return 0;
    }
    if (ac97_init()) {
        g_backend = SOUND_BACKEND_AC97;
        DLOG("[SOUND] Using AC'97\n");
        return 0;
    }
    DLOG("[SOUND] No audio controller found\n");
    g_backend = SOUND_BACKEND_NONE;
    return -1;
}

int sound_is_available(void) {
    switch (g_backend) {
        case SOUND_BACKEND_HDA:  return hda_is_available();
        case SOUND_BACKEND_AC97: return ac97_is_available();
        default:                 return 0;
    }
}

const char *sound_driver_name(void) {
    switch (g_backend) {
        case SOUND_BACKEND_HDA:  return "HDA";
        case SOUND_BACKEND_AC97: return "AC'97";
        default:                 return "none";
    }
}

void sound_set_volume(uint8_t volume) {
    if (g_backend == SOUND_BACKEND_HDA) hda_set_volume(volume);
    else if (g_backend == SOUND_BACKEND_AC97) ac97_set_volume(volume);
}

uint8_t sound_get_volume(void) {
    if (g_backend == SOUND_BACKEND_HDA) return hda_get_volume();
    if (g_backend == SOUND_BACKEND_AC97) return ac97_get_volume();
    return 0;
}

void sound_set_sample_rate(uint32_t rate) {
    if (g_backend == SOUND_BACKEND_HDA) hda_set_sample_rate(rate);
    else if (g_backend == SOUND_BACKEND_AC97) ac97_set_sample_rate(rate);
}

uint32_t sound_get_sample_rate(void) {
    if (g_backend == SOUND_BACKEND_HDA) return hda_get_sample_rate();
    if (g_backend == SOUND_BACKEND_AC97) return ac97_get_sample_rate();
    return 0;
}

int sound_play_tone(uint32_t frequency, uint32_t duration_ms) {
    if (g_backend == SOUND_BACKEND_HDA) return hda_play_tone(frequency, duration_ms);
    // ac97_play_tone() развёрнут наоборот — ненулевой результат есть успех
    // (см. разбор в sound.h) — приводим к общему 0=успех здесь же.
    if (g_backend == SOUND_BACKEND_AC97) return ac97_play_tone(frequency, duration_ms) ? 0 : -1;
    return -1;
}

int sound_play_pcm_stereo(const int16_t *samples, uint32_t frames) {
    if (g_backend == SOUND_BACKEND_HDA) return hda_play_pcm_stereo(samples, frames) ? 0 : -1;
    // ac97_stream_write() (не ac97_play_pcm_stereo() — тот одноразовый, со
    // слышимыми щелчками между слайсами) — развёрнут так же, ненулевой
    // результат есть успех.
    if (g_backend == SOUND_BACKEND_AC97) return ac97_stream_write(samples, frames) ? 0 : -1;
    return -1;
}

void sound_stop(void) {
    if (g_backend == SOUND_BACKEND_HDA) hda_stop();
    else if (g_backend == SOUND_BACKEND_AC97) ac97_stop();
}
