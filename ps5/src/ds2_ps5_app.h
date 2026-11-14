// Dead Space 2 for PS5 - the app.
//
// The PC app (src/app/ds2_app.h) with what a console needs instead of a
// launcher: the game's files are found and prepared at start-up
// (ds2_ps5_install.h), everything lives in the app's own folder (/app0, which
// is /data/homebrew/<title id>), and a screen says what to do when the files
// aren't there. The disc swap (src/app/ds2_disc_swap.cpp) is the PC's, as it is.
#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ps5_diag.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system.h>
#include <rex/ui/imgui_dialog.h>

#include <imgui.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <pthread.h>
#include <unistd.h>

#include <fmt/format.h>

#include "ds2_disc_swap.h"
#include "ds2_ps5_capture.h"
#include "ds2_ps5_install.h"

// The game's language: the Xbox 360's, taken from the PS5's unless set here.
// The World (En,Fr,Es) discs have English and French voices, and Spanish text
// with English voices.
REXCVAR_DEFINE_STRING(ps5_language, "auto", "PS5",
                      "Game language: auto (the PS5's), english, french or spanish");

// The screen shown while the game's files are prepared (the first start unpacks
// the DLC package). Off: they are prepared before anything is drawn, with a
// PS5 notification instead, as a way round a screen that doesn't work.
REXCVAR_DEFINE_BOOL(ps5_prepare_screen, true, "PS5",
                    "Show a progress screen while the game's files are prepared");

// A frames-a-second counter in the top right corner of the picture.
REXCVAR_DEFINE_BOOL(ps5_show_fps, false, "PS5", "Show the game's frames a second in the top right corner");

// For finding what limits the frame rate: where every thread spends its time,
// in the log every 30 s (rex/ps5_diag.h, StartProfiler). Costs a few percent.
REXCVAR_DEFINE_BOOL(ps5_profile, false, "PS5",
                    "Log where each thread spends its time, every 30 s (for performance work)");

extern "C" int sceSystemServiceParamGetInt(int param_id, int* value);
extern "C" int sceKernelSendNotificationRequest(int, void*, size_t, int);

namespace ds2ps5 {

inline void Notify(const std::string& text) {
  struct {
    char unused[45];
    char message[3075];
  } request;
  std::memset(&request, 0, sizeof(request));
  std::strncpy(request.message, text.c_str(), sizeof(request.message) - 1);
  sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

// The Xbox 360 language id (XLanguage) for the ps5_language setting.
inline uint32_t XboxLanguage(const std::string& setting) {
  constexpr uint32_t kEnglish = 1, kFrench = 4, kSpanish = 5;
  if (setting == "english") return kEnglish;
  if (setting == "french") return kFrench;
  if (setting == "spanish") return kSpanish;
  int ps5 = 1;
  if (sceSystemServiceParamGetInt(1 /* SCE_SYSTEM_SERVICE_PARAM_ID_LANG */, &ps5) != 0) ps5 = 1;
  switch (ps5) {
    case 2:   // French
    case 22:  // French (Canada)
      return kFrench;
    case 3:   // Spanish
    case 20:  // Spanish (Latin America)
      return kSpanish;
    default:
      return kEnglish;
  }
}

// A screen of text that stays up: what is missing and where to put it. There
// is no game to run behind it; the PS button closes the app.
class MessageScreen final : public rex::ui::ImGuiDialog {
 public:
  MessageScreen(rex::ui::ImGuiDrawer* drawer, std::string title, std::string body)
      : rex::ui::ImGuiDialog(drawer), title_(std::move(title)), body_(std::move(body)) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##ds2ps5-message", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav);
    // The built-in type is made for a desktop window: scaled up to read from a sofa.
    const float scale = io.DisplaySize.y > 0 ? io.DisplaySize.y / 540.0f : 2.0f;
    const float margin = io.DisplaySize.x * 0.08f;
    ImGui::SetWindowFontScale(scale * 1.4f);
    ImGui::SetCursorPos(ImVec2(margin, io.DisplaySize.y * 0.12f));
    ImGui::TextUnformatted(title_.c_str());
    ImGui::SetWindowFontScale(scale);
    ImGui::SetCursorPos(ImVec2(margin, io.DisplaySize.y * 0.24f));
    ImGui::PushTextWrapPos(io.DisplaySize.x - margin);
    ImGui::TextUnformatted(body_.c_str());
    ImGui::PopTextWrapPos();
    ImGui::End();
  }

 private:
  std::string title_, body_;
};

// The frames the game presented in the last half second, as frames a second,
// in the top right corner (ps5_show_fps). The game's own frames are counted
// (the ones the log's "ps5 diag" line counts), not the TV's refreshes.
class FpsCounter final : public rex::ui::ImGuiDialog {
 public:
  explicit FpsCounter(rex::ui::ImGuiDrawer* drawer)
      : rex::ui::ImGuiDialog(drawer),
        frames_(rex::ps5diag::Get(rex::ps5diag::kSwaps)),
        since_ms_(rex::ps5diag::NowMs()) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const uint64_t now_ms = rex::ps5diag::NowMs();
    if (now_ms - since_ms_ >= 500) {
      const uint64_t frames = rex::ps5diag::Get(rex::ps5diag::kSwaps);
      fps_ = double(frames - frames_) * 1000.0 / double(now_ms - since_ms_);
      frames_ = frames;
      since_ms_ = now_ms;
    }
    // (sized for the TV as the other screens are, and in from the corner: TVs crop the edge)
    const float scale = io.DisplaySize.y > 0 ? io.DisplaySize.y / 540.0f : 2.0f;
    const float margin = io.DisplaySize.y * 0.03f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - margin, margin), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.4f);
    ImGui::Begin("##ds2ps5-fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::SetWindowFontScale(scale * 0.8f);
    ImGui::Text("%.0f FPS", fps_);
    ImGui::End();
  }

 private:
  uint64_t frames_, since_ms_;
  double fps_ = 0.0;
};

