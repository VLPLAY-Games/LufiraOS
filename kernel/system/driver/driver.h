#pragma once

// driver.c — модель привилегий для userspace-драйверов (готовится под
// перенос сети/звука из ядра в userspace в 0.9.5). До этого любой процесс был
// симметричен по возможностям — единственная проверка была uid==0 (root).
// Этого недостаточно для PCI config space/port I/O/MMIO/IRQ: "любой root"
// это по сути прямой доступ к железу каждому процессу шелла, запущенному под
// root, а не только одному доверенному демону (netd/soundd). Здесь заводится
// явная заявка "процесс X владеет PCI-устройством Y" — остальные
// привилегированные syscall'ы в syscall.c разрешены только заявившему
// процессу и только в границах заявленного устройства.
//
// Один процесс может владеть несколькими устройствами (MAX_DRIVER_CLAIMS —
// общий потолок на систему, не на процесс), но каждое устройство — только
// одним процессом разом.

#include "lib/types.h"
#include "drivers/pci/pci.h"

#define MAX_DRIVER_CLAIMS 8   // одновременно заявленных PCI-устройств в системе
#define MAX_USER_IRQ      16  // legacy PIC — ровно столько линий бывает

void driver_init(void);

// Заявляет первое ещё не заявленное устройство с данными vendor_id/device_id
// за pid, копирует его в *out. 0 успех, -1 нет (больше) ни одного подходящего
// незанятого устройства, -2 таблица заявок переполнена.
int driver_claim_device(uint32_t pid, uint16_t vendor_id, uint16_t device_id,
                         pci_device_t *out);

// Снимает заявку pid на bus:device:function. 0 успех, -1 такой заявки у pid нет.
int driver_release_device(uint32_t pid, uint8_t bus, uint8_t device, uint8_t function);

// Истина, если bus:device:function заявлено именно pid — проверка перед
// каждым PCI config/MMIO/port-I/O/IRQ-register syscall'ом.
int driver_owns_device(uint32_t pid, uint8_t bus, uint8_t device, uint8_t function);

// Истина, если у pid есть хоть одна действующая заявка — ворота для
// SYS_DMA_ALLOC (не привязан к конкретному устройству, но не должен быть
// доступен произвольному процессу).
int driver_is_registered(uint32_t pid);

// Истина, если [phys, phys+length) целиком попадает в ОДИН memory BAR
// какого-либо устройства, заявленного pid — проверка для SYS_MMAP_MMIO.
int driver_mmio_allowed(uint32_t pid, uint64_t phys, uint64_t length);

// Истина, если [port, port+width) целиком попадает в ОДИН I/O BAR
// какого-либо устройства, заявленного pid — проверка для SYS_IO_IN/OUT.
int driver_io_port_allowed(uint32_t pid, uint16_t port, uint8_t width);

// Снимает все заявки и IRQ-регистрации pid — звать из process_exit()/
// terminate_process_by_signal() (тот же момент, что обнуляет g_wm_pid),
// иначе завершившийся драйвер навечно держит устройство недоступным для
// следующего запуска.
void driver_release_process(uint32_t pid);

// ----- IRQ -----

// Закрепляет аппаратную линию irq за pid (снимает маску в PIC) — вызывающий
// обязан уже владеть устройством, чей PCI_INTERRUPT_LINE равен irq (это
// проверяет syscall-обработчик, не этот файл). 0 успех, -1 неверный номер
// линии, -2 уже закреплена за другим pid.
int driver_irq_register(uint32_t pid, uint8_t irq);

// Из irq_handler() (irq.c) — для любой линии без зашитого в ядро обработчика
// (не 0/1/12). Увеличивает счётчик непринятых срабатываний и будит процесс,
// заблокированный в driver_irq_wait(), если есть. Не делает ничего, если
// линия не зарегистрирована — безопасно звать для каждой нераспознанной
// линии без доп. проверок на стороне irq.c.
void driver_irq_notify(uint8_t irq);

// Блокирует current_process, пока не придёт срабатывание irq (или сразу
// возвращает, если уже накопилось). Вызывающий обязан быть владельцем
// регистрации (проверяет syscall-обработчик). 0 при срабатывании, -1 если
// линия не закреплена за pid.
int driver_irq_wait(uint32_t pid, uint8_t irq);
