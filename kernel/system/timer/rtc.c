// rtc.c — CMOS Real Time Clock (порты 0x70/0x71), опросный доступ, без
// прерывания RTC (IRQ8) — тот же принцип "без прерываний для устройств
// кроме уже существующего таймера PIT", что и у остального ввода-вывода
// в этом ядре. Источник: CMOS-регистры 0x00-0x09 (секунды..год) + 0x0A/0x0B
// (статус A/B) — классический набор, описанный в OSDev wiki "CMOS".
#include "rtc.h"
#include "system/acpi/acpi.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define CMOS_REG_SECONDS    0x00
#define CMOS_REG_MINUTES    0x02
#define CMOS_REG_HOURS      0x04
#define CMOS_REG_DAY        0x07
#define CMOS_REG_MONTH      0x08
#define CMOS_REG_YEAR       0x09
#define CMOS_REG_STATUS_A   0x0A
#define CMOS_REG_STATUS_B   0x0B

#define STATUS_A_UPDATE_IN_PROGRESS 0x80
#define STATUS_B_BINARY_MODE   0x04 // 1 = значения уже двоичные, 0 = BCD
#define STATUS_B_24H_MODE      0x02 // 1 = 24-часовой формат, 0 = 12-часовой

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    outb(CMOS_ADDR, CMOS_REG_STATUS_A);
    return inb(CMOS_DATA) & STATUS_A_UPDATE_IN_PROGRESS;
}

typedef struct {
    uint8_t second, minute, hour, day, month, year, century;
} rtc_raw_t;

static void read_raw(rtc_raw_t *out, uint8_t century_reg) {
    out->second  = cmos_read(CMOS_REG_SECONDS);
    out->minute  = cmos_read(CMOS_REG_MINUTES);
    out->hour    = cmos_read(CMOS_REG_HOURS);
    out->day     = cmos_read(CMOS_REG_DAY);
    out->month   = cmos_read(CMOS_REG_MONTH);
    out->year    = cmos_read(CMOS_REG_YEAR);
    out->century = century_reg ? cmos_read(century_reg) : 0;
}

static int raw_equal(const rtc_raw_t *a, const rtc_raw_t *b) {
    return a->second == b->second && a->minute == b->minute && a->hour == b->hour &&
           a->day == b->day && a->month == b->month && a->year == b->year &&
           a->century == b->century;
}

#define BCD2BIN(x) ((uint8_t)(((x) & 0x0F) + (((x) >> 4) * 10)))

// Отдельные бюджеты для двух разных циклов ожидания: UIP снимается очень
// быстро (апдейт RTC длится заметно меньше миллисекунды), но это чистый
// опрос порта на полной скорости CPU — щедрый бюджет ничего не стоит.
// Внешний цикл "прочитали дважды, сравнили" — совсем другое: ему
// достаточно единиц попыток (несовпадение означает, что мы просто
// попали на границу обновления, что бывает редко).
#define UIP_WAIT_TRIES    1000000
#define COMPARE_TRIES     16

int rtc_get_time(rtc_time_t *out) {
    uint8_t century_reg = acpi_get_rtc_century_register();

    rtc_raw_t prev, cur;

    // UIP (Update In Progress) — RTC может менять регистры прямо во время
    // нашего чтения; ждём, пока он не снимется, перед ПЕРВЫМ чтением.
    for (int i = 0; update_in_progress() && i < UIP_WAIT_TRIES; i++);
    read_raw(&prev, century_reg);

    // Затем читаем ещё раз и сравниваем — если регистры совпали, апдейт
    // точно не случился ровно посреди чтения (OSDev-рекомендованный приём,
    // надёжнее одной только проверки UIP, которая покрывает не весь риск
    // гонки).
    int matched = 0;
    for (int tries = 0; tries < COMPARE_TRIES; tries++) {
        for (int i = 0; update_in_progress() && i < UIP_WAIT_TRIES; i++);
        read_raw(&cur, century_reg);
        if (raw_equal(&prev, &cur)) { matched = 1; break; }
        prev = cur;
    }
    if (!matched) return -1;

    uint8_t status_b = cmos_read(CMOS_REG_STATUS_B);

    uint8_t second = cur.second, minute = cur.minute, day = cur.day,
            month = cur.month, year = cur.year, century = cur.century;
    uint8_t hour = cur.hour;
    uint8_t pm = hour & 0x80; // PM-флаг (12-часовой режим) — ДО BCD-конверсии,
                              // бит 7 не входит ни в одну BCD-цифру часов.
    hour &= 0x7F;

    if (!(status_b & STATUS_B_BINARY_MODE)) {
        second = BCD2BIN(second);
        minute = BCD2BIN(minute);
        hour   = BCD2BIN(hour);
        day    = BCD2BIN(day);
        month  = BCD2BIN(month);
        year   = BCD2BIN(year);
        if (century_reg) century = BCD2BIN(century);
    }

    if (!(status_b & STATUS_B_24H_MODE)) {
        // 12-часовой формат (1-12, час "12" — особый случай в обе стороны).
        if (pm) {
            if (hour != 12) hour = (uint8_t)(hour + 12);
        } else {
            if (hour == 12) hour = 0;
        }
    }

    out->second = second;
    out->minute = minute;
    out->hour   = hour;
    out->day    = day;
    out->month  = month;
    // FADT.CENTURY == 0 означает "платформа не описывает регистр века"
    // (ACPI spec) — считаем все двузначные годы 20xx: разумное допущение
    // для ОС, у которой нет причин иметь дело с датами до 2000 года.
    out->year   = century_reg ? (uint16_t)(century * 100 + year) : (uint16_t)(2000 + year);

    return 0;
}
