// Dead Space 2 for PS5 - the Linux test build (DS2_PS5_LINUX): what the
// console's system libraries answer to the two calls the app makes itself.
#include <cstddef>
#include <cstdio>
#include <cstring>

// The system language: none, so ps5_language = auto is English.
extern "C" int sceSystemServiceParamGetInt(int, int*) { return -1; }

// A notification on the TV: on a PC, a line on the terminal. The request is
// the console's layout (45 bytes, then the text).
extern "C" int sceKernelSendNotificationRequest(int, void* request, size_t size, int) {
  if (request && size > 45) {
    const char* text = static_cast<const char*>(request) + 45;
    std::fprintf(stderr, "[notification] %.*s\n", int(strnlen(text, size - 45)), text);
  }
  return 0;
}
