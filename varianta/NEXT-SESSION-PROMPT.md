Ты продолжаешь проект variant A — полная статическая рекомпиляция (XenonRecomp) игры «South Park: Let's Go
Tower Defense Play!» (Xbox 360 XBLA → Linux/Vulkan). Рабочая директория:
/home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta. Ветка experimental/hle-graphics-spike.

✅ ВЫПОЛНЕНО за прошлые сессии (НЕ переоткрывай; коммиты НЕ запушены):
- Порча гостевой кучи / «device teardown» — FIX = корректный writeback NtFreeVirtualMemory(MEM_DECOMMIT)
  (commit 4e011b5). Boot 171→462 строки.
- Jump table sub_8228A208 (16-битная offset-таблица, XenonAnalyse её не видит) восстановлена вручную в
  sp_switch_tables.toml; инструкции vcmpbfp128/blrl реализованы в эмиттере (commit c10ab1a). Boot 462→1191
  строк, краш SIGTRAP в CRT-init математике УСТРАНЁН.
Полная история: NIGHT-LOG.md (хвост) + память [[sp_varianta_bootstrap]] Updates 10-11.

ЗАДАЧА СЕССИИ. Расшить ДЕДЛОК thread-join в CRT-инициализации. Boot дошёл до тихого устойчивого состояния:
все воркеры спят на ожиданиях (KeWaitForMultipleObjects/NtWaitForMultipleObjectsEx на g_waitCv), один
рендер-воркер (sub_821CC5D0 @0x821CC690) делает безобидный ~28 Гц опрос KeGetCurrentProcessType (НЕ блокер).
ГЛАВНЫЙ поток висит навсегда:
  _xstart → sub_82249638 → sub_82249678 → **sub_8214FFD0** (CRT static-init, глобальные конструкторы) →
  sub_82448D78 (тонкий thunk) → **sub_8244E2D8** → **NtWaitForSingleObjectEx(handle=0xF1000002, timeout=∞)**.
handle 0xF1000002 = **поток tid=4** (start = **sub_8242B4A8**, ppc_recomp.60.cpp:10096; создан suspended на
boot'е, затем NtResumeThread). Его KTHREAD резолвится в 0x90100C40 (thread-арена 0x90100000+; события — другая
арена 0x90400000+), dispatcher-заголовок там ВЕСЬ В НУЛЯХ (signal_state=0 = поток НЕ завершён). Значит
глобальный конструктор порождает воркер tid=4 и JOIN'ит его (ждёт завершения), а tid=4 не завершается (сам
запаркован в ожидании).

РЕЗЮМЕ (точка возобновления):
1. Выясни, ЧТО делает tid=4 (sub_8242B4A8) и на чём он стоит. gdb attach (готовые скрипты
   /tmp/{threads,t1,obj,h}.gdb): запусти `REX_KTRACE=0 ./runtime/out/sp_td_varianta ../private/extracted/
   default.xex &`, `sleep 6`, затем `gdb -batch -p <pid>` с `thread apply all bt 12`. Найди поток, у которого
   на дне стека sub_8242B4A8, и посмотри его текущее ожидание (какой объект/событие).
2. Реши, почему tid=4 не выходит: (a) ждёт событие/семафор, который должен взвести ДРУГОЙ поток (или сам
   главный) — найди недостающий SetEvent/Release; (b) поллит очередь, в которую никто не кладёт; (c) join
   преждевременный (главный не должен ждать здесь — тогда копай why guest enqueues a join). Ref 1:1 =
   third_party/rexglue-sdk/src/ (та же логика в рабочем prod). Сравни с prod-оракулом (рендерит) — он
   проходит этот join.
3. Проверь гипотезу: tid=4 (sub_8242B4A8) в CRT-регионе (0x8242xxxx) — вероятно CRT-воркер/тред-пул. Возможна
   проблема в нашей HLE-семантике ExCreateThread/NtResumeThread/Ke*Wait, или кооперативного токена (главный
   в NtWaitForSingleObjectEx ОСВОБОЖДАЕТ токен — это видно; значит tid=4 МОЖЕТ бежать, но стоит на ожидании).
