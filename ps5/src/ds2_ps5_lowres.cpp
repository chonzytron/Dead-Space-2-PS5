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
// ps5_lowres_culling = "full" (the default): the culling passes stay at 1x, where the light
// passes read their stencil anyway. The same pixels are marked (all four of
// each 2x2 block, as the 4x view marks them), with four times the pixels but
// no copies. The shadow attenuation and the effects are left as they are:
// what they draw is read back at half size.
//
// Either way, the log says every 30 s how many sections a frame each caller
// opened, and how many were kept at 1x.
//
// Direct3D's own clears do the same over the middle of the surface; below,
// ps5_lowres_clears.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ps5_diag.h>

REXCVAR_DEFINE_STRING(ps5_lowres_culling, "full", "PS5",
                      "The lighting's stencil culling: full (at 1x, no copies) or game (at 4x MSAA over "
                      "the 1x memory, as on the Xbox 360: copies each way here)");

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

// The clears (ps5_lowres_clears). D3D::ClearRect, under every clear of a
// surface, splits the rectangle in two: its edges, cleared as they are, and the
// part aligned to EDRAM tiles, cleared over the same memory as 4x MSAA at half
// the width and height (four times faster on the Xbox 360; a copy each way
// here, and on the Linux test build every copy that was left once the culling
// was at 1x). With "full", the whole rectangle takes Direct3D's own path for
// rectangles too small to split: one D3D::ClearRects of the rectangle as the
// surface is. The same pixels get the same values, with no 4x.
//
// D3D::ClearRect(device r3, left r4, top r5, right r6, bottom r7, colour r8,
// Z f1, GPU_SURFACEINFO r10, then on the caller's stack at +84 GPU_COLORINFO,
// +92 / +100 / +108 / +116 the rest) and D3D::ClearRects(device r3, count r4,
// rectangles r5, colour r6, Z f1, GPU_SURFACEINFO r8, GPU_COLORINFO r9,
// r10 and +84 two of the rest), as ClearRect itself calls it (0x822360C8).
REXCVAR_DEFINE_STRING(ps5_lowres_clears, "game", "PS5",
                      "Direct3D's clears: game (the middle at 4x MSAA over the 1x memory, as on the "
                      "Xbox 360: copies each way here) or full (all at the surface's own MSAA)");

// For finding out what the copies left are (not to play with: the picture is
// wrong): ps5_debug_skip_clears = true leaves out the game's Direct3D clears.
REXCVAR_DEFINE_BOOL(ps5_debug_skip_clears, false, "PS5",
                    "Debug: leave out the game's Direct3D clears (the picture is wrong)");

namespace {

uint32_t LoadGuest32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}
void StoreGuest32(uint8_t* base, uint32_t address, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + address, &v, 4);
}

std::atomic<uint64_t> clears_seen{0};

}  // namespace

extern "C" void __imp__D3D__ClearRects(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__D3D__ClearRect(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void D3D__ClearRect(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool full = REXCVAR_GET(ps5_lowres_clears) == "full";
  if (!full) {
    __imp__D3D__ClearRect(ctx, base);
    return;
  }
  if (clears_seen.fetch_add(1, std::memory_order_relaxed) == 0) {
    REXLOG_INFO("ps5 lowres: Direct3D's clears at the surface's own MSAA (ps5_lowres_clears = full)");
  }
  const uint32_t caller_sp = ctx.r1.u32;
  const uint32_t color_info = LoadGuest32(base, caller_sp + 84);
  const uint32_t rest_92 = LoadGuest32(base, caller_sp + 92);
  const uint32_t rest_108 = LoadGuest32(base, caller_sp + 108);
  // A frame of ClearRect's own size for the call: back chain, the rectangle at
  // +96, ClearRects' stack argument at +84.
  const uint32_t sp = caller_sp - 288;
  StoreGuest32(base, sp, caller_sp);
  StoreGuest32(base, sp + 96, ctx.r4.u32);
  StoreGuest32(base, sp + 100, ctx.r5.u32);
  StoreGuest32(base, sp + 104, ctx.r6.u32);
  StoreGuest32(base, sp + 108, ctx.r7.u32);
  StoreGuest32(base, sp + 84, rest_108);
  const uint32_t surface_info = ctx.r10.u32, color = ctx.r8.u32;
  ctx.r1.u64 = sp;
  ctx.r4.u64 = 1;
  ctx.r5.u64 = sp + 96;
  ctx.r6.u64 = color;
  ctx.r8.u64 = surface_info;
  ctx.r9.u64 = color_info;
  ctx.r10.u64 = rest_92;
  __imp__D3D__ClearRects(ctx, base);
  ctx.r1.u64 = caller_sp;
}

// (D3D::ClearSurface: every clear of a whole surface, from D3DDevice_Clear
// and D3DDevice_ClearF)
extern "C" void __imp__D3D__ClearSurface(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void D3D__ClearSurface(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool skip = REXCVAR_GET(ps5_debug_skip_clears);
  if (skip) return;
  __imp__D3D__ClearSurface(ctx, base);
}
