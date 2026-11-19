// Dead Space 2 for PS5 - 60 frames a second.
//
// The PC build's `framerate` mod (mods/framerate/framerate.cpp), built into the
// app as a setting: the console has no mod loader, and two functions are all
// it takes. The game holds itself to 30 in two places:
//
//  - EARS::Time keeps g_targetFrameTimeMSec = vblanks * ONE_FRAMETIME
//    (1 / 59.94 s) with vblanks = 2, and TimeManager::HandleEvents never moves
//    the game clock by less than that. Before each call the target is set to
//    one vblank.
//  - Displ_SwapScreens calls Displ_WaitForFrameLock(n), which waits until n
//    vblanks have passed since the last frame (n = 2 in play). n is capped at 1.
//
// Both are the game's own functions, taken over the way the SDK's hooks are:
// the recompiled code defines each function as a weak alias of __imp__<name>,
// and the definitions here win at link time and go on to the original.
//
// What the PC release says of it holds here: most of the game is fine at 60,
// some cutscene animation isn't quite. Measured on the Linux test build (New
// Game's intro, captures every 3 s, ps5_frame_rate = 30 / ps5_cutscenes_30 =
// true / false): at 60 the game's clocks (g_frameTimeSec, g_simFrameTimeInSec,
// the game time) all keep true time, and so do the cutscene's shots, its credits
// and its end; but some things inside it come early, as if counted in frames.
// Against the game as the Xbox 360 runs it (ps5_frame_rate = 30 with
// vsync_adaptive = false: a steady 30), in 3 s captures from the dark shot of
// the interrogation: the lights come on after 10 (true 30), 10-12 (the game's
// own 30 here), 5 (60); the room turns orange after 15, 17, 13. (With
// vsync_adaptive, as on the PS5, "its own 30" makes about 55 frames a second:
// the wait for the second vblank is taken for a late frame and let go. Its
// timing, measured above, is the Xbox 360's.) So the game goes back to its own
// 30 while a cutscene has hold of the player (ps5_cutscenes_30): the engine's
// word for that is a scripted sequence, and the player object says when it is
// in one (IScriptedSequenceTarget, at Player + 560: the running bit, 4, of the
// flags at + 16, set by StartScriptedSequence and cleared by
// EndScriptedSequence). It is read once a frame from Player::PlayerUpdateBlock,
// which is called with the player for as long as there is one; when it has not
// been seen in a sequence for a moment (a menu, a loading screen, the sequence
// over), the game is back at 60.
//
// A hacking station's minigame holds the player in a sequence too, and it is
// played, not watched: at 30 the first console runs dropped there for the 5 s
// of the hack. A sequence during which (or up to kHackLeadMs before which) the
// minigame said something of its own (HackingStationMiniGame's PlayerCame,
// PlayerWent, OnEnterMessage, OnExitMessage, OnSuccessfulSelection,
// OnGameSuccess, OnTimerRunOut) is a hack, and stays at 60 until it is over.
//
// More generally, a sequence the player plays rather than watches stays at 60:
// the hacking minigame, an enemy's grab and the buttons pressed to break free,
// the eye-poke machine, anything else the game hands the controller back in. So
// from the moment the controller does something new during a sequence (a button
// pressed, Start and Back aside, or a stick or trigger pushed from rest; one
// held from before the sequence doesn't count) to its end, it runs at 60. A
// cutscene watched with the controller put down stays at the game's own 30. The
// controller's state comes from XInputGetStateEx (ds2_ps5_script.cpp calls
// Ds2NotePadState with what the game gets).
//
// ps5_frame_rate = 30 in ds2.toml is the game as it shipped, throughout.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ps5_diag.h>

REXCVAR_DEFINE_INT32(ps5_frame_rate, 60, "PS5",
                     "Frames a second the game aims for: 60, or 30 (the game's own)");

REXCVAR_DEFINE_BOOL(ps5_cutscenes_30, true, "PS5",
                    "With ps5_frame_rate = 60: the game's own 30 while a cutscene has hold of the player");

