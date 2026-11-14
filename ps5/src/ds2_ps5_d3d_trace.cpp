// Dead Space 2 for PS5 - a record of the game's Direct3D calls (ps5_d3d_trace).
//
// The first step towards a native renderer (ps5/NATIVE_RENDERER.md): what the
// game asks the Xbox 360's Direct3D for, frame by frame, recorded on the
// console, so the renderer can be written against what the game really does.
//
// The game links the Xbox 360 SDK's Direct3D, and its symbols name the
// functions (D3DDevice_SetRenderTarget and the rest). Each one is taken over
// here the way the frame rate's are (ds2_ps5_framerate.cpp): the recompiled code
// defines it as a weak alias of __imp__<name>, the definition here wins at link
// time, notes the call and goes on to the original. Nothing the game does
// changes.
//
// The functions carry no types in the symbols, so a call is recorded as the
// registers it was made with (the first argument in r3, the device), and the
// structures pointers point at (surfaces, textures, rectangles, viewports) as
// the 32-bit words they hold. What they mean is worked out from the records.
//
// ps5_d3d_trace = 1 in ds2.toml:
//  - every 30 s, a line in the log: each function's calls a frame;
//  - every 60 s, one whole frame, call by call, in logs/d3d_frame_<n>.txt
//    (and every pixel and vertex shader the game creates, once, in
//    logs/d3d_shaders.txt).
// Off (0, the default), each function costs one test of a flag.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ps5_diag.h>

REXCVAR_DEFINE_INT32(ps5_d3d_trace, 0, "PS5",
                     "Record the game's Direct3D calls: 0 off, 1 a summary every 30 s and a whole "
                     "frame every 60 s in the logs folder");

