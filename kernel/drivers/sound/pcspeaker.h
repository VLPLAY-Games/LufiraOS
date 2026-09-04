#pragma once

#include <stdint.h>

#define PCSPEAKER_MIN_FREQUENCY 20
#define PCSPEAKER_MAX_FREQUENCY 20000

void pcspeaker_init(void);

void pcspeaker_start(uint32_t frequency);
void pcspeaker_stop(void);

void pcspeaker_set_gate(int enabled);
int  pcspeaker_get_gate(void);

uint8_t pcspeaker_get_volume(void);
void pcspeaker_set_volume(uint8_t volume);

void pcspeaker_tone(uint32_t frequency, uint32_t duration_ms);

int pcspeaker_is_available(void);