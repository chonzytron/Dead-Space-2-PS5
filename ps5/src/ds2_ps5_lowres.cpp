// Dead Space 2 for PS5 - the game's "low resolution" sections (ps5_lowres_culling).
//
// What 70% of the EDRAM transfers are (ps5/NATIVE_RENDERER.md). The game draws
// at 1x (no MSAA), and for some passes it reads the same colour and depth
// memory as 4x MSAA at half the width and height
// (Displ_StartLowRes_XenonSpecific .. Displ_EndLowRes_XenonSpecific): on the
// Xbox 360 each 4x "pixel" is then a 2x2 block of the real pixels, so a pass
// costs a quarter of the pixel work, and switching costs nothing. Here the two
// views are separate images, and every switch is a full-screen copy each way.
//
// Who does it (the recompiled code's callers):
//  - the deferred lighting's stencil culling
//    (EARS::Framework::DeferredLitHiStencilCullingClass): for each kind of
//    light, full-screen stencil-only passes mark the pixels the lights can
//    reach. Colour writes are off; only the stencil changes.
//  - the shadow attenuation pass
//    (SceneManagerInternal::RenderDeferredBuffer_CreateShadowAttenuation);
//  - low resolution effects (EARS::Alchemy::LowResolution, with
//    Displ_ResolveInHalf_XenonSpecific).
//
// ps5_lowres_culling = "full": the culling passes stay at 1x, where the light
// passes read their stencil anyway. The same pixels are marked (all four of
// each 2x2 block, as the 4x view marks them), with four times the pixels but
// no copies. The shadow attenuation and the effects are left as they are:
// what they draw is read back at half size.
//
// Either way, the log says every 30 s how many sections a frame each caller
// opened, and how many were kept at 1x.
#include <atomic>
#include <cstdint>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ps5_diag.h>

REXCVAR_DEFINE_STRING(ps5_lowres_culling, "game", "PS5",
                      "The lighting's stencil culling: game (at 4x MSAA over the 1x memory, as on the "
                      "Xbox 360: copies each way here) or full (at 1x, no copies). Experimental");

namespace {

enum Caller : int { kCulling, kShadow, kEffects, kOther, kCallerCount };
constexpr const char* kCallerName[kCallerCount] = {"lighting culling", "shadow attenuation",
                                                   "effects", "other"};

// Where the render thread is (the game renders on one thread: plain counters).
int culling_depth = 0, shadow_depth = 0, effects_depth = 0;
// Sections opened at 1x instead (their ends are skipped too).
int kept_open = 0;

std::atomic<uint64_t> sections[kCallerCount], kept[kCallerCount];

bool CullingAtFullResolution() {
  static const bool full = REXCVAR_GET(ps5_lowres_culling) == "full";
  return full;
}

Caller Current() {
  if (culling_depth) return kCulling;
  if (shadow_depth) return kShadow;
  if (effects_depth) return kEffects;
  return kOther;
}

// The summary, at most every 30 s, from the section starts.
void MaybeReport() {
  static uint64_t report_at_ms = 0, swaps_at_report = 0;
  const uint64_t now = rex::ps5diag::NowMs();
  if (!report_at_ms) {
    report_at_ms = now + 30000;
    swaps_at_report = rex::ps5diag::Get(rex::ps5diag::kSwaps);
    REXLOG_INFO("ps5 lowres: the lighting's stencil culling {}", CullingAtFullResolution()
                                                                     ? "at 1x (ps5_lowres_culling = full)"
                                                                     : "at 4x over the 1x memory, as the game does");
    return;
  }
  if (now < report_at_ms) return;
  const uint64_t swaps = rex::ps5diag::Get(rex::ps5diag::kSwaps);
  const double frames = double(swaps > swaps_at_report ? swaps - swaps_at_report : 1);
  std::string line;
  for (int i = 0; i < kCallerCount; ++i) {
    const uint64_t n = sections[i].exchange(0), k = kept[i].exchange(0);
    if (n) line += fmt::format(", {} {:.1f} (at 1x {:.1f})", kCallerName[i], double(n) / frames, double(k) / frames);
  }
  REXLOG_INFO("ps5 lowres: 4x sections a frame over {:.0f} frames{}", frames, line.empty() ? ", none" : line);
  report_at_ms = now + 30000;
  swaps_at_report = swaps;
}

}  // namespace

// The callers: where the render thread is.
#define DS2_LOWRES_CALLER(symbol, depth)                                      \
  extern "C" void __imp__##symbol(PPCContext& __restrict ctx, uint8_t* base); \
  extern "C" void symbol(PPCContext& __restrict ctx, uint8_t* base) {         \
    ++depth;                                                                  \
    __imp__##symbol(ctx, base);                                               \
    --depth;                                                                  \
  }

DS2_LOWRES_CALLER(EARS__Framework__DeferredLitHiStencilCullingClass__StartCullingType, culling_depth)
DS2_LOWRES_CALLER(EARS__Framework__DeferredLitHiStencilCullingClass__StartFullScreenCulling, culling_depth)
DS2_LOWRES_CALLER(EARS__Framework__DeferredLitHiStencilCullingClass__StartSkyCulling, culling_depth)
DS2_LOWRES_CALLER(EARS__Framework__DeferredLitHiStencilCullingClass__UpdateStencilAndHiStencilForInteriorExteriorSkyCulling,
                  culling_depth)
DS2_LOWRES_CALLER(EARS__Framework__SceneManagerInternal__RenderDeferredBuffer_CreateShadowAttenuation, shadow_depth)
DS2_LOWRES_CALLER(EARS__Alchemy__LowResolution__Glue_LowRes_Start, effects_depth)

extern "C" void __imp__Displ_StartLowRes_XenonSpecific_8290D7B0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void Displ_StartLowRes_XenonSpecific_8290D7B0(PPCContext& __restrict ctx, uint8_t* base) {
  const Caller caller = Current();
  sections[caller].fetch_add(1, std::memory_order_relaxed);
  MaybeReport();
  if (caller == kCulling && CullingAtFullResolution()) {
    // Stays at 1x: the render targets and the viewport are left as they are.
    kept[caller].fetch_add(1, std::memory_order_relaxed);
    ++kept_open;
    return;
  }
  __imp__Displ_StartLowRes_XenonSpecific_8290D7B0(ctx, base);
}

extern "C" void __imp__Displ_EndLowRes_XenonSpecific_8290D848(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void Displ_EndLowRes_XenonSpecific_8290D848(PPCContext& __restrict ctx, uint8_t* base) {
  if (kept_open > 0 && culling_depth) {
    --kept_open;
    return;
  }
  __imp__Displ_EndLowRes_XenonSpecific_8290D848(ctx, base);
}
