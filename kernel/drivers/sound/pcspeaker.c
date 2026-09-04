#include "pcspeaker.h"
#include "../../lib/types.h"

/* ============================================================
 * I/O
 * ============================================================ */

static inline void outb(uint16_t port, uint8_t value)
{
    asm volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    asm volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

/* ============================================================
 * PC SPEAKER / PIT CHANNEL 2
 * ============================================================ */

#define PIT_CHANNEL_2   0x42
#define PIT_COMMAND     0x43

#define SPEAKER_PORT    0x61

#define PIT_BASE_FREQ   1193182UL

#define PIT_CH2_MODE3   0xB6

#define SPEAKER_ENABLE  0x01
#define SPEAKER_GATE    0x02

/* ============================================================
 * State
 * ============================================================ */

static int initialized = 0;
static int gate_enabled = 0;

static uint8_t volume = 100;

/* ============================================================
 * Delay
 *
 * Пока оставляем простой delay, потому что exact API твоего
 * timer subsystem здесь не известен.
 *
 * Позже этот кусок можно перевести на kernel timer ticks.
 * ============================================================ */

static void pcspeaker_delay_us(uint32_t us)
{
    /*
     * Приближённая задержка.
     *
     * Это не high precision timer.
     * Для музыкальных нот вполне достаточно для текущего
     * раннего ядра.
     */

    for (uint32_t u = 0; u < us; ++u) {
        for (volatile uint32_t i = 0; i < 4; ++i) {
            asm volatile("pause");
        }
    }
}

static void pcspeaker_delay_ms(uint32_t ms)
{
    while (ms--) {
        pcspeaker_delay_us(1000);
    }
}

/* ============================================================
 * Init
 * ============================================================ */

void pcspeaker_init(void)
{
    uint8_t value;

    value = inb(SPEAKER_PORT);

    /*
     * Оставляем остальные bits нетронутыми.
     * Убираем speaker enable + gate.
     */
    value &= ~(SPEAKER_ENABLE | SPEAKER_GATE);

    outb(SPEAKER_PORT, value);

    gate_enabled = 0;
    initialized = 1;
}

/* ============================================================
 * Start tone
 * ============================================================ */

void pcspeaker_start(uint32_t frequency)
{
    uint32_t divisor;
    uint8_t speaker;

    if (!initialized) {
        pcspeaker_init();
    }

    if (frequency < PCSPEAKER_MIN_FREQUENCY) {
        frequency = PCSPEAKER_MIN_FREQUENCY;
    }

    if (frequency > PCSPEAKER_MAX_FREQUENCY) {
        frequency = PCSPEAKER_MAX_FREQUENCY;
    }

    divisor = PIT_BASE_FREQ / frequency;

    if (divisor < 1) {
        divisor = 1;
    }

    if (divisor > 65535) {
        divisor = 65535;
    }

    /*
     * PIT channel 2:
     *
     * channel = 2
     * access   = lobyte/hibyte
     * mode     = 3 (square wave)
     */
    outb(PIT_COMMAND, PIT_CH2_MODE3);

    outb(PIT_CHANNEL_2, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL_2, (uint8_t)((divisor >> 8) & 0xFF));

    /*
     * Состояние speaker.
     */
    speaker = inb(SPEAKER_PORT);

    speaker |= SPEAKER_ENABLE;

    if (gate_enabled) {
        speaker |= SPEAKER_GATE;
    } else {
        speaker &= ~SPEAKER_GATE;
    }

    outb(SPEAKER_PORT, speaker);
}

/* ============================================================
 * Stop
 * ============================================================ */

void pcspeaker_stop(void)
{
    uint8_t speaker;

    speaker = inb(SPEAKER_PORT);

    speaker &= ~(SPEAKER_ENABLE | SPEAKER_GATE);

    outb(SPEAKER_PORT, speaker);

    gate_enabled = 0;
}

/* ============================================================
 * Gate
 * ============================================================ */

void pcspeaker_set_gate(int enabled)
{
    uint8_t speaker;

    if (!initialized) {
        pcspeaker_init();
    }

    gate_enabled = enabled ? 1 : 0;

    speaker = inb(SPEAKER_PORT);

    if (gate_enabled) {
        speaker |= SPEAKER_GATE;
    } else {
        speaker &= ~SPEAKER_GATE;
    }

    outb(SPEAKER_PORT, speaker);
}

int pcspeaker_get_gate(void)
{
    return gate_enabled;
}

/* ============================================================
 * Volume
 *
 * PC speaker не имеет настоящего hardware volume.
 *
 * Здесь volume хранится как software mixer value.
 * ============================================================ */

uint8_t pcspeaker_get_volume(void)
{
    return volume;
}

void pcspeaker_set_volume(uint8_t new_volume)
{
    if (new_volume > 100) {
        new_volume = 100;
    }

    volume = new_volume;

    /*
     * Если выставили 0 — гарантированно выключаем speaker.
     */
    if (volume == 0) {
        pcspeaker_stop();
    }
}

/* ============================================================
 * Tone
 * ============================================================ */

void pcspeaker_tone(uint32_t frequency, uint32_t duration_ms)
{
    if (!initialized) {
        pcspeaker_init();
    }

    if (volume == 0) {
        pcspeaker_stop();
        pcspeaker_delay_ms(duration_ms);
        return;
    }

    /*
     * 100% — обычная нота.
     */
    if (volume >= 100) {
        pcspeaker_set_gate(1);
        pcspeaker_start(frequency);

        pcspeaker_delay_ms(duration_ms);

        pcspeaker_stop();
        return;
    }

    /*
     * Software volume modulation.
     *
     * Чем меньше volume, тем меньше времени в каждом
     * небольшом audio window speaker находится включённым.
     *
     * Например:
     *
     * 100% -> 100% ON
     * 50%  -> 50% ON / 50% OFF
     * 25%  -> 25% ON / 75% OFF
     */

    const uint32_t window_ms = 10;

    uint32_t remaining = duration_ms;

    pcspeaker_start(frequency);

    while (remaining > 0) {
        uint32_t current_window =
            (remaining > window_ms) ? window_ms : remaining;

        uint32_t on_time =
            (current_window * volume) / 100;

        uint32_t off_time =
            current_window - on_time;

        if (on_time > 0) {
            pcspeaker_set_gate(1);
            pcspeaker_delay_ms(on_time);
        } else {
            pcspeaker_set_gate(0);
        }

        if (off_time > 0) {
            pcspeaker_set_gate(0);
            pcspeaker_delay_ms(off_time);
        }

        remaining -= current_window;
    }

    pcspeaker_stop();
}

/* ============================================================
 * Availability
 * ============================================================ */

int pcspeaker_is_available(void)
{
    return initialized;
}