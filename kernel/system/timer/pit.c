#include "pit.h"
#include "drivers/console/console.h"
#include "system/process/process.h"
#include "drivers/usb/xhci.h"
#include "net/net.h"
#include "system/devmode/devmode.h"
#include "shell/shell.h"

// Порты I/O
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Счётчик тиков
static volatile uint64_t pit_ticks = 0;

void pit_init(void) {
    uint16_t divisor = PIT_DIVISOR;
    
    outb(PIT_COMMAND, PIT_SELECT_CH0 | PIT_ACCESS_LOHI | PIT_MODE_SQUARE | PIT_BINARY_MODE);
    outb(PIT_CHANNEL_0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL_0, (uint8_t)((divisor >> 8) & 0xFF));
    
    DLOG("[PIT] Timer initialized at %u Hz (divisor %u)\n", PIT_FREQUENCY, divisor);
}

void pit_set_frequency(uint32_t hz) {
    if (hz == 0) return;
    
    uint32_t divisor = PIT_BASE_FREQUENCY / hz;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1) divisor = 1;
    
    outb(PIT_COMMAND, PIT_SELECT_CH0 | PIT_ACCESS_LOHI | PIT_MODE_SQUARE | PIT_BINARY_MODE);
    outb(PIT_CHANNEL_0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL_0, (uint8_t)((divisor >> 8) & 0xFF));
}

uint64_t pit_get_ticks(void) {
    return pit_ticks;
}

// Настоящая, откалиброванная по тикам PIT задержка (в отличие от
// разбросанных по драйверам циклов "for (volatile int i = 0; i < N; i++)"
// без привязки к реальному времени). Требует, чтобы прерывания уже были
// разрешены (sti + irq_enable(0)) — иначе pit_ticks никогда не вырастет.
void pit_wait_ms(uint32_t ms) {
    if (ms == 0) return;

    uint64_t target = pit_ticks + (ms + 9) / 10; // PIT_FREQUENCY = 100 Гц = 10 мс/тик

    while (pit_ticks < target) {
        asm volatile("hlt");
    }
}

// Квант времени на процесс перед вынужденным переключением (5 тиков по
// 10 мс = 50 мс). Вытесняем ТОЛЬКО когда прерванный код реально исполнялся
// в ring3 (frame->cs == 0x33) — то есть настоящий пользовательский код
// процесса, а не код ядра (обработчики syscall/IRQ, ещё не полностью
// реентерабельный путь клавиатура/USB-в-таймере из input.c, hlt-цикл
// idle). Это не срез функциональности: ядерный код и так короткий и сам
// уступает в нужных местах, а вытеснять его на произвольном месте — это
// именно тот риск, ради обхода которого выбран этот гейт.
#define PREEMPT_TIMESLICE_TICKS 5
static uint32_t preempt_countdown = PREEMPT_TIMESLICE_TICKS;

// Грубая статистика загрузки CPU для команды cpuload — каждый тик относим
// либо к простою (idle-процесс был текущим), либо к работе, плюс копим
// то же самое персонально на current_process->cpu_ticks (см. process.h).
static volatile uint64_t g_total_ticks = 0;
static volatile uint64_t g_idle_ticks = 0;

uint64_t pit_get_total_ticks(void) { return g_total_ticks; }
uint64_t pit_get_idle_ticks(void) { return g_idle_ticks; }

// Обработчик прерывания таймера
void timer_irq_handler(interrupt_frame_t *frame) {
    pit_ticks++;

    g_total_ticks++;
    if (process_is_idle()) g_idle_ticks++;
    if (current_process) current_process->cpu_ticks++;

    process_t *p = process_list;

    if (p) {
        process_t *start = p;

        do {
            if (p->state == PROCESS_SLEEPING &&
                pit_ticks >= p->wakeup_tick)
            {
                p->state = PROCESS_READY;
            }

            // v0.8-мост, пункт 6 (SYS_ALARM) — тот же скан, та же причина
            // смотреть на ВСЕ процессы, не только current_process: будильник
            // мог быть взведён на процесс, который сейчас не исполняется
            // вовсе. process_signal() сам разбирается, спит процесс,
            // заблокирован или уже готов — тот же путь, что у kill().
            if (p->alarm_tick && pit_ticks >= p->alarm_tick) {
                p->alarm_tick = 0;
                process_signal(p->pid, SIGALRM);
            }

            p = p->next;
        } while (p && p != start);
    }

    update_cursor();
    // v0.8 (GUI+WM), этап 3: композитинг переехал в userspace WM-процесс
    // (lufira-packages/src/apps/wm.c) — он сам пишет в back buffer через
    // SYS_FB_PRESENT (syscall.c), больше не отсюда. console_tick_present()
    // ниже всё ещё нужен безусловно: это он флашит back buffer (который
    // теперь обновляет WM, а не только текстовая консоль) на реальный
    // экран.
    console_tick_present(); // v0.8-мост, пункт 2: флашит back buffer, только если "грязный"

    usb_poll();
    net_poll();

    // Собирает зомби, которых никто никогда не process_wait()'ит (типичный
    // случай — runbg без последующего wait, или foreground run/exec: шелл
    // сам никогда не вызывает process_wait(), просто опрашивает
    // child->state — см. command_run() в filesystem.c). Раньше не было
    // вообще никакой точки вызова: process_reap() был объявлен и
    // документирован, но никогда не вызывался (единственный другой путь
    // реального удаления — process_wait()'s process_remove_from_list() —
    // отрабатывает только когда КТО-ТО ЯВНО ждёт, чего шелл не делает), и
    // page_table/fd каждого run/exec'нутого процесса утекали навсегда.
    // Планировщик (см. schedule() в process.c) намеренно почти никогда не
    // выбирает idle_process, если жив хоть один другой READY-процесс
    // (шелл жив всегда) — так что дожидаться реального переключения
    // управления на сам idle-цикл (как было сделано раньше, в kernel.c)
    // практически никогда не происходит на практике. process_reap() не
    // блокирует и не переключает контекст (в отличие от
    // shell_handle_ctrl_c() ниже) — безопасно звать прямо отсюда, как
    // usb_poll()/net_poll().
    process_reap();

    // Только ЗДЕСЬ, после того как usb_poll()/net_poll() уже полностью
    // отработали — см. подробный комментарий у shell_ctrl_c_pending в
    // shell.h: shell_handle_ctrl_c() может увести управление насовсем через
    // реальное переключение контекста (если убиваемый foreground-процесс —
    // это current_process, обычный случай), и звать её раньше, прямо
    // изнутри разбора USB HID-отчёта, означало бы бросить недовооружённым
    // endpoint клавиатуры навсегда.
    if (shell_ctrl_c_pending) {
        shell_ctrl_c_pending = 0;
        shell_handle_ctrl_c();
    }

    // idle_process никогда не заходит сюда: он всегда исполняется в ring0
    // (свой hlt-цикл, никогда не переходит в ring3), так что cs==0x33 уже
    // само по себе его исключает — отдельная проверка "!= idle_process" не
    // нужна.
    if (frame->cs == 0x33 && current_process) {
        if (--preempt_countdown == 0) {
            preempt_countdown = PREEMPT_TIMESLICE_TICKS;
            // EOI на этот IRQ уже отправлен централизованно в самом начале
            // irq_handler() (kernel/system/cpu/irq.c) — schedule() отсюда
            // может не вернуться очень долго (или вообще, пока не вернут
            // управление сюда снова уже ДРУГИМ таймерным тиком), так что
            // делать это позже здесь было бы поздно и потребовало бы
            // отдельного ручного EOI, как раньше в elf.c.
            schedule();
        }
    }
}