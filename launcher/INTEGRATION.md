# South Park: Let's Go Tower Defense — Launcher & Integration Guide

This build is **self-launching**: run the executable, point it once at *your own*
copy of the game, and play. There is no separate launcher process, no required
command line, and no `.bat`.

It is also **separable by contract**: everything the built-in onboarding does is
also exposed as plain flags, a config file, and a `--validate` subcommand, so an
external frontend (Playnite, Steam, EmuDeck, Lutris, a custom script…) can drive
the *same* engine without the UI. "Combined by default, separable by contract."

> **Bring your own dump.** This package contains **no game data**. You must supply
> your own legally-obtained copy of *South Park: Let's Go Tower Defense* — either a
> single STFS package file (an Xbox 360 console dump) or a folder of extracted game
> files. Nothing is downloaded and your copy never leaves your machine.

---

## 1. For players (zero command line)

1. Run `south_park_td.exe`.
2. On first run, the onboarding wizard appears. Point it at your game:
   - type/paste the path, **or** use **Browse…** (an in-engine file browser), and
   - pick either the **STFS package file** or the **extracted folder**.
3. The wizard validates it (shows the title and title ID), tweak any settings, then
   **Play**. Your choice is remembered; next launch goes straight into the game.

Controls in the wizard: **mouse**, **keyboard** (Enter = Play, Esc = Quit), or a
**gamepad** (Start = Play, B = Quit, D-pad/A to navigate — good for a Steam Deck or
couch). To change your game or settings later, run with `--setup`.

---

## 2. The contract (for frontends)

### 2.1 Game source
`--game_data_root` accepts any of these and auto-detects which (no extraction step):
- a single **STFS package file** (the raw console dump) — **any filename**; it is
  identified by content (the `CON `/`LIVE`/`PIRS` magic), not by name;
- a **`default.xex`** file (its containing folder is used);
- an **extracted folder** containing `default.xex` at its root; **or**
- **any parent folder** of either. The launcher searches it with bounded recursion
  (depth ≤ 6) and resolves to the game inside — so a raw dump laid out as
  `<titleID>/000D0000/<hash>` works when you point at the title-ID folder, the
  game's display-name folder, etc.

When a folder holds several packages (e.g. the game **and** DLC), the launcher
prefers the **bootable title** (content types `kArcadeTitle`, `kGamesOnDemand`,
`kInstalledGame`, …) and ignores add-ons / saved games / avatars. The concrete
resolved path is what gets persisted and mounted (see the `path` field in §2.5).

### 2.2 cvars = the public API
Pass as `--name=value` (or `--name value`). Booleans accept `true`/`false`/`1`/`0`.

| cvar | type | default | meaning |
|------|------|---------|---------|
| `game_data_root` | path | *(none)* | Game source: STFS package file **or** extracted folder |
| `user_data_root` | path | *per-user dir* | Where saves / shader cache are written (must be writable) |
| `license_mask` | uint | `1`¹ | `1` = full version (saves progress); `0` = trial (persists nothing) |
| `window_width` | int | `1280`¹ | Window width |
| `window_height` | int | `720`¹ | Window height |
| `fullscreen` | bool | `false` | Start fullscreen *(applies next launch)* |
| `vsync` | bool | `true` | Vertical sync |
| `mnk_mode` | bool | `true`¹ | Use keyboard & mouse as a controller |
| `audio_mute` | bool | `false` | Mute audio |
| `always_win` | bool | `false` | Invincibility — your base can't be destroyed |
| `skip_arcade_logo` | bool | `true`¹ | Skip the XBLA arcade splash |
| `window_icon` | path | *bundled icon*¹ | `.ico` for the window/taskbar |

¹ These differ from the bare engine default: the launcher applies sensible defaults
underneath config and CLI (so `license_mask` is `1`, the window is 1280×720, the
arcade logo is skipped, keyboard works as a controller, and the bundled
`SouthPark.ico` is used) unless you override them.

**Launcher mode cvars:**

| cvar | meaning |
|------|---------|
| `--validate=<path>` | Validate a game source, print JSON, and exit (see §2.5) |
| `--setup` | Force the onboarding wizard even if a game is already configured |
| `--no_setup` / `--embedded` | Never show any UI; if the game is unresolved, exit `64` |

Online co-op is intentionally **not** exposed by the launcher (that feature is paused).

### 2.3 Resolution priority
For the game source and every setting: **CLI flag > saved config > built-in default**
(and the onboarding wizard is the fallback when nothing resolves and UI is allowed).
A flag present on the command line is never overwritten by the saved config.

### 2.4 Per-user config file
The launcher persists the last game + chosen settings here:

| OS | Path |
|----|------|
| Windows | `%LOCALAPPDATA%\SouthParkTD\launcher.toml` |
| macOS | `~/Library/Application Support/SouthParkTD/launcher.toml` |
| Linux | `$XDG_CONFIG_HOME/SouthParkTD/launcher.toml` (or `~/.config/SouthParkTD/…`) |

Format (TOML; `schema_version` lets the format evolve):
```toml
schema_version = 1
game_path = "/path/to/dump_or_folder"
[settings]
license_mask = "1"
window_width = "1280"
audio_mute   = "false"
# … one entry per cvar above
```
A frontend may write this file directly instead of (or in addition to) passing flags.

### 2.5 `--validate` — the engine is the validator
```
south_park_td --validate="/path/to/game"
```
Resolves the source (a folder/parent is searched — see §2.1), reads it with the
engine's own STFS reader (no window, no mount) and prints **one line of JSON** to
stdout, then exits:
```json
{"ok":true,"title":"South Park","titleID":"58410931","path":"…/000D0000/A760…","reason":""}
```
- `path` is the **concrete** source the launcher resolved to (the actual package
  file or extracted folder) — useful when you pointed `--validate` at a parent
  folder and want to know/record what it found.
- For an STFS package: `title` and `titleID` come from the package metadata.
- For an extracted folder: `title` is `"(extracted folder)"`, `titleID` empty.
- On failure: `{"ok":false,"title":"","titleID":"","path":"","reason":"<why>"}`.

### 2.6 Exit codes
| code | meaning |
|------|---------|
| `0` | success (or `--validate` reported `ok:true`) |
| `64` | `EX_USAGE` — `--no_setup`/`--embedded` but no game is configured |
| `65` | `EX_DATAERR` — `--validate` reported `ok:false` |

---

## 3. Frontend recipes

**Validate a dump the user picked, then add it to the library:**
```sh
south_park_td --validate="/path/to/dump"      # parse JSON; exit 0 = good
```

**Launch from a frontend (no UI, frontend owns the settings):**
```sh
south_park_td --game_data_root="/path/to/dump" \
              --user_data_root="/path/to/writable/saves" \
              --no_setup --license_mask=1 --fullscreen=true
```
`--no_setup` guarantees the engine never pops its own wizard; if the path is bad it
exits `64` so the frontend can surface the error.

**Playnite / Steam (Add a Non-Steam Game) / EmuDeck:** point the launch action at
`south_park_td.exe`, validate the user's dump once with `--validate`, then use the
launch line above. On a Steam Deck, you can instead ship with no flags and let the
gamepad-navigable onboarding wizard handle first-run.

---

## 4. Notes
- Cross-platform: the engine, window (Win32/GTK), graphics (D3D12/Vulkan) and the
  onboarding UI (ImGui) are all portable; only the per-user config directory differs
  by OS. The file picker is the in-engine ImGui browser (no native dialog dependency).
- Offline single-player is the whole product here; nothing phones home.
