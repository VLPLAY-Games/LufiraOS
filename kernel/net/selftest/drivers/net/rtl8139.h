#pragma once
/* Заглушка kernel/drivers/net/rtl8139.h для хостовой сборки selftest. */
#include "lib/types.h"
int rtl8139_found(void);
void rtl8139_poll(void);