namespace {

// What is recorded of each function: its name, how many integer argument
// registers it takes (r3 = the device, then r4...), whether it takes a float
// (f1), and which arguments point at something worth recording (a bit per
// register from r4: bit 0 = r4) with how many 32-bit words.
struct Traced {
  const char* name;
  uint8_t int_args;
  bool float_arg;
  uint8_t pointer_mask;
  uint8_t pointer_words;
};

enum Function : uint32_t {
  kSetRenderTarget,
  kSetDepthStencilSurface,
  kClear,
  kClearF,
  kResolve,
  kResolveEx,
  kDrawVertices,
  kDrawIndexedVertices,
  kDrawVerticesUP,
  kSetTexture,
  kSetVertexShader,
  kSetPixelShader,
  kCreatePixelShader,
  kCreateVertexShader,
  kBeginTiling,
  kEndTiling,
  kSwap,
  kSetViewport,
  kCreateTexture,
  kCreateSurface,
  kSetStreamSource,
  kSetIndices,
  kSetScissorRect,
  kSetPredication,
  kFunctionCount,
};

// (the argument lists are the Xbox 360 SDK's documented ones; the records
// show whether that is what reaches these functions)
constexpr Traced kTraced[kFunctionCount] = {
    // (device, index, surface)
    {"SetRenderTarget", 3, false, 0b10, 16},
    // (device, surface)
    {"SetDepthStencilSurface", 2, false, 0b1, 16},
    // (device, count, rects, flags, color, Z (f1), stencil, EDRAM clear)
    {"Clear", 7, true, 0b10, 4},
    // (device, flags, rect, color vector, Z (f1), stencil)
    {"ClearF", 5, true, 0b110, 4},
    // (device, flags, source rect, destination texture, destination point,
    //  level, slice, clear color, clear Z (f1), clear stencil, parameters)
    {"Resolve", 8, true, 0b110, 16},
    {"ResolveEx", 8, true, 0b110, 16},
    // (device, primitive type, start vertex, vertex count)
    {"DrawVertices", 4, false, 0, 0},
    // (device, primitive type, base vertex, start index, index count)
    {"DrawIndexedVertices", 5, false, 0, 0},
    // (device, primitive type, vertex count, data, stride)
    {"DrawVerticesUP", 5, false, 0, 0},
    // (device, sampler, texture)
    {"SetTexture", 3, false, 0b10, 16},
    // (device, shader)
    {"SetVertexShader", 2, false, 0, 0},
    {"SetPixelShader", 2, false, 0, 0},
    // (microcode, ...)
    {"CreatePixelShader", 3, false, 0, 0},
    {"CreateVertexShader", 3, false, 0, 0},
    // (device, flags, count, rects, clear color, clear Z (f1), clear stencil)
    {"BeginTiling", 6, true, 0b100, 4},
    // (device, resolve flags, rects, destination texture, clear color, ...)
    {"EndTiling", 6, true, 0b110, 16},
    // (device, front buffer, scaler parameters)
    {"Swap", 3, false, 0b1, 16},
    // (device, viewport)
    {"SetViewport", 2, false, 0b1, 6},
    {"CreateTexture", 8, false, 0, 0},
    {"CreateSurface", 8, false, 0, 0},
    // (device, stream, vertex buffer, offset, stride)
    {"SetStreamSource", 5, false, 0b10, 8},
    // (device, index buffer)
    {"SetIndices", 2, false, 0b1, 8},
    // (device, rect)
    {"SetScissorRect", 2, false, 0b1, 4},
    // (device, mask)
    {"SetPredication", 2, false, 0, 0},
};

struct TraceState {
  std::mutex mutex;
  uint64_t frame = 0;
  // Calls this frame, and over the summary's window.
  uint64_t frame_calls[kFunctionCount] = {};
  uint64_t window_calls[kFunctionCount] = {};
  uint64_t window_frames = 0;
  uint64_t window_start_ms = 0;
  // The frame being recorded call by call (empty: none).
  bool recording = false;
  uint64_t next_record_ms = 0;
  uint32_t records_written = 0;
  std::string record;
  uint64_t record_call = 0;
  // Shaders already written to d3d_shaders.txt (by microcode address).
  std::unordered_set<uint32_t> shaders_seen;
};

TraceState& State() {
  static TraceState state;
  return state;
}

bool Enabled() {
  static const bool enabled = REXCVAR_GET(ps5_d3d_trace) > 0;
  return enabled;
}

std::filesystem::path LogFolder() {
  return rex::filesystem::GetExecutableFolder() / "logs";
}

// A big-endian 32-bit word of guest memory (0 when the address is no use).
uint32_t GuestWord(const uint8_t* base, uint32_t address) {
  if (address < 0x10000 || address > 0xFFFFFFF0u) {
    return 0;
  }
  // (the 0xE0000000 range is 4 KB into the memory it mirrors: ds2_ps5_pch.h)
  const uint32_t host = address + (address >= 0xE0000000u ? 0x1000u : 0u);
  uint32_t value;
  std::memcpy(&value, base + host, 4);
  return __builtin_bswap32(value);
}

void AppendWords(std::string& out, const uint8_t* base, uint32_t address, uint32_t words) {
  if (address < 0x10000) {
    return;
  }
  out += " [";
  for (uint32_t i = 0; i < words; ++i) {
    out += fmt::format(i ? " {:08X}" : "{:08X}", GuestWord(base, address + i * 4));
  }
  out += "]";
}

void WriteFileInBackground(std::filesystem::path path, std::string text) {
  std::thread([path = std::move(path), text = std::move(text)] {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(text.data(), 1, text.size(), f);
      std::fclose(f);
    }
  }).detach();
}

