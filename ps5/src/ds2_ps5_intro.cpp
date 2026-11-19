// Dead Space 2 for PS5 - the logo video at start-up, skipped on request.
//
// The game starts with one video, movies\en\trio_frontend.vp6 (the EA and
// Visceral Games logos, the Dead Space 2 title forming out of them). It is
// played in three layers: a MoviePlayerSim (EARS::Framework::Modules) over an
// RtMoviePlayer (EARS::RtMovie) over RenderWare's rw::movie::MoviePlayer2. When
// a video ends, MoviePlayer2::IsFinished says so, the layers above see it at
// their next tick and end themselves, and the front end brings up the title
// screen (EARS::Apt::FE01Title).
//
// With ps5_skip_intro_videos = true the MoviePlayer2 playing the logo video
// says it is finished as soon as it is asked: everything above it then does
// what it does at the end of a video, and the title screen is up in 2 s
// instead of 16. (Saying so one layer up, or ending the player from outside,
// left the front end without parts of that.)
//
// One thing of the front end's own is timed to the video, not to its end: a
// screen named FE66 (the "Dead Space 2" title over the video's end) is shown
// about 7 s after the start, and the title screen hides it when it shows its
// "Press A" prompt (FE01Title::ShowPressStartButton calls
// UIScreenManager::HideScreen('FE66')), 1 s after it comes up. After a skip
// the prompt is up before FE66 is shown, so nothing hid it: it came up over
// the title screen or the main menu, in the wrong place, and kept their input
// (the single player menu's options gone though still answering, the title
// screen deaf). So after a skip, FE66 shown within 20 s of it, after the
// prompt, isn't shown: as the prompt would have hidden it. (Linux test build:
// the title screen answers Start whenever pressed, the menus as after the
// logos, and left alone it starts its trailer as it does after them.)
//
// MoviePlayerSim::Play(this r3, parameters r4): the parameters (2,512 bytes)
// hold the video's path; it is looked for in them by name rather than at an
// offset. It hands the video to a MoviePlayer2 through
// MoviePlayer2::QueuePlaybackRequest(this r3, ...), on the same thread;
// MoviePlayer2::IsFinished(this r3) -> r3. UIScreenManager::ShowScreen(this
// r3, the screen's nickname r4).
#include <atomic>
#include <cctype>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ps5_diag.h>

REXCVAR_DEFINE_BOOL(ps5_skip_intro_videos, false, "PS5",
                    "Skip the EA / Visceral Games logo video at start-up (to the title screen sooner)");

extern "C" void __imp__EARS__Framework__Modules__MoviePlayerSim__Play(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__rw__movie__MoviePlayer2__QueuePlaybackRequest(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__rw__movie__MoviePlayer2__IsFinished(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__EARS__Apt__FE01Title__ShowPressStartButton(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__EARS__Apt__UIScreenManager__ShowScreen(PPCContext& __restrict ctx, uint8_t* base);

namespace {

// While MoviePlayerSim::Play starts the logo video (on the thread calling it).
thread_local bool starting_logo = false;
// The MoviePlayer2 playing the logo video, to be said finished (0: none).
std::atomic<uint32_t> logo_player{0};
// After a skip: until when (NowMs) FE66 is kept from showing once the title
// screen's prompt is up (0: not), and whether it is.
constexpr uint32_t kTitleOverlay = 0x46453636;  // 'FE66'
std::atomic<uint64_t> overlay_watch_until_ms{0};
std::atomic<bool> prompt_shown{false};

// Whether the parameters name the logo video (its path, in ASCII, somewhere
// in them).
bool NamesLogoVideo(const uint8_t* base, uint32_t parameters) {
  static constexpr char kName[] = "trio_frontend";
  constexpr uint32_t kSize = 2512, kLength = sizeof(kName) - 1;
  for (uint32_t i = 0; i + kLength <= kSize; ++i) {
    uint32_t j = 0;
    while (j < kLength && std::tolower(base[parameters + i + j]) == kName[j]) ++j;
    if (j == kLength) return true;
  }
  return false;
}

}  // namespace

extern "C" void EARS__Framework__Modules__MoviePlayerSim__Play(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool skip = REXCVAR_GET(ps5_skip_intro_videos);
  const uint32_t parameters = ctx.r4.u32;
  starting_logo = skip && parameters && NamesLogoVideo(base, parameters);
  __imp__EARS__Framework__Modules__MoviePlayerSim__Play(ctx, base);
  starting_logo = false;
}

extern "C" void rw__movie__MoviePlayer2__QueuePlaybackRequest(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t player = ctx.r3.u32;
  __imp__rw__movie__MoviePlayer2__QueuePlaybackRequest(ctx, base);
  if (starting_logo) {
    logo_player.store(player, std::memory_order_relaxed);
    prompt_shown.store(false, std::memory_order_relaxed);
    overlay_watch_until_ms.store(rex::ps5diag::NowMs() + 20000, std::memory_order_relaxed);
    REXLOG_INFO("ps5 intro: the logo video is skipped (ps5_skip_intro_videos)");
  }
}

extern "C" void rw__movie__MoviePlayer2__IsFinished(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t player = ctx.r3.u32;
  __imp__rw__movie__MoviePlayer2__IsFinished(ctx, base);
  if (player && logo_player.load(std::memory_order_relaxed) == player) {
    if (!ctx.r3.u32) ctx.r3.u64 = 1;
    // (said once is enough: the layers above end it)
    logo_player.store(0, std::memory_order_relaxed);
  }
}

extern "C" void EARS__Apt__FE01Title__ShowPressStartButton(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__EARS__Apt__FE01Title__ShowPressStartButton(ctx, base);
  if (overlay_watch_until_ms.load(std::memory_order_relaxed)) prompt_shown.store(true, std::memory_order_relaxed);
}

extern "C" void EARS__Apt__UIScreenManager__ShowScreen(PPCContext& __restrict ctx, uint8_t* base) {
  if (ctx.r4.u32 == kTitleOverlay && prompt_shown.load(std::memory_order_relaxed)) {
    const uint64_t until = overlay_watch_until_ms.exchange(0, std::memory_order_relaxed);
    if (until && rex::ps5diag::NowMs() < until) {
      REXLOG_INFO("ps5 intro: the title overlay (FE66) after the prompt is left hidden, as the prompt hides it");
      return;
    }
  }
  __imp__EARS__Apt__UIScreenManager__ShowScreen(ctx, base);
}
