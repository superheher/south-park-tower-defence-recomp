Ты продолжаешь проект variant A — полная статическая рекомпиляция (XenonRecomp) игры «South Park: Let's Go
Tower Defense Play!» (Xbox 360 XBLA → Linux/Vulkan). Рабочая директория:
/home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta.

ЗАДАЧА СЕССИИ. Починить баг ПОРЧИ ПАМЯТИ (root-caused в прошлой сессии), который рушит GPU-инициализацию
тайтла: загрузчик .ptc пишет вершинные данные поверх структуры GPU-устройства. После фикса — продолжить к
кадрам (главный поток выходит из ожиданий GPU-completion → VdSwap → DRAW → Plume Vulkan + 19 шейдеров).

ПЕРВЫМ ДЕЛОМ ПРОЧИТАЙ (там точная точка возобновления):
1. Память ~/.claude/projects/-home-h-src-recomp/memory/sp_varianta_bootstrap.md — Update 6/7/8 (READ FIRST).
2. varianta/NIGHT-LOG.md — секции (хвост файла): «GPU CP built», «TEARDOWN ROOT-CAUSED», «allocation diff»,
   «ROOT FOUND: .ptc vertex-stream destinations are UN-RELOCATED».

ЧТО СДЕЛАНО В ПРОШЛОЙ СЕССИИ (закоммичено на experimental/hle-graphics-spike, НЕ запушено):
- 6d85089 — seed GPU-регистров (значения prod ReadRegister, главное reg 0x1951 interrupt-status=1 @0x7FC86544;
  + доставка interrupt на cpu 2 через KPCR+0x10C). Разблокировало vblank/interrupt-машинерию: boot прошёл
  ранний GPU-спин → XAudio → загрузка UI.xzp (23.5MB) → цикл рендера. Главный поток дошёл до именованной
  цепочки sub_822E2CF0→sub_822F2CE0→sub_8230F368→sub_8230F098→KeWaitForSingleObject.
- f6affa2 — РЕАЛЬНЫЙ PM4-интерпретатор в kernel.cpp (ExecutePM4/ExecuteType3/ExecuteRing, 1:1 с rexglue
  command_processor.cpp): обход кольца, рекурсия в IB, type-0 register writes, EVENT_WRITE_SHD fences,
  INTERRUPT-callbacks, счётчики DRAW/XE_SWAP. АДРЕС-МОДЕЛЬ (проверено): TranslatePhys(p)=0xA0000000|(p&0x1FFFFFFF)
  — physical GPU-память в окне 0xA0000000 (IB phys 0x90040→0xA0090040 = реальный PM4). Исполняет init-батч
  (ME_INIT 0x48 + 2 INDIRECT_BUFFER 0x3F + вложенные IB), пишет fences 0xA2010000=0x3/0xA2010004=0xA00100D4 →
  продвинул главный поток 0x36E70→0x36E30→0x388C4.
- 97decfb/175255b/be02891/b331362/8472803 — NIGHT-LOG + root-cause «teardown» (+ инструментовка REX_EVTRACE
  с захватом LR сигналящих/ждущих: g_evSignalLR/g_evWaitLR).

ЗАПУСК (проверь, что живёт): cd varianta && REX_KTRACE=1 timeout 25 ./runtime/out/sp_td_varianta
../private/extracted/default.xex  → exit 124 (живёт). Доходит до VdInitializeRingBuffer(0xA0002000),
vblank pump, XAudioRegisterRenderDriverClient, UI.xzp; потом порча памяти.

================================================================================================
БАГ К ПОЧИНКЕ (точная причина, проверено в gdb)
================================================================================================
Загрузчик ArcadeLogo.ptc пишет вершинные потоки по НЕ-РЕЛОЦИРОВАННЫМ адресам назначения, затирая структуру
GPU-устройства. Поэтому device+10900 (указатель на блок обработчика command-complete) обнуляется → главный
поток и render-воркеры навсегда встают на событиях GPU-completion (0x388C4, 0x29CD4/0x29D40).

ТОЧНЫЙ затирающий memcpy (sub_822A58F8, ppc_recomp.38.cpp:8757):
  dst=0x296B0  base(r11)=0x262B0  off(r26)=0x3400  src=0x5C560  size=0x400
