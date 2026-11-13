# VortexOS

Учебная операционная система на C для x86 (32-бит, защищённый режим):
ядро, файловая система в памяти, оболочка со скриптами и `PATH`, загрузчик
программ, SDK для приложений, графика и рабочий стол с окнами.

Загружается любым Multiboot-совместимым загрузчиком (GRUB, `qemu -kernel`).

## Возможности

**Ядро**
- Multiboot 1, GDT/IDT, PIC, исключения CPU с дампом регистров
- таймер PIT (100 Гц), часы CMOS, клавиатура и мышь PS/2, COM1
- куча (`kmalloc`/`kfree`), кооперативная многозадачность (потоки ядра)
- системные вызовы через `int 0x80`
- ramfs — файловая система в памяти, наполняется из initrd (архив ustar)
- загрузчик позиционно-независимых ELF32-программ

**Оболочка**
- встроенные команды: `help ls cd pwd cat cp mv rm mkdir touch echo set unset env which sh ps kill
  desktop info mem date uptime sleep color clear crash exit reboot halt`
- поиск программ и скриптов по `$PATH` (по умолчанию `/bin:/apps`)
- переменные (`set NAME=VALUE`, `$NAME`), аргументы скриптов `$0`…`$9`
- перенаправление вывода `>` и `>>`, фоновый запуск `&`, `Ctrl+C`
- `/etc/init.sh` выполняется при загрузке

**Графика и рабочий стол** (команда `desktop`)
- режим 1024×768×32 через Bochs VBE (QEMU `-vga std`, Bochs, VirtualBox)
- композитный оконный менеджер: перетаскивание окон, фокус, кнопка закрытия
- панель задач с часами и меню запуска (все программы из `/apps`)
- окна терминала — в каждом своя оболочка
- приложения: Calculator, Paint, Snake, About

Интерфейс на английском: в шрифте пока только ASCII.

## Сборка и запуск

Нужны `gcc` с поддержкой `-m32`, `binutils`, `make`, `tar`; для запуска — QEMU.

```sh
make              # build/vortex.bin + build/initrd.tar
make run          # запуск в QEMU   (или tools/run.sh)
make iso          # загрузочный ISO (нужны grub-mkrescue и xorriso)
make run-iso
make test         # автотест без QEMU (pip install unicorn)
make clean
```

С кросс-компилятором: `make CROSS=i686-elf-`.

После загрузки: `help`, `ls /bin`, `sysinfo`, `greet World`, `hello`, `desktop`.
На рабочем столе: меню **Vortex** слева внизу; в терминале `calc &` запускает
программу в фоне; «Exit desktop» возвращает в полноэкранную консоль.

## Скрипты

| Скрипт | Назначение |
| --- | --- |
| `tools/run.sh [iso\|debug]` | сборка и запуск в QEMU (debug — ожидание gdb на :1234) |
| `tools/new-app.sh NAME [gui]` | создать новое приложение из шаблона |
| `tools/emu_test.py` | автотест: загрузка ядра в эмуляторе CPU Unicorn |
| `tools/bdf2c.py` | конвертер шрифта BDF → `kernel/font.c` |
| `rootfs/etc/init.sh` | скрипт инициализации VortexOS (PATH, HOME, приветствие) |
| `rootfs/bin/sysinfo`, `greet`, `mkproject` | примеры скриптов оболочки VortexOS |

Скрипт VortexOS — текстовый файл с командами оболочки; положите его в
`rootfs/bin/`, и он станет командой:

```sh
# greet NAME
set WHO=$1
echo "Hello, $WHO!" > /tmp/greeting.txt
cat /tmp/greeting.txt
```

## SDK: свои приложения

```sh
tools/new-app.sh mytool        # apps/bin/mytool.c -> /bin/mytool  (консоль)
tools/new-app.sh mygame gui    # apps/gui/mygame.c -> /apps/mygame (окно, попадает в меню)
make run
```

Каждый файл `apps/bin/*.c` и `apps/gui/*.c` — отдельная программа. Она
собирается с `sdk/include/vortex.h` и библиотекой `libvx` (`sdk/lib`).

```c
#include <vortex.h>

int main(int argc, char **argv)
{
    int win = vx_win_create("Demo", 320, 200);      /* -1, если рабочий стол не запущен */
    struct vx_event ev;

    vx_win_fill(win, 0, 0, 320, 200, VX_RGB(255, 255, 255));
    vx_win_text(win, 16, 16, "Hello!", VX_RGB(0, 0, 0));
    for (;;) {
        vx_win_event(win, &ev, -1);                 /* ждать события */
        if (ev.type == VX_EV_KEY && ev.key == 27) return 0;
    }
}
```

Что есть в SDK (`sdk/include/vortex.h`):

- **консоль**: `printf`, `puts`, `getchar`, `gets_s`, `vx_color`, `vx_clear`
- **файлы**: `vx_read_file`, `vx_write_file`, `vx_stat`, `vx_readdir`, `vx_mkdir`, `vx_remove`
- **процессы**: `vx_spawn`, `vx_wait`, `vx_sleep`, `vx_ticks`, `vx_time`, `vx_getenv`, `vx_setenv`, `vx_exit`
- **окна**: `vx_win_create`, `vx_win_fill`, `vx_win_text`, `vx_win_blit`, `vx_win_event`
- **libc**: `mem*`, `str*`, `atoi`, `snprintf`, `malloc`/`free`, `rand`

Номера системных вызовов, коды клавиш и структуры — в `sdk/include/vortex/abi.h`.

## Структура

```
boot/            точка входа, заглушки прерываний, переключение задач, grub.cfg
kernel/          ядро: main, консоль, vga, gfx, term, desktop, task, fs, exec,
                 syscall, shell, драйверы (keyboard, mouse, timer, rtc, serial)
include/kernel.h внутренний заголовок ядра
sdk/             SDK приложений: заголовки и libvx
apps/bin/        консольные программы (hello, hexdump, guess)
apps/gui/        оконные программы (calc, paint, snake, about)
rootfs/          содержимое корневой ФС: /etc, /bin (скрипты), /home
tools/           скрипты разработки и автотест
third_party/     лицензия шрифта Spleen (BSD 2-Clause)
```

## Тестирование

`make test` загружает ядро в эмуляторе CPU Unicorn. Скрипт изображает
остальной компьютер (загрузчик, PIC, клавиатуру, мышь, часы, видеокарту),
набирает команды, запускает программы и скрипты, открывает рабочий стол,
нажимает кнопки мышью и сохраняет снимки экрана в `build/*.png`.

## Ограничения

- Приложения работают в кольце 0 в общем адресном пространстве: защиты памяти
  нет, ошибочная программа может повредить систему.
- Многозадачность кооперативная: программа, которая зациклилась без системных
  вызовов, останавливает всё.
- Файловая система живёт в памяти — изменения теряются при перезагрузке.
- Графика работает только с адаптером Bochs VBE; на другом железе остаётся
  текстовый режим.

## Что дальше

- страничная память, режим пользователя и изоляция процессов
- вытесняющий планировщик
- диск и постоянная файловая система (ATA + FAT)
- кириллица в шрифте и раскладке, история команд, условия и циклы в скриптах
