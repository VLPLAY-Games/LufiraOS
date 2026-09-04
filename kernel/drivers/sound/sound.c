#include "sound.h"
#include "pcspeaker.h"

/* ============================================================
 * State
 * ============================================================ */

static int sound_initialized = 0;

/* ============================================================
 * Init
 * ============================================================ */

void sound_init(void)
{
    pcspeaker_init();

    sound_initialized = 1;
}

/* ============================================================
 * Frequency
 * ============================================================ */

void sound_play_frequency(uint32_t frequency,
                          uint32_t duration_ms)
{
    if (!sound_initialized) {
        sound_init();
    }

    if (frequency == 0) {
        sound_pause(duration_ms);
        return;
    }

    pcspeaker_tone(frequency, duration_ms);
}

/* ============================================================
 * Musical note
 * ============================================================ */

void sound_play_note(sound_note_t note,
                     uint32_t duration_ms)
{
    sound_play_frequency((uint32_t)note, duration_ms);
}

/* ============================================================
 * Pause
 * ============================================================ */

void sound_pause(uint32_t duration_ms)
{
    if (!sound_initialized) {
        sound_init();
    }

    /*
     * Используем tone(0) как silence.
     */
    pcspeaker_stop();

    /*
     * Небольшой software delay.
     */
    for (uint32_t ms = 0; ms < duration_ms; ++ms) {
        for (volatile uint32_t i = 0; i < 100000; ++i) {
            asm volatile("pause");
        }
    }
}

/* ============================================================
 * Stop
 * ============================================================ */

void sound_stop(void)
{
    pcspeaker_stop();
}

/* ============================================================
 * Volume
 * ============================================================ */

void sound_set_volume(uint8_t volume)
{
    if (!sound_initialized) {
        sound_init();
    }

    pcspeaker_set_volume(volume);
}

uint8_t sound_get_volume(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    return pcspeaker_get_volume();
}

/* ============================================================
 * Availability
 * ============================================================ */

int sound_is_available(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    return pcspeaker_is_available();
}

/* ============================================================
 * Melody
 * ============================================================ */

void sound_play_melody(const sound_event_t *melody,
                       uint32_t count)
{
    if (!melody || count == 0) {
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {

        if (melody[i].note != 0) {
            sound_play_note(
                melody[i].note,
                melody[i].duration_ms
            );
        } else {
            sound_pause(melody[i].duration_ms);
        }

        if (melody[i].pause_ms > 0) {
            sound_pause(melody[i].pause_ms);
        }
    }

    sound_stop();
}