#include "pcspeaker.h"

/* ============================================================
 * I/O PORTS
 * ============================================================ */

#define PIT_CHANNEL2    0x42
#define PIT_COMMAND     0x43
#define SPEAKER_PORT    0x61

/* PIT input frequency: 1193182 Hz */
#define PIT_BASE_FREQ   1193182UL

/* PIT channel 2, square-wave generator */
#define PIT_CHANNEL2_CMD    0xB6

/* Speaker control bits in port 0x61 */
#define SPEAKER_ENABLE      0x01
#define SPEAKER_GATE        0x02

/* ============================================================
 * LOW LEVEL I/O
 * ============================================================ */

static inline void outb(uint16_t port, uint8_t value)
{
    asm volatile(
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    asm volatile(
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

/* ============================================================
 * STATE
 * ============================================================ */

static int speaker_initialized = 0;
static int speaker_enabled = 0;

/* ============================================================
 * SMALL DELAY
 *
 * We deliberately keep this local to the first driver.
 * Later this should use your kernel timer/scheduler.
 * ============================================================ */

static void pcspeaker_delay_ms(uint32_t ms)
{
    /*
     * This is intentionally only for the initial driver.
     *
     * It is not a precise real-time delay.
     * It merely keeps the tone audible long enough
     * for a first hardware test.
     *
     * Approximate value for a few GHz CPU.
     */
    for (uint32_t m = 0; m < ms; m++) {
        for (volatile uint32_t i = 0; i < 50000; i++) {
            asm volatile("pause");
        }
    }
}

/* ============================================================
 * INITIALIZATION
 * ============================================================ */

void pcspeaker_init(void)
{
    uint8_t value;

    /*
     * Preserve all unrelated bits of port 0x61.
     */
    value = inb(SPEAKER_PORT);

    /*
     * Disable speaker.
     */
    value &= ~(SPEAKER_ENABLE | SPEAKER_GATE);

    outb(SPEAKER_PORT, value);

    speaker_initialized = 1;
    speaker_enabled = 0;
}

/* ============================================================
 * START TONE
 * ============================================================ */

void pcspeaker_start(uint32_t frequency)
{
    uint16_t divisor;
    uint8_t speaker_state;

    if (!speaker_initialized)
        pcspeaker_init();

    if (frequency == 0)
        return;

    /*
     * PIT divider:
     *
     *     divisor = 1193182 / frequency
     *
     * PIT counter is 16-bit.
     */
    divisor = (uint16_t)(PIT_BASE_FREQ / frequency);

    /*
     * Avoid invalid PIT divisor.
     */
    if (divisor == 0)
        divisor = 1;

    /*
     * Channel 2:
     * 10110110b
     *
     * 10   = channel 2
     * 11   = access low byte then high byte
     * 011  = mode 3 (square wave)
     * 0    = binary mode
     */
    outb(PIT_COMMAND, PIT_CHANNEL2_CMD);

    /*
     * Send divisor, low byte first.
     */
    outb(PIT_CHANNEL2, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL2, (uint8_t)((divisor >> 8) & 0xFF));

    /*
     * Enable PIT channel 2 -> speaker.
     */
    speaker_state = inb(SPEAKER_PORT);

    speaker_state |= SPEAKER_ENABLE;
    speaker_state |= SPEAKER_GATE;

    outb(SPEAKER_PORT, speaker_state);

    speaker_enabled = 1;
}

/* ============================================================
 * STOP TONE
 * ============================================================ */

void pcspeaker_stop(void)
{
    uint8_t value;

    if (!speaker_initialized)
        return;

    value = inb(SPEAKER_PORT);

    /*
     * Disable speaker and PIT gate.
     */
    value &= ~(SPEAKER_ENABLE | SPEAKER_GATE);

    outb(SPEAKER_PORT, value);

    speaker_enabled = 0;
}

/* ============================================================
 * BEEP
 * ============================================================ */

void pcspeaker_beep(uint32_t frequency, uint32_t duration_ms)
{
    if (!speaker_initialized)
        pcspeaker_init();

    if (frequency == 0)
        return;

    if (duration_ms == 0)
        return;

    pcspeaker_start(frequency);

    pcspeaker_delay_ms(duration_ms);

    pcspeaker_stop();
}

/* ============================================================
 * STATUS
 * ============================================================ */

int pcspeaker_is_available(void)
{
    /*
     * PC Speaker has no standard PCI/device enumeration.
     *
     * From the OS perspective the I/O interface is available
     * on x86 machines/QEMU that emulate the legacy speaker.
     */
    return speaker_initialized;
}