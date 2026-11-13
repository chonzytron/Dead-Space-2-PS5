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
// some cutscene animation isn't quite. So the game goes back to its own 30
// while a cutscene has hold of the player (ps5_cutscenes_30): the engine's
// word for that is a scripted sequence, and the player object says when it is
// in one (IScriptedSequenceTarget, at Player + 560: the running bit, 4, of the
// flags at + 16, set by StartScriptedSequence and cleared by
// EndScriptedSequence). It is read once a frame from Player::PlayerUpdateBlock,
// which is called with the player for as long as there is one; when it has not
// been seen in a sequence for a moment (a menu, a loading screen, the sequence
// over), the game is back at 60.
//
// ps5_frame_rate = 30 in ds2.toml is the game as it shipped, throughout.
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
  const bool cutscene = InCutscene();
  // (in the log, the first hundred times it changes: enough to see it work)
  static std::atomic<bool> was{false};
  static std::atomic<int> logged{0};
  if (was.exchange(cutscene) != cutscene && logged.fetch_add(1) < 100) {
    REXLOG_INFO("ps5 frame rate: {}", cutscene ? "a cutscene has the player: 30" : "the cutscene is over: 60");
  }
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
