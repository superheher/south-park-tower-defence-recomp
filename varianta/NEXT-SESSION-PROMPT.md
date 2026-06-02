‼️‼️‼️ STATUS 2026-06-02 (cont.10) — ✅✅ ПРОРЫВ: справедливый планировщик + per-frame yield → ИНТРО-ФИЛЬМ ДЕКОДИРУЕТСЯ.
Полностью: NIGHT-LOG «2026-06-02 (cont. 10)». Кратко:
- СРЕДА ВОССТАНОВЛЕНА. cont.9 винил RAM — НЕВЕРНО. Реальная причина: **/tmp = tmpfs с per-user КВОТОЙ (usrquota,
  12791 МБ), забита.** Два дампа cont.7-gdb — /tmp/movieopen_out.txt (6.5 ГБ) + /tmp/moviestart_out.txt (5.6 ГБ).
  harness пишет per-команду `pwd >| /tmp/...cwd` → при полной квоте падает → Bash exit 1 / нет stdout. Фикс: rm
  этих двух файлов (квота 12791→122 МБ) → capture восстановлен. ⚠НЕ оставляй многогиговые дампы в /tmp; большие
  логи — в проект-дир или ограничивай размер.
- СБОРКА: «unbuilt» из cont.9 было перестраховкой — изменение УЖЕ собрано (13:43). Пересобрал начисто (EXIT=0),
  `nm` подтверждает override: `T sub_82167248` + `U __imp__sub_82167248`.
- ТЕСТЫ (все `timeout -s KILL`, /dev/shm чистится): (1) **дефолт-бут НЕ регрессит** (109к строк, 0 утечки
  fair-режима, 0 device-overwrite, без краша). (2) **REX_FAIRSCHED=1: планировщик-фикс РАБОТАЕТ** — tid=10
  (sub_82250420) + декодеры (0x82339428/58/88) теперь СТАРТУЮТ + бегут (раньше голодали вечно); но sub_8210AF90
  всё ещё НЕ зовётся (sub_8211B740 дивергирует до него → 0x828E82A6=0 → intro не авто-переходит). (3) **🎯
  A/B ДЕКОДЕРА (`[video] LATE swap#220`, та же сборка, отличие = REX_FAIRSCHED):** baseline = все 16 буферов
  `nz=0 varied=0` (чёрные); REX_FAIRSCHED=1 = buf4-11 `varied=8552..64589` ⇒ **ФИЛЬМ ДЕКОДИРУЕТСЯ.** ⇒ декодер
  БЫЛ ГОЛОДНЫЙ (не «застрял»/«gated») — это ОПРОВЕРГАЕТ вывод cont.6/cont.8 и подтверждает Step-1. (4)
  **РЕГРЕССИЯ ДЕТЕРМИНИЗМА (предупреждение cont.9):** REX_FAIRSCHED ✗ REX_XFLAG — рабочая cont.7-комба
  REX_XFLAG=1 REX_MOVIE_EOF=30 даёт attract+меню за 161к строк, а +REX_FAIRSCHED СТОПОРИТ до 2534 строк
  (attract=0, не выходит из intro; жив, но ~64× медленнее). REX_FAIRSCHED ОТДЕЛЬНО — ОК (42к строк). ⚠**НЕ
  комбинируй REX_FAIRSCHED с REX_XFLAG.** XFLAG был костылём для застрявшего фильма, который fair sched чинит
  по-настоящему ⇒ путь вперёд = ЕСТЕСТВЕННЫЙ movie→EOF→advance, не XFLAG.
- РЕШЕНИЕ: **ЗАКОММИЧЕНО** (ветка «works → big Step-1 win»). Fair scheduler (class FairMutex FIFO-токен + fair
  object-waits в SignalObject/WaitObject/KeWaitForMultipleObjects/NtWaitForMultipleObjectsEx) + per-frame yield
  (sub_82167248) + REX_INITDIAG диаг — всё за REX_FAIRSCHED/REX_INITDIAG, дефолт не тронут. Откатываемо (НЕ пушено).