// What is shown while the game's files are looked through and prepared: what
// is being done and, for a DLC package being unpacked, how far it is. Without
// it the screen stayed black for minutes on the first start, and the first
// player to try closed the app five times thinking it had hung.
class PrepareScreen final : public rex::ui::ImGuiDialog {
 public:
  PrepareScreen(rex::ui::ImGuiDrawer* drawer, std::shared_ptr<Progress> progress)
      : rex::ui::ImGuiDialog(drawer), progress_(std::move(progress)), started_(std::chrono::steady_clock::now()) {}

  // (the base class's Close is for the dialog itself to call)
  void Finish() { Close(); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (progress_->draws++ == 0) REXLOG_INFO("ds2 files: the screen is being drawn");
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##ds2ps5-prepare", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav);
    const float scale = io.DisplaySize.y > 0 ? io.DisplaySize.y / 540.0f : 2.0f;
    const float margin = io.DisplaySize.x * 0.08f;
    const float width = io.DisplaySize.x - 2 * margin;
    ImGui::SetWindowFontScale(scale * 1.4f);
    ImGui::SetCursorPos(ImVec2(margin, io.DisplaySize.y * 0.12f));
    ImGui::TextUnformatted("Getting Dead Space 2 ready");
    ImGui::SetWindowFontScale(scale);
    ImGui::SetCursorPos(ImVec2(margin, io.DisplaySize.y * 0.24f));
    ImGui::PushTextWrapPos(io.DisplaySize.x - margin);
    const int seconds = int(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_).count());
    // (a few dots that move, so a long step doesn't look stuck)
    ImGui::TextUnformatted((progress_->step() + std::string(size_t(1 + seconds % 3), '.')).c_str());
    const uint64_t total = progress_->total_bytes.load(), done = std::min(progress_->done_bytes.load(), total);
    if (total) {
      ImGui::SetCursorPos(ImVec2(margin, io.DisplaySize.y * 0.36f));
      ImGui::ProgressBar(float(double(done) / double(total)), ImVec2(width, 28.0f * scale),
                         fmt::format("{} of {} MB", done >> 20, total >> 20).c_str());
      ImGui::SetCursorPos(ImVec2(margin, io.DisplaySize.y * 0.48f));
      ImGui::TextUnformatted(
          "This is done once and takes a few minutes. If the app is closed before it is finished, the next "
          "start goes on from where this one stopped.");
    }
    ImGui::PopTextWrapPos();
    ImGui::End();
  }

 private:
  std::shared_ptr<Progress> progress_;  // (shared: the screen is drawn once more after Finish)
  std::chrono::steady_clock::time_point started_;
};

}  // namespace ds2ps5

