Ты продолжаешь проект variant A — полная статическая рекомпиляция (XenonRecomp) игры «South Park: Let's Go
Tower Defense Play!» (Xbox 360 XBLA → Linux/Vulkan). Рабочая директория:
/home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta. Ветка experimental/hle-graphics-spike.

✅ ВЫПОЛНЕНО за прошлые сессии (НЕ переоткрывай; коммиты НЕ запушены):
- Куча/«device teardown» — FIX в NtFreeVirtualMemory(MEM_DECOMMIT) writeback (commit 4e011b5).
- Jump table sub_8228A208 + инструкции vcmpbfp128/blrl (commit c10ab1a). Boot до 1191 строк.
- CRT-init thread-join ДЕДЛОК = экран бут-логотипа «нажми A/START, иначе через 5с дальше» (commit 6ecdea6).
  Расшит ДВУМЯ фиксами, ОБА проверены (детали — NIGHT-LOG «✅ CRT-init thread-join» + память Update 12):
  (1) **KeTimeStampBundle**: GetTickCount (CRT sub_82448748 = `*(*(0x820008B8)+16)` = TickCount поля
      KeTimeStampBundle, переменная-экспорт ordinal 0x00AD, плейсхолдер 0xAD000100) стоял на 0 ⇒ 5-сек.
      таймаут логотипа НИКОГДА не срабатывал. ФИКС: отдельный ~1мс host-поток (StartTimestampPump в
      SetupEnvironment) пишет uptime-ms в 0xAD000110 (как rexglue xboxkrnl_module.cpp). tid=4 ВЫШЕЛ из
      опроса логотипа.
  (2) **Кооперативный токен**: после логотипа очистка GPU-ресурсов (sub_8214F738 → sub_821C1000 →
      sub_821C6E58) ждёт GPU-fence через busy-spin **sub_821B9270** (db16cyc, без yield) — держит токен ⇒
      vblank-pump навсегда блокируется в std::mutex::lock(g_waitMutex) (gdb-подтверждено) ⇒ fence никто не
      двигает. ФИКС: сильный override слабого алиаса sub_821B9270 в kernel.cpp — отпускает токен на время
      backoff (Unlock→sleep 1мс→Lock), потом зовёт рекомпил-тело __imp__sub_821B9270; пропускает yield на
      потоке самого pump. Pump теперь крутится нормально, ring ОПУСТОШАЕТСЯ (RPTR==WPTR==37).

ЗАДАЧА СЕССИИ. **GPU command-processor: довести fence ресурса до target, чтобы очистка после логотипа
завершилась → tid=4 закончится → join главного снимется → boot пойдёт глубже.** Сейчас pump работает, ring
пуст (RPTR==WPTR==37, WPTR не растёт — игра больше не сабмитит, т.к. tid=4 всё ещё ждёт), но fence
`*(*(device+10896))` НЕ достигает target (= r30/r4 функции sub_821C6E58). Это чисто GPU-движок (мульти-
недельный фронтир): сабмиченный PM4 не доводит этот fence до target.

РЕЗЮМЕ (точка возобновления):
1. Считай адрес и значение fence. device ≈ 0x26F80 (title-heap-base 0x10000 + 0x16F80; проверь в этом
   билде). Указатель fence = `*(device+10896)` (BE), текущее значение = `*( *(device+10896) )`, target =
   r30 функции sub_821C6E58 (= её аргумент r4; см. ppc_recomp.17.cpp:12476 пролог `mr r30,r4`). Поставь
   bkpt на sub_821C6E58:12489 (вход, r3=device r4=target ещё валидны) и считай device/target/fenceptr/
   current. (⚠ bkpt на 12588 НЕ ловится — spin держит PC внутри sub_821B9270, лови на входе 12489.)
2. Сравни fenceptr с тем, что пишет pump: kReg_CP_RB_RPTR(0x7FC80710)/WPTR(0x7FC80714), g_rptrWriteBack
   (VdEnableRingBufferRPtrWriteBack), и адреса EVENT_WRITE_SHD в ExecutePM4 (kernel.cpp). Три гипотезы:
   (a) fenceptr == g_rptrWriteBack и target>WPTR(37) ⇒ игра ждёт, пока GPU съест БОЛЬШЕ, чем сабмичено —
       значит завершающий пакет так и не попал в ring (логотип не «отрисовался» null-GPU); надо, чтобы CP
       сам довёл RPtr/fence до target ИЛИ корректно обработал отрисовку логотипа.
   (b) fenceptr — отдельный fence, который пишет конкретный EVENT_WRITE, а ExecutePM4 его не туда/не пишет.
   (c) механизм fence иной (не RPtr/EVENT_WRITE). Подними семантику из rexglue command_processor.cpp.
