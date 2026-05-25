// south_park_td - in-engine launcher / onboarding implementation. See launcher.h.

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS  // _wgetenv
#endif

#include "launcher/launcher.h"

#include <rex/cvar.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/devices/stfs_xbox.h>

#include <toml++/toml.hpp>

#include <cstdint>
#include <cstdio>
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

// Launcher contract cvars. Defined here (file scope) so their registrars run
// before cvar::Init parses the command line — otherwise CLI11 rejects the flags.
REXCVAR_DEFINE_STRING(validate, "", "Launcher",
                      "Validate a game source path; print JSON {ok,title,titleID,reason} and exit");
REXCVAR_DEFINE_BOOL(setup, false, "Launcher",
                    "Force the onboarding wizard even if a game is already configured");
REXCVAR_DEFINE_BOOL(no_setup, false, "Launcher",
                    "Never show onboarding UI; exit 64 if no game is configured (embedded use)");
REXCVAR_DEFINE_BOOL(embedded, false, "Launcher", "Alias of --no_setup (for frontends)");

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

namespace {

std::string Utf16ToUtf8(const std::u16string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    uint32_t cp = in[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < in.size()) {
      uint32_t lo = in[i + 1];
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        ++i;
      }
    }
    if (cp == 0) break;  // stop at NUL (title_name field is NUL-padded)
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

std::string JsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c & 0xFF);
          o += buf;
        } else {
          o += c;
        }
    }
  }
  return o;
}

// Write one line to stdout in a way that works for a Windows GUI-subsystem exe:
// honor an inherited/redirected handle (frontend pipe) and, failing that, attach
// to the parent console (interactive terminal). POSIX: plain stdout.
void WriteStdoutLine(const std::string& s) {
  std::string line = s;
  line.push_back('\n');
#if defined(_WIN32)
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!h || h == INVALID_HANDLE_VALUE) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) h = GetStdHandle(STD_OUTPUT_HANDLE);
  }
  if (h && h != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
  }
#else
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fflush(stdout);
#endif
}

}  // namespace

ValidateResult Validate(const std::filesystem::path& path) {
  ValidateResult r;
  std::error_code ec;
  if (path.empty()) {
    r.reason = "no path given";
    return r;
  }
  if (!std::filesystem::exists(path, ec)) {
    r.reason = "path does not exist";
    return r;
  }
  if (std::filesystem::is_directory(path, ec)) {
    // Extracted folder: valid iff it contains default.xex.
    if (std::filesystem::is_regular_file(path / "default.xex", ec)) {
      r.ok = true;
      r.title = "(extracted folder)";
    } else {
      r.reason = "folder has no default.xex";
    }
    return r;
  }
  // A file: read it as an STFS/SVOD package via the engine's own device.
  auto hdr = rex::filesystem::StfsContainerDevice::ReadPackageHeader(path);
  if (!hdr) {
    r.reason = "not a valid STFS/SVOD package";
    return r;
  }
  // Prefer the fuller English display name; fall back to the short title_name.
  r.title = Utf16ToUtf8(hdr->metadata.display_name(rex::system::XLanguage::kEnglish));
  if (r.title.empty()) r.title = Utf16ToUtf8(hdr->metadata.title_name());
  uint32_t tid = hdr->metadata.execution_info.title_id;
  char buf[16];
  std::snprintf(buf, sizeof buf, "%08X", tid);
  r.title_id = buf;
  r.ok = true;
  return r;
}

std::string ValidateResultToJson(const ValidateResult& r) {
  std::string j = "{";
  j += "\"ok\":";
  j += (r.ok ? "true" : "false");
  j += ",\"title\":\"" + JsonEscape(r.title) + "\"";
  j += ",\"titleID\":\"" + JsonEscape(r.title_id) + "\"";
  j += ",\"reason\":\"" + JsonEscape(r.reason) + "\"";
  j += "}";
  return j;
}

BootstrapResult Bootstrap(const std::string& cli_game_path) {
  BootstrapResult res;

  // --validate <path>: headless validate, print JSON to stdout, exit.
  std::string vpath = rex::cvar::Query<std::string>("validate");
  if (!vpath.empty()) {
    ValidateResult vr = Validate(vpath);
    WriteStdoutLine(ValidateResultToJson(vr));
    res.action = BootstrapAction::kExit;
    res.exit_code = vr.ok ? 0 : 65;  // 0 = ok; 65 = EX_DATAERR (invalid input)
    return res;
  }

  // Resolve the game source + apply settings (CLI > config > none) and persist.
  std::string resolved = ResolveAndPersist(cli_game_path);
  res.game_path = resolved;

  const bool embedded = rex::cvar::Query<bool>("no_setup") ||
                        rex::cvar::Query<bool>("embedded") || CliHasFlag("no-setup");
  const bool force_setup = rex::cvar::Query<bool>("setup") || CliHasFlag("setup");

  if (force_setup) {
    res.action = BootstrapAction::kShowUI;  // onboarding even if already resolved (M4)
    return res;
  }
  if (resolved.empty() && embedded) {
    // Embedded/--no_setup with nothing to launch: fail with EX_USAGE.
    std::fprintf(stderr, "south_park_td: no game configured and --no_setup set.\n");
    res.action = BootstrapAction::kExit;
    res.exit_code = 64;  // EX_USAGE
    return res;
  }
  // resolved => launch; empty (interactive) => onboarding wizard runs (M4).
  res.action = BootstrapAction::kLaunch;
  return res;
}

}  // namespace splaunch