class Ds2Ps5App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Ds2Ps5App>(new Ds2Ps5App(ctx, "ds2", PPCImageConfig));
  }

 protected:
  // After ds2.toml is loaded and before the GPU backend is created. On the PS5
  // the backend is part of the program, so its flags exist already and take
  // their defaults here (the PC seeds one of them through the environment).
  void OnPostInitLogging() override {
    DefaultFlag("gpu_plugin", "xenos");
    // No keyboard or mouse to play with.
    DefaultFlag("mnk_mode", "false");
    // V-Synced presentation: the swapchain otherwise prefers IMMEDIATE, which tears.
    DefaultFlag("vulkan_allow_present_mode_immediate", "false");
    // As the PC launcher starts the game (launcher/launcher.py): the guest
    // clock straight from the host's, render-to-texture readback off.
    DefaultFlag("clock_no_scaling", "true");
    DefaultFlag("readback_resolve", "none");
    // One ordinary buffer for the console's 512 MB instead of a sparse one:
    // the PS5's driver is a port and has memory to spare.
    DefaultFlag("vulkan_sparse_shared_memory", "false");
    // Compiled pipelines kept between runs, so a shader only stutters the
    // first time it is seen (sdk/rexglue_ds2.patch, P4).
    if (!cache_root().empty()) {
      std::error_code ec;
      std::filesystem::create_directories(cache_root(), ec);
      DefaultFlag("p4_vk_pipeline_cache_path", (cache_root() / "vk_pipelines.bin").string().c_str());
    }
    DefaultFlag("user_language", std::to_string(ds2ps5::XboxLanguage(REXCVAR_GET(ps5_language))).c_str());
    REXLOG_INFO("Dead Space 2 for PS5 {}: language {} ({}), config {}", DS2_PS5_VERSION,
                rex::cvar::GetFlagByName("user_language"), REXCVAR_GET(ps5_language),
                (app_root() / "ds2.toml").string());
  }

  // The window and the overlay drawer exist; the runtime doesn't yet. The
  // game's files are looked for and prepared here, on a thread of their own
  // with a screen that says how far that is (the first start unpacks the DLC
  // package: minutes). The game starts when it is done; without the files the
  // app stays on a screen that says what to copy where.
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
    if (!imgui_drawer() || !REXCVAR_GET(ps5_prepare_screen)) {
      // No screen: prepared here, with a notification for what takes long.
      return PrepareWithoutScreen(defaults);
    }
    prepare_ = std::make_unique<Prepare>();
    prepare_->app = this;
    prepare_->defaults = defaults;
    prepare_->resume = std::move(resume);
    prepare_->progress->announce = [](const std::string& text) { ds2ps5::Notify(text); };
    prepare_->screen = new ds2ps5::PrepareScreen(imgui_drawer(), prepare_->progress);
    // (a stack of its own size: a new thread's default is the console's to choose)
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, size_t(4) << 20);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int failed = pthread_create(&thread, &attributes, &PrepareThread, prepare_.get());
    pthread_attr_destroy(&attributes);
    if (failed) {
      REXLOG_WARN("ds2 files: no thread to prepare them on ({}): prepared with nothing on the screen", failed);
      prepare_->screen->Finish();
      prepare_.reset();
      return PrepareWithoutScreen(defaults);
    }
    return std::nullopt;  // (resumed from PrepareDone)
  }

  // Runs after Runtime::Setup (the VFS exists) and before LoadXexImage: the
  // disc image as d: and the title update folder as game:, both discs mounted
  // for the automatic swap when both are there (src/app/ds2_disc_swap.cpp).
  void OnLoadXexImage(std::string& xex_image) override {
    const int boot = files_.boot_disc();
    if (!boot) Fail("No Dead Space 2 disc image is ready");
    if (!files_.iso[0].empty()) rex::cvar::SetFlagByName("ds2_disc1_iso", files_.iso[0].string());
    if (!files_.iso[1].empty()) rex::cvar::SetFlagByName("ds2_disc2_iso", files_.iso[1].string());
    try {
      ds2::disc_swap::Prepare(*runtime(), files_.iso[boot - 1], game_data_root());
    } catch (const std::exception& error) {
      Fail(error.what());
    }
    REXLOG_INFO("ds2: Disc {} ({}) with Title Update #2 from {}; loading {}", boot,
                files_.iso[boot - 1].string(), game_data_root().string(), xex_image);
  }

  // Before the overlay drawer goes: a dialog takes itself off it when destroyed.
  void OnShutdown() override { fps_counter_.reset(); }

  void OnPreLaunchModule() override {
    // (here and not earlier: over the game, not over the screens before it)
    if (REXCVAR_GET(ps5_profile)) rex::ps5diag::StartProfiler(30);
    if (runtime() && runtime()->graphics_system()) {
      ds2ps5::PictureCapture::Start(runtime()->graphics_system()->presenter(), app_root() / "logs");
    }
    if (REXCVAR_GET(ps5_show_fps) && imgui_drawer() && !fps_counter_) {
      fps_counter_ = std::make_unique<ds2ps5::FpsCounter>(imgui_drawer());
    }
    try {
      ds2::disc_swap::RegisterImport(*runtime());
    } catch (const std::exception& error) {
      Fail(error.what());
    }
  }

 private:
  // The preparing of the game's files, while it runs.
  struct Prepare {
    Ds2Ps5App* app = nullptr;
    rex::PathConfig defaults;
    std::function<void(rex::PathConfig)> resume;
    std::shared_ptr<ds2ps5::Progress> progress = std::make_shared<ds2ps5::Progress>();
    ds2ps5::PrepareScreen* screen = nullptr;  // (deletes itself once finished)
    ds2ps5::GameFiles files;
  };

  ds2ps5::GameFiles files_;
  std::unique_ptr<Prepare> prepare_;
  std::unique_ptr<ds2ps5::FpsCounter> fps_counter_;

  static void* PrepareThread(void* argument) {
    auto* prepare = static_cast<Prepare*>(argument);
    rex::ps5diag::RegisterThread("ds2 files");
    Ds2Ps5App* app = prepare->app;
    prepare->files = ds2ps5::PrepareGameFiles(app_root(), prepare->defaults.user_data_root, prepare->progress.get());
    rex::ps5diag::UnregisterThread();
    app->app_context().CallInUIThread([app] { app->PrepareDone(); });
    return nullptr;
  }

  std::optional<rex::PathConfig> PrepareWithoutScreen(const rex::PathConfig& defaults) {
    ds2ps5::Progress progress;
    progress.announce = [](const std::string& text) { ds2ps5::Notify(text); };
    files_ = ds2ps5::PrepareGameFiles(app_root(), defaults.user_data_root, &progress);
    return FilesPrepared(defaults);
  }

  // On the UI thread, when the files are prepared (or can't be).
  void PrepareDone() {
    std::unique_ptr<Prepare> prepare = std::move(prepare_);
    if (!prepare) return;
    files_ = std::move(prepare->files);
    REXLOG_INFO("ds2 files: prepared; the screen was drawn {} times", prepare->progress->draws.load());
    prepare->screen->Finish();
    if (auto paths = FilesPrepared(prepare->defaults)) prepare->resume(std::move(*paths));
  }

  // The paths to start the game with; without the game's files, the screen
  // that says what is missing (and nothing: the app stays on it).
  std::optional<rex::PathConfig> FilesPrepared(const rex::PathConfig& defaults) {
    for (const std::string& note : files_.notes) REXLOG_INFO("ds2 files: {}", note);
    rex::FlushLogging();
    const int boot = files_.boot_disc();
    if (!boot) {
      REXLOG_ERROR("ds2 files: {}", files_.problem);
      rex::FlushLogging();
      ds2ps5::Notify("Dead Space 2: game files are missing - see the screen");
      if (imgui_drawer()) {
        new ds2ps5::MessageScreen(imgui_drawer(), "Dead Space 2 needs your game files", files_.problem);
        return std::nullopt;  // (never resumed: the screen stays until the app is closed)
      }
      Fail(files_.problem);
    }
    rex::PathConfig paths = defaults;
    paths.game_data_root = files_.game_dir[boot - 1];
    return paths;
  }

  static std::filesystem::path app_root() { return rex::filesystem::GetExecutableFolder(); }

  static void DefaultFlag(const char* name, const char* value) {
    if (!rex::cvar::GetFlagInfo(name) || rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault) return;
    rex::cvar::SetFlagByName(name, value);
  }

  [[noreturn]] static void Fail(const std::string& message) {
    REXLOG_ERROR("{}", message);
    rex::FlushLogging();
    ds2ps5::Notify("Dead Space 2: " + message);
    sleep(3);  // (let the notification show)
    std::_Exit(1);
  }
};
