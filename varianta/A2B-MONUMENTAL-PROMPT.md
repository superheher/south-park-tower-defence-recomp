# MONUMENTAL BUILD — break the A↔B wall (live composited render)

> Миссия: заставить variant A ИСПОЛНЯТЬ реальный per-frame фрейм титла (реальная геометрия + transform'ы + текстуры
> + DRAW_INDX) и отдавать НАСТОЯЩИЙ GPU-completion, чтобы титл прошёл свой A↔B-гейт → живое интерактивное меню (и
> дальше gameplay) рисуется на собственном Vulkan variant A в game-accurate позициях.
>
> Это multi-week билд. Веди его как cont.N /loop-итерации: одно измерение/изменение → build → run → analyze →
> NIGHT-LOG + NEXT-SESSION-PROMPT + memory → commit (НЕ пушить) → next. ИЗМЕРЯЙ, не спекулируй.

## 0. Прочитать ПЕРВЫМ делом
- `varianta/NEXT-SESSION-PROMPT.md` (верх) + `varianta/NIGHT-LOG.md` секции **cont.34 / cont.59 / cont.73 / cont.22**.
- Память: `[[sp_varianta_loader_not_wall]]` (топик-файл), индекс-строка «SP variant A — DEEP RENDERER BUILD».
- `varianta/MORNING-REPORT-2026-06-07.md` — что уже работает (контент рендера).

## 1. Что УЖЕ сделано (cont.63–75, не повторять — это решено)
variant A декодит и рисует РЕАЛЬНЫЙ контент титла из working-буферов, всё guest-side:
- Полная boot-последовательность в реальных позициях (Microsoft → SP Digital Studios → Comedy Central → doublesix).
- Декод всех ассетов меню/level-select (портреты Stan/Kyle/Cartman 215×110, лого, иконки, баннеры, текст-атласы).
- Читаемый текст меню (REX_TEXTRENDER: глиф-квады по динамическому font-атласу) — START GAME/OPTIONS/LOBBY/JOIN/…
- Примитивы готовы: декодер текстур (rex_texture.h), screen-space sprite-геометрия, текст-рендер, compose-в-позицию.

## 2. СТЕНА (A↔B), точно и доказанно
- Титл строит РЕАЛЬНЫЙ per-frame рендер (реальная геометрия + World/Proj transform через SET_CONSTANT reg 0x4000 +
  текстуры reg 0x4800 + DRAW_INDX) в командный поток и ГЕЙТИТ продвижение на НАСТОЯЩИЙ GPU-completion (fence, который
  GPU выставляет ПОСЛЕ рендера). У variant A нет реального рендера → нет реального completion → титл висит на
  placeholder/intro-состоянии. Это ЦИКЛ (cont.21/34): титлу нужны реальные GPU-результаты чтобы продвинуться;
  variant A нужно чтобы титл продвинулся (кикнул реальный контент) чтобы дать реальные GPU-результаты.
- variant A исполняет сегменты **device+13568** (REX_EXECSEGS, cont.22), но они PLACEHOLDER (cont.59: реальные
  текстуры забинжены, НО verts=0,0 / idx-base sz=0x80000000 not-ready / VS vfetch берёт texture-слот). Реальный
  per-frame draw+flush поток — ГДЕ-ТО ЕЩЁ (реальный командный буфер титла), НЕ device+13568.
- cont.73 ДОКАЗАЛ: transform'ы реальных меню-лейблов — в НЕ-исполняемом реальном фрейме (EXECSEGS prim-13 draws несут
  только placeholder off-screen debug-лейбл; T=(334,866)→clip_y≈−1.5 над экраном, все onscreen=0).
- cont.25–30: force-shortcuts (фейк-fence, skip-gate) НЕ ломают цикл — оставляют титл без ресурсов/в placeholder;
  цикл дренится ТОЛЬКО реальным исполнением (cont.21 «строить A+B вместе»).

## 3. ЗАДАЧА = строить A (реальный рендерер) + B (реальный completion) вместе, чтобы цикл замкнулся
1. **НАЙТИ реальный per-frame командный поток титла** — где он строит реальные draws + transform'ы + flush
   (НЕ device+13568 placeholder). Входные точки:
   - `sub_821F8E60` (text/element-рендерер) → renderer **vtable[15]** = реальный SetStreamSource+DrawPrimitive
     (cont.47: `*(*(r31)+60)`); Lock vtable+120 / Unlock vtable+124 на VB-объекте r30 (→0xA022FFF0).
   - Draw-time deferred-state **FLUSH**: `sub_821BEC00` хранит binding в deferred per-stage device-state + ставит
     dirty `*(r31+13732)`, зовёт `sub_821CAA18` (cont.54). Flush пишет fetch-constants/SET_CONSTANT'ы ПЕРЕД
     DrawPrimitive — это и есть запись реального reg 0x4000 (transform) + reg 0x4800 (textures) в реальный буфер.
   - «Какой командный буфер» (cont.34): per-frame буфер, который таргетит present/VdSwap титла vs что исполняет
     variant A. VdSwap r3 = swap-буфер ptr (cont.34 [swapbuf] дамп r3±0x80).
2. **ИСПОЛНИТЬ этот реальный поток** в CP variant A (`ExecutePM4`): транслировать его DRAW_INDX → `vkCmdDraw` с
   реальными ALU-consts (reg 0x4000 transform) + fetch-consts (reg 0x4800 textures, cont.57) + реальной геометрией.
   Трансляция draw-state (renderpass/VkPipeline/draw-state) уже есть (cont.22 T2b backdrop;
   `rex_render::SubmitTexturedGeometry`/`g_texPipe` для текстур-квадов; menu-quad pipeline).
