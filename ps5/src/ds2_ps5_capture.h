// Dead Space 2 for PS5 - pictures of the game in the logs folder (ps5_capture).
//
// What the game showed, to put beside a log: a renderer change can be checked
// by looking at it, on the console as on the Linux test build. Every
// ps5_capture seconds the picture the game last presented (before it is scaled
// to the TV) is saved as logs/capture_<n>.bmp, at most 1280 pixels wide (a
// native 4K picture is shrunk three times), and the log says which frame it
// was. The last 100 are kept; numbering goes on from where it was.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ps5_diag.h>
#include <rex/ui/presenter.h>

REXCVAR_DEFINE_INT32(ps5_capture, 0, "PS5",
                     "Save the game's picture to the logs folder every this many seconds (0: never)");

namespace ds2ps5 {

class PictureCapture {
 public:
  // Starts the thread when ps5_capture is set (once).
  static void Start(rex::ui::Presenter* presenter, std::filesystem::path folder) {
    const int seconds = REXCVAR_GET(ps5_capture);
    if (seconds <= 0 || !presenter) return;
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    REXLOG_INFO("ps5 capture: the picture every {} s, to {}/capture_<n>.bmp", seconds, folder.string());
    std::thread([presenter, folder = std::move(folder), seconds] {
      rex::ps5diag::RegisterThread("capture");
      std::error_code ec;
      std::filesystem::create_directories(folder, ec);
      for (uint32_t n = 0;; ++n) {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        Capture(presenter, folder, n);
      }
    }).detach();
  }

 private:
  static void Capture(rex::ui::Presenter* presenter, const std::filesystem::path& folder, uint32_t n) {
    rex::ui::RawImage image;
    const uint64_t frame = rex::ps5diag::Get(rex::ps5diag::kSwaps);
    if (!presenter->CaptureGuestOutput(image) || !image.width || !image.height) {
      REXLOG_INFO("ps5 capture: no picture yet (frame {})", frame);
      return;
    }
    // Shrunk by a whole factor to at most 1280 wide, each pixel the average of its block.
    const uint32_t shrink = (image.width + 1279) / 1280;
    const uint32_t w = image.width / shrink, h = image.height / shrink;
    const uint32_t row_bytes = (w * 3 + 3) & ~3u;
    std::vector<uint8_t> bmp(54 + size_t(row_bytes) * h, 0);
    auto put32 = [&](size_t at, uint32_t v) {
      for (int i = 0; i < 4; ++i) bmp[at + i] = uint8_t(v >> (8 * i));
    };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(2, uint32_t(bmp.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);  // (bottom row first)
    bmp[26] = 1;
    bmp[28] = 24;
    put32(34, uint32_t(bmp.size() - 54));
    for (uint32_t y = 0; y < h; ++y) {
      uint8_t* out = bmp.data() + 54 + size_t(h - 1 - y) * row_bytes;
      for (uint32_t x = 0; x < w; ++x) {
        uint32_t sum[3] = {};
        for (uint32_t sy = 0; sy < shrink; ++sy) {
          const uint8_t* in = image.data.data() + size_t(y * shrink + sy) * image.stride + size_t(x * shrink) * 4;
          for (uint32_t sx = 0; sx < shrink; ++sx, in += 4) {
            sum[0] += in[0];
            sum[1] += in[1];
            sum[2] += in[2];
          }
        }
        const uint32_t count = shrink * shrink;
        out[x * 3 + 0] = uint8_t(sum[2] / count);  // (BMP: blue, green, red)
        out[x * 3 + 1] = uint8_t(sum[1] / count);
        out[x * 3 + 2] = uint8_t(sum[0] / count);
      }
    }
    char name[32];
    std::snprintf(name, sizeof(name), "capture_%03u.bmp", n % 100);
    const std::filesystem::path path = folder / name;
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(bmp.data(), 1, bmp.size(), f);
      std::fclose(f);
      REXLOG_INFO("ps5 capture: frame {} ({}x{}) in logs/{}", frame, image.width, image.height, name);
    } else {
      REXLOG_WARN("ps5 capture: can't write {}", path.string());
    }
  }
};

}  // namespace ds2ps5
