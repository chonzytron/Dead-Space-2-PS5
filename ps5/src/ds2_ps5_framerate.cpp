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
// some cutscene animation isn't quite, and ps5_frame_rate = 30 in ds2.toml is
// the game as it shipped.
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_INT32(ps5_frame_rate, 60, "PS5",
                     "Frames a second the game aims for: 60, or 30 (the game's own)");

extern "C" void __imp__EARS__Time__TimeManager__HandleEvents(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__Displ_WaitForFrameLock_8290C710(PPCContext& __restrict ctx, uint8_t* base);

namespace {

// Title Update #2 (the only build the PS5 app is made from): ONE_FRAMETIME's
// source and g_targetFrameTimeMSec, both floats in milliseconds.
constexpr uint32_t kOneVblankMs = 0x82D437E0, kTargetFrameMs = 0x82D475C0;

// Vblanks per frame: 1 for 60, 2 for anything else (the game's own).
int TargetVblanks() {
  static const int vblanks = REXCVAR_GET(ps5_frame_rate) >= 60 ? 1 : 2;
  return vblanks;
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

extern "C" void EARS__Time__TimeManager__HandleEvents(PPCContext& __restrict ctx, uint8_t* base) {
  if (TargetVblanks() == 1) {
    const float one = LoadFloat(base, kOneVblankMs);
    // (16.68 ms once the game has set its TV system up; nothing is touched before)
    if (one > 16.0f && one < 17.5f && LoadFloat(base, kTargetFrameMs) != one) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXLOG_INFO("ps5 frame rate: the game's target frame {:.3f} ms -> {:.3f} ms ({:.1f} FPS)",
                    LoadFloat(base, kTargetFrameMs), one, 1000.0 / double(one));
      }
      StoreFloat(base, kTargetFrameMs, one);
    }
  }
  __imp__EARS__Time__TimeManager__HandleEvents(ctx, base);
}

extern "C" void Displ_WaitForFrameLock_8290C710(PPCContext& __restrict ctx, uint8_t* base) {
  if (TargetVblanks() == 1 && int32_t(ctx.r3.u32) > 1) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("ps5 frame rate: the frame lock of {} vblanks -> 1", int32_t(ctx.r3.u32));
    }
    ctx.r3.u64 = 1;
  }
  __imp__Displ_WaitForFrameLock_8290C710(ctx, base);
}
