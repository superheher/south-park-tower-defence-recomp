// south_park_td - in-engine launcher / onboarding implementation. See launcher.h.

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS  // _wgetenv
#endif

#include "launcher/launcher.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>  // GetExecutableFolder
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/devices/stfs_xbox.h>
#include <rex/system/xcontent.h>  // XContentType

#include <toml++/toml.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
REXCVAR_DEFINE_BOOL(auto_dlc, true, "Launcher",
                    "Auto-install DLC found alongside the game (set false to skip)");
REXCVAR_DEFINE_BOOL(version, false, "Launcher", "Print the launcher version and exit");
REXCVAR_DEFINE_BOOL(print_config, false, "Launcher",
                    "Print the resolved game + settings + DLC as JSON and exit");

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
      // Graphics / presentation
      {"resolution_scale", CvarType::Int},
      {"present_effect", CvarType::Str},
      {"present_cas_additional_sharpness", CvarType::Double},
      {"present_letterbox", CvarType::Bool},
      {"present_allow_overscan_cutoff", CvarType::Bool},
      // Launcher behavior
      {"auto_dlc", CvarType::Bool},
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
    case CvarType::Double:
      return std::to_string(rex::cvar::Query<double>(def.name));
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

bool SaveGamePath(const std::string& game_path) {
  Config c = LoadConfig();
  c.schema_version = kSchemaVersion;
  c.game_path = game_path;
  return SaveConfig(c);
}

bool SaveGameAndSettings(const std::string& game_path) {
  Config c = LoadConfig();
  c.schema_version = kSchemaVersion;
  c.game_path = game_path;
  for (const auto& def : ManagedSettings()) c.settings[def.name] = CvarValueAsString(def);
  return SaveConfig(c);
}

std::string ResolveAndPersist(const std::string& cli_game_path) {
  Config cfg = LoadConfig();

  // Layer 1: launcher default values (beneath config and CLI).
  for (const auto& [name, value] : LauncherDefaults()) {
    if (!CliHasFlag(name)) ApplyCvar(name, value);
  }
  // Dynamic default: the bundled window icon next to the exe (if present and not
  // overridden by CLI or config). Set before the config layer so config wins.
  if (!CliHasFlag("window_icon") && rex::cvar::Query<std::string>("window_icon").empty()) {
    std::error_code ec;
    auto ico = rex::filesystem::GetExecutableFolder() / "SouthPark.ico";
    if (std::filesystem::exists(ico, ec)) ApplyCvar("window_icon", ico.string());
  }
  // Layer 2: saved config settings (override defaults, lose to CLI).
  for (const auto& [name, value] : cfg.settings) {
    if (!CliHasFlag(name)) ApplyCvar(name, value);
  }

  // Resolve the game source: CLI wins, else the saved config; then resolve a
  // folder/parent down to the concrete source the engine can mount in place.
  std::string raw = !cli_game_path.empty() ? cli_game_path : cfg.game_path;
  std::string resolved = raw.empty() ? std::string() : ResolveGameSource(raw);

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

// True if the file starts with an STFS/SVOD package magic (CON /LIVE/PIRS).
// Cheap (reads 4 bytes) so it is safe to call while scanning a folder.
bool HasStfsMagic(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  char m[4] = {};
  f.read(m, 4);
  if (f.gcount() < 4) return false;
  return std::memcmp(m, "CON ", 4) == 0 || std::memcmp(m, "LIVE", 4) == 0 ||
         std::memcmp(m, "PIRS", 4) == 0;
}

// Read the STFS content type if `p` is a valid package (magic + header), else
// nullopt. A folder dump can hold several packages (the game, DLC, ...), so we
// use this to prefer the bootable game over add-ons.
std::optional<rex::system::XContentType> StfsContentType(const std::filesystem::path& p) {
  if (!HasStfsMagic(p)) return std::nullopt;
  auto hdr = rex::filesystem::StfsContainerDevice::ReadPackageHeader(p);
  if (!hdr) return std::nullopt;
  rex::system::XContentType ct = hdr->metadata.content_type;  // be<XContentType> -> enum
  return ct;
}

// Confirm a file really is a mountable STFS/SVOD package (magic + valid header).
bool IsStfsPackage(const std::filesystem::path& p) { return StfsContentType(p).has_value(); }

// True for content types that contain a bootable title (vs DLC / saved games /
// avatars / themes / videos). South Park LGTDP is an XBLA kArcadeTitle.
bool IsTitleContentType(rex::system::XContentType ct) {
  using CT = rex::system::XContentType;
  switch (ct) {
    case CT::kXbox360Title:
    case CT::kInstalledGame:
    case CT::kXboxTitle:
    case CT::kGamesOnDemand:
    case CT::kGameDemo:
    case CT::kGameTitle:
    case CT::kArcadeTitle:
    case CT::kCommunityGame:
      return true;
    default:
      return false;
  }
}

// During a folder scan, only bother magic-checking files that plausibly are a
// package: extension-less files (console dumps) or large files. Avoids reading
// the head of every small asset.
bool WorthMagicCheck(const std::filesystem::path& p, std::uintmax_t size) {
  if (!p.has_extension()) return true;
  return size >= (std::uintmax_t(8) << 20);  // >= 8 MiB
}

}  // namespace

