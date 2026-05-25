// south_park_td - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

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
  // Bootstrap brain: apply launcher defaults + saved per-user config (CLI wins),
  // and resolve the game source by priority CLI > config > onboarding. Runs early
  // (before the window) so settings cvars are in place when the window/runtime
  // read them. If still unresolved here, the onboarding wizard (OnFinalizePaths)
  // takes over.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    std::string cli_game = paths.game_data_root.string();
    std::string resolved = splaunch::ResolveAndPersist(cli_game);
    if (!resolved.empty()) {
      paths.game_data_root = std::filesystem::path(resolved);
    }
  }

  // Other hooks available for later milestones:
  // std::optional<rex::PathConfig> OnFinalizePaths(...) override;  // M4 onboarding
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}  // M5 settings
};
