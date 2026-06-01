Ты продолжаешь проект variant A — полная статическая рекомпиляция (XenonRecomp) игры «South Park: Let's Go
Tower Defense Play!» (Xbox 360 XBLA → Linux/Vulkan). Рабочая директория:
/home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta. Ветка experimental/hle-graphics-spike.

✅ ПРЕДЫДУЩАЯ ЗАДАЧА ВЫПОЛНЕНА (порча гостевой кучи / «device teardown» — ПОЧИНЕНА, commit 4e011b5).
Не переоткрывай её. Кратко (полностью — NIGHT-LOG.md секция «✅ FIXED: ... DECOMMIT WRITEBACK», память
[[sp_varianta_bootstrap]] Update 10): причина была в нашем NtFreeVirtualMemory — на MEM_DECOMMIT он писал
обратно в out-параметры ВЕСЬ резерв кучи (*BaseAddress=0x10000, *RegionSize=0x100000) вместо запрошенного
под-диапазона (0x70000/0x10000). Гостевой RtlpDeCommitFreeBlock (sub_8244B018, ppc_recomp.65.cpp:4184-4190)
ЧИТАЕТ эти out-параметры обратно и сует [base,size) в UnCommittedRanges сегмента (sub_82449D58). ⇒ куча
считала, что декоммитнут ВЕСЬ резерв → UCR сбрасывался на базу → следующий extend коммитил ПО БАЗЕ
(req=0x10000 sz=0x80000), затирая живой заголовок кучи + устройство 0x26F80. Фикс = корректная NT-семантика
writeback (декоммит возвращает выровненный под-диапазон; release — весь аллок). ПРОВЕРЕНО (4 прогона,
детерминированно 462-463 строки): коммит после декоммита ложится МИМО устройства (req=0x70000/0x80000, НИКОГДА
req=0x10000), 0 коммитов-поверх-устройства, boot прошёл 171→462 строки и 3→6 ассетов (ArcadeLogo.ptc, UI.xzp
23.5MB, Strings.bin). Тулчейн/Vd*/PM4-CP/CRT/heap — всё DONE.

ЗАДАЧА СЕССИИ. Новый фронтир (тот же класс, что Update 3 — «function-boundary / jump-table»). Boot теперь
детерминированно упирается в ДВА INDIRECT-NULL (печатает их и идёт дальше → молчаливый стопор):
  [INDIRECT-NULL] target=0x8228A3B8 (caller lr=0x8228A210)  — ПРОПУЩЕННЫЙ case JUMP-TABLE.
  [INDIRECT-NULL] target=0x00000000 (caller lr=0x8224FDEC)  — null-вызов.
Цель: восстановить таблицу(ы)/закрыть null-вызов → boot идёт глубже к рендер-циклу → главный поток выходит из
GPU-completion ожиданий → VdSwap → DRAW → Plume Vulkan.

ПЕРВЫМ ДЕЛОМ ПРОЧИТАЙ:
1. Память [[sp_varianta_bootstrap]] — Update 10 (свежий, описывает фикс кучи + этот фронтир).
2. NIGHT-LOG.md — секция «✅ FIXED: ... DECOMMIT WRITEBACK» (хвост файла).

================================================================================================
ФРОНТИР 1 — JUMP-TABLE в sub_8228A208 (bctr → 0x8228A3B8)
================================================================================================
0x8228A3B8 — НЕ начало функции (нет в ppc_func_mapping.cpp) ⇒ это mid-function label, цель вычисляемого
`bctr` внутри sub_8228A208 (ppc_recomp.36.cpp:5667..8749 — большая ~3K-строчная функция; внутри ОДИН `bctr`,
в цикле). ⚠ caller lr=0x8228A210 СТАРЫЙ (bctr не ставит LR — это адрес после savegprlr-bl в прологе).

✅ ТАБЛИЦА УЖЕ ДЕКОДИРОВАНА (этап «discovery» сделан — ppc_recomp.36.cpp:5872-5891, loc_8228A390):
это 16-БИТНАЯ OFFSET-таблица (НЕ массив 32-битных адресов):
  bctr @ **0x8228A3B4**, индекс-регистр **r19**.
  `lis r12,-32243; addi r12,r12,-12488` → база offset-таблицы **0x820CCF38** (.rdata), записи u16.
  `lhzx r0, 0x820CCF38, r19*2` → 16-битный offset.
  `lis r12,-32215; addi r12,r12,-23624` → jump-base **0x8228A3B8**; цель = **0x8228A3B8 + offtab[r19]**.
  (offset 0 → 0x8228A3B8 = первый case, который XenonAnalyse и пропустил.)
ОСТАЛОСЬ: (a) прочитать offset-таблицу 0x820CCF38 из памяти и границу N (на r19) — gdb `x/<2N>hx
$g_base+0x820CCF38` (BE u16), либо найди `cmplwi r19,N; bgt default` выше loc_8228A390; (b) labels[i] =
0x8228A3B8 + offtab[i]; (c) истинный размер sub_8228A208 для sp_xenon.toml.

