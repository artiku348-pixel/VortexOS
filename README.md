# VortexOS

Учебная операционная система на C для x86 (32-бит, защищённый режим):
ядро со страничной памятью, процессы в режиме пользователя, диск с FAT16,
оболочка со скриптами и `PATH`, SDK для приложений, графика и рабочий стол
с окнами.

Загружается любым Multiboot-совместимым загрузчиком (GRUB, `qemu -kernel`).

## Возможности

**Ядро**
- Multiboot 1, GDT/IDT, PIC, исключения CPU с дампом регистров
- таймер PIT (100 Гц), часы CMOS, клавиатура и мышь PS/2, COM1
- менеджер физических страниц (битовая карта кадров по 4 КиБ) и страничная
  память: у каждого процесса свой каталог страниц, ядро недоступно из кольца 3,
  страница 0 не отображена (обращение по `NULL` — это page fault)
- режим пользователя: программы работают в кольце 3 (TSS, отдельный стек ядра);
  ошибка в программе завершает только её
- системные вызовы через `int 0x80`; каждый указатель от программы проверяется
- многозадачность: потоки ядра и пользовательские процессы, планировщик
  round-robin; код пользователя вытесняется по прерыванию таймера
- куча ядра (`kmalloc`/`kfree`), куча программ — `malloc` поверх `sbrk`
- загрузчик позиционно-независимых ELF32-программ

**Файловая система**
- единое дерево: корень наполняется из initrd (архив ustar), диск — в `/disk`
- драйвер ATA (PIO, LBA28) и FAT16 с длинными именами (VFAT)
- поток ядра `diskd` записывает изменения на диск через полсекунды после
  последней правки; `sync` — записать сразу, `disk` — состояние диска
- образ `build/disk.img` создаётся `tools/mkfat.py` из каталога `diskfs/`
  и читается обычными средствами Linux/macOS/Windows

**Оболочка**
- встроенные команды: `help ls cd pwd cat cp mv rm mkdir touch echo set unset env which sh ps kill
  desktop info mem disk sync date uptime sleep color clear crash exit reboot halt`
- поиск программ и скриптов по `$PATH` (по умолчанию `/bin:/apps`)
- переменные (`set NAME=VALUE`, `$NAME`), аргументы скриптов `$0`…`$9`
- перенаправление вывода `>` и `>>`, фоновый запуск `&`, `Ctrl+C`
- `/etc/init.sh` выполняется при загрузке

**Графика и рабочий стол** (команда `desktop`)
- режим 1024×768×32 через Bochs VBE (QEMU `-vga std`, Bochs, VirtualBox)
- композитный оконный менеджер: перетаскивание окон, фокус, кнопка закрытия
- панель задач с часами и меню запуска (все программы из `/apps`)
- окна терминала — в каждом своя оболочка
- приложения: Files (файловый менеджер с просмотром текста), Calculator, Paint,
  Snake, About

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
kernel/          ядро: main, pmm, vmm, task, syscall, exec, fs, fat, shell,
                 консоль, vga, gfx, term, desktop,
                 драйверы (ata, keyboard, mouse, timer, rtc, serial)
include/kernel.h внутренний заголовок ядра
sdk/             SDK приложений: заголовки и libvx
apps/bin/        консольные программы (hello, hexdump, guess, systest)
apps/gui/        оконные программы (files, calc, paint, snake, about)
rootfs/          содержимое корневой ФС: /etc, /bin (скрипты), /home
diskfs/          начальное содержимое диска (/disk)
tools/           скрипты разработки, mkfat.py (образы FAT16) и автотест
third_party/     лицензия шрифта Spleen (BSD 2-Clause)
```

## Память и защита

```
0x00001000 … конец ОЗУ   ядро, куча ядра, кадры страниц (только кольцо 0)
0x40000000 …             образ программы, затем её куча (sbrk)
           … 0x80000000  стек программы (64 КиБ)
0xFD000000 (обычно)      кадровый буфер видеокарты (только кольцо 0)
```

Программа `systest` показывает защиту в действии:

```
systest null      запись по адресу 0          -> Page fault, система работает
systest kernel    чтение памяти ядра          -> Page fault
systest priv      привилегированная команда   -> General protection fault
systest badptr    указатели ядра в вызовах    -> вызовы возвращают -1
systest malloc    4 МиБ через malloc/free
systest spin &    вечный цикл                 -> система отвечает, `kill` работает
```

## Диск

`make` создаёт `build/disk.img` (16 МиБ, FAT16) один раз и больше его не
трогает, поэтому сохранённые файлы переживают перезапуск. `make disk`
пересоздаёт образ из `diskfs/`.

```
echo hello > /disk/test.txt     # через полсекунды diskd запишет на диск
reboot
cat /disk/test.txt
python3 tools/mkfat.py ls build/disk.img      # посмотреть образ с хоста
```

## Тестирование

`make test` загружает ядро в эмуляторе CPU Unicorn. Скрипт изображает
остальной компьютер (загрузчик, PIC, клавиатуру, мышь, часы, видеокарту,
ATA-диск), набирает команды, запускает программы и скрипты, проверяет защиту
памяти и вытеснение, открывает рабочий стол, нажимает кнопки мышью и сохраняет
снимки экрана в `build/*.png`. Записанный системой образ диска проверяется
независимым разборщиком FAT16 из `tools/mkfat.py`, затем машина
«перезагружается» с тем же диском.

Unicorn эмулирует только процессор, все устройства в тесте — имитация.
В QEMU и на реальном железе система пока не запускалась.

## Ограничения

- Содержимое `/disk` целиком держится в памяти, а при записи том
  переписывается заново: просто и надёжно для небольших дисков, но не для
  больших. Сбой питания во время записи может повредить том.
- Только FAT16, первый диск первого канала IDE, без таблицы разделов.
- Корень (`/bin`, `/home`, …) по-прежнему в памяти: постоянно хранится только
  `/disk`.
- Код ядра не вытесняется (переключение только в `task_yield`), один процессор.
- У процессов нет прав доступа к файлам и нет `fork`/каналов.
- Графика работает только с адаптером Bochs VBE; на другом железе остаётся
  текстовый режим.

## Что дальше

- инкрементальная запись FAT, FAT32, таблица разделов
- вытесняемое ядро, приоритеты, блокировки
- кириллица в шрифте и раскладке, история команд, условия и циклы в скриптах
