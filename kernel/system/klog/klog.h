#pragma once

// Короткие журнальные записи в /logs/system.log — используются вместо
// подробного консольного вывода, когда режим разработчика выключен (см.
// devmode.h), чтобы события всё равно оставались на диске.

void klog_init(void);              // создать /logs/system.log, если его ещё нет — после lufirafs_init()
void klog(const char *format, ...);