- СЛЕДУЮЩЕЕ (по приоритету): **(a)** RE дивергенции sub_8211B740 (718-стр. хэндлер) → довести до sub_8210AF90 →
  0x828E82A6 → ЕСТЕСТВЕННЫЙ переход intro→меню (ретайрит REX_XFLAG; это «правильный фикс» из cont.8). **(b)**
  ПРЕЗЕНТОВАТЬ декодированные кадры — декодер пишет реальные кадры в пул (buf4-11); подключи декод-сурфейс к
  VdSwap/rex_render present → ВИДИМЫЙ интро-фильм (тот «decoded-frame shortcut» из cont.4, заброшенный лишь
  потому что не было кадра — теперь есть). **(c)** (ниже) убрать стопор fair+forced-transition, если XFLAG ещё
  нужен. ЗАПУСК для декод-работы: `REX_FAIRSCHED=1 REX_MOVIE_EOF=30` (+REX_INITDIAG для tid-диага); `[video]`
  диаг печатает на swap#220 (g_ktrace, дефолт-он).

———————————————————————————— (ниже — cont.9 статус) ————————————————————————————

‼️‼️‼️ STATUS 2026-06-02 (cont.9) — STEP 1 (СПРАВЕДЛИВЫЙ ПЛАНИРОВЩИК) НАЧАТ + НАСТОЯЩИЙ КОРЕНЬ найден; ⚠СРЕДА СЛОМАЛАСЬ.
Полностью: NIGHT-LOG «2026-06-02 (cont. 9)». Кратко:
- РЕАЛИЗОВАН справедливый FIFO кооп-токен (class FairMutex, gated REX_FAIRSCHED; дефолт-путь БЕЗ изменений):
  run-token g_tok отделён от object-waits (g_objM/g_objCv). ПРОВЕРЕНО (до поломки среды): дефолт-бут не
  регрессит; REX_FAIRSCHED=1 БУТИТСЯ СТАБИЛЬНО (доходит до intro, без краша/дедлока). Ядро токена корректно.
- НАСТОЯЩИЙ КОРЕНЬ (gdb): справедливость НЕ помогает — tid=10 (sub_82250420) и ДЕКОДЕРЫ всё равно не бегут.
  Лог GuestThreadRun: tid=10 «WAITING (serving=1305 held=1)», но никогда «GOT token». ⇒ ГЛАВНЫЙ ТРЕД держит
  токен (тикет 1305) и крутит intro-цикл НЕ ОТПУСКАЯ его: fence-forward делает его GPU-вейты мгновенными →
  не блокируется → не зовёт FairWaitUntil → не релизит токен. serving застрял на 1305 → НИКТО другой не бежит,
  НЕЗАВИСИМО от справедливости. ⇒ нужен кооп-TIME-SLICING: периодический yield главного.
- ТЕСТИРОВАЛ per-frame yield в sub_82167248 (gated g_fair) — НО сборка/тест НЕ ЗАВЕРШЕНЫ: СРЕДА ДЕГРАДИРОВАЛА
  (Bash stdout-capture сломался: даже `echo` даёт exit 1 / пусто). Причина: ~15 прогонов тайтла, `timeout`
  шлёт SIGTERM, тайтл его ИГНОРИРУЕТ → орфаны по 4 GiB mmap + /dev/shm → исчерпание RAM. ⚠ВСЕГДА `timeout -s KILL`.