extern "C" void __imp__EARS__Modules__Player__PlayerUpdateBlock(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__EARS__Time__TimeManager__HandleEvents(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__Displ_WaitForFrameLock_8290C710(PPCContext& __restrict ctx, uint8_t* base);

#define DS2_HACK_EVENTS(X)                                        \
  X(EARS__Modules__HackingStationMiniGame__PlayerCame)            \
  X(EARS__Modules__HackingStationMiniGame__PlayerWent)            \
  X(EARS__Modules__HackingStationMiniGame__OnEnterMessage)        \
  X(EARS__Modules__HackingStationMiniGame__OnExitMessage)         \
  X(EARS__Modules__HackingStationMiniGame__OnSuccessfulSelection) \
  X(EARS__Modules__HackingStationMiniGame__OnGameSuccess)         \
  X(EARS__Modules__HackingStationMiniGame__OnTimerRunOut)
#define DS2_DECLARE_IMP(name) extern "C" void __imp__##name(PPCContext& __restrict ctx, uint8_t* base);
DS2_HACK_EVENTS(DS2_DECLARE_IMP)
#undef DS2_DECLARE_IMP

namespace {

// All addresses and offsets are Title Update #2's, the only build the PS5 app
// is made from. ONE_FRAMETIME's source and g_targetFrameTimeMSec, both floats
// in milliseconds:
constexpr uint32_t kOneVblankMs = 0x82D437E0, kTargetFrameMs = 0x82D475C0;

// The player's IScriptedSequenceTarget flags (Player + 560 + 16) and the bit
// that says a sequence is running.
constexpr uint32_t kPlayerSequenceFlags = 576;
constexpr uint8_t kSequenceRunning = 4;
// A sequence counts as still running this long after the player was last seen
// in one: a few frames at 30, so that one long frame doesn't end it.
constexpr uint64_t kSequenceHoldMs = 250;

// When the player was last seen in a scripted sequence (rex::ps5diag::NowMs() | 1; 0: never).
std::atomic<uint64_t> sequence_seen_ms{0};

// When a hacking station's minigame last did something (NowMs() | 1; 0: never),
// and whether the sequence the player is in now is played (by them: see the
// top). A minigame event up to this long before a sequence starts makes it one.
constexpr uint64_t kHackLeadMs = 5000;
std::atomic<uint64_t> hack_event_ms{0};
std::atomic<bool> sequence_is_played{false};
// When the controller last did something new (NowMs() | 1; 0: never).
std::atomic<uint64_t> input_event_ms{0};

void NoteHackEvent(const char* what) {
  hack_event_ms.store(rex::ps5diag::NowMs() | 1, std::memory_order_relaxed);
  // (the first ones in the log: which of them the game calls, and when)
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 40) {
    REXLOG_INFO("ps5 frame rate: hacking station: {}", what);
  }
}

bool InCutscene() {
  const uint64_t seen = sequence_seen_ms.load(std::memory_order_relaxed);
  if (!seen) return false;
  const uint64_t now = rex::ps5diag::NowMs();
  return now <= seen || now - seen <= kSequenceHoldMs;  // (seen is odd: it can be 1 ahead)
}

// Vblanks per frame now: 1 for 60, 2 for the game's own 30.
int TargetVblanks() {
  static const bool sixty = REXCVAR_GET(ps5_frame_rate) >= 60;
  static const bool cutscenes_30 = REXCVAR_GET(ps5_cutscenes_30);
  if (!sixty) return 2;
  if (!cutscenes_30) return 1;
  const bool in_sequence = InCutscene();
  // When the sequence the player is in began (NowMs(); 0: not in one), and why
  // it is played, for the log.
  static std::atomic<uint64_t> sequence_start_ms{0};
  static const char* played_by = "";
  const uint64_t now = rex::ps5diag::NowMs();
  if (!in_sequence) {
    sequence_is_played.store(false, std::memory_order_relaxed);
    sequence_start_ms.store(0, std::memory_order_relaxed);
  } else {
    uint64_t start = sequence_start_ms.load(std::memory_order_relaxed);
    if (!start) {
      start = now;
      sequence_start_ms.store(start, std::memory_order_relaxed);
    }
    if (!sequence_is_played.load(std::memory_order_relaxed)) {
      const uint64_t hack = hack_event_ms.load(std::memory_order_relaxed);
      const uint64_t input = input_event_ms.load(std::memory_order_relaxed);
      if (hack && (now <= hack || now - hack <= kHackLeadMs)) {
        played_by = "hacking";
        sequence_is_played.store(true, std::memory_order_relaxed);
      } else if (input && input >= start) {
        played_by = "the controller";
        sequence_is_played.store(true, std::memory_order_relaxed);
      }
    }
  }
  const bool cutscene = in_sequence && !sequence_is_played.load(std::memory_order_relaxed);
  // (in the log, the first hundred times it changes: enough to see it work)
  static std::atomic<int> was{0};  // 0: play, 1: a cutscene, 2: a sequence played
  static std::atomic<int> logged{0};
  const int state = cutscene ? 1 : in_sequence ? 2 : 0;
  if (was.exchange(state) != state && logged.fetch_add(1) < 100) {
    if (state == 2) {
      REXLOG_INFO("ps5 frame rate: the player is playing it ({}), {:.1f} s in: 60", played_by,
                  double(now - sequence_start_ms.load(std::memory_order_relaxed)) / 1000.0);
    } else {
      REXLOG_INFO("ps5 frame rate: {}", state == 1 ? "a cutscene has the player: 30" : "the cutscene is over: 60");
    }
  }
  // (for the log's count of slow frames: frames at 30 on purpose aren't)
  rex::ps5diag::SetHalfRate(cutscene);
  return cutscene ? 2 : 1;
}

float LoadFloat(const uint8_t* base, uint32_t address) {
  uint32_t bits;
  std::memcpy(&bits, base + address, 4);
  bits = __builtin_bswap32(bits);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

void StoreFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  bits = __builtin_bswap32(bits);
  std::memcpy(base + address, &bits, 4);
}

}  // namespace

extern "C" void EARS__Modules__Player__PlayerUpdateBlock(PPCContext& __restrict ctx, uint8_t* base) {
  // (r3: the player, an EARS::Framework::Base&)
  if (const uint32_t player = ctx.r3.u32) {
    if (base[player + kPlayerSequenceFlags] & kSequenceRunning) {
      sequence_seen_ms.store(rex::ps5diag::NowMs() | 1, std::memory_order_relaxed);
    }
  }
  __imp__EARS__Modules__Player__PlayerUpdateBlock(ctx, base);
}

extern "C" void EARS__Time__TimeManager__HandleEvents(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool sixty = REXCVAR_GET(ps5_frame_rate) >= 60;
  if (sixty) {
    const float one = LoadFloat(base, kOneVblankMs);
    const float want = one * float(TargetVblanks());
    // (16.68 ms once the game has set its TV system up; nothing is touched before)
    if (one > 16.0f && one < 17.5f && LoadFloat(base, kTargetFrameMs) != want) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXLOG_INFO("ps5 frame rate: the game's target frame {:.3f} ms -> {:.3f} ms ({:.1f} FPS)",
                    LoadFloat(base, kTargetFrameMs), want, 1000.0 / double(want));
      }
      StoreFloat(base, kTargetFrameMs, want);
    }
  }
  __imp__EARS__Time__TimeManager__HandleEvents(ctx, base);
}

