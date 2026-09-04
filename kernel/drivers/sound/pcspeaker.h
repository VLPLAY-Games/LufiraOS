#ifndef LUFIRAOS_PCSPEAKER_H
#define LUFIRAOS_PCSPEAKER_H

#include <stdint.h>

/*
 * PC Speaker driver.
 *
 * Uses:
 *   PIT channel 2
 *   I/O port 0x42 - PIT channel 2 data
 *   I/O port 0x61 - PC speaker control
 */

void pcspeaker_init(void);

void pcspeaker_start(uint32_t frequency);
void pcspeaker_stop(void);

void pcspeaker_beep(uint32_t frequency, uint32_t duration_ms);

int pcspeaker_is_available(void);

#endif