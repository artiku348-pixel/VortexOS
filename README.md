# VortexOS

Учебная операционная система на C для x86 (32-бит, защищённый режим).
Загружается любым Multiboot-совместимым загрузчиком (GRUB, `qemu -kernel`).

## Что уже есть

- загрузка по Multiboot 1, собственный стек, переход в `kmain`
- консоль VGA 80×25 (цвета, прокрутка, аппаратный курсор) + дублирование вывода в COM1
- `kprintf` (`%c %s %d %u %x %p`, ширина и дополнение нулями)
- GDT (плоская модель), IDT, перенастройка PIC 8259
- обработчики исключений CPU с дампом регистров
- системный таймер PIT (100 Гц), `sleep_ms`
- драйвер клавиатуры PS/2 (Shift, Caps Lock, Backspace)
- куча ядра: `kmalloc` / `kfree` (first-fit со слиянием блоков)
- командная оболочка: `help clear echo info mem uptime sleep color crash reboot halt`

Интерфейс пока на английском: в текстовом режиме VGA нет кириллицы.

## Сборка и запуск

Нужны `gcc` с поддержкой `-m32`, `binutils`, `make`; для запуска — QEMU.

```sh
make            # build/vortex.bin
make run        # qemu-system-i386 -kernel build/vortex.bin
make iso        # загрузочный ISO (нужны grub-mkrescue и xorriso)
make run-iso
make test       # smoke-тест без QEMU (pip install unicorn)
make clean
```

С кросс-компилятором: `make CROSS=i686-elf-`.

`make test` загружает ядро в эмуляторе CPU Unicorn, подаёт прерывания таймера
и клавиатуры через IDT ядра, «набирает» команды в оболочке и проверяет вывод.

## Структура

```
boot/boot.S      точка входа, заголовок Multiboot, загрузка GDT/IDT
boot/isr.S       заглушки прерываний (векторы 0–47)
boot/grub.cfg    конфигурация GRUB для ISO
kernel/main.c    kmain: инициализация подсистем
kernel/vga.c     текстовая консоль
kernel/serial.c  COM1
kernel/printf.c  kprintf, panic
kernel/gdt.c     сегментация
kernel/idt.c     IDT, PIC, исключения, диспетчер IRQ
kernel/timer.c   PIT
kernel/keyboard.c клавиатура
kernel/heap.c    kmalloc/kfree
kernel/shell.c   оболочка
kernel/string.c  memset, memcpy, strlen, ...
include/kernel.h общий заголовок
linker.ld        скрипт компоновки (ядро по адресу 1 МиБ)
tools/emu_test.py smoke-тест
```

## Что дальше

- страничная память (paging) и менеджер физических страниц
- многозадачность: потоки ядра и планировщик
- режим пользователя и системные вызовы
- файловая система (initrd → FAT)