3. Доведи fence до target в CP (это и есть GPU-движок). Сверяйся 1:1 с third_party/rexglue-sdk/src/graphics/
   command_processor.cpp + prod-оракулом (рендерит 54-55 пайплайнов, проходит эту очистку — у него реальный
   GPU двигает fence). Когда fence>=target: sub_821C6E58 выйдет, 4 очистки завершатся, tid=4 закончится,
   join снимется.

⚠ ВТОРИЧНАЯ НАХОДКА (задокументирована, НЕ фикшена): sub_821B9270 содержит 5-сек GPU **watchdog** (вернёт 0
= прервать ожидание, если fence завис), но его тик = `*(KTHREAD+0x58)`, а FillKThread пишет T+0x54/0x56/0x5C
и ПРОПУСКАЕТ T+0x58 (в rexglue X_KTHREAD это `unk_58`) ⇒ тик=0, watchdog не срабатывает. Даже если затикать
T+0x58 — watchdog уходит в sub_821C8B30 (DbgPrint + GPU hang-recovery), т.е. деградированный путь «GPU
завис», НЕ чистый. Чистый фикс — п.3 (CP двигает fence), а не watchdog.

ДРУГИЕ БЛОКЕРЫ ВПЕРЕДИ: vupkd3d128 (78× default-case __builtin_debugtrap, graphics-path) — реализовать
недостающие pack-типы в XenonRecomp/recompiler.cpp, когда дойдёт.

ДИАГНОСТИКА / СБОРКА:
- Запуск: `find /dev/shm -maxdepth 1 -name 'xenia_memory_*' -delete; REX_KTRACE=1 stdbuf -oL -eL timeout 22
  ./runtime/out/sp_td_varianta ../private/extracted/default.xex > /tmp/boot.log 2>&1`. ⚠ grep ВСЕГДА с `-a`.
- gdb attach: запусти с REX_KTRACE=0 в фоне, `sleep 8`, `gdb -batch -p <pid> -x скрипт`. Готовые: /tmp/
  {full,cp,fence,loopwatch,timer}.gdb. Прогресс: `grep -avc KeGetCurrentProcessType` (non-spam=521 на
  текущем фронтире — KeGetCurrentProcessType это безобидный 28Гц опрос рендер-воркера, фильтруй его).
  Страховки: `grep -ac INDIRECT-NULL`(=0), `grep -ac "req=0x10000 sz=0x80000"`(=0).
- Правка рантайма: `ninja -C runtime/out sp_td_varianta` (секунды). Эмиттер: edit recompiler.cpp → cmake
  build XenonRecomp → `rm ppc/*.cpp; <XR> sp_xenon.toml ppc_context.h` → ninja → regen patch
  `git -C ../../third_party/XenonRecomp diff > patches/xenonrecomp-sp-instructions.patch`.
- ⚠ zsh: чистить shm только `find /dev/shm -maxdepth 1 -name 'xenia_memory_*' -delete` (не pkill/glob).
- Оракул prod: out/build/linux-amd64-release/south_park_td (Release/no-ctx, break по СИМВОЛУ, PIE base
  0x100000000) — рендерит, проходит очистку логотипа; сравнивай fence/CP.

ОГРАНИЧЕНИЯ. Скоуп только varianta/ (+ тулчейн через patches/xenonrecomp-sp-instructions.patch). НЕ пушить;
НЕ трогать prod-бинарь/librexruntime.so(1a3f6076)/rexglue-sdk/указатель суперпроекта. Автор superheher
<heh@vivaldi.net>; коммиты заканчивать: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>.
Хост — расходный стенд (sudo пароль <redacted>), prod не ломать.

ГОТОВО, КОГДА: (промежуточно) fence ресурса достигает target, sub_821C6E58 выходит, tid=4 завершается, join
главного снят, boot глубже (>1191 строк, новые ассеты/события); (далее) главный зовёт VdSwap; (цель) кадры
через VdSwap→Vulkan.