- СОСТОЯНИЕ: рабочее дерево kernel.cpp = НЕЗАКОММИЧЕННЫЕ изменения (fair sched + per-frame yield + REX_INITDIAG
  диаг, всё gated; дефолт не затронут). per-frame yield НЕ СОБРАН/НЕ ПРОТЕСТИРОВАН. Последний коммит b967e92 =
  чистый kernel.cpp (176b54d). СЛЕДУЮЩЕЕ: (1) перезапустить среду/освободить RAM (pkill -9 -x sp_td_varianta;
  rm /dev/shm/xenia_memory_*); (2) собрать kernel.cpp; (3) тест REX_FAIRSCHED=1 REX_INITDIAG=1 REX_MOVIE_EOF=30
  (без REX_XFLAG): даёт ли per-frame yield tid=10 «GOT token» → sub_8210AF90 (флаг) + бегут ли декодеры (декод
  фильма!) + проходит ли intro? (4) работает → большой Step-1 выигрыш (но следи за регрессией детерминизма, как
  показал yield-on-resume); не работает → корень = fence-forward стопгап (заменить настоящим fence, чтобы
  главный реально блокировался). (5) собрать-проверить → коммит или откат.

———————————————————————————— (ниже — cont.7 статус) ————————————————————————————

‼️‼️ STATUS 2026-06-02 (cont.7) — INTRO-ХЭНГ СНЯТ: тайтл теперь ПРОХОДИТ intro→attract→menu-setup.
Полностью: NIGHT-LOG секция «2026-06-02 (cont. 7) — intro→menu transition RE'd + FORCED». Кратко:
- Переход intro→меню = РАСШИФРОВАН. Каждый кадр движок зовёт AdvanceFrame фильма `sub_8232AAE0`; на EOS
  он возвращает **0x16660026** (норма в varianta — 0x166600E8 «нет кадра», декодер застрял НАВСЕГДА), тогда
  `sub_82425BF8` ПОСТИТ completion (`sub_8222A9F8`, канал 0xAAC0CCDD), который обрабатывает screen-машина
  `sub_82161920`(state==2)→`sub_82163118` (= advance в меню). prod-оракул подтверждает (фильм ~534 кадра/~22с,
  потом этот путь рвёт фильм и переключает; ⚠ current-screen `*(0x828EAB18+12)` НЕ меняется — это под-стейт).
- ДВА гейта, оба форсируются (env-gated рычаги в kernel.cpp, по умолчанию OFF, дефолт-бут НЕ регрессит):
  **REX_MOVIE_EOF=N** (форс EOS после N кадров фильма; иначе фильм крутится вечно — доказано 44820→30 advances),
  **REX_XFLAG=1** (форс глобал-байт **0x828E82A6** «transitions-enabled», в prod его ставит `sub_8210AF90`, в
  varianta =0 → `sub_82163118` НИКОГДА не зовётся). REX_SKIPINTRO теперь шлёт **VK_PAD_START через
  XamInputGetKeystrokeEx**.
- РЕЗУЛЬТАТ (REX_MOVIE_EOF=30 REX_XFLAG=1 REX_SKIPINTRO=1): тайтл выходит из intro в **attract-петлю**
  (intro↔towerDefense_attract_movie.wmv, засимлинкан в Movies/en-en/), затем в **menu/frontend-setup**
  (screen-машина `sub_82150770→sub_8215DBD0`, MmAllocatePhysicalMemoryEx меню-буферов) → упирается в
  **[INDIRECT-NULL] target=0xFFFFFFFF @ sub_8215DE84** (нулевой vtable/jump-table слот screen-setup).
- СЛЕДУЮЩЕЕ: (1) расшить INDIRECT-NULL 0xFFFFFFFF@sub_8215DE84 (восстановить таблицу/метод по workflow Update-3)
  → живое меню; (2) [cont.8 — РАССЛЕДОВАНО] правильный фикс REX_XFLAG = РАБОТА ПЛАНИРОВЩИКА (Step 1), не one-liner.
  ⚠ prod под gdb: `handle SIGSEGV nostop noprint pass` (write-watch), prod base=0x100000000.

