// south_park_td - first-run onboarding wizard (ImGui front-end of the bootstrap).
//
// Shown by SouthParkTdApp::OnFinalizePaths when no game is configured (or --setup
// is forced). Lets the player point the engine at THEIR own game - an STFS package
// file or an extracted folder - validate it (via the same splaunch::Validate the
// --validate contract uses), and Play. On Play it persists the choice and invokes
// the SDK's resume callback to construct the runtime and launch.
//
// The wizard runs BEFORE the runtime exists, so it does its own lightweight input:
// window mouse + keyboard (always), plus a focus-free gamepad source (REX_INPUT_FILE
// and, on Windows, XInput) so it is usable from a controller / couch / headless.

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <rex/ui/imgui_dialog.h>

struct ImGuiIO;

namespace splaunch {

class OnboardingDialog : public rex::ui::ImGuiDialog {
 public:
  // on_play(game_path): user confirmed a valid game; persist + resume/launch.
  // on_quit(): user aborted; quit the app.
  OnboardingDialog(rex::ui::ImGuiDrawer* drawer, std::string initial_path,
                   std::function<void(std::string)> on_play, std::function<void()> on_quit);

  // Set the game-location path externally (e.g. from a window file-drop). The
  // next frame re-validates it. Closes the in-ImGui browser if open.
  void SetPath(const std::string& path);

 protected:
  void OnShow() override;
  void OnDraw(ImGuiIO& io) override;

 private:
  void Revalidate();                 // re-run Validate() when the path changes
  void DrawBrowser(ImGuiIO& io);     // in-ImGui file/folder browser
  void RefreshEntries();             // (re)list browse_dir_
  void Confirm();                    // Play
  unsigned ReadPadEdges();           // returns newly-pressed gamepad buttons this frame

  std::function<void(std::string)> on_play_;
  std::function<void()> on_quit_;

  char path_buf_[1024] = {};
  std::string validated_path_;       // path last passed to Validate()
  bool result_ok_ = false;
  std::string result_title_;
  std::string result_title_id_;
  std::string result_reason_;
  std::string result_resolved_;      // concrete source found (may differ from input)
  std::vector<std::string> result_dlc_names_;  // DLC display names alongside the game

  bool browser_open_ = false;
  std::filesystem::path browse_dir_;
  std::vector<std::filesystem::path> dirs_;
  std::vector<std::filesystem::path> files_;
  bool entries_dirty_ = true;

  unsigned prev_pad_ = 0;            // previous gamepad mask (edge detection)
  bool first_draw_ = true;
};

}  // namespace splaunch
