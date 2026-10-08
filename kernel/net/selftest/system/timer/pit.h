#pragma once
/* Заглушка kernel/system/timer/pit.h для хостовой сборки selftest:
   "тики" идут от настоящих часов хоста, те же 10мс на тик. */
#include "lib/types.h"
uint64_t pit_get_ticks(void);
void pit_wait_ms(uint32_t ms);