// A shader's microcode, once, to d3d_shaders.txt: what the native renderer will
// have to translate. The Xbox 360 shader blob's own header says how long it is;
// up to 64 KB of it is written as words.
void NoteShader(TraceState& state, const uint8_t* base, const char* kind, uint32_t microcode) {
  if (microcode < 0x10000 || !state.shaders_seen.insert(microcode).second ||
      state.shaders_seen.size() > 20000) {
    return;
  }
  std::string text = fmt::format("{} shader, microcode at {:08X}:", kind, microcode);
  // (words until 64 KB or a run of 64 zero words: the record says the rest)
  uint32_t zeros = 0;
  for (uint32_t i = 0; i < 16384 && zeros < 64; ++i) {
    const uint32_t word = GuestWord(base, microcode + i * 4);
    zeros = word ? 0 : zeros + 1;
    text += fmt::format(i % 8 ? " {:08X}" : "\n  {:08X}", word);
  }
  text += "\n\n";
  std::error_code ec;
  std::filesystem::create_directories(LogFolder(), ec);
  if (std::FILE* f = std::fopen((LogFolder() / "d3d_shaders.txt").c_str(), "ab")) {
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
  }
}

void Note(Function function, const PPCContext& ctx, const uint8_t* base) {
  TraceState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  ++state.frame_calls[function];
  if (function == kCreatePixelShader || function == kCreateVertexShader) {
    NoteShader(state, base, function == kCreatePixelShader ? "pixel" : "vertex", ctx.r3.u32);
  }
  if (!state.recording) {
    return;
  }
  const Traced& traced = kTraced[function];
  std::string& out = state.record;
  out += fmt::format("{:6} {}", state.record_call++, traced.name);
  const PPCRegister* registers[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6,
                                    &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  for (uint32_t i = 0; i < traced.int_args && i < 8; ++i) {
    out += fmt::format(" {:08X}", registers[i]->u32);
  }
  if (traced.float_arg) {
    out += fmt::format(" f1={}", ctx.f1.f64);
  }
  for (uint32_t i = 0; i + 1 < traced.int_args && i < 7; ++i) {
    if (traced.pointer_mask & (1u << i)) {
      AppendWords(out, base, registers[i + 1]->u32, traced.pointer_words);
    }
  }
  out += '\n';
}

// At each Swap: the frame's counts, the summary every 30 s, and the start or
// end of a recorded frame.
void FrameEnd() {
  TraceState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const uint64_t now_ms = rex::ps5diag::NowMs();
  ++state.frame;
  ++state.window_frames;
  for (uint32_t i = 0; i < kFunctionCount; ++i) {
    state.window_calls[i] += state.frame_calls[i];
  }
  if (state.recording) {
    std::string header =
        fmt::format("Dead Space 2 for PS5: the Direct3D calls of frame {}, call by call.\n"
                    "Each line: number, function, the argument registers from r3 (the device),\n"
                    "f1 where the function takes a float, then [words] that pointer arguments\n"
                    "point at (big-endian 32-bit words of guest memory).\nCalls this frame:",
                    state.frame);
    for (uint32_t i = 0; i < kFunctionCount; ++i) {
      if (state.frame_calls[i]) {
        header += fmt::format(" {} {}", kTraced[i].name, state.frame_calls[i]);
      }
    }
    header += "\n\n";
    WriteFileInBackground(LogFolder() / fmt::format("d3d_frame_{:03}.txt", state.records_written),
                          header + state.record);
    REXLOG_INFO("ps5 d3d: frame {} recorded call by call in logs/d3d_frame_{:03}.txt ({} calls)",
                state.frame, state.records_written, state.record_call);
    ++state.records_written;
    state.recording = false;
    state.record.clear();
    state.record.shrink_to_fit();
  }
  std::fill(std::begin(state.frame_calls), std::end(state.frame_calls), 0);
  // (the first recording a minute in: past the menus)
  if (!state.next_record_ms) {
    state.next_record_ms = now_ms + 60000;
  }
  if (now_ms >= state.next_record_ms && state.records_written < 100) {
    state.recording = true;
    state.record_call = 0;
    state.record.reserve(size_t(1) << 20);
    state.next_record_ms = now_ms + 60000;
  }
  if (!state.window_start_ms) {
    state.window_start_ms = now_ms;
  }
  if (now_ms - state.window_start_ms >= 30000 && state.window_frames) {
    std::string line;
    for (uint32_t i = 0; i < kFunctionCount; ++i) {
      if (state.window_calls[i]) {
        line += fmt::format(", {} {:.1f}", kTraced[i].name,
                            double(state.window_calls[i]) / double(state.window_frames));
      }
    }
    REXLOG_INFO("ps5 d3d: calls a frame over {} frames{}", state.window_frames, line);
    std::fill(std::begin(state.window_calls), std::end(state.window_calls), 0);
    state.window_frames = 0;
    state.window_start_ms = now_ms;
  }
}

}  // namespace

