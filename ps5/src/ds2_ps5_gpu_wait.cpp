// Dead Space 2 for PS5 - the game's waits for the GPU, made gentler.
//
// The game's Direct3D waits for the GPU (a fence: D3D::CDevice::BlockOnFence;
// room in the ring buffer, and the like) by checking again and again with
// D3D::CBlocker::Check, which never sleeps or pauses: on the Xbox 360 a
// hardware thread of its own spins. Here the thread that does it uses a whole
// processor core all the time, even at 60 frames a second (a console profile:
// 99% of a core in every 30 s window; the Linux test build: 98%, nearly all in
// BlockOnFence and CBlocker::Check).
//
// The PS5 runs two threads on each core and the port leaves placing them to
// the system, so that core is shared with something: the game's main thread,
// or the emulator's command processor. A spinning thread takes a good part of
// the core from the other. With ps5_gentle_gpu_waits each check is preceded by
// a short pause (the processor's spin-wait hint, which gives the core over to
// the other thread meanwhile), and every 16th by a yield to any thread that is
// waiting for the processor. A check that would have ended the wait still
// does; the wait ends at most about a microsecond later.
//
// The console run ds2_031 (on since): in the stretches of 2,000 draws a frame
// or more, 11 frames over 20 ms against 124 in ds2_030, and the command
// processor's draws 7.5 ms a frame against 8.7: the core it shared is free.
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/thread.h>

REXCVAR_DEFINE_BOOL(ps5_gentle_gpu_waits, true, "PS5",
                    "The game's waits for the GPU pause between checks instead of spinning flat "
                    "out (ds2_ps5_gpu_wait.cpp)");

extern "C" void __imp__D3D__CBlocker__Check(PPCContext& __restrict ctx, uint8_t* base);

extern "C" void D3D__CBlocker__Check(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool gentle = [] {
    const bool on = REXCVAR_GET(ps5_gentle_gpu_waits);
    if (on) REXLOG_INFO("ps5 gpu waits: the game's waits for the GPU pause between checks");
    return on;
  }();
  if (gentle) {
#if defined(__x86_64__) || defined(_M_X64)
    for (int i = 0; i < 16; ++i) _mm_pause();
#endif
    thread_local uint32_t checks = 0;
    if (!(++checks & 15)) rex::thread::MaybeYield();
  }
  __imp__D3D__CBlocker__Check(ctx, base);
}
