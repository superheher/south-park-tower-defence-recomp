// south_park_td - onboarding wizard implementation. See onboarding_dialog.h.

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS  // std::fopen / std::getenv
#endif

#include "launcher/onboarding_dialog.h"

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>  // GetExecutableFolder (logs folder button)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>

#include "launcher/launcher.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace splaunch {

namespace fs = std::filesystem;

namespace {

// XInput wButtons bits (also the REX_INPUT_FILE hex convention used by the game).
constexpr unsigned kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008;
constexpr unsigned kPadStart = 0x0010, kPadBack = 0x0020, kPadA = 0x1000, kPadB = 0x2000;

// Read REX_INPUT_FILE (focus-free automation / headless) as a hex button mask.
unsigned ReadInputFileMask() {
  const char* path = std::getenv("REX_INPUT_FILE");
  if (!path || !*path) return 0;
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return 0;
  char buf[32] = {};
  size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  buf[n] = 0;
  return static_cast<unsigned>(std::strtoul(buf, nullptr, 16));
}

#if defined(_WIN32)
// XInput, dynamically loaded so we add no link dependency.
unsigned ReadXInputMask() {
  using XInputGetState_t = DWORD(WINAPI*)(DWORD, void*);
  static XInputGetState_t fn = []() -> XInputGetState_t {
    for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
      if (HMODULE m = LoadLibraryW(dll))
        return reinterpret_cast<XInputGetState_t>(GetProcAddress(m, "XInputGetState"));
    }
    return nullptr;
  }();
  if (!fn) return 0;
  struct { DWORD packet; struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } pad; } st{};
  if (fn(0, &st) != 0) return 0;  // user 0; ERROR_SUCCESS==0
  return st.pad.buttons;
}
#endif

unsigned ReadPadMask() {
  unsigned m = ReadInputFileMask();
#if defined(_WIN32)
  m |= ReadXInputMask();
#endif
  return m;
}

fs::path DefaultBrowseDir() {
#if defined(_WIN32)
  if (const char* up = std::getenv("USERPROFILE"); up && *up) return fs::path(up);
#else
  if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h);
#endif
  std::error_code ec;
  auto cwd = fs::current_path(ec);
  return ec ? fs::path(".") : cwd;
}

}  // namespace

OnboardingDialog::OnboardingDialog(rex::ui::ImGuiDrawer* drawer, std::string initial_path,
                                   std::filesystem::path user_data_root,
                                   std::function<void(std::string)> on_play,
                                   std::function<void()> on_quit)
    : rex::ui::ImGuiDialog(drawer),
      on_play_(std::move(on_play)),
      on_quit_(std::move(on_quit)),
      user_data_root_(std::move(user_data_root)) {
  std::snprintf(path_buf_, sizeof(path_buf_), "%s", initial_path.c_str());
}

void OnboardingDialog::OnShow() {}

void OnboardingDialog::SetPath(const std::string& path) {
  std::snprintf(path_buf_, sizeof(path_buf_), "%s", path.c_str());
  browser_open_ = false;  // a path was provided; no need for the browser
}

unsigned OnboardingDialog::ReadPadEdges() {
  unsigned mask = ReadPadMask();
  ImGuiIO& io = GetIO();
  // Drive ImGui gamepad navigation (level-triggered; ImGui debounces).
  io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (mask & kPadUp) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (mask & kPadDown) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (mask & kPadLeft) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (mask & kPadRight) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (mask & kPadA) != 0);   // A = activate
  io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (mask & kPadB) != 0);  // B = cancel
  unsigned edges = mask & ~prev_pad_;
  prev_pad_ = mask;
  return edges;
}

void OnboardingDialog::Revalidate() {
  if (validated_path_ == path_buf_) return;
  validated_path_ = path_buf_;
  if (validated_path_.empty()) {
    result_ok_ = false;
    result_title_.clear();
    result_title_id_.clear();
    result_reason_.clear();
    result_resolved_.clear();
    return;
  }
  ValidateResult r = Validate(fs::path(validated_path_));
  result_ok_ = r.ok;
  result_title_ = r.title;
  result_title_id_ = r.title_id;
  result_reason_ = r.reason;
  result_resolved_ = r.resolved;
  result_dlc_names_.clear();
  if (result_ok_ && !result_resolved_.empty())
    result_dlc_names_ = CollectDlcNames(fs::path(result_resolved_));
}

void OnboardingDialog::RefreshEntries() {
  dirs_.clear();
  files_.clear();
  std::error_code ec;
  for (fs::directory_iterator it(browse_dir_, fs::directory_options::skip_permission_denied, ec), end;
       it != end; it.increment(ec)) {
    if (ec) break;
    std::error_code ec2;
    if (it->is_directory(ec2))
      dirs_.push_back(it->path());
    else
      files_.push_back(it->path());
  }
  auto by_name = [](const fs::path& a, const fs::path& b) {
    return a.filename().string() < b.filename().string();
  };
  std::sort(dirs_.begin(), dirs_.end(), by_name);
  std::sort(files_.begin(), files_.end(), by_name);
  entries_dirty_ = false;
}

