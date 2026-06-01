# Промт для следующей сессии — Шаг 5: GPU command processor → рендерер (variant A)

> Скопируй всё, что ниже разделителя, в новую сессию Claude Code, запущенную из
> `/home/h/src/recomp/rexglue-recomps/south-park-recomp`.

---

Ты продолжаешь проект **variant A** — полная статическая рекомпиляция (XenonRecomp) игры
**«South Park: Let's Go Tower Defense Play!»** (Xbox 360 XBLA → Linux/Vulkan). Рабочая директория:
`/home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta`.

## ЗАДАЧА СЕССИИ
Реализовать **GPU command processor** так, чтобы игра **выдавала реальные кадры** (видимый вывод через
Vulkan). Это последний и самый большой шаг — целая подсистема, рассчитывай на длительную работу со
сборкой/тестами, а не на один фикс.

## ПЕРВЫМ ДЕЛОМ ПРОЧИТАЙ (там точная точка возобновления)
1. Память: `~/.claude/projects/-home-h-src-recomp/memory/sp_varianta_bootstrap.md` (особенно Update 5 и
   секции про GPU-frontier).
2. `varianta/NIGHT-LOG.md` — последние секции: «GPU engine implementation spec», «render-wait 0x36E70
   needs real PM4 execution», «Ring-buffer command structure fully decoded». Там полный спек + тупики.

## ТЕКУЩЕЕ СОСТОЯНИЕ (проверено, закоммичено)
- Игра **проходит ВСЮ инициализацию** (CRT/heap/TLS/импорты/jump-tables/аудио/ввод/Rtl/видео) и доходит
  до **своего цикла рендеринга**, отправляя реальные PM4-команды. Систему разблокировал один фикс:
  `NtQueryInformationFile` для класса 34 (FILE_NETWORK_OPEN_INFORMATION) писал `EndOfFile` по +8 вместо
  +40 → ассеты читались 0 байт (коммит `760b0bd`).
- Запуск: `REX_KTRACE=1 timeout 25 ./runtime/out/sp_td_varianta ../private/extracted/default.xex` →
  exit **124** (работает, не падает), ~1690 строк трейса, `INDIRECT-NULL`≈0.
- **Где встал:** главный поток висит в `KeWaitForSingleObject(event 0x36E70, timeout=∞)` по цепочке
  `sub_82211638 → sub_8221A8A0 → sub_822E2CF0 → sub_822F2CE0 → sub_8230F368 → sub_8230F098`.
  `0x36E70` — это **событие завершения GPU-команд**; освобождается ТОЛЬКО когда реальный CP исполнит
  отправленный PM4. Без интерпретатора PM4 событие не сигналится никогда.
- В кольце `@0xA0002000` лежит: `PM4_ME_INIT` (0xC0114800, op 0x48, dwords 0..18) + два
  `PM4_INDIRECT_BUFFER` (0xC0013F00, op 0x3F): IB1=[addr 0x90040, len 0x0B], IB2=[addr 0x10000, len 0x40].
  ⚠ По этим адресам сейчас НЕ PM4 (нули / UTF-16 строка) — нужна **GPU→CPU трансляция адресов** IB
  (см. ниже), и/или игра дописывает реальные command-IB только после «CP ready».
- В `kernel.cpp::VblankPump` уже есть **null-GPU CP** (коммит `d9b009d`): читает CP_RB_WPTR, продвигает
  RPtr в регистр + write-back, шлёт interrupt. RPtr доходит до WPtr=25 — но это **НЕ** освобождает
  `0x36E70`.

## УЖЕ ИСКЛЮЧЁННЫЕ ТУПИКИ (НЕ повторять — доказано на железе)
1. Продвижение RPtr к WPtr — недостаточно.
2. Graphics-interrupt callback с source=1 (command-complete) сам по себе — игнорируется.
3. `VdVerifyMEInitCommand` — игра его НЕ вызывает (нет шортката через стаб).
4. Попроцессорная доставка прерывания — опровергнуто: `FillKPcr` обнуляет KPCR, все потоки = процессор 0;
   callback (читает KPCR+0x10C) шлёт события proc-0 = `0x36e30`/`0x36e54`, но НЕ `0x36E70`.