‼️ cont.8 (2026-06-02, БЕЗ изменений кода — HEAD=176b54d): почему `sub_8210AF90` (единственный, кто ставит
0x828E82A6) не зовётся в varianta. ОТВЕТ: КООПЕРАТИВНЫЙ ПЛАНИРОВЩИК ГОЛОДАЕТ воркер (давний Step 1). 0x828E82A6 —
one-time флаг, ставит только sub_8210AF90 (teardown глобал-объекта 0x828E8AF8, как side-effect включает переходы),
зовётся косвенно воркером tid=10: sub_82450FD0(трамплин)→sub_82250420(work-loop: ждёт sub_8244DC18 →
*(workobj=0x828E8BB0)->vtable[0]=sub_8211B740 → sub_8210AF90). varianta СОЗДАЁТ+РЕЗюмит tid=10
(ExCreateThread start=0x82250420 SUSPENDED, NtResumeThread), НО он НИКОГДА не исполняет свой guest-entry — bt
всех тредов: 3 резюмленных треда залипли на GuestThreadRun:804 (g_waitMutex.lock = захват единого токена); main
в плотном intro-цикле держит/перехватывает токен, tid=10 резюмлен поздно → голодает. Band-aid (yield токена в
NtResumeThread): tid=10 ТОГДА стартует+обрабатывает work (sub_8211B740 бежит), НО (a) sub_8211B740 ВСЁ РАВНО не
доходит до sub_8210AF90 (ещё одна дивергенция состояния в 718-строчной init-функции) И (b) РЕГРЕССИРУЕТ
существующий REX_XFLAG-advance (ломает кооп-детерминизм). ОТКАЧЕНО. ⇒ правильный фикс = СПРАВЕДЛИВЫЙ планировщик
(Step 1 fibers) + RE состояния sub_8211B740, многосессионно; REX_XFLAG остаётся рабочим стопгапом (точно
воспроизводит side-effect флага sub_8210AF90).

———————————————————————————— (ниже — старый контекст fence/renderer) ————————————————————————————

‼️ STATUS 2026-06-01 (после fence-forward): boot ДОХОДИТ ДО intro-фильма и ЗАПУСКАЕТ его, но упёрся в
GPU-РЕНДЕРЕР. Подтверждено: (1) present-путь РАБОТАЕТ — VdSwap зовётся каждый кадр (из sub_821BFF48),
презентит последовательные фреймбуферы (улучшил VdSwap: ++g_gpuCounter). (2) intro-фильм
sp_xbox_0_intro.wmv грузится целиком (8.4MB), плеер инициализируется (criticalsections, GPU-память, 4
decoder-треда tid 11-14 @0x82339428). (3) НО фильм НЕ доигрывает: 180с прогон = 1.1M строк, но НЕ вышел из
intro (фильм не закрыт, меню не грузится) ⇒ это НЕ медленный декод, а РЕНДЕРЕР-gated: плееру нужно реально
рисовать/показывать кадры. Наш CP — минимальный PM4-интерпретатор (draws=no-op). **Фронтир бут-блокеров
ИСЧЕРПАН; дальше нужен настоящий PM4→Vulkan рендерер** (GPU-state из SET_CONSTANT, Xenos-шейдеры→SPIR-V,
DRAW_INDX→Vulkan draw, текстуры/RT, swapchain на VdSwap). Это ровно то, что есть в rexglue
command_processor + Vulkan backend. ОПЦИИ: (a) портировать минимальный рендерер; (b) интегрировать
rexglue Vulkan backend/Plume в рантайм varianta; (c) fake-skip intro (как fence-forward) — гонять ДАЛЬШЕ
title-ЛОГИКУ (меню/геймплей) для покрытия рекомпа без картинки. Это многонедельная фаза — РЕШЕНИЕ ЧЕЛОВЕКА.
Ниже — детали предыдущего фронтира (fence) для контекста.

Проект variant A — полная статическая рекомпиляция (XenonRecomp) «South Park: Let's Go Tower Defense
Play!» (Xbox 360 XBLA → Linux/Vulkan). Рабочая директория:
/home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta. Ветка experimental/hle-graphics-spike.
Коммиты НЕ пушить.

✅ ВЫПОЛНЕНО за прошлые сессии (НЕ переоткрывай):
- Куча/«device teardown», jump table sub_8228A208, инструкции vcmpbfp128/blrl, CRT-init thread-join
  (таймаут бут-логотипа: KeTimeStampBundle tick + token-yield) — всё расшито.