void OnboardingDialog::Confirm() {
  if (!result_ok_) return;
  // Launch the concrete source we resolved (which may be a package found inside
  // the folder the user pointed at), not the raw input.
  std::string chosen = result_resolved_.empty() ? std::string(path_buf_) : result_resolved_;
  auto cb = on_play_;  // copy: Close() deletes `this` after OnDraw returns
  Close();
  if (cb) cb(chosen);
}

void OnboardingDialog::DrawBrowser(ImGuiIO& io) {
  (void)io;
  if (entries_dirty_) RefreshEntries();

  ImGui::Separator();
  ImGui::TextUnformatted("Browse for your game:");
  ImGui::TextDisabled("%s", browse_dir_.string().c_str());

  if (ImGui::Button("Up") && browse_dir_.has_parent_path()) {
    auto parent = browse_dir_.parent_path();
    if (parent != browse_dir_) {
      browse_dir_ = parent;
      entries_dirty_ = true;
    }
  }
#if defined(_WIN32)
  DWORD drives = GetLogicalDrives();
  for (int i = 0; i < 26; ++i) {
    if (!(drives & (1u << i))) continue;
    ImGui::SameLine();
    char label[4] = {static_cast<char>('A' + i), ':', 0, 0};
    if (ImGui::Button(label)) {
      browse_dir_ = fs::path(std::string(1, static_cast<char>('A' + i)) + ":\\");
      entries_dirty_ = true;
    }
  }
#endif

  ImGui::BeginChild("##browser_list", ImVec2(0, 220), true);
  int id = 0;
  for (const auto& d : dirs_) {
    ImGui::PushID(id++);
    std::string label = "[DIR]  " + d.filename().string();
    if (ImGui::Selectable(label.c_str())) {
      browse_dir_ = d;
      entries_dirty_ = true;
    }
    ImGui::PopID();
  }
  for (const auto& fpath : files_) {
    ImGui::PushID(id++);
    if (ImGui::Selectable(fpath.filename().string().c_str())) {
      std::snprintf(path_buf_, sizeof(path_buf_), "%s", fpath.string().c_str());
      browser_open_ = false;
    }
    ImGui::PopID();
  }
  ImGui::EndChild();

  if (ImGui::Button("Use this folder")) {
    std::snprintf(path_buf_, sizeof(path_buf_), "%s", browse_dir_.string().c_str());
    browser_open_ = false;
  }
  ImGui::SameLine();
  if (ImGui::Button("Close browser")) browser_open_ = false;
}

