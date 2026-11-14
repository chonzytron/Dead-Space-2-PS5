// Dead Space 2 for PS5 - copying a file out of a package, past the file cache.
//
// The first version unpacked DLC with fread and fwrite, 4 KB of the package at
// a time. On the PS5 everything read or written that way stays in the file
// cache, and with an app running the console has little memory to spare: after
// a couple of GB every read and write waits on the console freeing some (the
// GTA IV port measured 85 MB/s falling to a few; rex/filesystem/direct_io.h).
// Unpacking Severed is 1 GB read and 1 GB written, and the first run on a
// console was still at it after 70 s.
//
// So, as that port's installer does: the package is read past the cache, in
// runs of blocks that follow one another in it, and the file is written past
// the cache from an aligned buffer, 8 MB at a time, and flushed as it goes.
//
// No game types in here: ps5/tests/ps5_io_test.cpp runs it on Linux.
#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <rex/filesystem/direct_io.h>

namespace ds2ps5::io {

constexpr size_t kChunk = size_t(8) << 20;        // what goes out in one write
constexpr uint64_t kFlushEvery = uint64_t(16) << 20;

// Memory aligned for direct transfers, kChunk of it.
class ChunkBuffer {
 public:
  ChunkBuffer() : data_(rex::filesystem::direct::AlignedAlloc(kChunk)) {}
  ChunkBuffer(const ChunkBuffer&) = delete;
  ChunkBuffer& operator=(const ChunkBuffer&) = delete;
  ~ChunkBuffer() { std::free(data_); }
  uint8_t* data() const { return data_; }

 private:
  uint8_t* data_;
};

// A new file written from start to end, a chunk at a time.
class OutFile {
 public:
  OutFile() = default;
  OutFile(const OutFile&) = delete;
  OutFile& operator=(const OutFile&) = delete;
  ~OutFile() {
    if (fd_ >= 0) ::close(fd_);
  }

  // `direct`: past the file cache, where the drive takes that.
  bool Open(const char* path, bool direct) {
    if (direct) {
      fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0666);
      direct_ = fd_ >= 0;
    }
    if (fd_ < 0) fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    flush_ = direct;
    return fd_ >= 0;
  }

  // Every chunk but the last is kChunk bytes, from a ChunkBuffer.
  bool Write(const uint8_t* p, size_t length) {
    while (length) {
      const ssize_t n = ::write(fd_, p, length);
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && errno == EINVAL && direct_) {
        // The kernel won't write this directly (the last piece's size, or a
        // drive that takes no direct writes): through the cache from here.
        rex::filesystem::direct::TurnOff(fd_);
        direct_ = false;
        continue;
      }
      if (n <= 0) return false;
      p += n, length -= size_t(n);
      unflushed_ += uint64_t(n);
    }
    if (flush_ && unflushed_ >= kFlushEvery) {
      ::fsync(fd_);
      unflushed_ = 0;
    }
    return true;
  }

  bool Close() {
    if (fd_ < 0) return false;
    if (flush_ && unflushed_) ::fsync(fd_);
    const bool ok = ::close(fd_) == 0;
    fd_ = -1;
    return ok;
  }

 private:
  int fd_ = -1;
  bool direct_ = false, flush_ = false;
  uint64_t unflushed_ = 0;
};

// Where a piece of a file is in the package.
struct Extent {
  uint64_t offset;
  uint64_t length;
};

// `extents` of `package`, one after the other, into a new file at `path`.
// `step(bytes)` is called after each chunk with what it added. False (with a
// partly written file left at `path`) when a read or write fails.
inline bool CopyExtents(rex::filesystem::direct::Reader& package, const std::vector<Extent>& extents,
                        const char* path, bool direct, ChunkBuffer& buffer,
                        const std::function<void(uint64_t)>& step) {
  if (!buffer.data()) return false;
  OutFile out;
  if (!out.Open(path, direct)) return false;
  size_t pending = 0;
  const auto flush = [&] {
    if (!pending) return true;
    if (!out.Write(buffer.data(), pending)) return false;
    if (step) step(pending);
    pending = 0;
    return true;
  };
  for (size_t i = 0; i < extents.size();) {
    // Blocks that follow one another in the package are read as one piece.
    uint64_t offset = extents[i].offset, length = extents[i].length;
    for (++i; i < extents.size() && extents[i].offset == offset + length; ++i) length += extents[i].length;
    while (length) {
      const size_t take = size_t(std::min<uint64_t>(length, kChunk - pending));
      if (!package.ReadAt(offset, buffer.data() + pending, take)) return false;
      pending += take, offset += take, length -= take;
      if (pending == kChunk && !flush()) return false;
    }
  }
  return flush() && out.Close();
}

}  // namespace ds2ps5::io