extern "C" void Displ_WaitForFrameLock_8290C710(PPCContext& __restrict ctx, uint8_t* base) {
  // (once a frame the game makes, before it is shown: the frames the game
  // really made, for the on-screen counter and the log; at its own 30 the
  // game hands the emulator about two pictures a frame)
  rex::ps5diag::Add(rex::ps5diag::kGameFrames);
  static const bool sixty = REXCVAR_GET(ps5_frame_rate) >= 60;
  if (sixty) {
    const int vblanks = TargetVblanks();
    if (int32_t(ctx.r3.u32) > vblanks) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXLOG_INFO("ps5 frame rate: the frame lock of {} vblanks -> {}", int32_t(ctx.r3.u32), vblanks);
      }
      ctx.r3.u64 = uint32_t(vblanks);
    }
  }
  __imp__Displ_WaitForFrameLock_8290C710(ctx, base);
}

// The minigame's own events (see the top): each one is noted, then the game's.
#define DS2_HOOK_HACK_EVENT(name)                                                     \
  extern "C" void name(PPCContext& __restrict ctx, uint8_t* base) {                   \
    NoteHackEvent(#name + (sizeof("EARS__Modules__HackingStationMiniGame__") - 1)); \
    __imp__##name(ctx, base);                                                         \
  }
DS2_HACK_EVENTS(DS2_HOOK_HACK_EVENT)
#undef DS2_HOOK_HACK_EVENT

// The first player's controller as the game gets it (ds2_ps5_script.cpp, from
// XInputGetStateEx): something new on it marks the sequence the player is in
// as played (see the top).
void Ds2NotePadState(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger, int16_t lx, int16_t ly,
                     int16_t rx, int16_t ry) {
  // XINPUT_GAMEPAD_START and _BACK: the pause menu, not playing.
  constexpr uint16_t kIgnored = 0x0010 | 0x0020;
  // A stick past about half way, or a trigger past half: pushed. Back under a
  // third (and a trigger under a quarter): at rest again.
  constexpr int kPushed = 16000, kRest = 11000;
  auto magnitude = [](int16_t v) { return v < 0 ? -int(v) : int(v); };
  const int stick = std::max(std::max(magnitude(lx), magnitude(ly)), std::max(magnitude(rx), magnitude(ry)));
  const int trigger = std::max(left_trigger, right_trigger);
  static uint16_t last_buttons = 0;
  static bool pushed = false;
  bool event = ((buttons & ~last_buttons) & ~kIgnored) != 0;
  if (!pushed && (stick > kPushed || trigger > 128)) {
    pushed = true;
    event = true;
  } else if (pushed && stick < kRest && trigger < 64) {
    pushed = false;
  }
  last_buttons = buttons;
  if (event) input_event_ms.store(rex::ps5diag::NowMs() | 1, std::memory_order_relaxed);
}
