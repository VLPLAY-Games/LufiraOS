#include "mouse.h"
#include "lib/types.h"
#include "drivers/input/input.h"

#define PS2_DATA_PORT           0x60
#define PS2_STATUS_PORT         0x64
#define PS2_COMMAND_PORT        0x64

#define PS2_CMD_READ_CONFIG     0x20
#define PS2_CMD_WRITE_CONFIG    0x60
#define PS2_CMD_DISABLE_AUX     0xA7
#define PS2_CMD_ENABLE_AUX      0xA8
#define PS2_CMD_DISABLE_KBD     0xAD
#define PS2_CMD_ENABLE_KBD      0xAE
#define PS2_CMD_WRITE_MOUSE     0xD4

#define MOUSE_CMD_RESET         0xFF
#define MOUSE_CMD_ENABLE        0xF4
#define MOUSE_CMD_DISABLE       0xF5
#define MOUSE_CMD_SET_DEFAULTS  0xF6
#define MOUSE_CMD_SET_SAMPLE    0xF3

#define MOUSE_ACK               0xFA
#define MOUSE_RESET_OK          0xAA

// НАЙДЕННЫЙ БАГ (причина зависания всей системы на старте): все функции
// ниже, ждущие байт от PS/2-контроллера/мыши, были безлимитными busy-wait
// циклами. На машине без физической PS/2-мыши (обычный случай для
// usb-только железа и части VM-конфигураций) AUX-порт никогда не присылает
// ни одного байта — mouse_init() вызывается ДО sti/xhci_init() (см.
// kernel.c), так что это происходило раньше, чем система успевала поднять
// хоть что-то ещё, включая сам USB/xHCI, который и предоставил бы реальную
// мышь. Итог — полное зависание на отсутствии необязательного устройства.
// Фикс: тот же паттерн ограниченного таймаута, что уже был в
// keyboard_init() (kernel/drivers/keyboard/keyboard.c) — ждём байт не
// бесконечно, а конечное число итераций, и если устройства нет —
// mouse_init() корректно завершается с mouse_initialized=0, как и раньше
// для остальных ошибок протокола.
#define PS2_TIMEOUT_ITERS 200000

static int mouse_initialized = 0;

static uint8_t packet[3];
static int packet_index = 0;

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// Таймаут и тут — иначе залипший контроллер (бит "input full" не падает)
// вешает даже запись команды, не только чтение ответа.
static void ps2_wait_write(void) {
    int timeout = PS2_TIMEOUT_ITERS;
    while (--timeout && (inb(PS2_STATUS_PORT) & 0x02)) {
    }
}

static void ps2_flush_output_buffer(void) {
    while (inb(PS2_STATUS_PORT) & 0x01) {
        (void)inb(PS2_DATA_PORT);
    }
}

// Возвращает 0 и байт в *out при успехе, -1 по таймауту (контроллер не
// отвечает вообще — большинство "чисто USB" машин).
static int ps2_read_controller_data(uint8_t *out) {
    int timeout = PS2_TIMEOUT_ITERS;
    while (--timeout) {
        if (inb(PS2_STATUS_PORT) & 0x01) {
            *out = inb(PS2_DATA_PORT);
            return 0;
        }
    }
    return -1;
}

// Как ps2_read_controller_data(), но отбрасывает байты НЕ от AUX-порта
// (бит 5) — например, залипший байт клавиатуры — и ждёт AUX-байт в рамках
// ОДНОГО общего таймаута, а не отдельного на каждую попытку (иначе шум от
// клавиатуры мог бы бесконечно продлевать ожидание).
static int ps2_read_mouse_data(uint8_t *out) {
    int timeout = PS2_TIMEOUT_ITERS;
    while (--timeout) {
        if (!(inb(PS2_STATUS_PORT) & 0x01)) continue;

        uint8_t status = inb(PS2_STATUS_PORT);
        uint8_t data = inb(PS2_DATA_PORT);

        if (status & 0x20) {
            *out = data;
            return 0;
        }
    }
    return -1;
}

