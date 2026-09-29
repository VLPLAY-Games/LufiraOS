#include "input.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/console/console.h"
#include "shell/shell.h"
#include "system/timer/pit.h"
#include "system/process/process.h"

static int mouse_x = 0;
static int mouse_y = 0;
static uint8_t mouse_buttons = 0;

// ===== Кольцевой буфер console-ввода (см. комментарий у console_input_read()
// в input.h) — v0.7 план, этап 5, под-этап 6. =====
#define CONSOLE_INPUT_BUF_SIZE 256
static volatile uint8_t console_input_buf[CONSOLE_INPUT_BUF_SIZE];
static volatile int console_input_head = 0;
static volatile int console_input_count = 0;
// Процесс, заблокированный внутри console_input_read() в ожидании байта —
// не список/очередь, а один слот: /dev/console сегодня читает не более
// одного процесса одновременно (будущий shell.elf), как и pipe_t в этом
// ядре тоже не поддерживает нескольких блокированных читателей разом.
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

// QEMU доставляет один и тот же keystroke сразу на PS/2 (IRQ1) и на USB HID
// (опрашивается из timer_irq_handler(), с задержкой до ~10мс) — без
// фильтрации один Enter превращался в два вызова shell_handle_enter()
// подряд, а второй мог стартовать прямо изнутри таймерного прерывания и
// портить память ядра (pmm_alloc_page() не рассчитан на реентерабельный
// вызов). Игнорируем повтор того же key, если он пришёл слишком быстро —
// но окно должно быть КОРОЧЕ реального автоповтора (иначе удержание клавиши
// снова перестанет работать, см. usb_hid.c: HID_REPEAT_RATE_TICKS=5 и
// типичный аппаратный PS/2 typematic, оба заведомо медленнее одного тика).
// Настоящее дублирование одного физического нажатия приходит с разницей в
// доли мс — микросекунды, а не десятки миллисекунд, так что одного тика с
// запасом хватает, чтобы поймать дубликат и не задеть законный повтор.
#define KEY_DEBOUNCE_TICKS 1

static int last_key = 0;
static uint64_t last_key_tick = 0;

// Перенесено из keyboard.c без изменений — единственное отличие в том, что
// теперь этот путь общий для PS/2 и (позже) USB HID клавиатуры.
void input_keyboard_event(int key) {
    if (key == 0) return;

    // Дедупликация PS/2+USB HID — см. комментарий у KEY_DEBOUNCE_TICKS
    // выше. Поднято ДО foreground_pid-гейта ниже (раньше было после), чтобы
    // относиться одинаково к обоим потребителям этого события — и старому
    // kernel-native шеллу (switch ниже), и новому console_input_push()
    // (следующий блок): дубликат не должен просачиваться ни в тот, ни в
    // другой. Видимое поведение старого шелла не меняется — тот всё равно
    // получает событие только после гейта, как и раньше.
    uint64_t now = pit_get_ticks();
    int is_duplicate = (key == last_key && (now - last_key_tick) < KEY_DEBOUNCE_TICKS);
    last_key = key;
    last_key_tick = now;
    if (is_duplicate) return;

    // Параллельный путь в кольцевой буфер /dev/console (см. input.h) — для
    // будущего shell.elf (v0.7 план, этап 5, под-этап 6), независимо от
    // foreground_pid ниже: та переменная — особенность СЕГОДНЯШНЕГО
    // kernel-native шелла (run/exec блокируют его собственный цикл), не
    // имеет смысла для процесса, который блокируется на настоящем
    // console_read(). Ctrl+Up/Down (скролл вьюпорта) и KEY_CTRL_C
    // сознательно НЕ попадают в этот поток — первое просто разметка
    // экрана, не часть строки ввода; второе станет настоящим SIGINT
    // (следующий под-этап), не байтом.
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

    // Пока на переднем плане реально исполняется run/exec (foreground_pid —
    // см. process.h/elf.c), шелл не должен ни печатать, ни отдавать команды
    // на выполнение: раньше ввод продолжал накапливаться и Enter взводил
    // shell_command_pending даже в это время, а выполнялся он лишь позже,
    // когда shell_task() снова окажется в своём цикле (после завершения
    // foreground-процесса) — то есть команда стартовала не тогда и не в
    // том состоянии, в котором её набирали, и внешне выглядело так, будто
    // "шелл работает параллельно с exec", хотя на самом деле просто
    // отложенно доигрывал устаревший ввод. Ctrl+C — единственное исключение:
    // это и есть штатный способ прервать foreground-процесс.
    if (foreground_pid != 0 && key != KEY_CTRL_C) return;

    switch (key) {
        case KEY_LEFT_ARROW:
            if (console_is_scrolled())
                console_scroll_to_bottom();

            shell_handle_left_arrow();
            return;

        case KEY_RIGHT_ARROW:
            if (console_is_scrolled())
                console_scroll_to_bottom();

            shell_handle_right_arrow();
            return;

        case KEY_UP_ARROW:
            if (keyboard_ctrl_pressed()) {
                console_scroll_up();
                return;
            }

            if (console_is_scrolled())
                console_scroll_to_bottom();

            shell_handle_up_arrow();
            return;

        case KEY_DOWN_ARROW:
            if (keyboard_ctrl_pressed()) {
                console_scroll_down();
                return;
            }

            if (console_is_scrolled())
                console_scroll_to_bottom();

            shell_handle_down_arrow();
            return;
        case '\t':  // Tab!
            shell_handle_tab();
            return;

        case KEY_CTRL_C:
            if (console_is_scrolled())
                console_scroll_to_bottom();

            // Не вызываем shell_handle_ctrl_c() отсюда напрямую — см.
            // подробный комментарий у shell_ctrl_c_pending в shell.h.
            shell_ctrl_c_pending = 1;
            return;
    }

    if (key == '\n') {
        shell_handle_enter();
        return;
    }
    if (key == '\b') {
        shell_handle_backspace();
        return;
    }

    shell_handle_char(key);
}

void input_mouse_event(int dx, int dy, uint8_t buttons) {
    mouse_x += dx;
    mouse_y += dy;

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;

    mouse_buttons = buttons;
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
