/* speaker.c - драйвер PC Speaker для LufiraOS */

#include "speaker.h"
#include "shell.h"
#include <stddef.h>

/* Глобальные переменные */
static uint8_t speaker_volume = 50;  /* Громкость по умолчанию (0-100) */
static uint8_t speaker_enabled = 1;  /* Включен ли динамик */

/* Вспомогательные функции для работы с портами */
static inline void outb(uint16_t port, uint8_t value) {
    asm volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Задержка в миллисекундах (простая реализация) */
static void speaker_delay_ms(uint32_t ms) {
    /* Примерная задержка: на 1 МГц процессоре 1 мс ≈ 1000 циклов */
    /* Регулируем в зависимости от желаемой скорости */
    volatile uint32_t i;
    for (i = 0; i < ms * 1000; i++) {
        asm volatile ("pause");
    }
}

/* Задержка в микросекундах */
static void speaker_delay_us(uint32_t us) {
    volatile uint32_t i;
    for (i = 0; i < us; i++) {
        asm volatile ("pause");
    }
}

/* Инициализация драйвера динамика */
void speaker_init(void) {
    terminal_writestring("[SPK] Initializing PC Speaker... ");
    
    /* Проверяем, доступен ли динамик */
    uint8_t val = inb(SPEAKER_PORT);
    if ((val & 0x03) != 0x03) {
        /* Динамик не обнаружен или недоступен */
        speaker_enabled = 0;
        terminal_writestring("NOT FOUND\n");
        return;
    }
    
    /* Сбрасываем динамик в выключенное состояние */
    speaker_silence();
    
    terminal_writestring("OK\n");
    terminal_writestring("[SPK] Speaker ready (volume: ");
    
    /* Выводим информацию о громкости */
    char vol_str[4];
    vol_str[0] = '0' + (speaker_volume / 100);
    vol_str[1] = '0' + ((speaker_volume % 100) / 10);
    vol_str[2] = '0' + (speaker_volume % 10);
    vol_str[3] = '\0';
    terminal_writestring(vol_str);
    terminal_writestring("%)\n");
}

/* Настроить таймер для генерации частоты */
static void speaker_set_frequency(uint32_t frequency) {
    if (frequency == 0 || !speaker_enabled) {
        return;
    }
    
    /* Частота PIT: 1,193,180 Гц */
    uint32_t divisor = 1193180 / frequency;
    
    /* Устанавливаем режим таймера для канала 2 */
    outb(PIT_COMMAND_PORT, PIT_CHANNEL2 | PIT_ACCESS_LOHI | PIT_MODE_SQUARE);
    
    /* Записываем делитель (сначала младший, потом старший байт) */
    outb(PIT_CHANNEL2_PORT, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL2_PORT, (uint8_t)((divisor >> 8) & 0xFF));
}

/* Включить звук с заданной частотой */
void speaker_beep(uint32_t frequency) {
    if (!speaker_enabled || frequency == 0) {
        return;
    }
    
    /* Устанавливаем частоту на таймере */
    speaker_set_frequency(frequency);
    
    /* Включаем динамик */
    uint8_t tmp = inb(SPEAKER_PORT);
    if ((tmp & 0x03) != 0x03) {
        outb(SPEAKER_PORT, tmp | 0x03);
    }
}

/* Выключить звук */
void speaker_silence(void) {
    if (!speaker_enabled) {
        return;
    }
    
    /* Выключаем динамик */
    uint8_t tmp = inb(SPEAKER_PORT);
    if ((tmp & 0x03) == 0x03) {
        outb(SPEAKER_PORT, tmp & ~0x03);
    }
}

/* Издать звук заданного типа */
void speaker_play_beep(beep_type_t type) {
    if (!speaker_enabled) {
        return;
    }
    
    switch (type) {
        case BEEP_SHORT:
            speaker_beep(1000);
            speaker_delay_ms(50);
            speaker_silence();
            break;
            
        case BEEP_MEDIUM:
            speaker_beep(800);
            speaker_delay_ms(150);
            speaker_silence();
            break;
            
        case BEEP_LONG:
            speaker_beep(600);
            speaker_delay_ms(300);
            speaker_silence();
            break;
            
        case BEEP_ERROR:
            /* Три коротких писка */
            for (int i = 0; i < 3; i++) {
                speaker_beep(200);
                speaker_delay_ms(100);
                speaker_silence();
                speaker_delay_ms(50);
            }
            break;
            
        case BEEP_SUCCESS:
            /* Восходящий звук */
            speaker_beep(300);
            speaker_delay_ms(50);
            speaker_beep(500);
            speaker_delay_ms(50);
            speaker_beep(700);
            speaker_delay_ms(50);
            speaker_silence();
            break;
            
        case BEEP_WARNING:
            /* Нисходящий звук */
            speaker_beep(800);
            speaker_delay_ms(100);
            speaker_beep(400);
            speaker_delay_ms(100);
            speaker_silence();
            break;
            
        case BEEP_STARTUP:
            speaker_play_startup_melody();
            break;
    }
}

/* Проиграть звук с заданной частотой и длительностью */
void speaker_play_frequency(uint32_t frequency, uint32_t duration_ms) {
    if (!speaker_enabled || frequency == 0) {
        return;
    }
    
    speaker_beep(frequency);
    speaker_delay_ms(duration_ms);
    speaker_silence();
}

/* Проиграть последовательность нот */
void speaker_play_notes(const note_t* notes, uint32_t count) {
    if (!speaker_enabled || notes == NULL || count == 0) {
        return;
    }
    
    for (uint32_t i = 0; i < count; i++) {
        if (notes[i].frequency > 0) {
            speaker_beep(notes[i].frequency);
            speaker_delay_ms(notes[i].duration);
        } else {
            /* Пауза */
            speaker_silence();
            speaker_delay_ms(notes[i].duration);
        }
    }
    speaker_silence();
}

/* Проиграть мелодию запуска системы */
void speaker_play_startup_melody(void) {
    if (!speaker_enabled) {
        return;
    }
    
    /* Простая восходящая гамма */
    const note_t startup_notes[] = {
        {NOTE_C4, 100},
        {NOTE_E4, 100},
        {NOTE_G4, 100},
        {NOTE_C5, 200},
        {0, 50}, /* Пауза */
        {NOTE_C5, 100}
    };
    
    speaker_play_notes(startup_notes, sizeof(startup_notes) / sizeof(note_t));
}

/* Проиграть мелодию ошибки */
void speaker_play_error_melody(void) {
    if (!speaker_enabled) {
        return;
    }
    
    const note_t error_notes[] = {
        {NOTE_C4, 200},
        {0, 50},
        {NOTE_C4, 200}
    };
    
    speaker_play_notes(error_notes, sizeof(error_notes) / sizeof(note_t));
}

/* Проиграть мелодию успеха */
void speaker_play_success_melody(void) {
    if (!speaker_enabled) {
        return;
    }
    
    const note_t success_notes[] = {
        {NOTE_E4, 100},
        {NOTE_G4, 100},
        {NOTE_C5, 200}
    };
    
    speaker_play_notes(success_notes, sizeof(success_notes) / sizeof(note_t));
}

/* Установить громкость (0-100) */
void speaker_set_volume(uint8_t volume) {
    if (volume > 100) {
        volume = 100;
    }
    
    speaker_volume = volume;
    
    /* В реальном PC Speaker нет регулировки громкости,
       но мы можем эмулировать ее через длительность импульсов */
    /* На самом деле просто сохраняем значение для информации */
}

/* Тестовая функция: гамма */
void speaker_play_scale(void) {
    if (!speaker_enabled) {
        terminal_writestring("Speaker is not available\n");
        return;
    }
    
    terminal_writestring("Playing musical scale...\n");
    
    const note_t scale[] = {
        {NOTE_C4, 200},
        {NOTE_D4, 200},
        {NOTE_E4, 200},
        {NOTE_F4, 200},
        {NOTE_G4, 200},
        {NOTE_A4, 200},
        {NOTE_B4, 200},
        {NOTE_C5, 400},
        {0, 100}, /* Пауза */
        {NOTE_C5, 200},
        {NOTE_B4, 200},
        {NOTE_A4, 200},
        {NOTE_G4, 200},
        {NOTE_F4, 200},
        {NOTE_E4, 200},
        {NOTE_D4, 200},
        {NOTE_C4, 400}
    };
    
    speaker_play_notes(scale, sizeof(scale) / sizeof(note_t));
    
    terminal_writestring("Scale complete.\n");
}

/* Функция для проверки работоспособности динамика */
void speaker_test(void) {
    if (!speaker_enabled) {
        terminal_writestring("Speaker test: NOT AVAILABLE\n");
        return;
    }
    
    terminal_writestring("Speaker test:\n");
    terminal_writestring("1. Short beep... ");
    speaker_play_beep(BEEP_SHORT);
    terminal_writestring("OK\n");
    
    terminal_writestring("2. Error beep... ");
    speaker_play_beep(BEEP_ERROR);
    terminal_writestring("OK\n");
    
    terminal_writestring("3. Success beep... ");
    speaker_play_beep(BEEP_SUCCESS);
    terminal_writestring("OK\n");
    
    terminal_writestring("4. Frequency sweep... ");
    for (uint32_t freq = 100; freq <= 1000; freq += 100) {
        speaker_beep(freq);
        speaker_delay_ms(20);
    }
    speaker_silence();
    terminal_writestring("OK\n");
    
    terminal_writestring("Speaker test completed successfully.\n");
}

/* Получить статус динамика */
int speaker_is_enabled(void) {
    return speaker_enabled;
}

int speaker_get_volume(void) {
    return speaker_volume;
}