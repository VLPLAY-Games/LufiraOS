// mailbox.c — см. подробный разбор архитектуры в mailbox.h.

#include "mailbox.h"
#include "system/process/process.h"
#include "lib/string.h"

static inline uint64_t mailbox_lock(void) {
    uint64_t flags;
    asm volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    asm volatile("cli");
    return flags;
}

static inline void mailbox_unlock(uint64_t flags) {
    asm volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

void mailbox_init(ipc_mailbox_t *mb) {
    mb->head = mb->tail = mb->count = 0;
    mb->waiter = NULL;
}

int mailbox_send(uint32_t dest_pid, uint32_t sender_pid, const void *data, uint32_t len) {
    if (len > MAX_IPC_MSG_PAYLOAD) return -1;

    process_t *dest = process_find_by_pid(dest_pid);
    if (!dest) return -1;

    ipc_mailbox_t *mb = &dest->mailbox;
    uint64_t flags = mailbox_lock();

    if (mb->count >= MAX_IPC_MAILBOX_SLOTS) {
        mailbox_unlock(flags);
        return -2;
    }

    ipc_msg_t *slot = &mb->slots[mb->tail];
    slot->sender_pid = sender_pid;
    slot->len = len;
    if (len > 0) memcpy(slot->payload, data, len);
    mb->tail = (mb->tail + 1) % MAX_IPC_MAILBOX_SLOTS;
    mb->count++;

    process_t *waiter = mb->waiter;
    mb->waiter = NULL;
    mailbox_unlock(flags);

    // Будим ВНЕ критической секции (тот же порядок, что у pipe_wake()) —
    // сама запись waiter->state не нуждается в cli/sti: schedule()
    // планировщика просматривает process_list независимо, лишний тик до
    // того, как PROCESS_READY станет видно, не ломает корректность, только
    // задержка на деле не больше одного тика.
    if (waiter) waiter->state = PROCESS_READY;

    return 0;
}

int mailbox_recv(void *out, int timeout_ms) {
    if (!current_process || !out) return -1;
    ipc_mailbox_t *mb = &current_process->mailbox;
    uint64_t elapsed_ms = 0;

    for (;;) {
        uint64_t flags = mailbox_lock();
        if (mb->count > 0) {
            ipc_msg_t *slot = &mb->slots[mb->head];
            memcpy(out, slot, sizeof(ipc_msg_t));
            mb->head = (mb->head + 1) % MAX_IPC_MAILBOX_SLOTS;
            mb->count--;
            mailbox_unlock(flags);
            return 0;
        }

        if (timeout_ms == 0) {
            mailbox_unlock(flags);
            return -1;
        }

        if (timeout_ms > 0) {
            // НАЙДЕННЫЙ БАГ (жалоба пользователя: WM грузит CPU, особенно
            // при печати) — WM собирает один кадр заново после КАЖДОГО
            // отдельного sys_win_draw_*() клиента, а не один раз на всю
            // пачку (lufira-packages/apps/wm.c зовёт это с timeout_ms>0
            // именно чтобы недолго подождать следующий вызов той же пачки
            // перед тем, как пересобирать сцену). Если ждать нечего —
            // ограниченный таймаут через короткий опрос (process_sleep(10),
            // шаг в один тик — та же гранулярность и тот же приём, что уже
            // у SYS_POLL, см. его комментарий, syscall.h), а не честная
            // PROCESS_BLOCKED-блокировка: нужно самим проснуться по
            // истечении срока, не только по чужому mailbox_send().
            if (elapsed_ms >= (uint64_t)timeout_ms) {
                mailbox_unlock(flags);
                return -1;
            }
            mailbox_unlock(flags);
            process_sleep(10);
            elapsed_ms += 10;
            continue;
        }

        // timeout_ms < 0 — ждать неограниченно. Тот же приём, что и
        // pipe_read()/console_input_read(): уступаем CPU, пока
        // mailbox_send() (другой процесс ИЛИ прямо IRQ-обработчик ввода,
        // см. mailbox.h) не положит что-то и не разбудит нас обратно —
        // настоящая блокировка, без опроса, нулевая стоимость CPU.
        //
        // НАЙДЕННЫЙ БАГ (репорт пользователя: WM полностью замирает,
        // особенно от движения мыши) — lost wakeup. Раньше здесь ВЫШЕ
        // стоял mailbox_unlock(flags) сразу после проверки mb->count==0,
        // т.е. между этой проверкой и выставлением себя waiter'ом ниже
        // IF на мгновение включался обратно. Если РОВНО в эту щель
        // придёт mailbox_send() из IRQ (мышь/клавиатура шлют прямо из
        // input_mouse_event()/input_keyboard_event(), см. input.c) — он
        // положит сообщение в очередь и никого не разбудит (waiter ещё
        // не выставлен), а мы следом выставим себя waiter'ом и
        // заблокируемся НАВСЕГДА, хотя сообщение уже лежит в mb->slots
        // и больше никто его не перепроверит. Раньше окно гонки почти
        // никогда не задевалось (один клиентский RPC "в полёте" за раз);
        // коалесцинг + частые mouse-move события из этого среза резко
        // увеличили шанс попасть именно в эту щель. Фикс: НЕ отпускаем
        // cli между проверкой count==0 и переходом в PROCESS_BLOCKED —
        // schedule() сам явно рассчитан на вызов при IF=0 (см. его
        // комментарий, process.c), а при возврате в любой процесс его
        // IF восстанавливается из ЕГО СОБСТВЕННОГО сохранённого кадра,
        // так что "недоделанный" mailbox_unlock() здесь ничему не
        // вредит.
        current_process->mailbox.waiter = current_process;
        current_process->state = PROCESS_BLOCKED;
        schedule();
    }
}
