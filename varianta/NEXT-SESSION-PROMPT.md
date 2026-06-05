‼️‼️⏸ STATUS 2026-06-05 (cont.24 /loop follow-up, автономно) — ⛔→📐 SPRITE-ТЕКСТУРИНГ = ИЗМЕРЕННЫЙ ТУПИК; трактабельный disk-resource путь ИСЧЕРПАН ⇒ /loop НА ПАУЗЕ. Коммит **c330e50** (НЕ пушен). Полностью: NIGHT-LOG «cont.24 … follow-up».
- **Измерено (REX_SCENE расширен — логит per-draw texture dims из Xenos fetch-конст):** UI-draws prim-5(спрайт)/prim-4(tri-list)/prim-13(текст) ВСЕ имеют **`tex=0x0`** — fetch-константа текстуры НЕ выставлена (не «пустая текстура» — самого per-draw БИНДА нет; loader-gated, стена cont.23). Бэкдроп prim-8 → **EDRAM 0xB0000000 (1×1, пуст)** = runtime-composed RT, не disk-ассет (⇒ эвристический disk-bg в task #8 был верным выбором). Атлас 0xA5004800 + EDRAM = НУЛИ (loader застрял).
- **Survey ассетов:** `LevelSelectBG.png` = ЕДИНСТВЕННЫЙ 1280×720 полноэкранный фон во всём `media/Assets` (нет main-menu/title/attract bg) ⇒ дефолт task #8 — единственный/оптимальный. Опция «меню-точный bg» закрыта.
- **⇒ ВЫВОД:** трактабельный disk-resource путь ИСЧЕРПАН — (a) sprite-texturing = ТУПИК (нет per-draw texture identity); (b) лучшего bg нет; (c) per-draw color = мелкий polish. Всё остальное (богатое/читаемое меню) сходится на ОДНОЙ стене: застрявший loader / **глубокий GPU-resource-creation билд** (Xenos tiled-texture decode + .xbv→SPIR-V + EDRAM RT + resolves + real CP completion) — по ~15 continuations это POOR /loop FIT, БЕЗ shortcut.
- **⏸ ПАУЗА /loop.** Сессия ДАЛА чистую веху — **task #8: бэкдроп меню рендерит реальное игровое арт** (5a76116) — и этот follow-up СТРОГО ОГРАНИЧИЛ остаток. **РЕШЕНИЕ для юзера:** (1) глубокий loader/GPU-resource-creation билд как focused-сессия (путь к богатому меню); (2) мелкий polish (per-draw цвета / рендер on-screen prim-4); (3) redirect. ⭐ЧТОБЫ ПРОДОЛЖИТЬ: повторно `/loop продолжай работу автономно` (юзер так делал раньше) ИЛИ выбрать направление.

———————————————————————————— (ниже — cont.24 task #8: textured backdrop, ГОТОВО) ————————————————————————————

‼️‼️‼️🟢🟢🟢 STATUS 2026-06-05 (cont.24 /loop, автономно) — ⭐⭐ TASK #8 ГОТОВ: БЭКДРОП МЕНЮ РЕНДЕРИТ РЕАЛЬНОЕ ИГРОВОЕ АРТ (UV-correct, 1:1 с исходником) через disk-resource путь. Коммит **5a76116** (код) + NIGHT-LOG cont.24 (НЕ пушены). Полностью: NIGHT-LOG «cont.24 … TASK #8 DONE».
- **Сделано:** disk-resource путь теперь текстурирует РЕАЛЬНУЮ карвленную геометрию тайтла (не тестовый квад). Новый мост `rex_render::SubmitTexturedGeometry(posUV, vertCount)` + растущий pos.xy+uv.xy VB; `LoadBackgroundOnce()` грузит фон 1280×720 (`media/Assets/Frontend/Graphics/LevelSelectBG.png` по умолч.; `REX_BGTEX=<path>` оверрайд) в textured pipeline. PresentOnce рисует бэкдроп через `g_texPipe` ПЕРВЫМ (под debug-панелями/текстом). CP (kernel.cpp): в карве prim-8 бэкдропа при `REX_MENUTEX` эмитит каждый квадрант как pos.xy(clip)+uv.xy с синтетич. UV `u=x/1280, v=y/720` → 4 квадранта пересобирают полный фон. Всё за `REX_MENUTEX` (нужен и `REX_MENUTEST`-present-путь).
- **⭐ РЕЗУЛЬТАТ** (`REX_RENDER=1 REX_MENUTEST=1 REX_MENUTEX=1 REX_EXECSEGS=3` + run-base, AMD POLARIS11): `loaded PNG LevelSelectBG.png (1280×720)` + uploaded; **`textured geometry submitted (24 verts)`** = ровно 4 квадранта×6. `/tmp/varianta_menu.ppm` (1280×720) **СОВПАДАЕТ с исходником 1:1** — радиальный sunburst + чёрные letterbox-полосы воспроизведены точно (PPM→PNG, просмотрено + сверены mean/stddev). 0 Vulkan-ошибок, 0 крашей (exit-124), дефолт-бут НЕ регрессит (398k строк, 0 crash-маркеров). Картинка отправлена. ⇒ ПЕРВЫЙ *узнаваемый* (vs debug-цвет) рендер контента меню.
- **⚠ Честные пределы:** текстурится SPARSE-контент застрявшего тайтла (бэкдроп + пара debug-панелей + 1 off-screen text label), НЕ богатое меню (нужен loader). `LevelSelectBG.png` = ЭВРИСТИЧЕСКИЙ фон (реальный бэкдроп тайтла = runtime-composed EDRAM render-to-texture, не один png) — доказывает путь; меню-точный фон выбирать по контексту/имени. Панели всё ещё flat-color (нужен UI sprite-атлас + per-draw texture identity). Текст-глифы заблокированы на этом пути (font-атлас = TTF-растеризация в рантайме, нет disk .png).
- **⭐NEXT (трактабельно, disk-resource):** (a) текстурировать SPRITE/panel draws (prim-5/13) UI-атласом `.png` через `SubmitTexturedGeometry` — нужна per-draw texture identity (какой атлас + UV; план помечает это как неопределённую часть); (b) выбрать меню-точный фон бэкдропа вместо LevelSelectBG-заглушки; (c) per-draw color/alpha из ALU-конст для textured draws. Богатое/читаемое меню — по-прежнему за глубоким loader-билдом (без изменений). План: `GPU-RESOURCE-BUILD-PLAN.md` «ALTERNATIVE PATH». Run-base: `REX_NOTOKEN=1 REX_CSLEAK=1 REX_CPCOMPLETE=1 REX_MOVIE_EOF=30`.

———————————————————————————— (ниже — cont.22, актуально как контекст; cont.23–24 disk-resource путь см. NIGHT-LOG) ————————————————————————————

‼️‼️‼️🟢🟢🟢 STATUS 2026-06-04 (cont.22 /loop) — ⭐ T2b-step-3/4: БОГАТЫЙ UI НАЙДЕН (спрайты/текст) + исправлен баг декода DRAW_INDX; остаётся per-draw OFFSET. Коммиты **ae27676, 208410e** (НЕ пушены). Полностью: NIGHT-LOG «T2b-step-3/step-4».
- **БАГ ИСПРАВЛЕН (ExecutePM4 DRAW_INDX):** init читался как data[0] для op 0x22 И 0x36, но у **op 0x22 (DRAW_INDX) initiator = data[1]** (data[0] — control/viz); только у 0x36 (DRAW_INDX_2) — data[0]. Это мис-декодило каждый op-0x22 draw (мусор prim=11 numI=33024). Фикс: `init=(op==PM4_DRAW_INDX)?GLD32(addr+4):GLD32(addr)`. (Census уже читал data[1].)
- **БОГАТЫЙ UI ВСКРЫТ** ([esdraw] op+vtxSlot): #0–3 = **op 0x22** prim=5 numI=4 (СПРАЙТ), prim=4 (tri-list), prim=5, prim=13 numI=252 (ТЕКСТ) = ровно census prim5/13. #4–7 = op 0x36 prim=8 (бэкдроп-ректы). Богатые кадры реальны: директорий device+13568 растёт до **20 дескрипторов / ~80 draws** на swap#156+.
- **ИСТОЧНИК ВЕРШИН спрайтов/текста ЛОКАЛИЗОВАН** (vtxSlot-скан): бэкдроп тянет **slot 0** (kVertex, per-draw 0xA01A87xx); спрайт/текст тянут **slot 1 = 0xA01FE0FC** (~10.5MB пул), а slot 0 = их ТЕКСТУРА. ⇒ **ОПРОВЕРГАЕТ вердикт cont.22 «slot 1 = red herring / VS его не тянут».**
- **⛔ ОСТАЁТСЯ (реально сложное):** спрайт/текст = **AUTO-INDEX (src=2)** + ГОЛОВА пула slot-1 — нули ⇒ это cont.22 проблема **per-draw OFFSET** (реальные верш. глубоко, +0x2A9F0; auto-index читает [0,numI) от головы = нули). Бэкдроп рендерится т.к. у него per-draw kVertex-базы (без оффсета).
- **⭐NEXT:** дизасм vfetch СПРАЙТ-VS (какой slot+offset; slot 0 — текстура, значит верш. в др. slot/offset) И/ИЛИ сэмплить БОГАТЫЙ кадр (swap#156+, пул заполнен) + скан slot-1 на заполненный регион per-draw → carve (prim5 tri-strip/prim13 quad-list) → текстура (slot-0 fetch + textured FS) + $worldviewProj (reg 0x4000). Бэкдроп уже рендерится. Диаги (gated): [esprim], [esdraw] op+vtxSlot, [esidx], [execsegs] NEW-MAX. Дефолт-бут НЕ регрессит.

———————————————————————————— (ниже — T2b-step-2: бэкдроп (4 квадранта) рендерится; step-3/4 выше нашёл богатый UI) ————————————————————————————

‼️‼️‼️🟢🟢🟢 STATUS 2026-06-04 (cont.22 /loop, автономно) — ⭐ T2b: РЕАЛЬНАЯ ГЕОМЕТРИЯ МЕНЮ ТАЙТЛА РЕНДЕРИТСЯ КОРРЕКТНО (4 квадранта-бэкдропа на Vulkan swapchain). Коммит **fd6b265** (НЕ пушен). Полностью: NIGHT-LOG «T2b-step-2».
- Связал carve→bridge→draw: в REX_EXECSEGS-кадре каждый контент-draw (fetch type-3 kVertex, prim 8 kRectangleList, numI 3) читает 3 float2-вершины из live fetch slot-0, выводит 4-й угол rect'а, 2 треуг., screen→Vulkan-NDC (`clip=x/640-1, y/360-1`), копит в `tl_esVerts`; после exec-цикла батч → `rex_render::SubmitMenuGeometry` → render-thread рисует через menu-quad float2-pipeline (REX_MENUTEST) → present+PPM.
- **РЕЗУЛЬТАТ** (`REX_RENDER=1 REX_MENUTEST=1 REX_EXECSEGS=3` + run-base): 24 верш/кадр = 4 ректа, /tmp/varianta_menu.ppm. Первый снимок (неверная формула `v3=v1+v2-v0`) = 4 параллелограмма (срез) — ДОКАЗАЛ, что путь carve→GPU→present работает на РЕАЛЬНЫХ вершинах тайтла, ошиблась лишь формула угла. **ФИКС:** kRectangleList 4-й угол = **`v3=v0+v2-v1`**, треуг. `(v0,v1,v2)+(v0,v2,v3)` (проверено по RAW-дампу v0=(640,360) v1=(1280,360) v2=(1280,720) → v3=(640,720)=чистый BR-квадрант). Пересъёмка = **4 ЧИСТЫХ axis-aligned квадранта-прямоугольника, тайлящих 1280×720 = РЕАЛЬНЫЙ per-EDRAM-tile (640×360) БЭКДРОП тайтла, КОГЕРЕНТНО** (vs прежний soup). Цвета = дебаг-палитра (тайтл бы текстурировал).
- **⇒ ВЕХА: variant A рендерит РЕАЛЬНУЮ геометрию меню тайтла КОРРЕКТНО** (впервые) — путь: CP-исполняет deferred-сегмент → читает live kVertex каждого draw → триангулирует prim → screen→clip → vkCmdDraw → present, подтверждено чистым 4-квадрантным снимком.
- **⭐NEXT (T2b прод.):** (1) дойти до БОГАТЫХ UI-сегментов (prim=5 numI=4 спрайты + prim=13 numI=252 текст — их НЕ было в 3 бэкдроп-сегментах, что исполняет REX_EXECSEGS; найти их кадр-триггер / cmd-buffer); (2) per-draw ТЕКСТУРА/цвет (fetch-текстуры 0x4800 + textured FS) + per-draw `$worldviewProj` (reg 0x4000) для не-screen-space draw'ов; (3) другие prim'ы (tri-strip/quad-list) в carve. Фундамент трансляции геометрии ДОКАЗАН. Дефолт-бут НЕ регрессит (T2b gated за REX_EXECSEGS+rex_render::Enabled).

———————————————————————————— (ниже — BREAKTHROUGH-2: вершины СУЩЕСТВУЮТ; T2b выше это уже отрендерил на экран) ————————————————————————————

‼️‼️‼️🟢🟢🟢 STATUS 2026-06-04 (cont.22 /loop, автономно) — ⭐⭐ BREAKTHROUGH-2: «ДЫРА С ВЕРШИНАМИ» ОПРОВЕРГНУТА — вершины контента СУЩЕСТВУЮТ и ДОСТУПНЫ. Полностью: NIGHT-LOG «cont.22 … BREAKTHROUGH (REX_EXECSEGS)». Коммит **3bebeb3** (НЕ пушен). Кратко (всё ИЗМЕРЕНО, run-base `REX_NOTOKEN=1 REX_CSLEAK=1 REX_CPCOMPLETE=1 REX_MOVIE_EOF=30` + `REX_EXECSEGS=3`):
- **Сделал `REX_EXECSEGS`** (kernel.cpp, gated, дефолт-бут НЕ регрессит — проверено: exit124 / 0 крашей / 191k строк). На VdSwap: walk директория device+13568 (парс census'а: top-byte 0x81=дескриптор, low 24b=длина dw; next dword=phys), resolve guest=0xA0000000|(phys&0x1FFFFFFF), **ExecutePM4 каждый сегмент** = РЕАЛЬНО ИСПОЛНЯЕМ deferred-контент (3 сегмента / ~7-8 DRAW_INDX на кадр). Отлично от REX_CHUNKCP (гонит директорий как inline-PM4 — неверно) и REX_SEGCP (скан r3-staging).
- **⭐ Измерено:** хардкод 0xA2000000 остаётся 0x00000000 / 0-real-floats ДАЖЕ с исполнением. НО **LIVE fetch slot-0, который ставят исполнённые draw'ы (reg-file 0x7FC80000+0x4800*4) = fc0=0x001A875B → ТИП 3 (kVertex) → guest 0xA01A8758, растёт ~0x88000/кадр** (= скорость роста staging-буфера ⇒ вершины inline в staging). RAW floats (6/6 кадров ИДЕНТИЧНО) = `640 360 | 1280 360 | 1280 720 | 0 0…` = **РЕАЛЬНЫЕ экранные float2-вершины для 1280×720** (точно совпадает с RE-форматом vfetch: float2/stride2dw/slot0). **ВЕРШИНЫ ЕСТЬ.**
- **⇒ Вывод последних ~6 continuations «slot 0=0xA2000000 ПУСТ / вершины НИКОГДА не пишутся / vertex-data gap» — АРТЕФАКТ ИЗМЕРЕНИЯ:** зонды REX_POOLCHK/poolwatch/40-VS читали fetch-константу из ГЛОБАЛЬНОГО reg-file на swap/kick = устаревший slot-0 от 3 KICKED КЛИРОВ (0x02000000→0xA2000000, пустой setup-пул, который loader-memcpy sub_8242BF10 заполняет ФИЛЛЕРОМ), потому что контент-сегменты НИКОГДА НЕ ИСПОЛНЯЮТСЯ. Вершины всегда были в собственном per-frame state контент-draw'ов; исполнение сегментов их вскрывает.
- **⚠ОТКРЫТЫЙ ПОДВОПРОС (строгость — НЕ утверждаю механизм):** census-shadow-walk `[drawgeo] SLOT0` ТОЖЕ видел 0xA2000000 для контент-draw'ов, ХОТЯ тот walker ОБРАБАТЫВАЕТ type-1 SET_CONSTANT в свой fetch-shadow ⇒ census-walk и live-ExecutePM4 РАСХОДЯТСЯ на тех же сегментах по НЕПРОВЕРЕННОЙ причине (per-draw тайминг / desync walker'а / live-исполнение идёт по пакетам, которых нет в линейном walk'е). T2b разрешит.
- **⇒ РЕНДЕРЕР БОЛЬШЕ НЕ ЗАБЛОКИРОВАН на «вершин нет».** T2a (исполнить сегменты) ✅ сделано+проверено. **⭐NEXT = T2b (task #3): DRAW_INDX→vkCmdDraw В ExecutePM4** — на каждый draw: читать live fetch slot-0 (type-3 kVertex base + d1 word-count) + numI + piece-3a draw-state → загрузить float2-вершины → vkCmdDraw в Vulkan RT (menu-quad pipeline уже принимает float2) → present. Это И рендерит меню, И разрешает открытый подвопрос (логирует live fetch каждого из ~7-8 draw'ов/кадр).
- **⚠ pre-existing краш ~45-50с (БАЗА крашится ТОЖЕ** — string-as-code INDIRECT-NULL в диапазоне loader'а sub_8224xxxx, за пределами стабильного 30с-окна; **НЕ регрессия execsegs** — execsegs gated). Новый диаг (gated, default-safe): `REX_EXECSEGS=N`.

———————————————————————————— (ниже — ранний cont.22, «КОНТЕНТ СТРОИТСЯ» ВЕРНО, но «vertex-data gap» ИСПРАВЛЕНА выше — был артефакт чтения fetch-константы не в то время) ————————————————————————————

‼️‼️‼️🟢🟢🟢 STATUS 2026-06-04 (cont.22 /loop, автономно) — ⭐⭐⭐ BREAKTHROUGH: КОНТЕНТ СТРОИТСЯ — вывод cont.10-22 «контента нет нигде» был АРТЕФАКТОМ ПАРСЕРА census'а; премиса A↔B coupling ЛОЖНА. Полностью: NIGHT-LOG «cont.22 … BREAKTHROUGH». Кратко (всё ИЗМЕРЕНО на стабильном full-menu `REX_NOTOKEN=1 REX_CSLEAK=1 REX_CPCOMPLETE=1 REX_MOVIE_EOF=30`):
- **Step (a) (result-gate scan, REX_RESULTSCAN + расширенный REX_CHUNKDUMP) ВСКРЫЛ артефакт.** Census device+13568 (1) обрезал сегменты на **1024 dw** (а контент-сегменты = 0xCDC-0xF68 = **3292-3944 dw**, начинаются с нулей/type-0 → весь контент за dw1024 СКРЫТ) и (2) продвигал только type-3 пакеты по count, прочие — по 1 dw → **рассинхрон после любого type-0/SET_CONSTANT** (ровно предупреждение cont.10 о false-positives, но оно давало и false-NEGATIVES, которые ушли в вывод «0 draws» через cont.10/12/21/22).
- **ПЕРЕПИСАЛ walker правильно** (зеркало ExecutePM4: type-0 на reg-count, type-1 на 2, type-2 на 0, type-3 на data-count) **+ DESYNC-детект.** Контент-сегменты парсятся **`clean` (без DESYNC) = валидный PM4**, не мусор-буферы.
- **`[segdump]` полный walk сегмента (desc#0, 0xD38) — РЕШАЮЩЕЕ ДОКАЗАТЕЛЬСТВО:** полный draw — `scissor → ALU-конст reg0x4000 cnt=1008 (v0=1.0f) → reg0x4400 cnt=1024 → RB_DEPTHCONTROL cnt=12 → SQ_PROGRAM_CNTL=0x10010001 (СОВПАДАЕТ с piece-3a kicked) → RB_SURFACE_INFO cnt=16 → PA_CL_VPORT → FETCH-конст reg0x4800 cnt=186 (текстуры+верт.стримы) → COHER_STATUS_HOST → WAIT_REG_MEM → DRAW_INDX`. Связное состояние, не порча; стабильно от кадра к кадру.
- **Исправленный census (измерено, на кадр меню):** **realDraws≈15, rectDraws≈21, texFetch(fetch-const loads)≈45, EDRAM resolve(RB_COPY)≈330, VIZ_QUERY≈40** — каждый кадр, в сегментах device+13568, всё `clean`. ⇒ тайтл СТРОИТ текстурный контент (шейдеры, ~2000 ALU-конст, 186 fetch-конст, viewport/blend/depth/surface, resolve'ы, occlusion-queries). «Только клиры/0 текстур» был парсер.
- **⇒ RE-GROUNDING:** ВЕРНО (cont.10/c11): тайтл строит deferred render-программу (device+13568), а variant A её **НИКОГДА НЕ ИСПОЛНЯЕТ** (fence-forward фейкает completion БЕЗ исполнения). АРТЕФАКТ (cont.10c10/c12/21/22): «сегменты = 0 draws» ⇒ «контента нет» ⇒ «A↔B нужен реальный GPU-pixel-результат до BUILD контента» — всё downstream от сломанного парсера; producer/consumer-тупик и «render the clears» гнались за фантомом.
- **NEXT = piece 3b (ПЕРЕ-СКОУП): ИСПОЛНЯТЬ deferred-сегменты** (не «рендерить клиры»): CP variant A на каждый кадр walk'ает директорий device+13568 (descriptor-walk уже есть в census; guest=0xA0000000|(addr&0x1FFFFFFF), each → ExecutePM4), транслируя DRAW_INDX→vkCmdDraw (piece-1 renderpass/pipeline + piece-3a draw-state + загруженные ALU0x4000/fetch0x4800 конст + шейдеры) → present; и продвигать completion-fence как РЕАЛЬНЫЙ результат исполнения (заменив fence-forward). **OPEN RE (порядок исполнения):** как prod линкует сегменты директория к ring-исполнению — INDIRECT_BUFFER из kicked-IB / callback-запись `0001057C 821CC7A0 <ctx>` / GPU auto-consume курсора (device+13568..13572). Определить (prod CP-trace). Диаг сессии (gated, default-safe, дефолт-бут НЕ регрессит): REX_RESULTSCAN, исправленный REX_CHUNKDUMP (correct walker + DESYNC + result-gates + [segdump]). Коммит cont.22-loop (НЕ пушен).

———————————————————————————— (ниже — ранний cont.22 статус, ПРЕМИСА «A↔B coupling / контента нет» ИСПРАВЛЕНА выше — был артефакт парсера) ————————————————————————————

‼️‼️‼️ STATUS 2026-06-04 (cont.22, автономно) — ⭐ RE-GROUNDING cont.21: тайтл НЕ на menu-content рендере (застрял во frontend MOVIE/ATTRACT-петле); ring-flow ≠ content; настоящий краш = ПОРЧА DISPATCH-ТАБЛИЦЫ (таблица в guest-WRITABLE памяти). Полностью: NIGHT-LOG cont.22. Кратко (3 независимых измерения ИСПРАВЛЯЮТ вывод cont.21 «build the PM4→Vulkan translator NOW»):
- **(1) Счётчик device+0x2b04 (измерено):** r3==dev==g_interruptData==**0x26F80** (CPCOMPLETE дренит ВЕРНЫЙ счётчик). Тайтл НЕ простаивает — зовёт kick-gate sub_821C6C80 **15768×** (16 KICK + **15752 DEFER**), долбит submit. Счётчик растёт 0→0xA+ т.к. CPCOMPLETE дренит **1/vblank (~60/с)**, а тайтл инкрементит **~7–8/vblank** → гейт закрыт RATE-mismatch'ем (НЕ «тайтл перестал сабмитить»). **Prod** (`tools/prod_counter.gdb`, dev=0x40016f80): счётчик строго **0↔1, cmax=1 на 33000 сэмплах** ⇒ faithful drain = **per-submission (≤1)**, не 1/vblank.
- **(2) Ring-flow ≠ content:** при ЛЮБОЙ частоте kick'ов (16 натурально / 2725 форс cont.15) — только `init=0x30088` rects, 0 текстур. Поток ринга НЕ даёт контент.
- **(3) OPEN-вопрос cont.21 РЕШЁН (bt):** main-thread = `sub_82150970(frontend) → sub_82425BF8(MOVIE WIDGET) → … → sub_821C6E58(GPU-fence) → sub_821B9270(spin)`. Тайтл ГОНИТ movie-виджет и спинит на GPU-fence — НЕ на menu-content. «Шейдеры загружены» = precache во время movie-экрана. Форс-путь (MOVIE_EOF+XFLAG) проходит movie но встаёт на menu-SETUP null-vtable sub_8215DE84. **НИ ОДИН путь не доходит до menu-content рендера** ⇒ переводить нечего, translator ПРЕЖДЕВРЕМЕНЕН.
- **(4) Настоящий краш = ПОРЧА DISPATCH-ТАБЛИЦЫ.** Краш в **VblankPump → FireGfxInterrupt(cb=0x821C7170, source=0) → CallGuest(0x821C7170) → garbage rip 0xffffffff40f62598** (нет фрейма sub_821C7170 — `DispatchLookup` САМ вернул мусор). gdb-чтение таблицы: ВСЕ слоты render/frontend-функций (0x82150970..0x821CC7A0) = мусор, байты = `"game:\media\"` + floats + 0xFFFFFFFF. Причина: таблица лежит на `g_base + PPC_IMAGE_BASE+PPC_IMAGE_SIZE = g_base+0x82930000` — **в guest-writable mmap, сразу за образом** — и **post-image данные тайтла (path-строки, floats) ЗАТИРАЮТ слоты его же render-функций**. `CallGuest`/`PPCInvokeGuest` зовут мусор → SIGSEGV. **Вероятно КОРЕНЬ старых render-path «string-as-code»/INDIRECT-NULL крашей cont.10–21** (этой сессией: INDIRECT-NULL targets 0x67616D65="game", 0x6C6F6261="Glob", 0x41737365="Asse"ts @lr=0x82204D08).
- **ФИКСЫ (default-safe, дефолт-бут НЕ регрессит — проверено):** (a) bound PPCInvokeGuest `IMAGE_SIZE→CODE_SIZE` (таблица покрывает только code; [code_end 0x825F0C18, image_end 0x82930000) индексировал ЗА таблицу в мусор). (b) `ValidHostFn` — [lo,hi] хост-указателей из PPCFuncMappings[]; в PPCInvokeGuest И CallGuest skip+log мусорного слота вместо вызова. **Проверка:** под gdb натуральный путь прожил все 55с БЕЗ краша (guard поймал **2107** порченых dispatch'ей); non-gdb 3×: **2/3 прожили 45с** (guard 1515/1528), 1/3 крашнулся (40 ловит до краша иным путём). ⇒ **ЧАСТИЧНАЯ** митигация (ловит dispatch-READ класс; порчу НЕ останавливает).
- **✅ НАСТОЯЩИЙ ФИКС СДЕЛАН — таблица ПЕРЕНЕСЕНА из guest-памяти (commit pending):** dispatch-таблица теперь в ОТДЕЛЬНОМ host-`mmap` (`g_funcTableBase`, runtime.cpp), НЕ в g_base+0x82930000. Новый `HostFnAt()` читает её; PPCInvokeGuest+DispatchLookup используют. Скоуп = runtime.cpp+kernel.cpp+kernel.h ТОЛЬКО (рекомп-TU идут через PPCInvokeGuest по rex_indirect.h ⇒ PPC_LOOKUP_FUNC/ppc_context.h НЕ трогаются, БЕЗ масс-пересборки). **РЕЗУЛЬТАТ: ПОРЧА УСТРАНЕНА** — loader печатает `dispatch table at host 0x7f… OUT of guest space`, `ValidHostFn` guard фаерит **0×** (было 40–2107/прогон), дефолт-бут НЕ регрессит. ⚠ОСТАЁТСЯ ОТДЕЛЬНЫЙ intermittent краш (non-gdb 2/3 прожили 45с, 1/3 краш с guardfires=0 = НЕ порча таблицы) — вероятно **cont.12(c9) render-DPC гонка** (sub_821BF748 диспатчит stale pooled-объект, чьё поле — in-range valid fn → неверный диспатч; ValidHostFn не ловит in-range). Нужен реальный sync (cont.9/10 Step-1), не table-фикс. Guards оставлены как дешёвый tripwire.
- **NEXT (исправленные приоритеты):** (1) ✅**ПЕРЕНЕСТИ dispatch-таблицу СДЕЛАНО** (порча устранена; см. выше) — следующий слой = residual render-DPC гонка cont.12(c9) (in-range stale-callback диспатч); чинить реальным sync ЛИБО просто идти дальше к меню; (2) **дойти до НАСТОЯЩЕГО меню** (истинный gate контента): movie/attract→menu переход — movie GPU-fence sub_821C6E58 / movie-EOS через реальный VC-1 декодер + fair scheduler (cont.9/10 Step-1); или форс-путь — восстановить menu-setup vtable sub_8215DE84 (cont.7); (3) **ПОТОМ** pillar B (DRAW_INDX→Vulkan). Per-submission counter-drain (≤1 как prod) — downstream рычаг, не gate. Запуск-база: `REX_NOTOKEN=1 REX_CSLEAK=1 REX_CPCOMPLETE=1`. Новый тул: `tools/prod_counter.gdb`. Коммит cont.22 (gated, default-safe, НЕ пушен).

———————————————————————————— (ниже — cont.21 статус, ПРЕМИСА «build translator now» ИСПРАВЛЕНА выше) ————————————————————————————

‼️‼️‼️ STATUS 2026-06-04 (cont.21, автономно) — ⭐ PROD-ORACLE RE-GROUNDING: cont.13–20 МОДЕЛЬ ИСПРАВЛЕНА. producer/consumer = COMPLETION-BOOKKEEPING (1/кадр), НЕ draw-путь; `*(item+16)=0` И В PROD (премиса cont.19/20 ОПРОВЕРГНУТА). Полностью: NIGHT-LOG cont.21. Кратко:
- **Прод-оракул** (rexglue south_park_td, символы; ABI rdi=&ctx rsi=base, PPCContext.r3@off0 — verified disasm) дал ground-truth, который cont.13–20 ВЫВОДИЛИ. Инструменты: `tools/prod_producer.gdb`, `tools/prod_cpmap.gdb` (gated, reusable).
- **(1)** producer sub_821CC7A0 фаерится ~4192×/120с = **~1/кадр** из `ExecutePacketType3_INTERRUPT→DispatchInterruptCallback→sub_821C7170`. Work item `{+0=self+0x124, +4=0x80000000, +12=4, +16=0, +20=0}` — **`*(item+16)=0 И В PROD`**. ⇒ producer/consumer = per-submission GPU-completion BOOKKEEPING (НЕ per-draw; per-draw был бы ~800/кадр). **cont.19/20 «work data INCOMPLETE т.к. *(item+16) null» — ЛОЖНАЯ ПРЕМИСА** (null нормален).
- **(2)** Настоящие draw'ы = PM4 `DRAW_INDX` в **IB'ах physical-окна** (prod: tiny primary ring 0→25→31→37→42→86 → IB'ы; **3592-dw IB @phys 0x1dc90540 = guest 0xBDC90540**, 3×/кадр, несёт draw'ы) — исполняются HOST CP→Vulkan. (Разрешает cont.10 «0 draw'ов в device+13568»: draw'ы НЕ в segment-directory bookkeeping, а в IB'ах, киккнутых в primary ring — variant A до них не доходит, киккает 6×.)
- **(3)** Vulkan-сторона variant A = **PRESENT-ONLY** (header: «No PM4 translation yet»; НЕТ VkPipeline/vkCmdDraw/pipeline-cache — только clear-present + movie YUV-blit). «Vulkan READY» было ПЕРЕОЦЕНКОЙ — PM4→Vulkan draw-транслятор НЕ существует.
- **(4)** Столл variant A = **kick-gate DEADLOCK** (подтв. в меню-loop): REX_KICKGATE → **6 KICK (counter=0, init) → 74 DEFER**, `device+0x2b04` РАСТЁТ 0→9→0xA, НЕ декрементится. main-thread в movie-widget sub_82425BF8→sub_821C6E58 (GPU fence-wait). PUMPCB/BOOTSTRAP (драйв producer/consumer) НЕ ломает дедлок (kicks=6, WPTR=37) — bookkeeping, не та completion, что декрементит counter.
- **⭐ ИСПРАВЛЕННАЯ МОДЕЛЬ:** variant A без реального GPU. fence-forward стопгап фейкает completion → тайтл ковыляет через init+intro+menu-ЛОГИКУ, но **подавляет submission draw'ов** (тайтл перестаёт киккать draw-IB'ы, completion фейкнут). **RENDERER-DESIGN.md «faithful CP» — ВЕРНО, но цель НЕ «build non-null *(item+16)»** (null и в prod) — а: (A) заставить completion-handshake ОСЦИЛЛИРОВАТЬ (counter-декремент + fence-advance как РЕАЛЬНЫЕ результаты исполнения каждой киккнутой submission) → тайтл КИККАЕТ draw-IB'ы; (B) транслировать `DRAW_INDX`→Vulkan. По объёму = structural-floor **GPU command processor**. producer/consumer (cont.13–20) = ЗАКРЫТАЯ тупиковая ветка.
- **✅ PILLAR A — ПЕРВЫЙ ГЕЙТ СЛОМАН (REX_CPCOMPLETE, gated, committed 49b318c, default boot unregressed):** pump декрементит `device+0x2b04` на 1/vblank (faithful per-completion, НЕ cont.15 blanket force). РЕЗУЛЬТАТ: **kicks 6→14, WPTR 37→61**, main-thread ПРОХОДИТ render fence-wait → ЖИВОЙ цикл (2 gdb-сэмпла = разные fns). На NATURAL пути (NOTOKEN+CSLEAK, без force): то же + тайтл **ДОХОДИТ ДО FRONTEND, грузит menu-шейдеры (Simple.xbv, SPTextured.xbv/.xbp), 60с БЕЗ КРАША** (vs baseline render-SIGSEGV ~33с). ⇒ CPCOMPLETE = ring-flow + STABILITY база.
- **⛔ ВТОРОЙ ГЕЙТ = A↔B COUPLING (доказано, не ещё один completion-механизм):** kicks плато на 14/WPTR 61, только setup-IB'ы (≤266 dw) + init=0x30088 rects, 0 textured. CPCOMPLETE+PUMPCB (драйв producer/consumer) НЕ помог (kicks=14). ⇒ тайтл доходит до меню + грузит шейдеры, но НЕ сабмитит content-draw-IB'ы пока не увидит РЕАЛЬНЫЕ GPU-результаты. Нельзя flow ring (A) без рендера (B), и наоборот — строить ВМЕСТЕ.
- **NEXT = PILLAR B + coupling:** **DRAW_INDX→Vulkan** транслятор (VkPipeline из reg-file 0x4000 ALU/0x4800 fetch→верт.стримы+текстуры, RT/viewport/blend + загруженные .updb шейдеры→SPIR-V → vkCmdDraw в swapchain), + исполнение киккнутой submission даёт РЕАЛЬНУЮ completion (fence-advance + counter-decrement как результат реальной GPU-работы), итерировать A↔B пока тайтл не дойдёт от setup-IB'ов до content-draw-IB'ов. Запуск-база: `REX_NOTOKEN=1 REX_CSLEAK=1 REX_CPCOMPLETE=1 ...default.xex`.

———————————————————————————— (ниже — cont.16–17 статус, ПРЕМИСА ИСПРАВЛЕНА выше) ————————————————————————————

‼️‼️ STATUS 2026-06-04 (cont.16–17, автономно) — FIX-ПОПЫТКА идёт: route-B direct-invoke ФАЕРИТ render-producer (6886×) с РЕАЛЬНЫМ ctx, но consumer не дренит (process-context obstacle). Полностью: NIGHT-LOG cont.16/17. Поверх cont.13–15 (полная модель ниже).
- **Completion object B в variant A = ВЕСЬ НОЛЬ** (B[0]/B+0x10/B+0x14 = 0) vs prod {4, producer 0x821CC7A0, ctx 0xddd10180}. B пишется через aliased CP-mapping (HW-watchpoint не ловит) ⇒ B-populate напрямую ненадёжен.
- **REX_FINDCB:** callback-записи `{0001057C, 821CC7A0, ctx}` НАЙДЕНЫ — staging 2/кадр + device+13568 chunk 4 — с РЕАЛЬНЫМ ctx=**0xC0090180 / 0xC0117B00** (0xC-window GPU-phys).
- **REX_INVOKECB (route-B direct-invoke):** зову `sub_821CC7A0(ctx)` на каждую запись (минуя B) → **producer фаерится 6886×** (было 0), НО consumer sub_821CC310 НЕ дренит ([consumer]=0) + краш INDIRECT-NULL lr=0x821BF834. ⚠ KeSetEvent producer'а не будит consumer'а — **procType-mismatch**: я зову producer на контексте VdSwap'а (его procType), а consumer дренит per-process ring procType'а tid=10.
- **⭐КЛЮЧ:** cont.15 показал, что producer, запущенный через ПАМП-interrupt (контекст g_pumpKpcr), БУДИТ consumer ([consumer]=1) — значит procType пампа СОВПАДАЕТ с consumer'ом. Тогда было пусто только из-за B+0x14=null. Теперь ctx РЕАЛЬНЫЙ (из записей).
- **NEXT (лучшая ставка):** на каждую запись — populate B{+0x10=0x821CC7A0, +0x14=ctx} + запустить sub_821C7170(source=1) НА ПАМП-КОНТЕКСТЕ (как FireGfxInterrupt, g_pumpKpcr — но без гонки с реальным пампом: либо из самого пампа, либо отдельный пап-контекст) → producer(ctx) на верном procType → consumer дренит → draw via *(item+16). Альтернативы: (2) звать producer с c.r13=KPCR tid=10; (3) сигналить consumer's KeWait напрямую. Диаг REX_FINDCB/REX_INVOKECB (gated, default boot unregressed). Коммиты …b227c8b, НЕ пушены.

‼️‼️‼️ STATUS 2026-06-04 (cont.13–15, автономно) — РЕНДЕРЕР ПОЛНОСТЬЮ СМОДЕЛИРОВАН (validated). Это cross-thread producer/consumer pipeline; гейт = pending-counter device+0x2b04, который consumer (tid=10) не декрементит → deadlock. Полностью: NIGHT-LOG cont.13/14/15. Кратко:

- **Настоящий render-путь (НЕ +16752/sub_821BF748 — это ТУПИК, vblank-DPC, null и в prod):** per-frame команд-стрим (IB в PRIMARY ring) несёт INTERRUPT-пакеты (op 0x54) → CP шлёт guest gfx-interrupt **source=1** → sub_821C7170 → callback `*(*(device+0x2A94)+0x10)` = producer **sub_821CC7A0** (4192×/120с в prod) → enqueue → consumer **sub_821CC310** (на tid=10) → draw через `*(item+16)` (lr=0x821CC4B0).
- **ГЕЙТ = pending-counter device+0x2b04 (11012).** Цепочка submit: sub_82249638→…→sub_821CC830→sub_821C6D58(flush)→sub_821C6C80→sub_821C6600(kick). variant A доходит до flush (3322×) и sub_821C6C80 (1410×) per-frame, но киккает 6×. sub_821C6C80 киккает ТОЛЬКО когда `*(device+0x2b04)==0`. prod: осциллирует 0↔1 (kick 7062×/45с). variant A: МОНОТОННО растёт 0→1→…→0xA→… и НЕ сбрасывается.
- **device+0x2b04 = pending-segment counter** (proven HW-watchpoint): render-thread инкрементит (…→sub_821CCA28→sub_821C6C80); **consumer sub_821CC310→sub_821CC140 декрементит**. Consumer на **tid=10** (sub_82450FD0→sub_821CC5D0→sub_821CC310). variant A: sub_821CC5D0 входит 2× но БЛОКИРУЕТСЯ в KeWaitForSingleObject (ждёт KeSetEvent от producer'а) → consumer не бежит → counter не декрементится → нет kicks → нет INTERRUPT → producer не фаерится. **DEADLOCK.** (Параллельно: даже на редких source=1 fires `*(B+0x10)` null — producer не зарегистрирован.)
- **BOOTSTRAP-эксперимент (REX_BOOTSTRAP) ВАЛИДИРОВАЛ модель:** регистрация producer'а в *(B+0x10) → producer ФАЕРИТСЯ ([enq]=1, было 0), consumer БЕЖИТ ([consumer]=1) — chain работает; но work item ПУСТОЙ (item=0, handler=null). Форс kick-gate (device+0x2b04=0) → kicks ТЕКУТ (2725×, было 6), но draws ТОЛЬКО init=0x30088 rects (0 текстур), потом краш. ⇒ **текстурные draws НЕ в kicked-сегментах — они producer/consumer work items с handler=*(item+16).**
- **ВЫВОД:** рендерер требует FAITHFUL CP/GPU-completion модель (не stopgap). Все куски размечены.
- **NEXT (настоящий рендерер, multi-session):** при потреблении kicked-сегмента CP variant A должен МОДЕЛИРОВАТЬ completion: (a) декрементить device+0x2b04 (мимикрия consumer'а) → kicks текут; (b) фаерить source=1 interrupt с B под этот сегмент → producer enqueue'ит РЕАЛЬНЫЙ work item (handler!=null); (c) consumer (tid=10, бежит под NOTOKEN) дренит → draw via *(item+16). ОТКРЫТЫЙ RE: реальный work item дренится ВНУТРИ sub_821CC310 (не *(r3) — то queue-head sentinel); как kicked-сегмент мапится на completion-object B + handler. Диаг: REX_INTLOG/REX_KICKGATE/REX_ENQLOG/REX_BOOTSTRAP. Коммиты 1ddcf83/4716c1a/b90246b (НЕ пушены), default boot unregressed.

‼️‼️‼️ STATUS 2026-06-04 (cont.13, автономно) — РЕФРЕЙМ: render-продюсер = source=1 gfx-interrupt DPC (sub_821C7170 → sub_821CC7A0), а НЕ sub_821BF748/+16752 (лид c12 — ТУПИК). Полностью: NIGHT-LOG «cont.13». Кратко:
- **sub_821BF748/+16752 — ТУПИК (доказано в prod):** HW-watchpoint на слоте 0x4001b0f0 за 160с (полный vblank) НЕ сработал, dispatch +387 = 0 раз ⇒ в prod этот колбэк тоже null. sub_821BF748 = vblank (source=0) DPC, не render-путь.
- **Настоящий render-путь (prod):** producer sub_821CC7A0 зовётся **4192×/120с** (consumer sub_821CC310 4191×, sub_821BF860 2095×). BT: CommandProcessor::WorkerThreadMain→ExecutePrimaryBuffer→ExecuteIndirectBuffer→ExecutePacketType3_INTERRUPT→DispatchInterruptCallback→ExecuteInterrupt→**sub_821C7170**→sub_821CC7A0. ⇒ per-frame команд-стрим (IB в PRIMARY ring) несёт INTERRUPT-пакеты (op 0x54) → CP шлёт guest gfx-interrupt **source=1** → sub_821C7170 → producer → enqueue → consumer → draws.
- **sub_821C7170 декодирован (prod disasm):** source==1 зовёт `callback = *(*(g_interruptData+0x2A94)+0x10)` (null-check; вызов на +204, guest LR **0x821C71A4** = наш старый INDIRECT-NULL). **0x2A94=10900** = РОВНО поле STOPGAP'а в kernel.cpp. prod: device(r4/g_interruptData)=0x40016f80, B=*(dev+0x2A94)=0xffc9a000, *(B+0x10)=0x821cc7a0.
- **variant A (REX_INTLOG+REX_ENQLOG, NOTOKEN+CSLEAK, меню):** producer=0, consumer=0, PM4_INTERRUPT=1. source=1 сработал 3×, ВСЕ: `iData=0x00026F80 B=*(+10900)=0xA2011000 *(B+0x10)=0x00000000 -> FIRE`. ⇒ B ВАЛИДНО (не sentinel) ⇒ STOPGAP НЕ виноват; **слот *(B+0x10) ПУСТ** (producer не зарегистрирован) ⇒ null-check в sub_821C7170 пропускает вызов. + INTERRUPT'ы почти не шлются. Оба расхождения ⇒ variant A НЕ исполняет per-frame команд-стрим (IB с INTERRUPT + сегменты device+13568 с записями `0001057C 821CC7A0 <ctx>`).
- **МЕТРИКА УСПЕХА рендера:** `REX_ENQLOG [enq]` (sub_821CC7A0) > 0.
- **NEXT:** (a) кто пишет *(B+0x10)=0x821CC7A0 в prod (регистрация колбэка; watchpoint на B+0x10 с первого interrupt'а НЕ сработал за 90с ⇒ пишется раньше, в graphics-init — армить раньше / трейсить sub_821C73D8, который ставит device+0x2A94=B / следовать сегмент-записям device+13568); (b) фикс: гнать CP variant A исполнять per-frame IB/сегменты (INTERRUPT→producer→consumer) — route B с понятым теперь механизмом — vs. собственный flush тайтла sub_821C6D58. Диаг: **REX_INTLOG** ([int], default boot unregressed). Коммиты НЕ запушены.

‼️‼️‼️ STATUS 2026-06-03 (cont.12 c12) — ДИЗАМБИГУАЦИЯ A-vs-B = ПУТЬ A: render-путь ДОСТИГАЕТСЯ, но его callback-очереди ПУСТЫ (продюсеры render-работы не запускаются). Полностью: NIGHT-LOG «cont.12(c12)». Кратко:
- **sub_821CC* work-queue DORMANT:** producer sub_821CC7A0 + consumer sub_821CC310 = **0 вызовов каждый** за меню-прогон (REX_ENQLOG) ⇒ её 0x821CC7A0-callback'и в сегментах = МЁРТВЫЕ данные, НЕ активный render-путь. (⇒ путь B «исполнять сегменты + звать 0x821CC7A0» слабо поддержан — это очередь, которой тайтл сам не пользуется.)
- **Активная render-попытка = sub_821BF748** (владелец INDIRECT-NULL 0x821BF834): spinlock-защищённый **DPC/callback-queue процессор** (KfAcquireSpinLock@r3+16920, счётчик+16756, mftb@+16760, head/tail+16908/+16912, callback@**+16752**). Он КРУТИТСЯ каждый кадр, но callback @+16752 = **null на чистом прогоне (null-check пропускает → нет draw'а) / мусор 0xFFFFFFFF при гонке (→ INDIRECT-NULL)** — render-callback очередь **ПУСТА**, ничего не enqueue'нуто.
- ⇒ **Путь A, но реальный разрыв UPSTREAM:** render-путь достигается (sub_821BF748 работает), но **никакой продюсер не ставит текстурную render-работу** в очередь (+16752 остаётся null/garbage). Починка гонки лишь превратит crash→null-skip (ВСЁ РАВНО нет draw'а) ⇒ гонка НЕ гейт (подтверждает c10/c11). Гейт = **почему ничего не enqueue'ится**.
- **NEXT SESSION (глубокий корень):** prod-оракул — брейк в prod на sub_821BF748, прочитать ЖИВОЙ `*(r31+16752)` (реальный callback prod'а) + walk back к его ENQUEUER'у; проверить, почему меню variant A никогда не зовёт этот enqueuer (недостающий продюсер / нужное ему состояние объекта). Коммит: bbe19a5 (+ 212fd4a, 6eace49, e962c8c, acdf31b, 75d9d18 …), default-safe, НЕ пушены. Диаг: REX_ENQLOG ([enq]/[consumer]).

‼️ STATUS 2026-06-03 (cont.12 c11) — ⭐⭐ОБЪЕДИНЯЮЩИЙ ВЫВОД: тайтл СТРОИТ deferred render-ПРОГРАММУ (device+13568 сегменты), но variant A её НИКОГДА НЕ ИСПОЛНЯЕТ. Полностью: NIGHT-LOG «cont.12(c11)». Кратко:
- Трассировал executor-callback **0x821CC7A0** (в сегмент-записях `0001057C 821CC7A0 <ctx>`): это `sub_821CC7A0` — **producer work-queue** (пишет в per-process ring base=*(0x8200098C/90)+procType*108+11328, счётчик+11412; `KeSetEvent` будит consumer'а sub_821CC310, который dequeue→call *(item+16)→эмитит draw'ы).
- **Захукал (REX_ENQLOG): 0 вызовов за весь меню-прогон** (меню достигнуто, 36 shader-загрузок). ⇒ Сегмент-callback'и **заQUEUEны, но не вызываются** — deferred render-программа ПОСТРОЕНА, но не ИСПОЛНЯЕТСЯ. Fence-forward стопгап удовлетворяет deferred-segment waits ([fencefwd] fence 93717→93729), так что тайтл ПРОХОДИТ дальше, будто GPU исполнил программу — без её исполнения. render-path INDIRECT-NULL'ы (sub_821BF834, «WAIT_REG_MEM callbacks 0x821BF860/0x821CC7A0») — вероятно ПРОВАЛЕННЫЕ вызовы callback'ов этой программы.
- ⇒ **ОБЪЕДИНЯЕТ весь поиск рендерера:** тайтл строит текстурную render-программу (prod на том же guest = 54 пайплайна), но variant A её НЕ ИСПОЛНЯЕТ — доходят только напрямую-кикнутые init=0x30088 rect'ы. НЕ парс (device+13568 парсится), НЕ покрытие (draw'ы найдены=rect'ы), НЕ только гонка (чистые прогоны тоже без текстур), НЕ draw-логика — **deferred-программа просто не запускается** (тот самый стопгап-hazard cont.11).
- **2 пути рендерера (multi-session, выбрать next session):** (A) заставить тайтл исполнить СВОЮ программу — починить вызов render-callback'ов (INDIRECT-NULL гонка sub_821BF834 + GPU↔CPU handshake); ИЛИ (B) исполнять device+13568 сегменты САМИ — формат директория ВЗЛОМАН ({0x81LLLLLL,addr}, resolve guest=0xA0000000|(addr&0x1FFFFFFF)) → real CP walk'ает, исполняет PM4 каждого сегмента, вызывает его callback'и, заменяя fence-forward (fence растёт как РЕАЛЬНЫЙ результат). NEXT: захукать 0x821BF860 + consumer sub_821CC310 — подтвердить attempted-and-fail vs never-reached (различает A vs B).
- Коммиты: 212fd4a, 6eace49 (+ acdf31b, 75d9d18, e962c8c, bfdbeb1, 62d82a5, 279fd80), default-safe, НЕ пушены. Диаг: REX_ENQLOG ([enq]), REX_CHUNKDUMP (segment-follow).

‼️ STATUS 2026-06-03 (cont.12 c10) — ⭐ ПИВОТАЛЬНЫЙ ЭКСПЕРИМЕНТ = НЕТ: device+13568 = STATE-директорий, текстурных draw'ов нет даже на чистом прогоне ⇒ гонка НЕ гейт, дивергенция глубже. Полностью: NIGHT-LOG «cont.12(c10)». Кратко:
- **Раскрыт формат device+13568:** это НЕ inline-PM4, а **СЕГМЕНТ-ДИРЕКТОРИЙ** тайтла — записи + дескрипторы **{0x81LLLLLL, phys_addr}** (LLLLLL=длина сегмента в dword) + 0xC1/C2-указатели. Resolve сегмента: **guest = 0xA0000000 | (addr & 0x1FFFFFFF)** (проверено — сегменты парсятся как PM4). Brute-скан директория на op-0x22 = ЛОЖНЫЕ срабатывания (записи 0xC134xxxx декодируются как op 0x22) — прежние «1-16 draw'ов/чанк» = шум. Меню-директорий = 11-20 дескрипторов, init = 3.
- **Все сегменты = render STATE/EVENT/CALLBACK, 0 draw'ов.** На 3 ПОДТВЕРЖДЁННО-ЧИСТЫХ меню-бутах (g_nonBenignInd==0, swap#4002-4019): каждый сегмент = `realDraws=0 rectDraws=0 texFetch=0`. Сигнатуры: `C0004600`(COND_WRITE), `C0006000/61`, `C0025800`(EVENT_WRITE), SET_CONSTANT (8/4, не-текстурные), записи `00000A31/00010A2F`, и CALLBACK-записи **`0001057C 821CC7A0 <ctx>`** (указатель на код executor'а sub_821CC*). Реальные draw'ы (init=0x30088 rect'ы) идут в ГЛАВНЫЙ RING; текстурных draw'ов нет нигде.
- **⇒ Гонка NOTOKEN — НЕ гейт рендерера.** Её фикс убирает краш, но тайтл ВСЁ РАВНО эмитит только rect'ы+state, никогда текстурные draw'ы (prod на том же guest = 54 пайплайна). Блокер = **более глубокая дивергенция в draw-issuing пути**, НЕ гонка, НЕ парс (device+13568 парсится, в нём просто нет draw'ов), НЕ покрытие (draw'ы найдены — они все rect'ы). Объединяет весь поиск: ring + staging(SEGCP) + device+13568 — ВСЕ показывают только init=0x30088 rect'ы, 0 текстур.
- **СЛЕДУЮЩЕЕ (глубоко):** трассировать executor-callback **0x821CC7A0** (в записях `0001057C 821CC7A0`) + семейство sub_821CC310 — что эмитит per-work-item + почему только rect'ы (деградировавшее состояние render-объектов / контент-draw подсистема, которая не запускается). Race-fix + device+13568-парсер полезны downstream, но НЕ ведущий блокер. Коммиты: e962c8c, bfdbeb1 (+ acdf31b, 75d9d18, 62d82a5), default-safe, НЕ пушены. Диаг: REX_CHUNKDUMP (segment-follow [chunkscan]/[chunkdump]), g_nonBenignInd (race-индикатор).

‼️ STATUS 2026-06-03 (cont.12 c8/c9) — ⭐ RENDER-GATE РАСКРЫТ: тайтл строит ТОЛЬКО rect'ы; render-path INDIRECT-NULL = ГОНКА под NOTOKEN. Полностью: NIGHT-LOG «cont.12(c8)» + «cont.12(c9)». Кратко:
- ✅ **Option A (взлом device+13568) доказанно MOOT.** REX_CHUNKDUMP (per-swap brute draw-скан + структурный PM4-дамп) по 14460-swap прогону меню: чанки device+13568 = лишь 1-3 draw'а (макс 16), ВСЕ `init=0x30088` вырожденные rect'ы. 3 независимых скана (SEGCP=120 / CP=60 / brute) — все rect'ы, **0 текстур**, и в интро, и в меню. **Извлекать нечего** — тайтл НЕ строит текстурные draw'ы.
- ⭐ **Корень — render-path INDIRECT-NULL на uninit-объектах, и это ГОНКА.** REX_INDDUMP по неск. прогонам: набор INDIRECT-NULL'ов ВАРЬИРУЕТСЯ run-to-run (float-vtable+краш / garbage / полностью чисто без краша) ⇒ гонка. render-command объекты (sub_821CC310: r31=*(r3), spinlock@+0, callback@+16) ПУЛЯТСЯ; преемптивный NOTOKEN иногда запускает executor ДО того, как enqueuer поставил +16 → читает stale-float предыдущего жильца (320f/38.5f=screen-coord) как fn-ptr → INDIRECT-NULL → пропуск контент-draw'ов → краш. Cooperative сериализует (нет гонки) но морит декодер; NOTOKEN кормит декодер но гонит render. ⇒ нужен Step-1: РЕАЛЬНАЯ синхронизация (и декодер, и race-free render).
- ✅ **Починен 1 ДЕТЕРМИНИРОВАННЫЙ сайт (recompiler-gap, не гонка): sub_82367BD8** (commit 75d9d18). sub_82367B88 = 12-way switch; таблица @0x82367BA8 мис-распознана XenonAnalyse как КОД (12 адрес-dword'ов декодированы как `lwz r17,X(r22)`), поглотив case-0 handler @0x82367BD8 (=`li r3,0;blr`). Восстановил в PPCInvokeGuest (always-on switch). 0x82367BD8 INDIRECT-NULL (4 сайта в sub_82368xxx) исчез; дефолтный бут не регрессит. **Паттерн для др. jump-table gap'ов.**
- 🎯 **ПИВОТАЛЬНЫЙ вопрос next session:** строит ли RACE-FREE прогон текстурный контент в device+13568? Нужен структурный device+13568 парс НА ПОДТВЕРЖДЁННО-ЧИСТОМ буте (brute-скан текстур не видит; CHUNKCP десинкается). ⇒ Option A не мёртв — **загейчен на сначала получить чистый прогон**. РЕНДЕРЕР = шаг1: race-free планирование; шаг2: парс device+13568 на чистом буте → подтвердить/извлечь текстурные draw'ы. Коммиты сессии: acdf31b, 75d9d18 (+ ранние 4555fb8..3759320), default-safe, НЕ пушены.
- Диаг: REX_CHUNKDUMP ([chunkscan]/[chunkdump]), REX_INDDUMP ([inddump]). Запуск меню: `REX_NOTOKEN=1 REX_CSLEAK=1 ./runtime/out/sp_td_varianta ../private/extracted/default.xex`.

‼️ STATUS 2026-06-03 (cont.12) — ✅ ИСТИННАЯ КОНКУРЕНТНОСТЬ (REX_NOTOKEN) ЗАРАБОТАЛА; но планировщик — НЕ корень блокеров.
Полностью: NIGHT-LOG «2026-06-03 (cont. 12)». Кратко:
- BlockFenceYield (REX_BLOCKFENCE, прототип cont.11) — ОПРОВЕРГНУТ (0 эффекта на CPU: воркеры блокированы на
  g_objCv-сигнале объекта, НЕ на run-токене; форвард не в горячем цикле). УДАЛЁН из дерева.
- REX_NOTOKEN (preemptive) был полу-реализован → дедлок@124стр. ПОЧИНЕНО 2 баги (оба default-safe, кооп-путь
  байт-в-байт): (#1) GuestThreadRun держал g_waitMutex всю жизнь потока под NOTOKEN (нет g_coop-гейта) →
  self-deadlock в WaitObject; → 124→1387стр+VdSwap. (#2) fence-forward (sub_821C6E58/5DF0) был под if(g_coop) →
  под NOTOKEN GPU-fence-спин sub_821B9270 не удовлетворялся → дедлок; добавил g_preempt=!g_coop, гейт
  g_coop||g_preempt; → 1387→369750стр (2× дальше дефолта), доходит до интро-цикла. **2 потока по 99% (199% CPU)
  = 2 ядра** — конкурентность подтверждена; без краша; дефолтный кооп-бут НЕ регрессит (проверено).
- ⭐ НО конкурентность НЕ разблокирует ни фильм, ни переход — ОПРОВЕРГАЕТ «голодовку» cont.11: под полными
  ядрами декодеры (sub_82339428) БЛОКИРОВАНЫ на object-wait (frame-pool чёрный, varied=0), а tid=10
  (sub_8211B740) проходит ДАЛЬШЕ (мимо SIMD-цикла sub_822C14E8) но БЛОКИРУЕТСЯ глубже в sub_82435C48
  (~0% CPU = wait, не spin), всё равно не доходит до sub_8210AF90 за 90с с полным ядром. ⇒ потоки ЖДУТ
  невыполненных условий/сигналов (title-specific RE), а не голодают по CPU. Планировщик НЕ корневой блокер.
- ЗАКОММИЧЕНО (default-safe, НЕ пушено): 2 NOTOKEN-фикса + удаление BlockFenceYield. ЗАПУСК конкурентности:
  `REX_NOTOKEN=1 ./runtime/out/sp_td_varianta ../private/extracted/default.xex` (⚠ NOTOKEN ≠ FAIRSCHED:
  взаимоисключающие; под NOTOKEN sub_821B9270-спины жгут ядро вместо token-yield, безвредно но расточительно).
- ✅**НАПРАВЛЕНИЕ (b) ПРОЙДЕНО (USER выбрал, commit 8b82515):** tid=10-блокер sub_82435C48 РАСШИФРОВАН — это
  ДЕДЛОК на ОСИРОТЕВШЕЙ критической секции 0x82818628. gdb: tid=10 висит в RtlEnterCriticalSection (guest-CS, не
  spin); host recursive_mutex.__owner = УЖЕ ВЫШЕДШИЙ LWP. REX_CSLEAK-диаг назвал виновника: GPU/video-init поток
  **sub_8242B4A8** выходит, держа CS 0x82818628 (его RtlLeaveCriticalSection-ветка пропущена — fence-forward
  стопгап не воспроизводит настоящую GPU-последовательность). Под кооп-токеном не всплывает (сериализация); под
  NOTOKEN — дедлок. ФИКС (NOTOKEN-gated, default-safe): при выходе guest-потока освобождать удерживаемые CS.
  РЕЗУЛЬТАТ: tid=10 разблокирован → тайтл **ВЫХОДИТ ИЗ ИНТРО в menu/frontend-setup** (грузит
  Global/Textures/Global.bin, аллоцирует menu-буферы) — РЕАЛЬНЫЙ ПРОГРЕСС — затем КАСКАД [INDIRECT-NULL]
  (нулевые vtable/jump-table слоты, вкл. CP/render-диапазон 0x821BF834/0x821C71A4) → SIGSEGV ~34с. ⭐СХОДИТСЯ с
  форс-путём REX_XFLAG: ОБА упираются в одни menu-setup INDIRECT-NULL блокеры (0x82292D08/0x8236859C/sub_8215DE84);
  новые GPU-range INDIRECT-NULL ⇒ меню ПЫТАЕТСЯ РЕНДЕРИТЬ через GPU-vtable'ы, которых нет (нет настоящего CP) ⇒
  **РЕНДЕРЕР = блокер живого меню**. (sub_8210AF90 всё ещё НЕ срабатывает — выход из интро идёт другим путём.)
  Диаг REX_CSLEAK (gated). ⚠ NOTOKEN ≠ FAIRSCHED.
- ✅**INDIRECT-NULL recovery НАЧАТА через prod-оракул (USER выбрал, commits 7f901e9/65ec22a):** REX_INDDUMP
  (дамп GPR+vtable-цепочки на null-сайтах) классифицировал каскад: (1)⭐КОРЕНЬ lr=0x82292D08 в sub_82292CE0 —
  виртуальный вызов на **null-синглтоне** *(0x827FD56C)=0 (не сконструирован); (2) 4 сайта→0x82367BD8 =
  пропущенная **boundary-limited** jump-table sub_82367B88 (таблица 0x82367BA8); (3) 3 сайта sub_825AB = мусор
  (downstream-симптом). **Prod-оракул** (база ФИКС 0x100000000, $rsi=base на входе fn, r31=const): sub_82292CE0 →
  obj=0x45FE78B0, vtable=0x820948B0, vt[1]=0x824927B8; prod-путь main→sub_82249970→sub_82150770→sub_82292CE0.
  Конструктор класса (статически): vtable 0x820948B0 ставит ТОЛЬКО sub_824883E0 ← sub_824898C0 ← getter
  sub_8248F4C8 (кэш 0x82819358) ← sub_8248F988. ✅**КОРЕНЬ НАЙДЕН+ИСПРАВЛЕН (commit 724c104):** конструкция
  валилась E_OUTOFMEMORY (0x8007000E) — цепочка getter→sub_82497720→sub_82497678→sub_824A5E50→sub_824A5DD0→
  **ExAllocatePoolTypeWithTag**, а это (+ExAllocatePoolWithTag/ExFreePool) был СТАБ, возвращавший NULL → ВСЕ
  пул-аллокации меню падали. РЕАЛИЗОВАЛ их (kernel.cpp, НЕ gated — реальный примитив: 16-байт bump над ≥8MiB
  аренами из g_virtNext; ABI r3=size r4=tag r5=type). РЕЗУЛЬТАТ: синглтон СТРОИТСЯ (sub_824883E0 0→1), меню идёт
  НАМНОГО ДАЛЬШЕ — грузит СВОИ РЕНДЕР-АССЕТЫ (шейдеры SPTextured/SPBackdropTextured.xbv/.xbp, текстуры
  Global.bin/LipSyncTextures). Дефолт-бут НЕ регрессит ([stub] ExAllocatePoolType исчез). **Cluster item 1 РЕШЁН.**
- ⚡**Post-pool краши = NOTOKEN+stub-CP ГОНКИ** (host VblankPump-CP фаерит GPU-прерывания / re-run'ит ринг
  параллельно menu-потоку): (item2, commit fc297da) **gfx-interrupt sub_821C7170** дереференсит completion-объект
  `*(device+10900)`, который = sentinel 0xFFFFFFFF между сабмишенами → наш CP фаерит source=1 спурьёзно →
  wild-deref 0xFFFFFFFF → SIGSEGV. GUARD в FireGfxInterrupt (skip source=1 если device+10900==0xFFFFFFFF) —
  краш ушёл, дефолт не регрессит. НО тайтл всё равно дохнет ~32с в **sub_82204D08**: INDIRECT-NULL каскад с
  таргетами-ASCII (path-строка исполняется как vtable) = downstream-коррупция. ⇒ **past пул-фикса меню — болото
  variant-A конкурентности+неинициализированных объектов: (a) NOTOKEN guest-гонки; (b) VblankPump host-CP
  асинхронно vs guest GPU-submission; (c) string-as-code коррупция; (d) jump-table sub_82367B88.** Фиксить
  краши по одному под NOTOKEN-гонками = whack-a-mole; СТРУКТУРНЫЙ фикс = **синхронизированный CP (рендерер).**
  Пул + guard — корректные стопгапы, довели меню до загрузки ШЕЙДЕРОВ.
- ✅**USER выбрал CP-СИНХРОНИЗАЦИЮ → СДЕЛАНО (commit 6079c3a):** корень гонок (b) — VblankPump (host) под NOTOKEN
  не берёт лока, исполняет ExecuteRing+FireGfxInterrupt(guest-колбэк) конкурентно с guest. Фикс: `g_gpuMutex`
  (NOTOKEN-only) — памп держит его на CP+interrupt-батче, и guest GPU-границы (kick sub_821C6600 + completion-setup
  sub_821C73D8 пишет device+10900) тоже → сериализация памп↔guest БЕЗ глобал-токена (декодеры конкурентны;
  recursive). Без дедлока, дефолт не регрессит. РЕЗУЛЬТАТ: **gfx-interrupt краш sub_821C7170 УШЁЛ**, тайтл прошёл
  В СВОЙ RENDER-КОД → SIGSEGV ~33с на INDIRECT-NULL с **FLOAT-таргетами** (0x43A04000=320.5f, 0x421A0000=38.5f) в
  0x821Bxxxx/0x821Cxxxx = render-код тайтла читает ВЕРШИННЫЕ/render-данные как функции (stub-CP не держит draw-state).
- ⭐⭐**МЕНЮ ДОСТИГЛО ГРАНИЦЫ РЕНДЕРЕРА (естественно, без REX_XFLAG).** Цепочка сессии: пул→синглтон→шейдеры;
  CP-sync→памп не гонит guest→меню РЕНДЕРИТ; рендеринг требует настоящего GPU draw-state. **СЛЕДУЮЩЕЕ = РЕНДЕРЕР
  (RENDERER-PHASE-PLAN, теперь достижим через natural menu):** транслятор draw-state (vfetch reg0x4800→верт.буферы+layout,
  ALU reg0x4000→юниформы, текстуры, RT/viewport) + 19 .updb шейдеров→SPIR-V → vkCmdDraw в swapchain (рендер-тред владеет).
  Прочее (не на пути к рендереру): jump-table sub_82367B88 (boundary-limited), декодеры/фильм под NOTOKEN.
  Запуск: `REX_NOTOKEN=1 REX_CSLEAK=1 ...default.xex` → меню(шейдеры) → render-SIGSEGV ~33с. Диаг REX_INDDUMP.
- ⚠**cont.12(c5): транслятор ЗАБЛОКИРОВАН на ВХОДЕ.** Проверка (REX_DRAWLOG главный ринг + REX_SEGCP route B во
  время меню): до CP долетают ТОЛЬКО `init=0x30088 numInd=3 prim=8` rect'ы (60+120), 0 текстур — реальные draw'ы
  меню НЕ доходят (та же coverage-проблема cont.11: они в custom deferred cmd-buffer пуле тайтла device+13568:
  PM4+inline-вершины+0x8100xxxx сегмент-линки, растёт ~0x88680/кадр; REX_CHUNKCP linear-parse десинкается). Vulkan-
  сторона ГОТОВА (CP извлекает state SET_CONSTANT→reg 0x4000 ALU/0x4800 fetch+tex; шейдеры портированы; рендер-тред
  владеет swapchain) — нет ВХОДА. ⇒ настоящая работа рендерера = EXTRACTION draw'ов из deferred cmd-buffer'а.
- ✅**USER выбрал: ИНТЕРЦЕПТ draw-record функции** (хукнуть fn тайтла, пишущую DRAW_INDX, транслировать в источнике,
  обходя binary-формат). cont.12(c6): случайный prod-сэмплинг ШУМНЫЙ (поймал blocked-потоки+main-loop, не быструю
  draw-fn). **NEXT (таргетированно): найти draw-record fn** инструментировав cmd-buffer записи variant A — тайтл
  пишет DRAW_INDX_2 (op 0x36) в чанк device+13568; хукнуть cmd-buffer reserve/write (advance *(device+13568+4)
  writeptr, cont.11-map) + лог guest-LR при записи op-0x36 header ⇒ LR внутри draw-fn. Затем прочитать её D3D-state
  (верт.стримы/шейдер/текстуры/RT из device-объекта) → vkCmdDraw. Альт: hardware-watchpoint на cmd-buffer (flaky).
  Остаток рендерера = эта draw-extraction + per-draw-type Vulkan-трансляция (multi-session).
- ⭐⭐**МЕНЮ КИКАЕТ РЕАЛЬНУЮ GPU-РАБОТУ — РИНГ ОЖИЛ** (был WPTR=37 весь intro): SIGSEGV ~34с в стеке VblankPump→
  ExecuteRing(rptr=37)→ExecutePM4→ExecuteType3(op=0x54 EVENT_WRITE)→FireGfxInterrupt→**sub_821C7170**→
  PPC_STORE(r31+0) r31=garbage. Меню грузит Global/Meshes/Textures + эмитит PM4 EVENT_WRITE → fires gfx-interrupt
  → колбэк sub_821C7170 (его же INDIRECT-NULL 0x821C71A4 target=0x003F8000 garbage пропущен → r31 garbage →
  store крашит). ⇒ рендерер ТЕПЕРЬ имеет живой PM4; краш = downstream null-vtable в gfx-interrupt пути.
- **СЛЕДУЮЩЕЕ (РЕНДЕРЕР, по приоритету):** кластер неинициализированных menu/GPU-объектов, каждый отдельно
  восстановим: (1) почему getter sub_8248F4C8 пропускает конструкцию синглтона (lazy-ветка — сравнить с prod
  hardware-watchpoint'ом на 0x182FD56C: gdb `watch` через stop()-колбэк падал на software, делать top-level;
  ИЛИ найти кто пишет 0x827FD56C); (2) gfx-interrupt краш sub_821C7170 (garbage r10/r31 — INDIRECT-NULL
  0x821C71A4 = ещё null-vtable; чинить FireGfxInterrupt контекст ИЛИ восстановить vtable); (3) jump-table
  sub_82367B88 (boundary-limited, function-split фикс). Запуск: `REX_NOTOKEN=1 REX_CSLEAK=1 REX_INDDUMP=1
  ./runtime/out/sp_td_varianta ../private/extracted/default.xex` → menu-load+SIGSEGV ~34с. Прод-оракул:
  `cd out/build/linux-amd64-release; SDL_VIDEODRIVER=x11 LD_LIBRARY_PATH=. ./south_park_td
  --game_data_root=.../private/extracted --user_data_root=.../private/userdata` (символы, base=0x100000000;
  под gdb `handle SIGSEGV nostop noprint pass`). Tooling: /tmp/prod_oracle.gdb (read obj/vtable). Прочие
  хвосты: (a) декодеры висят на object-wait под NOTOKEN (фильм); sub_8242B4A8 CS-skip — упираются в настоящий CP.

———————————————————————————— (ниже — cont.11 статус) ————————————————————————————

‼️‼️‼️ STATUS 2026-06-02 (cont.11) — ✅✅ ДЕКОДИРОВАННЫЙ ИНТРО-ФИЛЬМ НА ЭКРАНЕ В ЦВЕТЕ. Рендерер increment 3: present декод-кадра (grayscale→ЦВЕТ).
Полностью: NIGHT-LOG «2026-06-02 (cont. 11)». Кратко (USER выбрал на развилке «оба: сперва present, потом RE»):
- ГЕОМЕТРИЯ КАДРА РАСШИФРОВАНА. ffprobe: фильм 1280×720 wmv3 (VC-1). Буфер пула = 0x101440=1,053,760 B (МЕНЬШЕ
  полного 720p YUV420=1,382,400 ⇒ не планарный 420). REX_VIDEODUMP дампит varied-буферы пула на swap#220;
  визуализация (numpy/PIL) + row-diff автокорреляция → РЕЗКИЙ минимум на **stride 1344** (не 1280). Рендер на
  1344 = идеально чистый кадр (горы/ёлки/город RHINOPLASTY/Картман). ⇒ **Y-плоскость = ЛИНЕЙНАЯ, pitch 1344,
  1280×720, offset 0, 8-bit.** 360-tiling (Xenia GetTiledOffset2D) ДЕЛАЕТ ХУЖЕ ⇒ surface НЕ tiled, просто
  паддинг-pitch. ⚠ХРОМА НЕ расшифрована: после Y остаётся ~86KB хвоста — мало для 420 (нужно ~460KB);
  NV12/планар-догадки → неверные цвета (magenta/green). ⇒ пока **grayscale (luma)**, цвет = NEXT (a).
- РЕАЛИЗОВАНО (vulkan_render.cpp+rex_render.h+kernel.cpp, gated REX_RENDER, дефолт-бут НЕ регрессит): VdSwap
  публикует g_videoBufs[16] в рендер-тред (PublishVideo, ВНУТРИ существующего `if(Enabled())` ⇒ 0 стоимости в
  дефолте). Рендер-тред читает гостевую память напрямую (extern g_base), каждый present выбирает ЧИСТЫЙ кадр,
  разворачивает luma → host-visible BGRA staging (gray=Y,Y,Y,255) → vkCmdCopyBufferToImage в swapchain (БЕЗ
  шейдеров). ВЫБОР КАДРА (гонка с декодером, пишущим память): v1 freshest→ТИРИНГ; v2 settled≥2→чёрный ШОВ
  (коп-планировщик СТОПОРИТ декодер на полпути → полузаписанный буфер выглядит settled; доказано
  REX_RENDER_DUMPSEL — дамп выбранного буфера, оффлайн на 1344 = чёрные полосы); v3 (финал) settled≥2 И
  complete (сетка 72×32: строка mean luma≤12 = незаписанная чёрная полоса; нужно ≥69/72 записанных) → только
  ПОЛНОСТЬЮ декодированные кадры → ЧИСТЫЙ полный кадр (min=43, mean=152). ✅ Скриншот доставлен. REX_RENDER_SHOT=N
  теперь триггерит на N-й ДЕКОД-кадр (развязка с гонкой render/decode).
- ТЕСТЫ (timeout -s KILL, /dev/shm чистится): (1) REX_RENDER=1 REX_FAIRSCHED=1 → окно (RADV POLARIS11), декод
  идёт (video buf selection валиден, чередует buf6/buf9), 0 device-overwrite, без краша, чистый grayscale-кадр.
  Движение ЕСТЬ но МЕДЛЕННОЕ (без REX_MOVIE_EOF тайтл ползёт: ~191 swaps/55s — коп-троттл/перф, НЕ баг present).
  (2) дефолт-бут НЕ регрессит (0 device-overwrite, без краша, 0 строк [render], доходит до intro).
- ЗАКОММИЧЕНО (gated, откатываемо, НЕ пушено).
- ✅**(a) ЦВЕТ — СДЕЛАНО** (тот же cont.11): хрома = ОТДЕЛЬНЫЕ аллокации, НЕ в хвосте Y. Decoder NtAlloc-лог
  (LR 0x8244DD2C) = повторяющийся ТРИПЛЕТ/кадр: Y(req 0x101440) → U → V (req 0x40520 каждая) = планарный I420.
  В g_videoBufs: Y на idx 0,3,6,9,12,15,18 (size 0x101440), его U=idx+1, V=idx+2 (0x40520). Хрома pitch=672
  (=luma/2), 640×360, mean≈128 (автокорр на малых буферах). YUV→RGB = FULL-RANGE BT.601, порядок U=первая
  малая, V=вторая. Реализовано: kernel ловит base+size (g_videoBufSz[24]); PublishVideo шлёт оба; рендер-тред
  выбирает ТОЛЬКО Y (size 0x101440), читает U/V из след. 2 слотов, целочисл. YUV→RGB на CPU → BGRA (chroma 2× nearest).
  ON SCREEN: R≠G≠B (mean 134/160/158), дефолт-бут НЕ регрессит. **СЛЕДУЮЩЕЕ:** **(b) ПЛАВНОЕ ДВИЖЕНИЕ**
  — тайтл ползёт без REX_MOVIE_EOF (декодер коп-троттлен); быстрее планировщик / настоящий блок-fence ускорит
  воспроизведение (Step 1). **(c)** [опция (a) исходного промпта] RE sub_8211B740 дивергенции → ЕСТЕСТВЕННЫЙ
  intro→menu (ретайрит REX_XFLAG). ✅НАЧАТО (cont.11): prod-оракул gdb-bt на __imp__sub_8210AF90 дал ТОЧНУЮ
  цепочку sub_82450FD0→sub_82250420(tid=10)→**sub_8211B740**→sub_8210AF90 (в prod прямой вызов +0x1220; в
  varianta — ОДИН из 7 индирект-vtable PPC_CALL_INDIRECT_FUNC(ctr), потому статически невидим — у sub_8210AF90
  НОЛЬ прямых вызовов). sub_8211B740 (ppc_recomp.3.cpp:11035, 720 строк) = ~7 индирект-вызовов под ~6 ветками
  (0x8211B7A4 beq на sub_8224FB68 lookup; +818/8EC/9C0/9AC/BBE8). Дивергенция (state/data) = ОДНА ветка идёт не
  туда. СЛЕДУЮЩЕЕ: инструментировать branch-path sub_8211B740 в varianta (REX_FAIRSCHED, tid=10 бежит) + diff с
  prod (gdb-step до bctrl→sub_8210AF90) → первая расходящаяся ветка + её входное состояние = цель фикса. ЗАПУСК ВИДИМОГО ФИЛЬМА: `REX_RENDER=1 REX_FAIRSCHED=1
  ./runtime/out/sp_td_varianta ../private/extracted/default.xex`. Диаг (env-gated): REX_VIDEODUMP (дамп
  буферов пула /tmp/vbufN.raw на swap#220), REX_RENDER_DUMPSEL (дамп выбранного рендером буфера /tmp/selbuf.raw),
  REX_RENDER_SHOT=N (скрин N-го декод-кадра → /tmp/varianta_shot.ppm). Хост-визуализатор кадра: stride 1344, 8bpp gray.
- 🚧**РЕНДЕРЕР (USER выбрал FULL PM4→Vulkan)** — ОСНОВА заложена + НАЙДЕН гейт-блокер. ⛔КРИТИЧНО: НИ ОДНО
  достижимое состояние НЕ строит реальные текстур-draw'ы — intro = только untextured rects (init=0x30088, 8/кадр,
  0 текстур); forced-transition (REX_XFLAG) тоже только rects + INDIRECT-NULL (target=0x0@lr=0x82292D08,
  0x82367BD8@0x8236859C, + sub_8215DE84). Movie quad = НЕ PM4 (scaler/overlay path, шейдер SpMovie 3-плоскости
  YUV). ⇒ транслятор draw-state ЗАБЛОКИРОВАН пока тайтл не дойдёт до ЖИВОГО экрана (меню/геймплей) — это
  ПРЕДУСЛОВИЕ (deep RE экранных INDIRECT-NULL / sub_8211B740). ✅ШЕЙДЕР-ТУЛЧЕЙН ГОТОВ (varianta/tools/shaderc/):
  19 .updb = ОРИГИНАЛЬНЫЙ D3D9 HLSL (ps_3_0, все .psh; VS только .xbv → писать generic VS); libshaderc стоит →
  compile.cpp (GLSL→SPIR-V) + build.sh; портировал 5 шейдеров (все паттерны, вкл. SpMovie YUV→RGB) → ВАЛИДНЫЙ
  SPIR-V. NEXT рендерера (по зависимости): (1) разблокировать прогресс экранов (INDIRECT-NULL/sub_8211B740) →
  реальные draw'ы; (2) draw-state транслятор (vfetch reg0x4800→верт.буферы; ALU reg0x4000→юниформы; текстуры
  reg0x4800→VkImage; RT/viewport) + VkPipeline (портир.PS + generic VS) + vkCmdDraw в swapchain (рендер-тред уже
  владеет); (3) портировать остальные 14 шейдеров. Полностью: NIGHT-LOG «cont.11, renderer».

———————————————————————————— (ниже — cont.10 статус) ————————————————————————————

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