4. Чини → tid=4 завершается → join главного снимается → boot идёт глубже → к рендеру (VdSwap/DRAW).

ДРУГИЕ ИЗВЕСТНЫЕ БЛОКЕРЫ ВПЕРЕДИ (не сейчас, но на радаре):
- vupkd3d128 (78× в ppc/) — D3D-распаковка вершин, default-case эмиттера = __builtin_debugtrap (graphics-path,
  ещё не достигнут). Когда дойдёт — реализовать недостающие pack-типы в XenonRecomp/recompiler.cpp
  (case PPC_INST_VUPKD3D128, типы кроме 0/1; апстрим Unleashed их использует — кради семантику оттуда).
- XamLoaderLaunchTitle — пока stub (returns 0), НЕ блокер сейчас.

ДИАГНОСТИКА / СБОРКА:
- Запуск из varianta/: `find /dev/shm -maxdepth 1 -name 'xenia_memory_*' -delete; REX_KTRACE=1 stdbuf -oL -eL
  timeout 22 ./runtime/out/sp_td_varianta ../private/extracted/default.xex > /tmp/boot.log 2>&1`.
- ⚠ grep по логу ВСЕГДА с `-a` (в логе бинарные байты из имён .embsec секций → иначе grep молчит).
- Прогресс: число строк (171→462→1191), `grep -ac NtCreateFile`, и страховки `grep -ac "req=0x10000 sz=0x80000"`
  (=0, устройство цело) + `grep -ac INDIRECT-NULL` (=0).
- Правка рантайма: `ninja -C runtime/out sp_td_varianta` (секунды для kernel.cpp; полный реген ppc → все 88 TU).
- Правка ЭМИТТЕРА (инструкции/jump-tables): edit XenonRecomp/recompiler.cpp →
  `cmake --build ../../third_party/XenonRecomp/out/build --target XenonRecomp` →
  `rm ppc/*.cpp; <XR> sp_xenon.toml ../../third_party/XenonRecomp/XenonUtils/ppc_context.h` (XR =
  third_party/XenonRecomp/out/build/XenonRecomp/XenonRecomp) → `ninja -C runtime/out sp_td_varianta`.
  Затем regen* патч: `git -C ../../third_party/XenonRecomp diff > patches/xenonrecomp-sp-instructions.patch`.
  ⚠ jump-table: добавляй [[switch]] (base=адрес bctr, r=индекс-рег, default, labels=resolved-адреса) в
  sp_switch_tables.toml; functions-override в sp_xenon.toml НУЖЕН только если XenonAnalyse РАЗРЕЗАЛ функцию.
- ⚠ zsh: НЕ pkill/glob; чистить shm только `find /dev/shm -maxdepth 1 -name 'xenia_memory_*' -delete`.
- Оракул prod: out/build/linux-amd64-release/south_park_td (Release/no-ctx, break по СИМВОЛУ, PIE base
  0x100000000) — рендерит 54-55 Vulkan-пайплайнов; проходит этот join — сравнивай.

ОГРАНИЧЕНИЯ. Скоуп только varianta/ (+ тулчейн через patches/xenonrecomp-sp-instructions.patch). НЕ пушить; НЕ
трогать prod-бинарь/librexruntime.so(1a3f6076)/rexglue-sdk/указатель суперпроекта. Автор superheher
<heh@vivaldi.net>; коммиты заканчивать: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>.
Хост — расходный стенд (sudo пароль <redacted>), prod не ломать.

ГОТОВО, КОГДА: (промежуточно) tid=4 завершается, join главного снят, boot идёт глубже (>1191 строк, новые
ассеты/события); (далее) главный поток зовёт VdSwap; (цель) в окне видны кадры через VdSwap→Vulkan.
