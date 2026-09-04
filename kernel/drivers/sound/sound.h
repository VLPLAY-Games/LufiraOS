#pragma once

#include <stdint.h>

/* ============================================================
 * Musical notes
 * ============================================================ */

typedef enum
{
    SOUND_NOTE_C4 = 262,
    SOUND_NOTE_CS4 = 277,
    SOUND_NOTE_D4 = 294,
    SOUND_NOTE_DS4 = 311,
    SOUND_NOTE_E4 = 330,
    SOUND_NOTE_F4 = 349,
    SOUND_NOTE_FS4 = 370,
    SOUND_NOTE_G4 = 392,
    SOUND_NOTE_GS4 = 415,
    SOUND_NOTE_A4 = 440,
    SOUND_NOTE_AS4 = 466,
    SOUND_NOTE_B4 = 494,

    SOUND_NOTE_C5 = 523,
    SOUND_NOTE_CS5 = 554,
    SOUND_NOTE_D5 = 587,
    SOUND_NOTE_DS5 = 622,
    SOUND_NOTE_E5 = 659,
    SOUND_NOTE_F5 = 698,
    SOUND_NOTE_FS5 = 740,
    SOUND_NOTE_G5 = 784,
    SOUND_NOTE_GS5 = 831,
    SOUND_NOTE_A5 = 880,
    SOUND_NOTE_AS5 = 932,
    SOUND_NOTE_B5 = 988,

    SOUND_NOTE_C6 = 1047,
    SOUND_NOTE_CS6 = 1109,
    SOUND_NOTE_D6 = 1175,
    SOUND_NOTE_DS6 = 1245,
    SOUND_NOTE_E6 = 1319,
    SOUND_NOTE_F6 = 1397,
    SOUND_NOTE_FS6 = 1480,
    SOUND_NOTE_G6 = 1568,
    SOUND_NOTE_GS6 = 1661,
    SOUND_NOTE_A6 = 1760,
    SOUND_NOTE_AS6 = 1865,
    SOUND_NOTE_B6 = 1976

} sound_note_t;

/* ============================================================
 * API
 * ============================================================ */

void sound_init(void);

void sound_play_frequency(uint32_t frequency,
                          uint32_t duration_ms);

void sound_play_note(sound_note_t note,
                     uint32_t duration_ms);

void sound_pause(uint32_t duration_ms);

void sound_stop(void);

void sound_set_volume(uint8_t volume);
uint8_t sound_get_volume(void);

int sound_is_available(void);

/* ============================================================
 * Melody
 * ============================================================ */

typedef struct
{
    sound_note_t note;
    uint16_t duration_ms;
    uint16_t pause_ms;
} sound_event_t;

void sound_play_melody(const sound_event_t *melody,
                       uint32_t count);