void OnboardingDialog::OnDraw(ImGuiIO& io) {
  if (first_draw_) {
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    first_draw_ = false;
  }
  unsigned edges = ReadPadEdges();

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_Always);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
  if (ImGui::Begin("Set up South Park: Let's Go Tower Defense", nullptr, flags)) {
    ImGui::TextWrapped(
        "Welcome! Point the engine at YOUR own copy of the game - or just "
        "DRAG-AND-DROP it onto this window. You can give the STFS package file (your "
        "Xbox 360 console dump), an extracted-files folder, or ANY folder that "
        "contains it - setup finds the game inside automatically (and skips DLC). "
        "Nothing is downloaded; your copy never leaves this machine.");
    ImGui::Spacing();

    ImGui::TextUnformatted("Game location:");
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::InputText("##path", path_buf_, sizeof(path_buf_));
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
      browser_open_ = !browser_open_;
      if (browser_open_) {
        fs::path p(path_buf_);
        std::error_code ec;
        if (!p.empty() && fs::is_directory(p, ec))
          browse_dir_ = p;
        else if (!p.empty() && p.has_parent_path() && fs::exists(p.parent_path(), ec))
          browse_dir_ = p.parent_path();
        else
          browse_dir_ = DefaultBrowseDir();
        entries_dirty_ = true;
      }
    }

    Revalidate();

    if (path_buf_[0] == 0) {
      ImGui::TextDisabled("No game selected yet.");
    } else if (result_ok_) {
      std::string label = result_title_.empty() ? std::string("valid game") : result_title_;
      if (!result_title_id_.empty()) label += "  (titleID " + result_title_id_ + ")";
      ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.35f, 1.0f), "OK: %s", label.c_str());
      // If we found the game inside the folder the user pointed at, show where.
      if (!result_resolved_.empty() && result_resolved_ != path_buf_)
        ImGui::TextDisabled("Found: %s", result_resolved_.c_str());
      // Warn if this is a different game than this build expects.
      constexpr const char* kExpectedTitleId = "58410931";  // South Park: Let's Go Tower Defense
      if (!result_title_id_.empty() && result_title_id_ != kExpectedTitleId)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                           "Warning: titleID %s is not South Park (58410931) - it may not run.",
                           result_title_id_.c_str());
      // Show the DLC that will be auto-installed alongside the game.
      if (!result_dlc_names_.empty()) {
        std::string joined;
        for (const auto& n : result_dlc_names_) {
          if (!joined.empty()) joined += ", ";
          joined += n;
        }
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "Add-ons: %s", joined.c_str());
      }
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Not a valid game: %s",
                         result_reason_.c_str());
    }

    if (browser_open_) DrawBrowser(io);

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
      auto BoolCvar = [](const char* label, const char* name) {
        bool b = rex::cvar::Query<bool>(name);
        if (ImGui::Checkbox(label, &b)) ApplyCvar(name, b ? "true" : "false");
      };
      {
        bool full = rex::cvar::Query<uint32_t>("license_mask") != 0;
        if (ImGui::Checkbox("Full version (unlock everything; saves progress)", &full))
          ApplyCvar("license_mask", full ? "1" : "0");
      }
      BoolCvar("Fullscreen", "fullscreen");
      BoolCvar("Use keyboard & mouse as a controller", "mnk_mode");
      BoolCvar("Mute audio", "audio_mute");
      BoolCvar("Invincibility (your base can't be destroyed)", "always_win");
      BoolCvar("Auto-install DLC found next to the game", "auto_dlc");

      ImGui::Separator();
      ImGui::TextUnformatted("Graphics");
      {
        int rs = rex::cvar::Query<int32_t>("resolution_scale");
        rs = rs < 1 ? 1 : (rs > 3 ? 3 : rs);
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt("Internal resolution", &rs, 1, 3, "%dx (sharper)"))
          ApplyCvar("resolution_scale", std::to_string(rs));
      }
      {
        static const char* kEffects[] = {"bilinear", "cas", "fsr"};
        std::string cur = rex::cvar::Query<std::string>("present_effect");
        int idx = 0;
        for (int i = 0; i < 3; ++i)
          if (cur == kEffects[i]) idx = i;
        ImGui::SetNextItemWidth(220);
        if (ImGui::Combo("Output filter", &idx, kEffects, 3)) ApplyCvar("present_effect", kEffects[idx]);
        if (idx != 0) {
          float sh = static_cast<float>(rex::cvar::Query<double>("present_cas_additional_sharpness"));
          ImGui::SetNextItemWidth(220);
          if (ImGui::SliderFloat("Sharpness", &sh, 0.0f, 1.0f, "%.2f"))
            ApplyCvar("present_cas_additional_sharpness", std::to_string(sh));
        }
      }

      if (ImGui::TreeNode("Advanced")) {
        BoolCvar("Skip arcade logo", "skip_arcade_logo");
        BoolCvar("VSync", "vsync");
        BoolCvar("Letterbox (keep aspect ratio)", "present_letterbox");
        BoolCvar("Crop overscan", "present_allow_overscan_cutoff");
        int w = rex::cvar::Query<int32_t>("window_width");
        ImGui::SetNextItemWidth(160);
        if (ImGui::InputInt("Window width", &w)) ApplyCvar("window_width", std::to_string(w));
        int h = rex::cvar::Query<int32_t>("window_height");
        ImGui::SetNextItemWidth(160);
        if (ImGui::InputInt("Window height", &h)) ApplyCvar("window_height", std::to_string(h));
        ImGui::TextDisabled("Window size / fullscreen take effect on the next launch.");

        ImGui::Separator();
        ImGui::TextUnformatted("Folders & saves");
        if (ImGui::Button("Open data folder") && !user_data_root_.empty())
          splaunch::OpenInFileManager(user_data_root_);
        ImGui::SameLine();
        if (ImGui::Button("Open logs folder"))
          splaunch::OpenInFileManager(rex::filesystem::GetExecutableFolder() / "logs");
        ImGui::SameLine();
        if (ImGui::Button("Open game folder") && !result_resolved_.empty()) {
          std::filesystem::path gp(result_resolved_);
          splaunch::OpenInFileManager(std::filesystem::is_directory(gp) ? gp : gp.parent_path());
        }
        if (ImGui::Button("Backup saves now")) {
          std::string dest = splaunch::BackupSaves(user_data_root_);
          last_backup_msg_ = dest.empty() ? "Nothing to back up yet." : ("Backed up to: " + dest);
        }
        if (!last_backup_msg_.empty()) ImGui::TextDisabled("%s", last_backup_msg_.c_str());
        ImGui::TreePop();
      }
      ImGui::TextDisabled("Saved when you press Play. Re-run with --setup to change later.");
    }
    // Online co-op is intentionally not exposed here (that feature is paused).

    ImGui::Separator();
    const bool can_play = result_ok_;
    if (!can_play) ImGui::BeginDisabled();
    if (ImGui::Button("Play", ImVec2(140, 0))) Confirm();
    if (!can_play) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(140, 0))) {
      auto cb = on_quit_;
      Close();
      if (cb) cb();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Gamepad: Start = Play, B = Quit  |  Keyboard: Enter / Esc");

    // Explicit shortcuts (independent of ImGui focus, so reliable from automation).
    if ((edges & kPadStart) && can_play) Confirm();
    if (can_play && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) Confirm();
    if ((edges & kPadB) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      auto cb = on_quit_;
      Close();
      if (cb) cb();
    }
  }
  ImGui::End();
}

}  // namespace splaunch
