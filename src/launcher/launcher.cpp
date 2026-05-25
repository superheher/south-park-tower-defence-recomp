// south_park_td - in-engine launcher / onboarding implementation. See launcher.h.

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS  // _wgetenv
#endif

#include "launcher/launcher.h"

#include <rex/cvar.h>

#include <toml++/toml.hpp>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW
#include <shlobj.h>    // SHGetKnownFolderPath
#elif defined(__APPLE__)
#include <crt_externs.h>  // _NSGetArgv / _NSGetArgc
#endif

namespace splaunch {

namespace {

constexpr char kProductDir[] = "SouthParkTD";

#if defined(_WIN32)
std::filesystem::path LocalAppData() {
  if (const wchar_t* e = _wgetenv(L"LOCALAPPDATA"); e && *e)
    return std::filesystem::path(e);
  PWSTR p = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &p))) {
    std::filesystem::path result(p);
    CoTaskMemFree(p);
    return result;
  }
  return {};
}
#else
std::filesystem::path HomeDir() {
  if (const char* h = std::getenv("HOME"); h && *h)
    return std::filesystem::path(h);
  return {};
}
#endif

// Process argv, per-OS. Used only for CLI-flag presence detection.
std::vector<std::string> ProcessArgs() {
  std::vector<std::string> out;
#if defined(_WIN32)
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv) {
    for (int i = 0; i < argc; ++i) {
      // Flag names are ASCII; a lossy narrow conversion is fine for comparison.
      std::wstring w(argv[i]);
      std::string s;
      s.reserve(w.size());
      for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
      out.push_back(std::move(s));
    }
    LocalFree(argv);
  }
#elif defined(__APPLE__)
  int argc = *_NSGetArgc();
  char** argv = *_NSGetArgv();
  for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
#else
  std::ifstream f("/proc/self/cmdline", std::ios::binary);
  if (f) {
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string cur;
    for (char c : all) {
      if (c == '\0') {
        if (!cur.empty()) out.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    if (!cur.empty()) out.push_back(cur);
  }
#endif
  return out;
}

}  // namespace

std::filesystem::path ConfigDir() {
#if defined(_WIN32)
  auto base = LocalAppData();
  return base.empty() ? std::filesystem::path(kProductDir) : base / kProductDir;
#elif defined(__APPLE__)
  auto home = HomeDir();
  return home.empty() ? std::filesystem::path(kProductDir)
                      : home / "Library" / "Application Support" / kProductDir;
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
    return std::filesystem::path(xdg) / kProductDir;
  auto home = HomeDir();
  return home.empty() ? std::filesystem::path(kProductDir) : home / ".config" / kProductDir;
#endif
}

std::filesystem::path ConfigFile() { return ConfigDir() / "launcher.toml"; }

bool CliHasFlag(std::string_view name) {
  const std::string prefix = "--" + std::string(name);
  for (const auto& a : ProcessArgs()) {
    if (a == prefix) return true;
    if (a.size() > prefix.size() && a.compare(0, prefix.size(), prefix) == 0 &&
        a[prefix.size()] == '=')
      return true;
  }
  return false;
}

const std::vector<SettingDef>& ManagedSettings() {
  static const std::vector<SettingDef> kSettings = {
      {"license_mask", CvarType::UInt},   {"window_width", CvarType::Int},
      {"window_height", CvarType::Int},   {"fullscreen", CvarType::Bool},
      {"vsync", CvarType::Bool},          {"mnk_mode", CvarType::Bool},
      {"audio_mute", CvarType::Bool},     {"always_win", CvarType::Bool},
      {"skip_arcade_logo", CvarType::Bool}, {"window_icon", CvarType::Str},
  };
  return kSettings;
}

const std::map<std::string, std::string>& LauncherDefaults() {
  // Base layer (beneath config and CLI). Full settings polish (icon path, free
  // cursor) is layered in by the app at M5; these are the value defaults that
  // make a config-only launch full-version and sensibly windowed.
  static const std::map<std::string, std::string> kDefaults = {
      {"license_mask", "1"},        // FULL version (trial persists nothing)
      {"window_width", "1280"},     // sensible windowed size
      {"window_height", "720"},
      {"skip_arcade_logo", "true"}, // skip the blocky XBLA arcade logo
      {"mnk_mode", "true"},         // keyboard usable as a controller out of the box
  };
  return kDefaults;
}