std::string ResolveGameSource(const std::filesystem::path& input) {
  std::error_code ec;
  if (input.empty() || !std::filesystem::exists(input, ec)) return "";

  // A file: an STFS package (any name), or a default.xex (use its folder).
  if (std::filesystem::is_regular_file(input, ec)) {
    if (input.filename() == "default.xex") return input.parent_path().string();
    if (IsStfsPackage(input)) return input.string();
    return "";
  }

  if (!std::filesystem::is_directory(input, ec)) return "";

  // Fast path: an extracted folder with default.xex at its root.
  if (std::filesystem::is_regular_file(input / "default.xex", ec)) return input.string();

  // Otherwise search shallowly for a default.xex or an STFS package (any name) -
  // so pointing at a parent of a raw dump (<titleID>/000D0000/<hash>) just works.
  // A title folder may hold several packages (game + DLC); prefer the bootable
  // game (a title content type) and only fall back to anything else.
  constexpr int kMaxDepth = 6;
  constexpr int kMaxExamined = 20000;
  int examined = 0;
  std::string fallback;  // a non-title package (DLC, etc.): used only if no game found
  std::filesystem::recursive_directory_iterator it(
      input, std::filesystem::directory_options::skip_permission_denied, ec),
      end;
  for (; it != end; it.increment(ec)) {
    if (ec) break;
    if (++examined > kMaxExamined) break;
    std::error_code ec2;
    if (it->is_directory(ec2)) {
      if (it.depth() >= kMaxDepth) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec2)) continue;
    const auto& p = it->path();
    if (p.filename() == "default.xex") return p.parent_path().string();  // extracted game
    auto size = it->file_size(ec2);
    if (ec2 || !WorthMagicCheck(p, size)) continue;
    auto ct = StfsContentType(p);
    if (!ct) continue;
    if (IsTitleContentType(*ct)) return p.string();  // the bootable game
    if (fallback.empty()) fallback = p.string();     // DLC/other: remember, keep looking
  }
  return fallback;
}

std::vector<std::string> CollectDlc(const std::filesystem::path& game_source) {
  std::vector<std::string> dlc;
  std::error_code ec;
  if (game_source.empty() || !std::filesystem::exists(game_source, ec)) return dlc;

  // For a dump file .../<titleID>/<type>/<file>, DLC lives in a sibling type
  // folder .../<titleID>/00000002/<file>, so scan the title-id folder.
  std::filesystem::path scan_root;
  if (std::filesystem::is_directory(game_source, ec)) {
    scan_root = game_source.parent_path();
  } else {
    auto type_dir = game_source.parent_path();
    scan_root = type_dir.has_parent_path() ? type_dir.parent_path() : type_dir;
  }
  if (scan_root.empty() || !std::filesystem::is_directory(scan_root, ec)) return dlc;

  constexpr int kMaxDepth = 5;
  constexpr int kMaxExamined = 20000;
  int examined = 0;
  std::filesystem::recursive_directory_iterator it(
      scan_root, std::filesystem::directory_options::skip_permission_denied, ec),
      end;
  for (; it != end; it.increment(ec)) {
    if (ec) break;
    if (++examined > kMaxExamined) break;
    std::error_code ec2;
    if (it->is_directory(ec2)) {
      if (it.depth() >= kMaxDepth) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec2)) continue;
    const auto& p = it->path();
    if (std::filesystem::equivalent(p, game_source, ec2)) continue;  // not the game
    auto size = it->file_size(ec2);
    if (ec2 || !WorthMagicCheck(p, size)) continue;
    auto ct = StfsContentType(p);
    if (ct && *ct == rex::system::XContentType::kMarketplaceContent) dlc.push_back(p.string());
  }
  return dlc;
}

