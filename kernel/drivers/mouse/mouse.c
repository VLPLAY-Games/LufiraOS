#include "mouse.h"
#include <stdint.h>

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

static int mouse_initialized = 0;
static int mouse_x = 0;
static int mouse_y = 0;
static uint8_t mouse_buttons = 0;

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

static void ps2_wait_write(void) {
    while (inb(PS2_STATUS_PORT) & 0x02) {
    }
}

static void ps2_flush_output_buffer(void) {
    while (inb(PS2_STATUS_PORT) & 0x01) {
        (void)inb(PS2_DATA_PORT);
    }
}

static uint8_t ps2_read_controller_data(void) {
    while (!(inb(PS2_STATUS_PORT) & 0x01)) {
    }
    return inb(PS2_DATA_PORT);
}

static uint8_t ps2_read_mouse_data(void) {
    for (;;) {
        while (!(inb(PS2_STATUS_PORT) & 0x01)) {
        }

        uint8_t status = inb(PS2_STATUS_PORT);
        uint8_t data = inb(PS2_DATA_PORT);

        if (status & 0x20) {
            return data;
        }
    }
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

    uint8_t ack = ps2_read_mouse_data();
    return (ack == MOUSE_ACK) ? 0 : -1;
}

void mouse_init(void) {
    // Все отладочные printf убраны - статус выводится в kernel.c
    mouse_initialized = 0;
    packet_index = 0;
    mouse_x = 0;
    mouse_y = 0;
    mouse_buttons = 0;

    ps2_flush_output_buffer();

    ps2_write_command(PS2_CMD_DISABLE_AUX);
    ps2_write_command(PS2_CMD_DISABLE_KBD);

    ps2_write_command(PS2_CMD_READ_CONFIG);
    uint8_t config = ps2_read_controller_data();

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

    uint8_t ack = ps2_read_mouse_data();
    if (ack != MOUSE_ACK) {
        return;
    }

    uint8_t reset_ok = ps2_read_mouse_data();
    if (reset_ok != MOUSE_RESET_OK) {
        return;
    }

    // ID байт всё равно нужно прочитать
    uint8_t id_byte = ps2_read_mouse_data();
    (void)id_byte;

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

    mouse_x += dx;
    mouse_y -= dy;

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;

    mouse_buttons = packet[0] & 0x07;
}

int mouse_is_initialized(void) {
    return mouse_initialized;
}
