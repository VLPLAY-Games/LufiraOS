/* speaker.h - драйвер PC Speaker для LufiraOS */

#ifndef SPEAKER_H
#define SPEAKER_H

#include <stdint.h>

/* Порта для управления PC Speaker */
#define SPEAKER_PORT       0x61    /* Управление динамиком */
#define PIT_CHANNEL2_PORT  0x42    /* Канал 2 программируемого таймера */
#define PIT_COMMAND_PORT   0x43    /* Порт команд таймера */

/* Команды для программируемого интервального таймера (PIT) */
#define PIT_CHANNEL2       0x40    /* Номер канала 2 в команде */
#define PIT_ACCESS_LOHI    0x30    /* Сначала младший, затем старший байт */
#define PIT_MODE_SQUARE    0x06    /* Режим 3: генератор квадратной волны */

/* Частоты для стандартных нот (в Гц) */
#define NOTE_C4     262    /* До 4-й октавы */
#define NOTE_D4     294    /* Ре */
#define NOTE_E4     330    /* Ми */
#define NOTE_F4     349    /* Фа */
#define NOTE_G4     392    /* Соль */
#define NOTE_A4     440    /* Ля */
#define NOTE_B4     494    /* Си */
#define NOTE_C5     523    /* До 5-й октавы */

/* Длительности нот (в миллисекундах) */
#define DURATION_WHOLE     2000    /* Целая нота */
#define DURATION_HALF      1000    /* Половина */
#define DURATION_QUARTER   500     /* Четверть */
#define DURATION_EIGHTH    250     /* Восьмая */
#define DURATION_SIXTEENTH 125     /* Шестнадцатая */


/* Типы звуковых сигналов */
typedef enum {
    BEEP_SHORT,     /* Короткий писк */
    BEEP_MEDIUM,    /* Средний писк */
    BEEP_LONG,      /* Длинный писк */
    BEEP_ERROR,     /* Звук ошибки */
    BEEP_SUCCESS,   /* Звук успеха */
    BEEP_WARNING,   /* Звук предупреждения */
    BEEP_STARTUP    /* Звук запуска системы */
} beep_type_t;

/* Структура для музыкальной ноты */
typedef struct {
    uint32_t frequency;   /* Частота в Гц */
    uint32_t duration;    /* Длительность в мс */
} note_t;

/* Прототипы функций */

/* Инициализация драйвера динамика */
void speaker_init(void);

/* Включить звук с заданной частотой */
void speaker_beep(uint32_t frequency);

/* Выключить звук */
void speaker_silence(void);

/* Издать звук заданного типа */
void speaker_play_beep(beep_type_t type);

/* Проиграть последовательность нот */
void speaker_play_notes(const note_t* notes, uint32_t count);

/* Проиграть звук с заданной частотой и длительностью */
void speaker_play_frequency(uint32_t frequency, uint32_t duration_ms);

/* Проиграть мелодию запуска системы */
void speaker_play_startup_melody(void);

/* Проиграть мелодию ошибки */
void speaker_play_error_melody(void);

/* Проиграть мелодию успеха */
void speaker_play_success_melody(void);

/* Установить громкость (0-100) - программная эмуляция */
void speaker_set_volume(uint8_t volume);

/* Тестовая функция: гамма */
void speaker_play_scale(void);

/* Получить статус динамика */
int speaker_is_enabled(void);

int speaker_get_volume(void);

#endif /* SPEAKER_H */