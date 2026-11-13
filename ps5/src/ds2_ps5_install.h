// Dead Space 2 for PS5 - getting the game ready from the player's own files.
//
// The PC build is prepared by tools/build.py and the launcher (default.xex out
// of the Disc 1 image, default.xexp out of Title Update #2, the DLC package
// unpacked). A console has neither, so the app does the same at start-up, from
// whatever the player put in its install folder. Nothing is downloaded and no
// game data is part of the app.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace ds2ps5 {

struct GameFiles {
  // By disc number - 1. An image that is there and is that disc of Dead Space 2.
  std::filesystem::path iso[2];
  // game/disc<N>: default.xex and default.xexp, both checked against the hashes
  // the recompiled code was made from. Empty when that disc can't be started.
  std::filesystem::path game_dir[2];
  // What was found and done, for the log.
  std::vector<std::string> notes;
  // Why the game can't start (empty when it can), in words for the screen.
  std::string problem;

  // The disc to start from (1 or 2), 0 when neither is ready.
  int boot_disc() const { return !game_dir[0].empty() ? 1 : !game_dir[1].empty() ? 2 : 0; }
};

// How far PrepareGameFiles is, for the screen shown meanwhile (it runs on a
// thread of its own: unpacking a DLC package takes minutes).
struct Progress {
  // Of the DLC being unpacked (0 of 0 before and after).
  std::atomic<uint64_t> done_bytes{0}, total_bytes{0};
  // Something long is starting: said to the player some other way than the
  // screen as well (a PS5 notification), should the screen not be showing.
  std::function<void(const std::string&)> announce;
  // How many times the screen was drawn (for the log).
  std::atomic<uint64_t> draws{0};

  void SetStep(std::string step) {
    std::lock_guard lock(mutex_);
    step_ = std::move(step);
  }
  std::string step() {
    std::lock_guard lock(mutex_);
    return step_;
  }

 private:
  std::mutex mutex_;
  std::string step_;
};

// Looks through <app>/install (and <app> itself) for disc images, Title Update
// #2 and DLC packages, stages what is missing under <app>/game, and unpacks DLC
// under <content_root> where the runtime's content manager looks for it.
// A DLC package that was unpacked whole stays so; one that was interrupted is
// gone on with at the next start, from the first file that isn't there yet.
GameFiles PrepareGameFiles(const std::filesystem::path& app_root,
                           const std::filesystem::path& content_root, Progress* progress = nullptr);

}  // namespace ds2ps5