5. Аргументы callback верны: r3=source, r4=interrupt_callback_data_ (совпадает с rexglue
   `DispatchInterruptCallback args[]={source, interrupt_callback_data_}`).
⇒ Освобождает `0x36E70` только реальное исполнение PM4 (EOP/fence/EVENT_WRITE, которое драйвер игры
превращает в `KeSetEvent(0x36E70)`).

## ПЛАН РЕАЛИЗАЦИИ (рекомендую ПЕРЕИСПОЛЬЗОВАТЬ движок, а не писать с нуля)
Эталон, который УЖЕ рендерит этот тайтл: `third_party/rexglue-sdk/src/graphics/` (Xenia 1:1).
- `command_processor.cpp`: `ExecutePrimaryBuffer` (~:290 — цикл RPtr→WPtr + store RPtr в write-back),
  `ExecutePacket` dispatch по `packet>>30` (~:818-837), type-3 (opcode=(p>>8)&0x7F, count=((p>>16)&0x3FFF)+1,
  ~:920-939), `ExecutePacketType3_ME_INIT` (~:1082), `ExecutePacketType3_INDIRECT_BUFFER` (~:1200).
- `graphics_system.cpp`: `WriteRegister` (~:275, `r=(addr&0xFFFF)/4`), `DispatchInterruptCallback` (~:306),
  GPU MMIO base **0x7FC80000** (~:145-151), `MarkVblank`→`DispatchInterruptCallback(0,2)`.
- `packet_disassembler.cpp`: имена опкодов PM4.

Порядок (каждый пункт — отдельный, тестируемый инкремент):
1. **Интерпретатор PM4** в `VblankPump`/отдельном CP-потоке: обходить кольцо RPtr→WPtr, диспатч по типу.
   type-0 = записи регистров в reg-file `@0x7FC80000`; type-2 = NOP; type-3 = по опкоду.
2. **INDIRECT_BUFFER (0x3F)**: рекурсия в IB. РАЗОБРАТЬСЯ с трансляцией адреса IB (сырые 0x90040/0x10000
   указывают не на PM4 — нужно сопоставить с физической GPU-памятью игры; см. `MmAllocatePhysicalMemoryEx`
   возвращает 0xA274xxxx, окно 0xA0000000 = физический низ). Сверься с тем, как rexglue читает IB.
3. **ME_INIT (0x48)** + type-0 register writes.
4. Найти и **воспроизвести EOP/fence/EVENT_WRITE**, который драйвер игры превращает в
   `KeSetEvent(0x36E70)` → ожидание освобождается → игра начинает слать реальные draw-IB (всё ещё без
   пикселей). Проверка: главный поток выходит из `KeWaitForSingleObject(0x36E70)`, в трейсе появляется
   `VdSwap`.
5. **DRAW_* пакеты → Plume Vulkan** + 19 шейдеров (`private/extracted/media/shaders/*.updb`, оригинальный
   HLSL). Reference по маппингу D3D→RHI: Sonic Unleashed `gpu/video.cpp`. → **реальные кадры**.

## КЛЮЧЕВЫЕ АДРЕСА / ФАКТЫ
- GPU MMIO base `0x7FC80000`; CP_RB_RPTR = reg 0x1C4 @ `0x7FC80710`; CP_RB_WPTR = reg 0x1C5 @ `0x7FC80714`.
- Кольцо `@0xA0002000` (size 0x1000); RPtr write-back `@0x201003C`; WPtr сейчас = 25 dwords.
- Глобалы в `kernel.cpp`: `g_ringBufferBase`, `g_rptrWriteBack`, `g_interruptCallback` (=0x821C7170),
  `g_interruptData`, `g_pumpKpcr`, `g_pumpStack`. `CallGuest(addr, ctx)` исполняет гостевую функцию.