3. **ОТДАТЬ реальный GPU-fence/completion** из этого исполнения → удовлетворить completion-spin титла:
   - Spin: `KeGetCurrentProcessType` caller **lr=0x821C6F78** в `sub_821C6F50` (+ 0x821BFF64 / 0x821C0864,
     render-range, per-frame). Completion-drain `sub_821BF298` читает completion-obj **device+10896 (0x2A90)**,
     обрабатывает `*(dev+12924)` items at `dev+12928` stride16, handler vtable[9] of `*(dev+12900)`.
   - Существующие fence-forwards `sub_821C6E58`/`sub_821C5DF0` ЭТОТ spin НЕ покрывают.
   - → титл продвигается + кикает ещё реальный контент → цикл замыкается.

## 4. ГЛАВНЫЙ МЕТОД — PROD-ORACLE (cont.51–53)
Рабочий full-stack билд **`south_park_td`** (`out/build/linux-amd64-release/`) гоняет титл КОРРЕКТНО — это ground
truth. gdb 17.1 работает на обоих (НЕ stripped, 1:1 sub_-нейминг). prod g_base=**0x100000000** (Xenia /dev/shm;
guest 0x82XX→host 0x182XX; MAP_FIXED, детерминированно). HW-watchpoint на prod-глобале → СИМВОЛИЗИРОВАННЫЙ writer-bt.
**Сравнивай prod vs variant A**: как prod строит реальный командный буфер (per-frame DrawPrimitive → какой буфер,
SET_CONSTANT reg 0x4000) и как CP его потребляет (INDIRECT_BUFFER? build-cursor auto-consume? 0001057C callback?),
потом найди дивергенцию в variant A. Tools: `varianta/tools/{prod_read.py, oracle.gdb, diverge.gdb, vawatch.gdb}`.
⚠ KILL prod = `pkill -x south_park_td` (точное `-x`, НИКОГДА `-f` — самоубьёт мой shell, cont.51).

## 5. HARNESS (build / run / правила)
- **Build:** `ninja -C varianta/runtime/out` (~5s, перекомпил kernel.cpp + линк). Рантайм: `varianta/runtime/kernel.cpp`
  (PM4 CP + fence-forward + relocated dispatch table + draw-state extract + все REX_* gated диаги),
  `varianta/runtime/{rex_texture.h, rex_render.h, vulkan_render.cpp}`. Бинарь: `varianta/runtime/out/sp_td_varianta`.
- **Run (variant A):** из `varianta/`: `[FLAGS] ./runtime/out/sp_td_varianta ../private/extracted/default.xex`.
  Стабильная база: `REX_NOTOKEN=1 REX_CSLEAK=1 REX_CPCOMPLETE=1 REX_MOVIE_EOF=30`. Рендер: `DISPLAY=:0 REX_RENDER=1
  REX_EXECSEGS=3 …`. Ввод: `REX_SKIPINTRO=1` (intro→menu→Level 1). Полезные диаги: REX_TEXBIND/TEXDECODE/TEXSEQ/
  TEXDUMP/DRAWCAP/FCOMPOSE/TEXTRENDER/MSSPLASH (текущая сессия), REX_CHUNKDUMP/SCENE/EXECSEGS/SPINTRACE/RESULTSCAN (CP).
- **⚠ Хазарды:** REX_RENDER игнорит SIGTERM → ВСЕГДА `timeout -s KILL -k 5 N` (cont.30, GUI висел 6ч). НИКОГДА не
  idle-wait GUI bg-run. Kill variant A = `pkill -9 -x sp_td_varianta` (точное `-x`). Чистить `/dev/shm/xenia_memory_*`
  между прогонами (`find /dev/shm -maxdepth 1 -name 'xenia_memory_*' -delete`). ⚠ dev-shell = **zsh** → `rm glob*`
  ПАДАЕТ на no-match (юзать `find -delete`). grep по логам — `grep -a` (есть NUL-байты).
- **Default boot UNREGRESSED после КАЖДОГО шага** (всё за REX_*-флагом): plain headless boot = ~390k строк, 0 реальных
  crash-маркеров (игнорить `[stub] ExTerminateThread`), нормальный fence-forward в конце.

## 6. ПЕРВЫЕ КОНКРЕТНЫЕ ШАГИ (cont.76+)
1. Prod-oracle реального командного буфера: trace/HW-watch prod'ового per-frame DrawPrimitive → в какой буфер он
   пишет реальные draws + SET_CONSTANT reg 0x4000, и КАК CP его потребляет. Замапь поток prod → variant A.
2. В variant A найди ТОТ ЖЕ буфер (реальный per-frame поток) и дивергенцию (почему variant A исполняет device+13568
   placeholder вместо него) — измерением (gated диаг / gdb), не рассуждением.
3. Исполни реальный поток (ExecutePM4) → реальный DRAW_INDX → vkCmdDraw с реальным transform+текстурой → захвати
   (REX_RENDER_SHOT) ПЕРВЫЙ реальный композированный кадр.
4. Подведи реальный GPU-fence из этого исполнения к completion-spin'у → титл продвигается. Замкни A↔B.

## 7. РИГОР (жёсткие правила проекта)
- ИЗМЕРЯЙ, не спекулируй — каждое «X не работает» только из gated-диага/gdb (баг PM4-парсера однажды стоил ~12
  continuations, молча скрыв контент; «ничего нет» проверяй all-packet-type DESYNC-walker'ом).
- Default boot UNREGRESSED после каждого шага.
- Один cont.N = одно измерение/изменение → build → run → analyze → NIGHT-LOG (append) + NEXT-SESSION-PROMPT (prepend)
  + memory `[[sp_varianta_loader_not_wall]]` → commit «varianta cont.N: …» (НЕ пушить) → schedule next.
- Author: superheher <heh@vivaldi.net>. Scope: только `varianta/`.
