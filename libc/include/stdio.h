// stdio.h — только printf(). Никаких файловых потоков/scanf в этой версии
// (см. план v0.4 Phase 4: "простой printf", не полноценный stdio).
#pragma once

int printf(const char *fmt, ...);