// The functions taken over: a note when tracing, then the game's own.
#define DS2_D3D_TRACE(function, symbol)                                             \
  extern "C" void __imp__##symbol(PPCContext& __restrict ctx, uint8_t* base);       \
  extern "C" void symbol(PPCContext& __restrict ctx, uint8_t* base) {               \
    if (Enabled()) Note(function, ctx, base);                                       \
    __imp__##symbol(ctx, base);                                                     \
  }

DS2_D3D_TRACE(kSetRenderTarget, D3DDevice_SetRenderTarget_8221BE58)
DS2_D3D_TRACE(kSetDepthStencilSurface, D3DDevice_SetDepthStencilSurface_8221C1A8)
DS2_D3D_TRACE(kClear, D3DDevice_Clear_82236D30)
DS2_D3D_TRACE(kClearF, D3DDevice_ClearF_82236DF0)
DS2_D3D_TRACE(kResolve, D3DDevice_Resolve_82235B10)
DS2_D3D_TRACE(kResolveEx, D3DDevice_ResolveEx_82234A38)
DS2_D3D_TRACE(kDrawVertices, D3DDevice_DrawVertices_8228DAA8)
DS2_D3D_TRACE(kDrawIndexedVertices, D3DDevice_DrawIndexedVertices_8228DEC0)
DS2_D3D_TRACE(kDrawVerticesUP, D3DDevice_DrawVerticesUP_8228DA60)
DS2_D3D_TRACE(kSetTexture, D3DDevice_SetTexture_82224300)
DS2_D3D_TRACE(kSetVertexShader, D3DDevice_SetVertexShader_82220658)
DS2_D3D_TRACE(kSetPixelShader, D3DDevice_SetPixelShader_822202B0)
DS2_D3D_TRACE(kCreatePixelShader, D3DDevice_CreatePixelShader_822201C0)
DS2_D3D_TRACE(kCreateVertexShader, D3DDevice_CreateVertexShader_82220568)
DS2_D3D_TRACE(kBeginTiling, D3DDevice_BeginTiling_82233AD0)
DS2_D3D_TRACE(kEndTiling, D3DDevice_EndTiling_82233E48)
DS2_D3D_TRACE(kSetViewport, D3DDevice_SetViewport_8221BC28)
DS2_D3D_TRACE(kCreateTexture, D3DDevice_CreateTexture_82223F50)
DS2_D3D_TRACE(kCreateSurface, D3DDevice_CreateSurface_82224070)
DS2_D3D_TRACE(kSetStreamSource, D3DDevice_SetStreamSource_8221BCA8)
DS2_D3D_TRACE(kSetIndices, D3DDevice_SetIndices_8221BDC8)
DS2_D3D_TRACE(kSetScissorRect, D3DDevice_SetScissorRect_8221BB08)
DS2_D3D_TRACE(kSetPredication, D3DDevice_SetPredication_82233960)

// Swap ends a frame: noted, then the frame's bookkeeping.
extern "C" void __imp__D3DDevice_Swap_82229C10(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void D3DDevice_Swap_82229C10(PPCContext& __restrict ctx, uint8_t* base) {
  if (Enabled()) {
    Note(kSwap, ctx, base);
    FrameEnd();
  }
  __imp__D3DDevice_Swap_82229C10(ctx, base);
}