static void ps2_write_command(uint8_t cmd) {
    ps2_wait_write();
    outb(PS2_COMMAND_PORT, cmd);
}

static void ps2_write_data(uint8_t data) {
    ps2_wait_write();
    outb(PS2_DATA_PORT, data);
}

static int mouse_send_command(uint8_t cmd) {
    ps2_write_command(PS2_CMD_WRITE_MOUSE);
    ps2_write_data(cmd);

    uint8_t ack;
    if (ps2_read_mouse_data(&ack) != 0) return -1;
    return (ack == MOUSE_ACK) ? 0 : -1;
}

void mouse_init(void) {
    // Все отладочные printf убраны - статус выводится в kernel.c
    mouse_initialized = 0;
    packet_index = 0;

    ps2_flush_output_buffer();

    ps2_write_command(PS2_CMD_DISABLE_AUX);
    ps2_write_command(PS2_CMD_DISABLE_KBD);

    ps2_write_command(PS2_CMD_READ_CONFIG);
    uint8_t config;
    if (ps2_read_controller_data(&config) != 0) {
        return; // нет PS/2-контроллера вообще (или не отвечает) - не виснем
    }

    config |= (1 << 1);   // IRQ12
    config |= (1 << 0);   // IRQ1
    config &= ~(1 << 5);  // enable mouse clock

    ps2_write_command(PS2_CMD_WRITE_CONFIG);
    ps2_write_data(config);

    ps2_write_command(PS2_CMD_ENABLE_AUX);
    ps2_write_command(PS2_CMD_ENABLE_KBD);

    ps2_flush_output_buffer();

    ps2_write_command(PS2_CMD_WRITE_MOUSE);
    ps2_write_data(MOUSE_CMD_RESET);

    uint8_t ack;
    if (ps2_read_mouse_data(&ack) != 0 || ack != MOUSE_ACK) {
        return; // AUX-порт есть, но мышь не подключена/не отвечает
    }

    uint8_t reset_ok;
    if (ps2_read_mouse_data(&reset_ok) != 0 || reset_ok != MOUSE_RESET_OK) {
        return;
    }

    // ID байт всё равно нужно прочитать (таймаут тут не критичен -
    // дальнейшие mouse_send_command() всё равно сами защищены).
    uint8_t id_byte;
    (void)ps2_read_mouse_data(&id_byte);

    if (mouse_send_command(MOUSE_CMD_SET_DEFAULTS) != 0) {
        return;
    }

    if (mouse_send_command(MOUSE_CMD_SET_SAMPLE) != 0) {
        return;
    }

    if (mouse_send_command(100) != 0) {
        return;
    }

    if (mouse_send_command(MOUSE_CMD_ENABLE) != 0) {
        return;
    }

    mouse_initialized = 1;
}

void mouse_irq_handler(void) {
    if (!mouse_initialized)
        return;

    uint8_t status = inb(PS2_STATUS_PORT);
    if (!(status & 0x20))
        return;

    uint8_t data = inb(PS2_DATA_PORT);

    if (packet_index == 0 && !(data & 0x08))
        return;

    packet[packet_index++] = data;

    if (packet_index < 3)
        return;

    packet_index = 0;

    if (packet[0] & 0xC0)
        return;

    int dx = packet[1];
    int dy = packet[2];

    if (packet[0] & 0x10) dx |= 0xFFFFFF00;
    if (packet[0] & 0x20) dy |= 0xFFFFFF00;

    // PS/2 репортует dy с положительным направлением вверх, а
    // input_mouse_event() (как и экранные координаты) считает вниз
    // положительным — отсюда инверсия знака.
    input_mouse_event(dx, -dy, packet[0] & 0x07);
}

int mouse_is_initialized(void) {
    return mouse_initialized;
}
