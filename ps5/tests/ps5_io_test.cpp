// Dead Space 2 for PS5 - ds2_ps5_io.h, run on the build machine.
//
//   clang++ -std=c++20 -D_GNU_SOURCE -I thirdparty/rexglue-sdk-ps5/include -I ps5/src \
//       ps5/tests/ps5_io_test.cpp -o /tmp/ps5_io_test && /tmp/ps5_io_test <an empty directory>
//
// Makes a "package" of random bytes, copies scattered and adjoining pieces of
// it out with CopyExtents, through the cache and past it, and compares what
// was written with what should have been. Prints OK or what differed.
#include <cstdio>
#include <cstring>
#include <random>

#include "ds2_ps5_io.h"

namespace {

std::vector<uint8_t> ReadAll(const std::string& path) {
  std::vector<uint8_t> data;
  if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
    uint8_t block[65536];
    for (size_t n; (n = std::fread(block, 1, sizeof(block), f)) > 0;) data.insert(data.end(), block, block + n);
    std::fclose(f);
  }
  return data;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return std::fprintf(stderr, "usage: ps5_io_test <an empty directory>\n"), 2;
  const std::string dir = argv[1];
  std::mt19937_64 random(12345);

  // The package: 40 MB.
  std::vector<uint8_t> package(size_t(40) << 20);
  for (size_t i = 0; i + 8 <= package.size(); i += 8) {
    const uint64_t v = random();
    std::memcpy(package.data() + i, &v, 8);
  }
  const std::string package_path = dir + "/package";
  std::FILE* f = std::fopen(package_path.c_str(), "wb");
  if (!f || std::fwrite(package.data(), 1, package.size(), f) != package.size() || std::fclose(f) != 0) {
    return std::fprintf(stderr, "can't write %s\n", package_path.c_str()), 2;
  }
  rex::filesystem::direct::Reader reader;
  if (!reader.Open(package_path.c_str())) return std::fprintf(stderr, "can't open the package\n"), 2;

  // Files as an STFS package has them: 4 KB blocks, mostly one after the
  // other, with a hash table's worth skipped now and then, a jump back, and a
  // last block of any size. Sizes around the 8 MB chunk and its multiples.
  const uint64_t sizes[] = {0,         1,          4095,         4096,          4097,       100000,
                            (8u << 20) - 1, 8u << 20, (8u << 20) + 1, (16u << 20) + 4096, 30000001};
  int failures = 0;
  for (int round = 0; round < 6; ++round) {
    const bool direct = round % 2 == 1;
    ds2ps5::io::ChunkBuffer buffer;
    for (const uint64_t size : sizes) {
      std::vector<ds2ps5::io::Extent> extents;
      std::vector<uint8_t> expected;
      uint64_t at = 4096 * (random() % 64), left = size;
      while (left) {
        const uint64_t length = std::min<uint64_t>(left, 4096);
        if (random() % 170 == 0) at += 4096 * (1 + random() % 2);  // (a hash table in between)
        if (random() % 2000 == 0) at = 4096 * (random() % 512);    // (a file in pieces)
        if (at + length > package.size()) at = 4096 * (random() % 64);
        extents.push_back({at, length});
        expected.insert(expected.end(), package.begin() + at, package.begin() + at + length);
        at += length, left -= length;
      }
      const std::string out_path = dir + "/out";
      uint64_t stepped = 0;
      const bool ok = ds2ps5::io::CopyExtents(reader, extents, out_path.c_str(), direct, buffer,
                                              [&](uint64_t bytes) { stepped += bytes; });
      const std::vector<uint8_t> got = ReadAll(out_path);
      if (!ok || got != expected || stepped != size) {
        ++failures;
        std::printf("FAILED: %llu bytes, %s: copy %s, %zu bytes written, %llu counted\n",
                    static_cast<unsigned long long>(size), direct ? "direct" : "cached", ok ? "ok" : "failed",
                    got.size(), static_cast<unsigned long long>(stepped));
      }
    }
  }
  std::printf(failures ? "%d FAILED\n" : "OK\n", failures);
  return failures ? 1 : 0;
}