- **GPU CP fence frontier — РАСШИТ (СТОПГАП, последний коммит varianta):** CRT-init join СНЯТ, **игра дошла
  до ГЛАВНОГО ЦИКЛА** (intro). Полностью — NIGHT-LOG секция «✅ CRT-init join LIFTED … GAME MAIN LOOP».
  - Причина: тайтл строит командные сегменты (fence += 2 каждый; builder sub_821C6A08 пишет
    `device+10908 = fence+2` = head), но киккает в ring (CP_RB_WPTR 0x7FC80714, sub_821C6600) только часть
    (head=17, а в ring только fences 3,5; sub_821C6600 фукнул 6× и встал). Остальное «авто-флашит» реальная
    GPU. Наш CP исполняет только заккиканное → старые fence (target<head) недостижимы → fence-spin навечно.
  - Фикс (СТОПГАП, kernel.cpp): forward GPU-fence маркера до запрошенного target в 2 хитнутых вейтерах:
    `sub_821C6E58` (счётчик, `*(device+10896)` = 0xA2010000) и `sub_821C5DF0` (post-frame segptr,
    `*(fenceptr+4)`, низкие 2 бита = wrap-генерация). CP синхронный и БЕЗ рендерера ⇒ у дефер-сегмента нет
    эффекта на вейтер кроме самого fence (draw/state — no-op). **0 device-overwrite, без краша.**
  - ⚠ Всего 6 spin-сайтов sub_821B9270: sub_821BFF48 / C5DF0 / C6420 / C6E58 / CB690 / CC140. Хитнуты пока
    2. Если встанет на новом — добавь аналогичный forward (каждый знает свой target в r4/r5).
  - ⚠ Стопгап driving sub_821C6D58 (флаш тайтла из вейта) НЕ работает: двигает head, но НИЧЕГО не киккает
    (нужен GPU↔CPU WAIT_REG_MEM handshake, см. п.3). Поэтому форвардим маркер напрямую.

ЗАДАЧА СЕССИИ (по приоритету):
1. **Отсутствует intro-фильм (КОНТЕНТ, не баг рекомпа).** Цикл крутится на
   `NtCreateFile('game:\Media\Assets\Movies\en-en\sp_xbox_0_intro.wmv')` → **MISS** (каждый кадр). В
   `private/extracted/media/Assets/Movies/` есть все Level1-11, но НЕТ подкаталога `en-en/` и нет
   `sp_xbox_0_intro.wmv` (локализованные intro-фильмы не распакованы). Варианты:
   (a) найти/распаковать en-en-набор из XBLA-пакета (`/home/h/src/recomp/South_Park_..._XBLA.zip`) и
       положить по пути; (b) сделать корректный пропуск intro при отсутствии фильма — посмотри
       `NtCreateFile` в kernel.cpp: какой STATUS возвращает MISS; тайтл его, видимо, считает retry-able
       (крутит вместо skip). Цель — выйти из intro в меню/геймплей.
2. **PRESENT/RENDER путь (главный технический фронтир).** Замерено: после init **главный RING МЁРТВ** —
   `WPTR=37`, `ExecuteRing` срабатывает 1 раз, `XE_SWAP`=0 за весь ран. `VdSwap` = stub (0 вызовов). Т.е.
   тайтл крутит цикл (main активен в sub_821D5CA8, грузит ассеты, intro-стейт-машина) но НИ киккает ring,
   НИ зовёт VdSwap → нет отображаемой GPU-работы. ⚠ВАЖНО: `VdGetSystemCommandBuffer` возвращает плейсхолдер
   0xBEEF0000/1 — но это НЕ баг: **prod (рендерит!) возвращает ТЕ ЖЕ 0xBEEF0000/1** (rexglue
   xboxkrnl_video.cpp:340). Значит system command buffer — НЕ дифференциатор. Реальный вопрос: почему
   тайтл (даже с форварднутыми fence) больше НЕ киккает ring и не зовёт VdSwap, а prod — зовёт? Скорее
   всего тайтл решает киккать/презентить по СОСТОЯНИЮ GPU (fence/RPTR/swap-counter прогресс через реальное
   исполнение дефер-сегментов + vblank), которое наш форвард не воспроизводит целиком. СЛЕДУЮЩИЙ ШАГ =
   СРАВНЕНИЕ С PROD-ОРАКУЛОМ: запусти out/build/linux-amd64-release/south_park_td под gdb, bkpt на
   sub_821C6600 (kick), VdSwap, sub_821D5CA8; посмотри, как prod-тайтл презентит на intro (киккает ли ring
   дальше? зовёт ли XE_SWAP/VdSwap? какие fence/swap-counter он видит?) — и воспроизведи это состояние.