Массив из 16 баз назначения (в кадре sub_822A58F8: r7→*(r1+1412)+60, читается циклом loc_822A5F6C):
  array[i] = 0x82B0 + i*0x3C00  →  0x82B0, 0xBEB0, … 0x262B0(#8), 0x29EB0(#9), … 0x406B0
Эти базы — СЫРЫЕ СМЕЩЕНИЯ (0x82B0..0x442B0): ниже базы кучи (0x10000), сквозь кучу тайтла, ПОПЕРЁК блока
устройства 0x26F40. Запись dst=array[i]+off попадает в устройство → затирает device+10900.

ПОЧЕМУ ЭТО БАГ (строго): диапазон 0x82B0..0x442B0 пересекает границу кучи и устройство — так не выглядит ни
одна валидная аллокация. sub_821BE840 ВЫДЕЛИЛ реальные .ptc-буферы в физ.окне по 0xA2016000/0xA23AE000
(3.7MB; sizes a1=0x500, a2=0x398000, a3=0x21E800). 16 потоков (240KB) умещаются внутри ⇒ назначения ДОЛЖНЫ
быть 0xA2016000 + (0x82B0 + i*0x3C00). Вместо этого база 0xA2016000 НЕ ПРИБАВЛЕНА — потоки не релоцированы.
КЛАСС бага = как исходный системный NtQueryInformationFile: данные/поле читается нулём, НЕ codegen
(рекомп-код 1:1 с prod). PROD: устройство 0x40016F80, рендерит; prod ТОЖЕ зовёт sub_822A2158 (копия —
нормальна); порчены только не-релоцированные назначения variant A.

ПЛАН ФИКСА:
1. Найти, где собирается массив баз (0x82B0+i*0x3C00): цепочка sub_8214F738 (открывает «game:\media\
   ArcadeLogo.ptc», handle 0xF2000002) → sub_822A2158 → sub_822A7C08 → массив в r7=(sub_822A7C08 r1+128) →
   sub_822A58F8:8757. Массив строится в sub_822A2158/sub_822A7C08 (sub_822A58F8 только ЧИТАЕТ его через r7).
2. Найти, какое поле .ptc-объекта должно нести базу буфера (0xA2016000) и почему в variant A оно 0 (или
   читается не то поле). Трассировать вверх: sub_821BE840 выделяет буферы (sub_82448090, размеры из
   sub_821BE038, хранит в объекте); где сохранение/чтение базы расходится.
3. Исправить так, чтобы назначения релоцировались на 0xA2016000 (или на правильный выделенный буфер).
   ⚠ Фикс — в гостевых ДАННЫХ/HLE (как class NtQueryInformationFile), НЕ в codegen. Возможно, расхождение в
   каком-то kernel-import out-param / поле объекта, питающем sub_821BE038/sub_821BE840.
4. Проверить: device+10900 НЕ обнуляется (watchpoint), boot идёт дальше.

КЛЮЧЕВЫЕ АДРЕСА/ФУНКЦИИ:
- Устройство (D3D-контекст) = g_interruptData = 0x26F80 (блок 0x26F40, 24KB; struct на +0x40; memset 0x5F00;
  спан 0x26F80..0x2CE80). Аллок: sub_8212DBA0 (sub_82448090(24448), caller-LR 0x8212DCEC) → sub_821C7F08 →
  sub_821D7438 (первая запись 0xFFFFFFFF). prod-устройство = 0x40016F80 (читается из лога prod:
  «[gpu] SetInterruptCallback(821C7170, 40016F80)»).
- Затирающая цепочка: sub_8214F730→sub_8214F738(ppc_recomp.6.cpp:18921)→sub_822A2158(ppc_recomp.37.cpp:23176)
  →sub_822A7C08(ppc_recomp.38.cpp:13688)→sub_822A58F8(ppc_recomp.38.cpp:8757, memcpy=sub_8242BF10).
- Куча: аллокатор sub_82448090, free sub_82448128. База кучи тайтла = 0x10000 (variant A) vs 0x40000000 (prod)
  — но коллизия ОТНОСИТЕЛЬНАЯ (не-релокация), не из-за базы.
- GPU: ring @0xA0002000, regs @0x7FC80000 (reg r → 0x7FC80000+r*4), RPtr-WB @0x201003C, interrupt cb 0x821C7170.

ИНСТРУМЕНТЫ (готовые gdb-скрипты в /tmp/ из прошлой сессии — переиспользуй/перепиши):
- corrupt.gdb — ловит ровно затирающий device+10900 memcpy + контекст (base/off/src/массив).
- bufsize.gdb — размеры/возвраты аллокаций sub_821BE840.
- ptrarray2.gdb — дамп 16-элементного массива баз (⚠ g_base читать СВЕЖИМ в python: на этапе загрузки
  скрипта он 0; см. rd() с per-call чтением g_base).
- devalloc2.gdb / watch_teardown.gdb — watchpoint на гостевой адрес от __imp___xstart, обход писателей.
- REX_CPTRACE=1 (трасса PM4-пакетов), REX_EVTRACE=1 (счётчики+LR сигналов/ожиданий событий, читать через gdb
  `p '(anonymous namespace)::g_evSignalLR'`).
- Reference-oracle (prod рендерит): out/build/linux-amd64-release/south_park_td, НЕ stripped, base 0x100000000,
  break по символу. ⚠ prod = Release: НЕТ символа ctx — читай гостевую память по membase, ставь break по символу.
  Запуск: cd out/build/linux-amd64-release && env SDL_VIDEODRIVER=x11 LD_LIBRARY_PATH=. DISPLAY=:0 gdb -batch
  -x SCRIPT --args ./south_park_td --game_data_root=<abs private/extracted> --user_data_root=<abs
  private/userdata> --log_file=/tmp/prod.log. ⚠ много breakpoint'ов на горячих функциях роняют prod в SIGSEGV
  (интерференция) — ставь МИНИМУМ.

СБОРКА/ЗАПУСК/ОТЛАДКА:
- Правишь varianta/runtime/kernel.cpp; сборка: cd varianta/runtime/out && ninja sp_td_varianta (kernel.cpp —
  секунды; rex_indirect.h → полная пересборка). ⚠ запускай из varianta/ (пути относительные); cwd сбрасывается
  после gdb-прогонов prod (cd заново).
- -O0: break ppc_recomp.N.cpp:LINE и хост-фреймы работают; читай ctx.rN.u32/base. Гостевая память BE
  (GLD32/GST32 свапают; в python rd() — ручной bswap).
- gdb attach для чтения памяти БЕЗОПАСЕН; live-степпинг ctx конфликтует с кооп-токеном.
- ⚠ pkill с ненулевым кодом ОТМЕНЯЕТ соседние tool-calls — запускай ОТДЕЛЬНО. Чисти стейл-процессы:
  pkill -9 -f "out/sp_td_varianta"; rm -f /dev/shm/xenia_memory_*.
- Реген ppc/ НЕ нужен для этого бага (он в данных/HLE, не codegen).

ПОСЛЕ ФИКСА (если устройство больше не порчено):
- Перепроверить ожидания GPU-completion: главный поток на 0x388C4 (sub_8230F098), render-воркеры sub_821CC5D0
  на 0x29CD4(30ms)/0x29D40(∞). Возможно, fences/interrupts интерпретатора теперь их удовлетворят.
- Цель-веха: главный поток выходит из ожиданий, игра зовёт VdSwap. Затем type-3 DRAW_INDX/DRAW_INDX_2 →
  Plume Vulkan + 19 шейдеров (private/extracted/media/shaders/*.updb, оригинальный HLSL; ref Sonic Unleashed
  gpu/video.cpp) → кадры.

ОГРАНИЧЕНИЯ. Скоуп только varianta/ (+ тулчейн через patches/xenonrecomp-sp-instructions.patch). НЕ пушить; НЕ
трогать prod-бинарь/librexruntime.so(1a3f6076)/rexglue-sdk/указатель суперпроекта. Автор superheher
<heh@vivaldi.net>; коммиты заканчивать: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>.
Хост — расходный стенд (sudo пароль <redacted>), prod не ломать.

ГОТОВО, КОГДА: (промежуточно) device+10900 не обнуляется .ptc-загрузчиком и boot идёт глубже; (далее)
главный поток вышел из KeWaitForSingleObject и игра зовёт VdSwap; (цель) в окне видны кадры через VdSwap→Vulkan.