std::vector<std::string> CollectDlcNames(const std::filesystem::path& game_source) {
  std::vector<std::string> names;
  for (const auto& p : CollectDlc(game_source)) {
    std::string name;
    if (auto hdr = rex::filesystem::StfsContainerDevice::ReadPackageHeader(p)) {
      name = Utf16ToUtf8(hdr->metadata.display_name(rex::system::XLanguage::kEnglish));
      if (name.empty()) name = Utf16ToUtf8(hdr->metadata.title_name());
    }
    if (name.empty()) name = std::filesystem::path(p).filename().string();
    names.push_back(name);
  }
  return names;
}

std::string AutoDetectGameNearExe() {
  auto dir = rex::filesystem::GetExecutableFolder();
  if (dir.empty()) return "";
  return ResolveGameSource(dir);  // bounded recursive scan of the exe's folder
}

void OpenInFileManager(const std::filesystem::path& path) {
  std::error_code ec;
  if (path.empty() || !std::filesystem::exists(path, ec)) return;
#if defined(_WIN32)
  ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
  std::system(("open '" + path.string() + "'").c_str());
#else
  std::system(("xdg-open '" + path.string() + "' &").c_str());
#endif
}

std::string BackupSaves(const std::filesystem::path& user_data_root) {
  std::error_code ec;
  if (user_data_root.empty() || !std::filesystem::is_directory(user_data_root, ec)) return "";

  std::time_t t = std::time(nullptr);
  char ts[32] = {};
  std::strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", std::localtime(&t));
  auto dest = user_data_root / "backups" / ts;
  std::filesystem::create_directories(dest, ec);
  if (ec) return "";

  bool any = false;
  for (std::filesystem::directory_iterator it(user_data_root, ec), end; it != end;
       it.increment(ec)) {
    if (ec) break;
    auto name = it->path().filename().string();
    if (name == "cache" || name == "backups") continue;  // skip large / transient
    std::error_code ec2;
    std::filesystem::copy(it->path(), dest / name,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          ec2);
    if (!ec2) any = true;
  }
  if (!any) {
    std::filesystem::remove_all(dest, ec);
    return "";
  }
  return dest.string();
}

bool IsDlcInstalled(const std::filesystem::path& user_data_root, uint32_t title_id,
                    const std::filesystem::path& dlc_package) {
  char tid[16];
  std::snprintf(tid, sizeof tid, "%08X", title_id);
  // Mirrors ContentManager: <root>/0000000000000000/<titleID>/00000002/<filename>/
  auto dest = user_data_root / "0000000000000000" / tid / "00000002" / dlc_package.filename();
  std::error_code ec;
  return std::filesystem::exists(dest, ec);
}

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
  // Resolve a folder/parent down to the concrete game source it contains.
  std::string resolved = ResolveGameSource(path);
  if (resolved.empty()) {
    r.reason = std::filesystem::is_directory(path, ec)
                   ? "no game here (need default.xex or an STFS package inside)"
                   : "not a valid game (STFS package or default.xex)";
    return r;
  }
  r.resolved = resolved;
  std::filesystem::path rp(resolved);
  if (std::filesystem::is_directory(rp, ec)) {
    // Extracted folder (contains default.xex at its root).
    r.ok = true;
    r.title = "(extracted folder)";
    return r;
  }
  // A package file: read title/titleID via the engine's own device.
  auto hdr = rex::filesystem::StfsContainerDevice::ReadPackageHeader(rp);
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
  j += ",\"path\":\"" + JsonEscape(r.resolved) + "\"";
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

  // --version: print and exit.
  if (rex::cvar::Query<bool>("version")) {
    WriteStdoutLine("south_park_td launcher (config schema " + std::to_string(kSchemaVersion) + ")");
    res.action = BootstrapAction::kExit;
    res.exit_code = 0;
    return res;
  }

  // Resolve the game source + apply settings (CLI > config > none) and persist.
  std::string resolved = ResolveAndPersist(cli_game_path);
  res.game_path = resolved;

  // --print-config: emit the resolved game + effective settings + DLC as JSON, exit.
  if (rex::cvar::Query<bool>("print_config")) {
    std::string j = "{\"game_path\":\"" + JsonEscape(resolved) + "\",\"settings\":{";
    bool first = true;
    for (const auto& def : ManagedSettings()) {
      if (!first) j += ",";
      first = false;
      j += "\"" + std::string(def.name) + "\":\"" + JsonEscape(CvarValueAsString(def)) + "\"";
    }
    j += "},\"dlc\":[";
    first = true;
    for (const auto& n : CollectDlcNames(std::filesystem::path(resolved))) {
      if (!first) j += ",";
      first = false;
      j += "\"" + JsonEscape(n) + "\"";
    }
    j += "]}";
    WriteStdoutLine(j);
    res.action = BootstrapAction::kExit;
    res.exit_code = 0;
    return res;
  }

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
