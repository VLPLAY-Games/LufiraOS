#pragma once

#include "lib/types.h"

void mouse_init(void);
void mouse_irq_handler(void);
int mouse_is_initialized(void);
