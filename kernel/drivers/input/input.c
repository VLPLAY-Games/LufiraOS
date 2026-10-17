#include "input.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/console/console.h"
#include "shell/shell.h"
#include "system/timer/pit.h"
#include "system/process/process.h"
#include "system/ipc/mailbox.h"
#include "system/ipc/wm_protocol.h"
#include "lib/string.h"

static int mouse_x = 0;
static int mouse_y = 0;
static uint8_t mouse_buttons = 0;

// ===== Кольцевой буфер console-ввода (см. console_input_read() в input.h) =====
#define CONSOLE_INPUT_BUF_SIZE 256
static volatile uint8_t console_input_buf[CONSOLE_INPUT_BUF_SIZE];
static volatile int console_input_head = 0;
static volatile int console_input_count = 0;
// Процесс, ждущий байт в console_input_read() — один слот, не очередь:
// /dev/console читает не более одного процесса одновременно, как и pipe_t.
static process_t *console_input_waiter = NULL;

static void console_input_push(uint8_t byte) {
    if (console_input_count >= CONSOLE_INPUT_BUF_SIZE) return; // переполнение — молча роняем, как и реальный tty
    console_input_buf[console_input_head] = byte;
    console_input_head = (console_input_head + 1) % CONSOLE_INPUT_BUF_SIZE;
    console_input_count++;

    if (console_input_waiter) {
        console_input_waiter->state = PROCESS_READY;
        console_input_waiter = NULL;
    }
}

int console_input_has_data(void) {
    return console_input_count > 0;
}

int console_input_read(uint8_t *out, int max) {
    if (max <= 0) return 0;

    while (console_input_count == 0) {
        console_input_waiter = current_process;
        current_process->state = PROCESS_BLOCKED;
        schedule();
    }

    int start = (console_input_head - console_input_count + CONSOLE_INPUT_BUF_SIZE) % CONSOLE_INPUT_BUF_SIZE;
    int n = 0;
    while (n < max && console_input_count > 0) {
        out[n] = console_input_buf[(start + n) % CONSOLE_INPUT_BUF_SIZE];
        n++;
        console_input_count--;
    }
    return n;
}

// QEMU доставляет один keystroke сразу на PS/2 (IRQ1) и на USB HID (опрос
// из timer_irq_handler(), задержка ~10мс) — без фильтрации Enter даёт два
// вызова shell_handle_enter(), второй иногда стартует прямо из таймерного
// IRQ и портит память ядра (pmm_alloc_page() не реентерабелен). Окно
// дебаунса короче реального автоповтора (HID_REPEAT_RATE_TICKS=5 в
// usb_hid.c, typematic PS/2) — настоящий дубликат приходит с разницей в
// микросекунды, одного тика достаточно, чтобы не задеть законный повтор.
#define KEY_DEBOUNCE_TICKS 1

static int last_key = 0;
static int last_pressed = -1; // -1 = ещё не было ни одного события
static uint64_t last_key_tick = 0;

// Общий путь для PS/2 и USB HID клавиатуры.
void input_keyboard_event(int key) {
    input_keyboard_event_ex(key, 1);
}