std::string CvarValueAsString(const SettingDef& def) {
  switch (def.type) {
    case CvarType::Bool:
      return rex::cvar::Query<bool>(def.name) ? "true" : "false";
    case CvarType::Int:
      return std::to_string(rex::cvar::Query<int32_t>(def.name));
    case CvarType::UInt:
      return std::to_string(rex::cvar::Query<uint32_t>(def.name));
    case CvarType::Str:
    default:
      return rex::cvar::Query<std::string>(def.name);
  }
}

bool ApplyCvar(std::string_view name, std::string_view value) {
  return rex::cvar::SetFlagByName(name, value);
}

Config LoadConfig() {
  Config cfg;
  std::error_code ec;
  auto path = ConfigFile();
  if (!std::filesystem::exists(path, ec))
    return cfg;  // exists=false

  toml::table tbl;
  try {
    tbl = toml::parse_file(path.string());
  } catch (const toml::parse_error&) {
    return cfg;  // treat a corrupt config as "no config"
  }

  cfg.exists = true;
  cfg.schema_version = static_cast<int>(tbl["schema_version"].value_or<int64_t>(kSchemaVersion));
  cfg.game_path = tbl["game_path"].value_or<std::string>("");

  if (auto* settings = tbl["settings"].as_table()) {
    for (const auto& [key, val] : *settings) {
      std::string s;
      if (val.is_boolean())
        s = val.as_boolean()->get() ? "true" : "false";
      else if (val.is_integer())
        s = std::to_string(val.as_integer()->get());
      else if (val.is_floating_point())
        s = std::to_string(val.as_floating_point()->get());
      else if (val.is_string())
        s = val.as_string()->get();
      else
        continue;
      cfg.settings[std::string(key.str())] = std::move(s);
    }
  }
  return cfg;
}

bool SaveConfig(const Config& cfg) {
  std::error_code ec;
  auto dir = ConfigDir();
  std::filesystem::create_directories(dir, ec);

  // TOML basic strings ("...") with escaping: handles backslashes (Windows
  // paths) AND apostrophes (e.g. "...Let's Go Tower Defense Play!..."), which a
  // literal string ('...') cannot contain.
  auto esc = [](const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
      switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
      }
    }
    out += "\"";
    return out;
  };

  std::string text;
  text += "# South Park: Let's Go Tower Defense - launcher config\n";
  text += "# Auto-managed by the in-engine launcher. Safe to hand-edit.\n";
  text += "schema_version = " + std::to_string(cfg.schema_version) + "\n\n";
  text += "# Your game: an extracted folder OR a single STFS package file (your own dump).\n";
  text += "game_path = " + esc(cfg.game_path) + "\n\n";
  text += "[settings]\n";
  for (const auto& [k, v] : cfg.settings) {
    text += k + " = " + esc(v) + "\n";
  }

  // Atomic-ish write: temp file then rename over the target.
  auto target = ConfigFile();
  auto tmp = target;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    if (!out) return false;
  }
  std::filesystem::remove(target, ec);
  std::filesystem::rename(tmp, target, ec);
  if (ec) {
    // Fall back to a direct write if rename failed (e.g. cross-device).
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out);
  }
  return true;
}

std::string ResolveAndPersist(const std::string& cli_game_path) {
  Config cfg = LoadConfig();

  // Layer 1: launcher default values (beneath config and CLI).
  for (const auto& [name, value] : LauncherDefaults()) {
    if (!CliHasFlag(name)) ApplyCvar(name, value);
  }
  // Layer 2: saved config settings (override defaults, lose to CLI).
  for (const auto& [name, value] : cfg.settings) {
    if (!CliHasFlag(name)) ApplyCvar(name, value);
  }

  // Resolve the game source: CLI wins, else the saved config.
  std::string resolved = !cli_game_path.empty() ? cli_game_path : cfg.game_path;

  // Persist: remember the game + only the settings explicitly set on the CLI
  // this run (existing saved settings are preserved; defaults are NOT frozen).
  if (!resolved.empty()) {
    Config out = cfg;
    out.schema_version = kSchemaVersion;
    out.game_path = resolved;
    for (const auto& def : ManagedSettings()) {
      if (CliHasFlag(def.name)) out.settings[def.name] = CvarValueAsString(def);
    }
    SaveConfig(out);
  }
  return resolved;
}

}  // namespace splaunch
