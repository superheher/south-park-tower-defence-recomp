// south_park_td - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>

#include "launcher/launcher.h"
#include "launcher/onboarding_dialog.h"

class SouthParkTdApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<SouthParkTdApp>(new SouthParkTdApp(ctx, "south_park_td",
        PPCImageConfig));
  }

 protected:
  // Bootstrap brain: handle the CLI contract modes (--validate / --no_setup) and
  // resolve the game source by priority CLI > config > onboarding. Runs early
  // (before the window) so settings cvars are in place when the window/runtime
  // read them. The action is remembered for OnFinalizePaths (M4 onboarding).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    bootstrap_ = splaunch::Bootstrap(paths.game_data_root.string());
    if (bootstrap_.action == splaunch::BootstrapAction::kExit) {
      std::exit(bootstrap_.exit_code);  // --validate / --no_setup-unresolved
    }
    if (!bootstrap_.game_path.empty()) {
      paths.game_data_root = std::filesystem::path(bootstrap_.game_path);
    }
  }

  // Onboarding seam: the window + ImGui drawer are live here, but the runtime is
  // not yet constructed. If the game is unresolved (or --setup forced it), show
  // the wizard and return nullopt so the event loop keeps pumping (the dialog
  // renders); the wizard calls `resume(paths)` on Play to construct + launch.
  // If already resolved, return the paths to launch synchronously.
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults,
      std::function<void(rex::PathConfig)> resume) override {
    const bool force = bootstrap_.action == splaunch::BootstrapAction::kShowUI;
    std::error_code ec;
    const bool resolved =
        !defaults.game_data_root.empty() && std::filesystem::exists(defaults.game_data_root, ec);
    if (resolved && !force) {
      return defaults;  // launch immediately
    }

    // First-run convenience: if we have no path yet, scan the launcher's own
    // folder for a dump the user dropped next to it and pre-fill it.
    std::string initial = defaults.game_data_root.string();
    if (initial.empty()) initial = splaunch::AutoDetectGameNearExe();

    // Show the onboarding wizard (self-owned; deletes itself on close). We keep a
    // raw pointer only to route window file-drops into it; both close paths null
    // it before it self-deletes.
    onboarding_ = new splaunch::OnboardingDialog(
        imgui_drawer(), initial, user_data_root(),
        [this, defaults, resume](std::string game_path) {
          onboarding_ = nullptr;
          // Persist the chosen game + the settings the user reviewed in the wizard.
          splaunch::SaveGameAndSettings(game_path);
          rex::PathConfig pc = defaults;
          pc.game_data_root = std::filesystem::path(game_path);
          // Defer so ConstructRuntime does not run inside the ImGui draw loop.
          app_context().CallInUIThreadDeferred([resume, pc]() { resume(pc); });
        },
        [this]() {
          onboarding_ = nullptr;
          app_context().QuitFromUIThread();
        });
    return std::nullopt;  // async: wizard drives the resume
  }

  // On the first launch the cold shader-cache build can leave the window black for
  // a while. Show a small non-blocking overlay so it does not look hung. Only when
  // the shader cache is empty (i.e. genuinely the first run).
  void OnPostSetup() override {
    if (!imgui_drawer()) return;
    // NOTE: do NOT override the input active-callback here. The SDK's callback
    // returns "active" unless a built-in overlay is open; overriding it to read
    // ImGui WantCaptureMouse every poll breaks guest keyboard (the mnk driver
    // gates on is_active(), and WantCaptureMouse is read cross-thread from the
    // guest poll). The F5 settings overlay is navigable by keyboard/gamepad.
    std::error_code ec;
    const auto& cache = cache_root();
    const bool first_launch =
        cache.empty() || !std::filesystem::exists(cache, ec) || std::filesystem::is_empty(cache, ec);
    if (first_launch) new splaunch::LoadingDialog(imgui_drawer());
  }

  // Drag-and-drop: dropping a dump file/folder onto the window during onboarding
  // fills the wizard's path (which then auto-validates). The engine already
  // accepts drops (Win32 WM_DROPFILES -> OnFileDrop); we just consume it here.
  void OnFileDrop(rex::ui::FileDropEvent& e) override {
    if (onboarding_) onboarding_->SetPath(e.filename().string());
  }

  // After the XEX is loaded (title_id known, content manager up) but before the
  // game launches: auto-install any DLC that shipped alongside the game, so the
  // title's content enumeration finds it. Uses the engine's own ContentManager.
  void OnPostLoadXexImage() override {
    if (!rex::cvar::Query<bool>("auto_dlc")) return;
    auto* rt = runtime();
    if (!rt || !rt->kernel_state()) return;
    auto* cm = rt->kernel_state()->content_manager();
    if (!cm) return;
    const uint32_t title_id = rt->kernel_state()->title_id();
    for (const auto& dlc : splaunch::CollectDlc(game_data_root())) {
      std::filesystem::path dlc_path(dlc);
      if (splaunch::IsDlcInstalled(user_data_root(), title_id, dlc_path)) {
        REXLOG_INFO("[launcher] DLC already installed: {}", dlc);
        continue;
      }
      rex::X_RESULT r = cm->InstallContent(dlc_path);
      if (r == 0)  // X_ERROR_SUCCESS
        REXLOG_INFO("[launcher] installed DLC: {}", dlc);
      else
        REXLOG_WARN("[launcher] DLC install failed (0x{:08X}): {}", static_cast<uint32_t>(r), dlc);
    }
  }

  // In-game settings: F5 toggles a settings overlay (no restart, no .bat). Same
  // panel as the wizard; changes apply live where possible and persist on close.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    rex::ui::RegisterBind("bind_launcher_settings", "F5", "Toggle in-game settings",
                          [this, drawer] {
                            if (launcher_settings_) {
                              launcher_settings_->RequestClose();
                            } else {
                              launcher_settings_ = new splaunch::SettingsDialog(
                                  drawer, user_data_root(), game_data_root().string(),
                                  [this]() { launcher_settings_ = nullptr; });
                            }
                          });
  }

  void OnShutdown() override { rex::ui::UnregisterBind("bind_launcher_settings"); }

 private:
  splaunch::BootstrapResult bootstrap_;          // result of the early bootstrap
  splaunch::OnboardingDialog* onboarding_ = nullptr;  // open wizard (for file-drop), else null
  splaunch::SettingsDialog* launcher_settings_ = nullptr;  // open in-game settings, else null
};
