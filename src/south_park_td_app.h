// south_park_td - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

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

    // Show the onboarding wizard (self-owned; deletes itself on close).
    new splaunch::OnboardingDialog(
        imgui_drawer(), defaults.game_data_root.string(),
        [this, defaults, resume](std::string game_path) {
          // Persist the chosen game + the settings the user reviewed in the wizard.
          splaunch::SaveGameAndSettings(game_path);
          rex::PathConfig pc = defaults;
          pc.game_data_root = std::filesystem::path(game_path);
          // Defer so ConstructRuntime does not run inside the ImGui draw loop.
          app_context().CallInUIThreadDeferred([resume, pc]() { resume(pc); });
        },
        [this]() { app_context().QuitFromUIThread(); });
    return std::nullopt;  // async: wizard drives the resume
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

  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}  // M5 settings

 private:
  splaunch::BootstrapResult bootstrap_;  // result of the early bootstrap
};
