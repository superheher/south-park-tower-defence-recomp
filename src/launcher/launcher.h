// south_park_td — in-engine launcher / onboarding (the "bootstrap brain")
//
// This module is the separable contract layer described in launcher/INTEGRATION.md:
//   * cvars are the public API (game_data_root, license_mask, window_*, ... ).
//   * a per-user config file (per-OS user dir) persists the last game + settings.
//   * resolution priority is CLI > config > onboarding.
//
// It lives in the freely-editable app (NOT rexglue) and drives the engine purely
// through public SDK seams (cvars + ReXApp hooks), so an external frontend can do
// the exact same thing via flags + --validate without any of this code.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace splaunch {

// ---------------------------------------------------------------------------
// Per-OS user paths
// ---------------------------------------------------------------------------

// Directory for launcher config/state. Created on demand by SaveConfig().
//   Windows : %LOCALAPPDATA%\SouthParkTD
//   macOS   : ~/Library/Application Support/SouthParkTD
//   Linux   : $XDG_CONFIG_HOME/SouthParkTD  (or ~/.config/SouthParkTD)
std::filesystem::path ConfigDir();

// The launcher config file: ConfigDir() / "launcher.toml".
std::filesystem::path ConfigFile();

// True if the process command line contains --<name> or --<name>=...
// Used to honor CLI > config: a setting present on the CLI is never overwritten
// by the saved config. Cross-platform (Win32 GetCommandLineW / Linux
// /proc/self/cmdline / macOS _NSGetArgv); returns false if it cannot be read.
bool CliHasFlag(std::string_view name);

// ---------------------------------------------------------------------------
// Settings the launcher manages (cvar name + value type for round-tripping)
// ---------------------------------------------------------------------------

enum class CvarType { Bool, Int, UInt, Str };

struct SettingDef {
  const char* name;
  CvarType type;
};

// The canonical set of cvars the launcher persists and exposes in its UI.
// (Online co-op cvars are intentionally excluded — that feature is paused.)
const std::vector<SettingDef>& ManagedSettings();

// Launcher-preferred default values (cvar name -> string value), applied as the
// base layer beneath config and CLI. These are the "sensible defaults" so a
// fresh config-driven launch is full-version, sensibly windowed, no arcade logo.
const std::map<std::string, std::string>& LauncherDefaults();

// Read the current value of a managed cvar as a string (type-aware).
std::string CvarValueAsString(const SettingDef& def);

// Apply a value to a cvar by name (thin wrapper over rex::cvar::SetFlagByName).
bool ApplyCvar(std::string_view name, std::string_view value);

// ---------------------------------------------------------------------------
// Config model
// ---------------------------------------------------------------------------

constexpr int kSchemaVersion = 1;

struct Config {
  int schema_version = kSchemaVersion;
  std::string game_path;                        // last selected game source (folder OR STFS file)
  std::map<std::string, std::string> settings;  // cvar name -> value (strings)
  bool exists = false;                          // a config file was found + parsed
};

// Load from ConfigFile(). exists=false (and defaults) if no file / parse error.
Config LoadConfig();

// Write atomically to ConfigFile() (creates ConfigDir()). Returns success.
bool SaveConfig(const Config& cfg);

// Persist a chosen game source path (keeping any existing saved settings).
// Used by the onboarding wizard when the user confirms a game.
bool SaveGamePath(const std::string& game_path);

// Persist a chosen game source path AND the current values of every managed
// setting (the user has reviewed them in the wizard, so their choices win).
bool SaveGameAndSettings(const std::string& game_path);

// ---------------------------------------------------------------------------
// Bootstrap resolution (the "brain")
// ---------------------------------------------------------------------------

// Resolve the game source + apply settings, with priority CLI > config > none:
//   1. apply LauncherDefaults() to cvars (skipping any flag present on the CLI),
//   2. apply the saved config's settings (also skipping CLI flags),
//   3. pick the game source: `cli_game_path` if non-empty, else the saved game,
//   4. if resolved, persist the game path + any settings explicitly set on the
//      CLI this run (so one-off CLI overrides are remembered, defaults are not
//      frozen).
// Returns the resolved game source path (empty string if still unresolved, in
// which case onboarding should run).
std::string ResolveAndPersist(const std::string& cli_game_path);

// ---------------------------------------------------------------------------
// Validation (the engine IS the contract — frontends reuse this via --validate)
// ---------------------------------------------------------------------------

struct ValidateResult {
  bool ok = false;
  std::string title;     // human-readable title (may be empty)
  std::string title_id;  // 8 hex digits, e.g. "58410931" (empty for folders)
  std::string reason;    // failure reason (empty on success)
  std::string resolved;  // the concrete game source actually found (file or folder)
};

// Resolve a user-supplied path to the concrete game source the engine can mount,
// or "" if none is found. Accepts, in order of convenience:
//   * an STFS/SVOD package file (any filename — detected by content), or
//   * a `default.xex` file (uses its containing folder), or
//   * an extracted folder containing default.xex at its root, or
//   * ANY parent folder of either: searched with bounded recursion for a
//     default.xex or an STFS package (so a raw console dump laid out as
//     <titleID>/000D0000/<hash> just works when you point at any ancestor).
std::string ResolveGameSource(const std::filesystem::path& input);

// Validate a game source using the engine's OWN STFS reader (no mount, no
// window). Resolves `path` first (see ResolveGameSource), so a folder is fine.
ValidateResult Validate(const std::filesystem::path& path);

// First-run convenience: scan the executable's own folder (recursively, bounded)
// for a game and return the resolved source, or "" if none. Lets a user just
// drop their dump next to the launcher and have onboarding pre-fill it.
std::string AutoDetectGameNearExe();

// ---------------------------------------------------------------------------
// DLC / add-on content
// ---------------------------------------------------------------------------

// Find DLC packages (STFS marketplace content) that sit alongside the resolved
// game - e.g. a console dump's <titleID>/00000002/<hash> next to the game's
// <titleID>/000D0000/<hash>. Returns their host paths (empty if none). The game
// itself is excluded.
std::vector<std::string> CollectDlc(const std::filesystem::path& game_source);

// True if a DLC package is already extracted into the engine's content store
// under user_data_root (so re-installing each launch can be skipped).
bool IsDlcInstalled(const std::filesystem::path& user_data_root, uint32_t title_id,
                    const std::filesystem::path& dlc_package);

// Serialize a ValidateResult as one line of JSON: {"ok":...,"title":...,
// "titleID":...,"reason":...}.
std::string ValidateResultToJson(const ValidateResult& r);

// ---------------------------------------------------------------------------
// Bootstrap entry — called once, early (before window/runtime). Handles the
// CLI contract modes and resolves the game source.
// ---------------------------------------------------------------------------

enum class BootstrapAction {
  kLaunch,   // proceed; game_path is the resolved source ("" => run onboarding)
  kShowUI,   // force the onboarding wizard (game_path may be a current guess)
  kExit,     // stop now with exit_code (e.g. --validate, or --no_setup unresolved)
};

struct BootstrapResult {
  BootstrapAction action = BootstrapAction::kLaunch;
  int exit_code = 0;
  std::string game_path;  // resolved source path (may be empty)
};

// Process --validate / --setup / --no_setup|--embedded and CLI>config>none game
// resolution. `cli_game_path` is the game_data_root cvar value (CLI, may be "").
BootstrapResult Bootstrap(const std::string& cli_game_path);

}  // namespace splaunch