- PPCContext: r3@0x00, r1@0x10, r4@0x20, lr@0x100; `base` = аргумент функции (в gdb = `$rsi`); guest mem =
  `base + addr` (big-endian; `GLD32/GST32` свапают, `PPC_LOAD_U32/PPC_STORE_U32` тоже).

## REFERENCE-ORACLE (бесценный инструмент — диффай против рабочей сборки)
`out/build/linux-amd64-release/south_park_td` — рабочая сборка (rexglue-sdk), которая **рендерит** тайтл;
НЕ stripped, ~30310 гостевых символов `sub_*`, **тот же layout PPCContext**, guest base `0x100000000`.
Запуск под gdb (read-only): `cd out/build/linux-amd64-release && timeout N env SDL_VIDEODRIVER=x11
LD_LIBRARY_PATH=. DISPLAY=:0 gdb -batch -x <script> --args ./south_park_td
--game_data_root=<abs private/extracted> --user_data_root=<abs private/userdata> --log_file=run.log`.
- Это **PIE** → ставь брейки по СИМВОЛУ (`break sub_XXXX`), не по адресу из `nm`.
- После каждого прогона: `bash tools/gamectl.sh kill_all` + `rm -f /dev/shm/xenia_memory_*`.
- ⚠ НИКОГДА не модифицируй prod-бинарь / `librexruntime.so` (md5 1a3f6076) / `third_party/rexglue-sdk`.
- Используй oracle, чтобы захватывать гостевое состояние (что CP/драйвер делают в правильной сборке) в тех
  же гостевых функциях и сравнивать с variant A. Именно так был найден системный баг этой сессии.

## СБОРКА / ЗАПУСК / ОТЛАДКА
- Правишь рантайм: `varianta/runtime/kernel.cpp` (+ `rex_indirect.h`/`runtime.cpp` при необходимости).
- Сборка: `cd varianta/runtime/out && ninja sp_td_varianta` (для kernel.cpp — секунды; правка
  `rex_indirect.h` → полная пересборка ~90 TU).
- Запуск: `REX_KTRACE=1 timeout 25 ./runtime/out/sp_td_varianta ../private/extracted/default.xex
  > /tmp/run.log 2>&1`; exit 124 = живёт. Окружение: есть DISPLAY=:0 / wayland.
- Отладка: `gdb -batch -x script --args ...`; `-O0`, поэтому хостовые фреймы и `break ppc_recomp.N.cpp:LINE`
  работают; читай `ctx.rN.u32`, `base`(=$rsi). Атач к живому процессу: `gdb -batch -p PID -ex "thread
  apply all bt"`. ⚠ `pkill` с ненулевым кодом отменяет соседние tool-calls — запускай его отдельно.
- Регенерация `ppc/` (если меняешь тулчейн): пересобрать XenonRecomp + запустить `XenonRecomp sp_xenon.toml
  ppc_context.h` (НЕ XenonAnalyse — он затрёт ручные switch-таблицы); затем полная пересборка. `rm ppc/*.cpp`
  перед регеном (иначе кэш «identical file» оставит старые TU).

## ОГРАНИЧЕНИЯ
- Скоуп: только `varianta/` (+ тулчейн через `patches/xenonrecomp-sp-instructions.patch`).
- **Не пушить.** Не трогать prod `.so` (1a3f6076), `third_party/rexglue-sdk`, не двигать указатель
  суперпроекта. Автор коммитов: `superheher <heh@vivaldi.net>`; в конце сообщения коммита:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- Хост — расходный тестовый стенд: sudo свободно (пароль <redacted>), но prod-сборку не ломать.

## ГОТОВО, КОГДА
В окне видны кадры игры (через `VdSwap` → Vulkan-present), либо как минимум очищенный/частичный кадр от
реального исполнения PM4. Промежуточная веха: главный поток выходит из `KeWaitForSingleObject(0x36E70)`
и игра начинает звать `VdSwap` / слать draw-пакеты.
