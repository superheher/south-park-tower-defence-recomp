# Launcher — Quality-of-Life Backlog

A running list of launcher/onboarding ideas **not yet implemented**, so they can be
picked up and closed later. Tick the box when done. Each item notes its rough
**value/effort** and a concrete **How** (files, cvars, approach) grounded in the
current codebase.

Effort legend: ⚡ quick · 🔧 medium · 🏗 larger.
Everything here is intended to stay **app-side** (`south-park-recomp/src/launcher/`,
wired via `ReXApp` hooks) unless it says "SDK patch" — engine edits go through
`south-park-recomp/patches/` only.

---

## Already done (for context — do not redo)
See `INTEGRATION.md` and the `launcher-onboarding` memory for details.
- STFS/folder direct-mount + auto file-vs-dir; CLI>config>onboarding resolution; per-user
  `launcher.toml`; `--validate` JSON + exit codes; `--setup`/`--no_setup`/`--embedded`.
- Onboarding wizard (path field + in-ImGui browser), gamepad/keyboard/mouse, drag-and-drop
  (Windows), auto-detect a dump in the exe's own folder, friendly folder/parent resolution
  that prefers the game and skips DLC.
- Settings panel + sensible defaults (FULL license, free cursor, icon, 1280×720, skip arcade
  logo, mnk), **graphics** (internal resolution, output filter + sharpness, letterbox/overscan),
  Mute, Invincibility, auto-install DLC (+ DLC shown by name in the wizard), wrong-game titleID
  warning.
- Auto-install DLC into the content store (game enumerates + opens it).
- Folder buttons (data/logs/game) + Backup saves; `--version` / `--print_config`.
- Embedded .exe icon; first-launch "compiling shaders" overlay.

---

## Settings / UX
- [ ] **In-game settings (no restart)** — 🔧 open the curated settings UI from a hotkey while
      playing, instead of re-running with `--setup`.
      *How:* in `OnCreateDialogs`, `rex::ui::RegisterBind("bind_launcher_settings","F5",...)` to
      toggle an `ImGuiDialog` that reuses the wizard's settings rendering (factor the settings
      block out of `OnboardingDialog::OnDraw` into a shared helper). Hot cvars apply live; window/
      fullscreen/resolution are restart-scoped → persist to config, note "applies next launch".
- [ ] **"Reset to defaults" button** — ⚡ clear the `[settings]` in `launcher.toml` and re-apply
      `LauncherDefaults()`. Add to the settings panel.
- [ ] **UI / font scaling for high-DPI (4K)** — 🔧 the ImGui UI is small on 4K.
      *How:* set `io.FontGlobalScale` and/or `ImGui::GetStyle().ScaleAllSizes(s)` from the window
      DPI (or a `launcher_ui_scale` cvar) in `OnConfigureFonts` / before drawing dialogs.
- [ ] **Better error explanations** — ⚡ e.g. if `ResolveGameSource` finds only DLC/no title, say
      "found add-ons but no game here". Extend `Validate()` reasons.

## Game selection
- [ ] **Recent games list (MRU)** — 🔧 remember multiple dumps/versions, pick from history.
      *How:* bump `schema_version`, add a `recent = [...]` array to `launcher.toml`; the wizard
      shows a small selectable list above the path field; `ResolveAndPersist` pushes the chosen
      game onto the MRU.
- [ ] **Drag-and-drop on Linux/GTK** — 🔧 currently Windows-only.
      *How (SDK patch):* in `window_gtk.cpp` set up a drop target (GtkDropTarget / `drag-dest`) and
      fire `Window::OnFileDrop(FileDropEvent)` like `window_win.cpp:1107` does. App side already
      consumes `OnFileDrop`.
- [ ] **Auto-scan a few more first-run locations** — ⚡ besides the exe folder, optionally check
      Downloads/Desktop. *How:* extend `AutoDetectGameNearExe` (keep it bounded; mind scan time).

## Launch / convenience
- [ ] **Remember window position across launches** — 🔧 (size is already a setting).
      *How:* add `window_x`/`window_y` (SDK patch to `window.cpp`, or app-side via `SetWindowPos`
      on the HWND); read at startup, save on move/close into `launcher.toml`.
- [ ] **Desktop / Start-Menu shortcut on first run** — 🔧 offer to create a `.lnk`.
      *How (Windows):* `IShellLink` + `IPersistFile` to write a shortcut to the exe (with the
      embedded icon). Gate behind a one-time prompt / a settings button.
- [ ] **Single-instance guard** — 🔧 don't launch twice.
      *How (Windows):* named `CreateMutexW`; if already present, `SetForegroundWindow` the existing
      window and exit. Do it in `windowed_app_main_win.cpp` (SDK patch) or early in the app.
- [ ] **Portable mode** — 🔧 keep saves next to the exe (USB/transfer) instead of `%LOCALAPPDATA%`.
      *How:* a `portable` toggle/cvar → set `user_data_root = exe_dir/userdata` before runtime in
      `OnConfigurePaths`. (A `portable.txt` next to the exe could auto-enable it.)
- [ ] **File association for STFS packages** — 🏗 double-click a dump → launch.
      *How (Windows):* register a ProgID + extension in the registry; pass the file as
      `--game_data_root`. Advanced / installer territory.

## Saves / data
- [ ] **Restore-from-backup** — ⚡ complement to "Backup saves now": pick a `backups/<ts>` and copy
      it back. *How:* list `user_data_root/backups/*`, copy the chosen one over the live profile.
- [ ] **Playtime / last-played** — ⚡ cosmetic; store session start/end + total in `launcher.toml`,
      show in the wizard/settings.

## Graphics / audio
- [ ] **Volume slider** — 🔧 only Mute exists today.
      *How (SDK patch):* add an `audio_gain`/`volume` cvar in `sdl_audio_driver.cpp` (scale samples),
      then expose a slider in settings (ManagedSettings, CvarType::Double already exists).
- [ ] **FPS limit / present-mode options** — 🔧 if useful; expose relevant present cvars
      (`vulkan_allow_present_mode_immediate`, etc.) — verify they apply for this title.

## Robustness / diagnostics
- [ ] **Surface crashes** — 🔧 `main.cpp` already writes `crash_backtrace.txt`. On the next launch,
      if a recent crash file exists, show a dialog "last run crashed — open log?" with an
      Open-logs button. *How:* check mtime of `crash_backtrace.txt` at startup.
- [ ] **First-launch overlay: dismiss on first guest frame** — 🔧 today it's timeout/input only,
      because no frame signal is wired (`SetGuestFrameStats` is never called). *How:* wire a
      `FrameStatsProvider` (guest present count) and have `LoadingDialog` close when count>0.

## Frontend integration
- [ ] **Metadata export helper** — 🔧 emit a small manifest (title, titleID, icon path) for
      Playnite/Steam, e.g. `--export_metadata=<dir>`. Reuses `Validate` + the embedded icon.
- [ ] **Gamepad-nav polish on a real Steam Deck** — 🔧 test/iterate the wizard nav with an actual
      controller (onboarding gamepad input is via XInput/REX_INPUT_FILE; Linux onboarding gamepad
      would need SDL gamepad pre-runtime).