WORKFLOW восстановления (проверен в Update 3, README XenonRecomp «functions containing jump tables»):
1. Прочитай offset-таблицу + N (см. выше). labels[i] = 0x8228A3B8 + offtab[i].
2. Найди истинный КОНЕЦ функции (blr последнего case) если XenonAnalyse её урезал.
3. Добавь в **sp_xenon.toml** `functions = [{ address = 0x8228A208, size = <байт> }]` (override границ) и в
   **sp_switch_tables.toml** `[[switch]]` (base = 0x8228A3B4, r = 19, default = <вне-диапазона case>,
   labels = [0x8228A3B8+offtab[i] ...]). Формат labels = явные адреса (см. существующие 94 записи в toml).
4. ЧИСТЫЙ регген: `rm ppc/*.cpp` (⚠ «identical file»-оптимизация оставляет устаревшие TU!), затем XenonRecomp
   (тулчейн — свежий клон third_party/XenonRecomp на диске, не сабмодуль; точную команду см. patches/ и git-историю).
5. Пересборка: `ninja -C runtime/out sp_td_varianta`.
⚠ Если 16-бит-offset формат не поддержан XenonRecomp напрямую — задай labels ЯВНО (resolved-адреса), как в
существующих entries; XenonRecomp эмитит switch по labels, не пересчитывая offset-таблицу.

================================================================================================
ФРОНТИР 2 — null-вызов target=0x0 (caller lr=0x8224FDEC, ppc_recomp.30.cpp)
================================================================================================
Класс Update 4 (стаб вернул 0 в r3 но НЕ заполнил out-param / указатель на функцию = 0), ЛИБО ещё одна
таблица. Локализуй: bl перед 0x8224FDEC в ppc_recomp.30.cpp; что туда должно было записать ненулевой
указатель (трасса вверх, ref 1:1 = third_party/rexglue-sdk/src/). Возможно закроется тем же реггеном, что и
Фронтир 1 — проверь после него.

================================================================================================
ДИАГНОСТИКА / ПРОВЕРКА
================================================================================================
- Запуск: из varianta/ → `find /dev/shm -maxdepth 1 -name 'xenia_memory_*' -delete; REX_KTRACE=1 stdbuf -oL
  -eL timeout 22 ./runtime/out/sp_td_varianta ../private/extracted/default.xex > /tmp/boot.log 2>&1`.
- ⚠ grep по логу: лог СОДЕРЖИТ бинарные байты (имена .embsec_ секций) → grep видит его как binary и МОЛЧИТ.
  Всегда `grep -a`.
- LR теперь печатается в Nt{Allocate,Free}VirtualMemory KTRACE (оставлено — помогло поймать баг кучи).
- Прогресс мерь так: число строк (было 171 → стало 462), число ассетов `grep -ac NtCreateFile`, и НЕТ
  `grep -a "req=0x10000 sz=0x80000"` (страховка, что устройство не затёрто). Boot многопоточный, но сейчас
  детерминирован ~462 строки.
- INDIRECT-NULL логирует PPCIndirectNull (kernel.cpp) и ПРОПУСКАЕТ вызов; цели уникальны за прогон.
- gdb -batch -x SCRIPT --args ./runtime/out/sp_td_varianta ../private/extracted/default.xex. -O0: break по
  ppc_recomp.N.cpp:LINE, читать ctx.rN.u32. Гостевая память BE. Долгие прогоны уходят в фон — жди уведомления.
- Оракул prod (out/build/linux-amd64-release/south_park_td, read-only под gdb, ТА ЖЕ PPCContext-раскладка,
  base 0x100000000, PIE — break по СИМВОЛУ): доходит до рендера (54-55 Vulkan-пайплайнов). Сравнивай.

СБОРКА. Правишь varianta/runtime/*.cpp → `ninja -C runtime/out sp_td_varianta` (секунды). Для jump-table —
правишь *.toml → чистый регген ppc/ → ninja. ⚠ zsh: НЕ используй pkill/glob; чистить shm только `find
/dev/shm -maxdepth 1 -name 'xenia_memory_*' -delete`. cd внутри compound-команды может дёрнуть пермишн —
используй `ninja -C <dir>` и абсолютные пути.

ОГРАНИЧЕНИЯ. Скоуп только varianta/ (+ тулчейн через patches/xenonrecomp-sp-instructions.patch). НЕ пушить; НЕ
трогать prod-бинарь/librexruntime.so(1a3f6076)/rexglue-sdk/указатель суперпроекта. Автор superheher
<heh@vivaldi.net>; коммиты заканчивать: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>.
Хост — расходный стенд (sudo пароль <redacted>), prod не ломать.

ГОТОВО, КОГДА: (промежуточно) оба INDIRECT-NULL закрыты, boot идёт глубже (>462 строк, новые ассеты/потоки);
(далее) главный поток вышел из KeWaitForSingleObject и игра зовёт VdSwap; (цель) в окне видны кадры через
VdSwap→Vulkan.
