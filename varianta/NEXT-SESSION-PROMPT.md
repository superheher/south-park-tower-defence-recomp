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
2. **VdSwap → первый кадр.** Сейчас `VdSwap` = stub (возвр. 0), вызовов 0. Когда тайтл выйдет из intro и
   позовёт VdSwap — это путь к реальному present (Vulkan, фаза рендерера).
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