// Играм (порт doomgeneric) нужно знать не только "нажата", но и "отпущена"
// (держать "идём вперёд", пока WASD зажаты). Прокидываем оба случая, но
// только WM/GUI-клиентам — обычный консольный путь получает исключительно
// нажатия: return сразу после проверки WM, чтобы не плодить лишние
// символы в текстовом вводе.
void input_keyboard_event_ex(int key, int pressed) {
    if (key == 0) return;

    // Дедупликация PS/2+USB HID (см. KEY_DEBOUNCE_TICKS выше) — дубликат не
    // должен просочиться ни в ring buffer, ни в Ctrl+C/scroll ниже. pressed
    // входит в сигнатуру дубликата, иначе быстрый тап (нажал-отпустил в
    // одном тике) схлопнулся бы в одно событие.
    uint64_t now = pit_get_ticks();
    int is_duplicate = (key == last_key && pressed == last_pressed &&
                         (now - last_key_tick) < KEY_DEBOUNCE_TICKS);
    last_key = key;
    last_pressed = pressed;
    last_key_tick = now;
    if (is_duplicate) return;

    // Пока зарегистрирован WM-процесс (process_get_wm_pid()) — клавиатура
    // идёт ему сырым событием через mailbox, а не в ring buffer /dev/console:
    // текстовый шелл не должен получать ввод, пока открыто GUI-окно (как и
    // в любой оконной системе фоновый терминал не видит клавиш). Какому
    // окну отдать фокус — решает сам WM, ядро этого не знает.
    uint32_t wm_pid = process_get_wm_pid();
    if (wm_pid) {
        wm_request_t req;
        memset(&req, 0, sizeof(req));
        req.opcode = WM_INPUT_KEY;
        req.a[0] = key;
        req.a[1] = pressed;
        mailbox_send(wm_pid, WM_SENDER_KERNEL, &req, sizeof(req));
        return;
    }

    // Нет WM — обычный текстовый /dev/console. Отпускания клавиш не нужны
    // (строка ввода не отличает "держат" от "нажали один раз"), только нажатия.
    if (!pressed) return;

    // Кольцевой буфер /dev/console (см. input.h) — то, что читает shell.elf
    // через SYS_READ. Ctrl+Up/Down (скролл вьюпорта) и KEY_CTRL_C не
    // попадают в этот поток: первое не часть строки ввода, второе
    // доставляется как SIGINT через shell_ctrl_c_pending (конец функции).
    switch (key) {
        case KEY_LEFT_ARROW:
        case KEY_RIGHT_ARROW:
            console_input_push((uint8_t)key);
            break;
        case KEY_UP_ARROW:
        case KEY_DOWN_ARROW:
            if (!keyboard_ctrl_pressed()) console_input_push((uint8_t)key);
            break;
        case KEY_CTRL_C:
            break;
        default:
            console_input_push((uint8_t)key);
            break;
    }

    // Ctrl+Up/Down — скролл вьюпорта истории консоли, не исполнение команд.
    // Ctrl+C взводит shell_ctrl_c_pending, обрабатывается в
    // timer_irq_handler() (см. shell_ctrl_c_pending в shell.h).
    if (key == KEY_UP_ARROW && keyboard_ctrl_pressed()) {
        console_scroll_up();
        return;
    }
    if (key == KEY_DOWN_ARROW && keyboard_ctrl_pressed()) {
        console_scroll_down();
        return;
    }
    if (key == KEY_CTRL_C) {
        if (console_is_scrolled())
            console_scroll_to_bottom();

        shell_ctrl_c_pending = 1;
        return;
    }
}

void input_mouse_event(int dx, int dy, uint8_t buttons) {
    mouse_x += dx;
    mouse_y += dy;

    // Зажимаем курсор по разрешению экрана (screen_width/height_pixels,
    // console.h) — raw dx/dy с PS/2/USB не ограничены сверху. -1, чтобы
    // курсор не висел вплотную за последним валидным пикселем.
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (screen_width_pixels && mouse_x >= (int)screen_width_pixels) mouse_x = (int)screen_width_pixels - 1;
    if (screen_height_pixels && mouse_y >= (int)screen_height_pixels) mouse_y = (int)screen_height_pixels - 1;

    mouse_buttons = buttons;

    // v0.8 (GUI+WM), этап 3: раньше WM (gui_tick(), кернел-резидентный
    // gui.c) САМ опрашивал input_mouse_get_x/y/buttons() каждый PIT-тик -
    // теперь, когда композитинг и вся эта логика в userspace, у WM нет
    // способа "подождать тика" (нет больше PIT-driven gui_tick()) - вместо
    // опроса толкаем событие ему сразу отсюда, с места настоящего
    // аппаратного прерывания/опроса устройства (PS/2 IRQ или USB HID poll
    // из timer_irq_handler(), см. их вызывающих). WM получает это тем же
    // блокирующим SYS_IPC_RECV, что и клавиатуру/клиентские RPC - никакого
    // отдельного опроса мыши ему теперь не нужно вовсе.
    uint32_t wm_pid = process_get_wm_pid();
    if (wm_pid) {
        wm_request_t req;
        memset(&req, 0, sizeof(req));
        req.opcode = WM_INPUT_MOUSE;
        req.a[0] = mouse_x;
        req.a[1] = mouse_y;
        req.a[2] = (int32_t)mouse_buttons;
        mailbox_send(wm_pid, WM_SENDER_KERNEL, &req, sizeof(req));
    }
}

int input_mouse_get_x(void) {
    return mouse_x;
}

int input_mouse_get_y(void) {
    return mouse_y;
}

uint8_t input_mouse_get_buttons(void) {
    return mouse_buttons;
}