3. **(ДОЛГО) Чистый CP вместо стопгапа.** Заменить fence-forward на непрерывный CP, который идёт по
   WAIT_REG_MEM-цепочке дефер-IB и ИСПОЛНЯЕТ их (fence двигается как РЕЗУЛЬТАТ). ОБЯЗАТЕЛЬНО до реального
   рендерера, иначе дефер-draw'ы пропадут. Хвост ring (IB 0x975E0) содержит WAIT_REG_MEM на адреса
   0x2011xxx с код-адресом 0x821CC7A0 = и есть GPU↔CPU handshake, который надо смоделировать.

ДИАГНОСТИКА / СБОРКА:
- Запуск: `find /dev/shm -maxdepth 1 -name 'xenia_memory_*' -delete; REX_KTRACE=1 stdbuf -oL -eL timeout 22
  ./runtime/out/sp_td_varianta ../private/extracted/default.xex > /tmp/boot.log 2>&1`. ⚠ grep ВСЕГДА `-a`.
- Прогресс: `grep -avc KeGetCurrentProcessType` (на intro-цикле >170k за 30с — это норм, цикл крутит кадры
  без vsync, т.к. fence-вейты мгновенные). Страховка: `grep -ac 'req=0x10000 sz=0x80000'` (=0
  device-overwrite). Логи фикса: `[fencefwd]` под REX_KTRACE=1; `[cp] T3 op=...` (полные PM4-опкоды) под
  REX_CPTRACE=1.
- gdb: запусти REX_KTRACE=0 в фоне, `sleep ~10`, `gdb -batch -p <pid> -x /tmp/allbt.gdb` (готовый скрипт:
  info threads + thread apply all bt). Рекомпил-функции видны как `__imp__sub_XXXX` — стек читается прямо.
- Правка рантайма: `ninja -C runtime/out sp_td_varianta` (секунды). Эмиттер (если нужно): edit
  recompiler.cpp → cmake build XenonRecomp → `rm ppc/*.cpp; <XR> sp_xenon.toml ppc_context.h` → ninja →
  `git -C ../../third_party/XenonRecomp diff > patches/xenonrecomp-sp-instructions.patch`.
- ⚠ zsh: чистить shm только `find /dev/shm ... -delete` (не pkill/glob). Bash-tool сбрасывает cwd —
  используй абсолютные пути или cd в начале каждого вызова.
- Оракул prod: `out/build/linux-amd64-release/south_park_td` (рендерит 54-55 пайплайнов, проходит intro;
  символы есть: `__imp__sub_821C6E58` и т.д.) — сравнивай поведение fence/CP.

ОГРАНИЧЕНИЯ. Скоуп только varianta/ (+ тулчейн через patches/xenonrecomp-sp-instructions.patch). НЕ пушить;
НЕ трогать prod-бинарь / librexruntime.so(1a3f6076) / rexglue-sdk / указатель суперпроекта. Автор superheher
<heh@vivaldi.net>; коммиты заканчивать: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>.
Хост — расходный стенд (sudo пароль <redacted>), prod не ломать.

ГОТОВО, КОГДА: (промежуточно) intro пройден → меню/геймплей; (далее) тайтл зовёт VdSwap; (цель) кадры через
VdSwap → Vulkan present на экране.
