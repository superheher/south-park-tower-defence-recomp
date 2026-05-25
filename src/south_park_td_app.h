// south_park_td - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdlib>

#include <rex/rex_app.h>

#include "launcher/launcher.h"

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

  // Other hooks for later milestones:
  // std::optional<rex::PathConfig> OnFinalizePaths(...) override;  // M4 onboarding
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}  // M5 settings

 private:
  splaunch::BootstrapResult bootstrap_;  // result of the early bootstrap (for M4)
};
