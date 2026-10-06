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

void console_input_push(uint8_t byte) {
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
    // выше: дубликат не должен просачиваться ни в ring buffer ниже, ни в
    // Ctrl+C/scroll-обработку в конце функции.
    uint64_t now = pit_get_ticks();
    int is_duplicate = (key == last_key && (now - last_key_tick) < KEY_DEBOUNCE_TICKS);
    last_key = key;
    last_key_tick = now;
    if (is_duplicate) return;

    // v0.8 (GUI+WM), этап 3: пока зарегистрирован WM-процесс (SYS_WM_REGISTER,
    // process_get_wm_pid()) - клавиатура идёт ЕМУ (сырым событием через
    // mailbox.h), а не в обычный ring buffer /dev/console: текстовый шелл в
    // это время не должен получать ввод вовсе (как и в любой настоящей
    // оконной системе, фоновый терминал не видит клавиш, пока открыто
    // GUI-окно). Раньше (первый срез) эту развилку делал gui_handle_key()
    // прямо внутри ядра - теперь сам WM решает, какому окну это отдать
    // (фокус - понятие, которое ядро больше не обязано знать).
    uint32_t wm_pid = process_get_wm_pid();
    if (wm_pid) {
        wm_request_t req;
        memset(&req, 0, sizeof(req));
        req.opcode = WM_INPUT_KEY;
        req.a[0] = key;
        mailbox_send(wm_pid, WM_SENDER_KERNEL, &req, sizeof(req));
        return;
    }

    // Кольцевой буфер /dev/console (см. input.h) — то, что реально читает
    // shell.elf через SYS_READ. Ctrl+Up/Down (скролл вьюпорта) и KEY_CTRL_C
    // сознательно НЕ попадают в этот поток — первое просто разметка
    // экрана, не часть строки ввода; второе доставляется как настоящий
    // SIGINT через shell_ctrl_c_pending (см. конец функции), не байтом.
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

    // НАЙДЕННЫЙ БАГ (v0.7 план, этап 5, под-этап 6 продолжение — репорт
    // пользователя: "буква дублируется на экране, но не дублируется по
    // логике"): здесь раньше стоял ПОЛНЫЙ диспетчер к shell_handle_*()
    // (kernel/shell/shell.c) — мёртвому кернел-native шеллу. shell_task()
    // удалена, shell_run_pending_command() нигде не вызывается, но ЭТОТ
    // диспетчер, в отличие от них, вызывался ПРЯМО ОТСЮДА, на каждое
    // нажатие клавиши, независимо от shell_task(). shell_handle_char()/
    // shell_handle_backspace()/shell_refresh_input_line() и т.д. рисуют
    // ПРЯМО на экране через put_char_graphic() по СВОИМ собственным
    // command_start_x/command_start_y/current_line/cursor_position_in_line
    // (статические переменные в shell.c) — координаты, выставленные один
    // раз (или никогда не выставленные, оставшиеся 0) и никогда не
    // синхронизированные с current_x/current_y настоящей консоли, которые
    // использует новый shell.elf через SYS_WRITE на /dev/console. Итог:
    // каждая набранная буква рисовалась ДВАЖДЫ — один раз правильно (через
    // ring buffer -> shell.elf -> sys_write -> console_write -> put_char,
    // на настоящей текущей позиции курсора) и один раз призрачно (через
    // этот мёртвый путь, всегда в одном и том же старом месте у верха
    // экрана, что и описал пользователь скриншотом) — НЕ дублируясь в
    // логике, потому что мёртвый путь никогда не трогает line[]/len
    // userspace-шелла, только пиксели.
    //
    // foreground_pid-гейт (раньше "if (foreground_pid != 0 && key !=
    // KEY_CTRL_C) return;") тоже был специфичен для старого шелла
    // (запрещал ему рисовать/исполнять, пока run/exec уже исполняется на
    // переднем плане) — новому shell.elf он не нужен: пока его дочерний
    // процесс foreground, сам shell.elf блокирован в SYS_WAIT и не читает
    // /dev/console вовсе, значит ввод просто копится в ring buffer до его
    // следующего SYS_READ, как и положено.
    //
    // Что осталось легитимным и перенесено ниже без изменений:
    //  - Ctrl+Up/Down — скролл вьюпорта истории консоли (console_scroll_*),
    //    отдельная от шелла функция, не исполнение команд.
    //  - Ctrl+C — взводит shell_ctrl_c_pending для реального вызова
    //    shell_handle_ctrl_c() из timer_irq_handler() (см. подробный
    //    комментарий у shell_ctrl_c_pending в shell.h) — ЭТОТ механизм
    //    живой: именно на нём держится SYS_SET_FOREGROUND/Ctrl+C для
    //    shell.elf (v0.7 план, этап 5, под-этап 6, фаза 2).
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

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;

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
