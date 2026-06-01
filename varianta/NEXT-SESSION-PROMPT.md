Ты продолжаешь проект variant A — полная статическая рекомпиляция (XenonRecomp) игры «South Park: Let's Go
Tower Defense Play!» (Xbox 360 XBLA → Linux/Vulkan). Рабочая директория:
/home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta.

ЗАДАЧА СЕССИИ. Починить БАГ ПОРЧИ ГОСТЕВОЙ КУЧИ (root-caused в этой сессии), из-за которого аллокация
буфера вершинных потоков .ptc возвращает блок НИЖЕ базы кучи, затирающий структуру GPU-устройства. После
фикса — к кадрам (главный поток выходит из ожиданий GPU-completion → VdSwap → DRAW → Plume Vulkan).

⚠ ВАЖНО: ПРЕДЫДУЩАЯ ВЕРСИЯ ПРИЧИНЫ БЫЛА НЕВЕРНОЙ. Старый NEXT-SESSION-PROMPT и NIGHT-LOG «ROOT FOUND:
.ptc vertex-stream destinations are UN-RELOCATED (надо прибавить 0xA2016000)» — ОШИБКА. Реальная причина
найдена и проверена в gdb (см. ниже).

ПЕРВЫМ ДЕЛОМ ПРОЧИТАЙ (точная точка возобновления):
1. Память ~/.claude/projects/-home-h-src-recomp/memory/sp_varianta_bootstrap.md — **Update 9** (READ FIRST;
   Update 8 ОТМЕНЁН Update'ом 9).
2. varianta/NIGHT-LOG.md — секция (хвост файла) «⛔ PRIOR ROOT-CAUSE CORRECTED: HEAP FREE-LIST corruption».
3. varianta/runtime/kernel.cpp:78 — inline-NOTE с кратким резюме.

================================================================================================
РЕАЛЬНАЯ ПРИЧИНА (проверено в gdb)
================================================================================================
*(obj+192)=0x82B0 — это НЕ файловое смещение, а ВОЗВРАТ АЛЛОКАТОРА. `sub_822A8150` (зовётся в
sub_822A2158:23167, прямо перед sub_822A7C08) аллоцирует буфер потоков:
`sub_82448090(size=0x78000, flags=0x24870000)` → пишет в obj+192. Эта аллокация ВОЗВРАЩАЕТ **0x82B0** —
адрес НИЖЕ базы кучи тайтла (0x10000), перекрывающий живое устройство на 0x26F40. (Объект — это
стек-структура в кадре sub_822A2158, ~0x980AF7B0, т.к. стеки воркеров в окне 0x98000000; на +0 лежит магия
"PTC+" из заголовка файла — это и сбило прошлую сессию на «файловые смещения».)

ЦЕПОЧКА АЛЛОКАТОРА: sub_82448090(flags bit0=0) → sub_8244DF68(0,sz) → sub_8244CDF0 (единый хэндл кучи =
*(0x82902448)=0x10000) → **sub_8244B950** (классическая NT-куча, сигнатура 0xEEFFEEFF на heap+0x10).
Запрос 0x78000 → индекс 0x7800; heap+0x28 VirtualMemoryThreshold=0xF000, 0x7800<порога ⇒ поиск по
**free-list[0]** (loc_8244BBFC→loc_8244BC08), НЕ virtual-alloc и НЕ sub_8244ADD8 (проверено по bt).

ФАНТОМНЫЙ БЛОК (улика, /tmp/narrow2.gdb — break ppc_recomp.65.cpp:7704 при ctx.r3.u32 in [0x1000,0x10000)):
обход free-list[0] ВЫБИРАЕТ сфабрикованный блок r3=**0x82A0** (= heapbase 0x10000 − 0x7D60), заголовок
(BE u32): +0=0x80000000 (поле размера 0x8000 *16-байтных* единиц = 0x80000 Б ≥ запроса → обход принимает),
+4=0x00100000 (похоже на размер резерва кучи), +8=0x00010180 (Flink = sentinel free-list[0] heap+0x180),
+0xC=0x000582A8 (Blink → **в буфер загрузки .ptc**: файл-буфер=0x50000, 0x582A8=0x50000+0x82A8). Т.е.
**free-list[0] испорчен фантомным «свободным» блоком ниже базы кучи, чья ссылка указывает в .ptc-буфер**;
аллок 0x78000 переиспользует его → массив назначений 0x82B0+i*0x3C00 → записи #8/#9 (0x262B0/0x29EB0)
попадают в устройство → device+10900=0 → ожидания GPU-completion (main 0x388C4; render sub_821CC5D0
0x29CD4/0x29D40) навсегда встают.

НЕ ЗАВИСИТ ОТ БАЗЫ (поэтому фикс — НЕ смена layout): g_virtNext 0x10000→блок 0x82B0; 0x40000000 (как у
prod; куча тайтла тогда ложится РОВНО как у prod — устройство-struct=0x40016F80, адрес из prod-лога
«SetInterruptCallback(821C7170,40016F80)»)→блок 0x3FFF82B0=base−0x7D50, ВСЁ РАВНО перекрывает устройство.
Базу вернул на 0x10000. Совпадение устройства с точным адресом prod при базе 0x40000000 ДОКАЗЫВАЕТ, что
куча variant A синхронна prod ДО устройства ⇒ расхождение — чисто в free-list после аллокации устройства.

================================================================================================
ПЛАН ФИКСА
================================================================================================
1. Поймать ВСТАВКУ фантомного блока. Он линкуется В СЕРЕДИНЕ цепочки (watchpoint только на sentinel
   heap+0x184 его НЕ поймал). Варианты:
   (a) Инструментировать RtlFreeHeap (sub_82448128) + коалесинг: логировать каждый (block, size,
       prev-size, новые ссылки free-list) и пометить ПЕРВУЮ ссылку ниже base кучи + гостевой LR.
   (b) PROD-DIFF: prod использует ТУ ЖЕ ABI `__imp__sub_XXX` и ИМЕЕТ символы кучи
       (nm out/build/linux-amd64-release/south_park_td: sub_82448090/sub_8244B950/sub_8244DF68). Логировать
       последовательность операций кучи (alloc/free размеры+результаты ОТНОСИТЕЛЬНО базы) в обоих и найти
       ПЕРВУЮ расходящуюся операцию после аллокации устройства. ⚠ prod=Release (нет ctx) → читать гостевую
       память по membase; много breakpoint'ов на горячих функциях кучи роняют prod.
2. ДВА ГЛАВНЫХ ПОДОЗРЕВАЕМЫХ (класс NtQueryInformationFile — значение/состояние, не codegen в общем):
   (a) Ошибка ЭМИТТЕРА XenonRecomp в пути free/coalesce кучи (sub_82448128/RtlFreeHeap, backward-coalesce
       через поле PreviousSize блока) — prod собран ДРУГИМ рекомпилятором (rexglue), так что баг эмиттера
       только-в-variant-A разойдётся.
   (b) Семантика variant-A NtAllocateVirtualMemory/NtQueryVirtualMemory: трекается ОДИН VRegion на резерв и
       возвращается состояние commit на ВЕСЬ регион (не по-страничный) — куча может неверно отслеживать
       committed/uncommitted диапазоны и коалесить с ошибкой. См. kernel.cpp NtAllocate/NtQuery/NtFree.
3. Исправить расхождение HLE/эмиттера → аллок 0x78000 ложится в валидное место кучи → устройство не порчено.
4. Проверить: device+10900 НЕ обнуляется; boot идёт глубже.

КЛЮЧЕВЫЕ АДРЕСА/ФУНКЦИИ:
- Устройство = g_interruptData = 0x26F80 (блок 0x26F40, 24KB). Хэндл кучи = *(0x82902448) = 0x10000.
- sub_822A8150 (ppc_recomp.38.cpp:14077; аллок +192 на :14464-14466) ← sub_822A2158 (ppc_recomp.37:23167).
- sub_8244B950 (ppc_recomp.65.cpp:7244; free-list[0] поиск loc_8244BBFC:7611, выбор блока :7704).
- sub_82448090 (ppc_recomp.64.cpp:17230), sub_8244DF68 (ppc_recomp.65.cpp:14904).

ИНСТРУМЕНТЫ (готовые gdb-скрипты в /tmp/ — переиспользуй/перепиши):
- trace.gdb (путь через sub_822A8150), narrow2.gdb (ловит блок ниже базы + заголовок + bt — РАБОТАЕТ),
  heaplog.gdb (карта alloc size/flags/result), heapdump.gdb (дамп struct кучи), heapfl.gdb/watchfl.gdb
  (watchpoint'ы). REX_KTRACE=1 (трасса импортов), читать g_base свежим в python (rd() с per-call bswap).
- ⚠ break на sub_8244B950 (горячая, на КАЖДЫЙ alloc) с python-stop() ТОРМОЗИТ так, что до .ptc не доходит
  за 150с. Используй БЫСТРЫЙ conditional break (как narrow2: ранний return False, фильтр r3<0x10000).

СБОРКА/ЗАПУСК/ОТЛАДКА:
- Правишь varianta/runtime/kernel.cpp; сборка: cd varianta/runtime/out && ninja sp_td_varianta (секунды).
  Запуск ИЗ varianta/: REX_KTRACE=1 timeout 25 ./runtime/out/sp_td_varianta ../private/extracted/default.xex.
- ⚠ zsh: НЕ используй pkill/glob (`/dev/shm/xenia_memory_*`) — несовпадение даёт ненулевой код и рушит
  команду. Чистить shm: `find /dev/shm -maxdepth 1 -name 'xenia_memory_*' -delete`. Заканчивай команды `; true`.
- gdb -batch -x SCRIPT --args ./runtime/out/sp_td_varianta ../private/extracted/default.xex. Долгие прогоны
  уходят в фон — жди уведомления. -O0: break по ppc_recomp.N.cpp:LINE, читать ctx.rN.u32. Гостевая память BE.

ОГРАНИЧЕНИЯ. Скоуп только varianta/ (+ тулчейн через patches/xenonrecomp-sp-instructions.patch). НЕ пушить; НЕ
трогать prod-бинарь/librexruntime.so(1a3f6076)/rexglue-sdk/указатель суперпроекта. Автор superheher
<heh@vivaldi.net>; коммиты заканчивать: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>.
Хост — расходный стенд (sudo пароль <redacted>), prod не ломать.

ГОТОВО, КОГДА: (промежуточно) аллок 0x78000 .ptc возвращает валидный блок В пределах кучи, device+10900 не
обнуляется, boot идёт глубже; (далее) главный поток вышел из KeWaitForSingleObject и игра зовёт VdSwap;
(цель) в окне видны кадры через VdSwap→Vulkan.
