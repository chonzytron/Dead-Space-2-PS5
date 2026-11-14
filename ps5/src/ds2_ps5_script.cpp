// Dead Space 2 for PS5 - a controller that presses buttons by itself
// (ps5_input_script), for tests.
//
// A renderer change is checked by playing to the same place with each setting
// and comparing the pictures (ps5_capture). Without someone holding the
// controller, this does the pressing: through the menus, into a save, a few
// steps forward. It goes into what the game reads for the first player
// (XInputGetStateEx, as the Xbox 360 SDK gives it to the game), on top of a
// real controller if there is one, and the game is told a controller is
// there.
//
// ps5_input_script = "<seconds>:<buttons>[/<ms held>] ...", times counted from
// the game's first look at the controller, presses held 150 ms unless told:
//   ps5_input_script = "20:start 24:a 28:a 32:a 60:lsup/3000"
// Buttons: a b x y start back up down left right lb rb lt rt ls rs, and
// lsup lsdown lsleft lsright rsup rsdown rsleft rsright (a stick pushed all
// the way). Several at once with +: "40:lt+rt/1000". Each press is logged.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ps5_diag.h>

REXCVAR_DEFINE_STRING(ps5_input_script, "", "PS5",
                      "Buttons pressed by themselves, for tests: \"<seconds>:<buttons>[/<ms held>] ...\" "
                      "(ps5/src/ds2_ps5_script.cpp)");

namespace {

struct Press {
  uint64_t at_ms = 0, until_ms = 0;
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  std::string text;
  bool logged = false;
};

bool AddButton(Press& press, const std::string& name) {
  struct Button {
    const char* name;
    uint16_t bit;
  };
  static constexpr Button kButtons[] = {
      {"up", 0x0001},    {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008}, {"start", 0x0010},
      {"back", 0x0020},  {"ls", 0x0040},   {"rs", 0x0080},   {"lb", 0x0100},    {"rb", 0x0200},
      {"a", 0x1000},     {"b", 0x2000},    {"x", 0x4000},    {"y", 0x8000},
  };
  for (const Button& b : kButtons) {
    if (name == b.name) {
      press.buttons |= b.bit;
      return true;
    }
  }
  constexpr int16_t kFull = 32767;
  if (name == "lt") press.left_trigger = 255;
  else if (name == "rt") press.right_trigger = 255;
  else if (name == "lsup") press.ly = kFull;
  else if (name == "lsdown") press.ly = -kFull;
  else if (name == "lsleft") press.lx = -kFull;
  else if (name == "lsright") press.lx = kFull;
  else if (name == "rsup") press.ry = kFull;
  else if (name == "rsdown") press.ry = -kFull;
  else if (name == "rsleft") press.rx = -kFull;
  else if (name == "rsright") press.rx = kFull;
  else return false;
  return true;
}

std::vector<Press> Parse(const std::string& script) {
  std::vector<Press> presses;
  std::string item;
  std::istringstream in(script);
  while (in >> item) {
    if (!item.empty() && item.back() == ',') item.pop_back();
    const size_t colon = item.find(':');
    if (colon == std::string::npos) {
      REXLOG_WARN("ps5 input script: '{}' has no time, left out", item);
      continue;
    }
    Press press;
    press.text = item;
    std::string what = item.substr(colon + 1);
    uint64_t held_ms = 150;
    if (const size_t slash = what.find('/'); slash != std::string::npos) {
      held_ms = std::strtoull(what.c_str() + slash + 1, nullptr, 10);
      what.resize(slash);
    }
    press.at_ms = uint64_t(std::strtod(item.c_str(), nullptr) * 1000.0);
    press.until_ms = press.at_ms + held_ms;
    bool ok = true;
    size_t start = 0;
    while (start <= what.size()) {
      const size_t plus = what.find('+', start);
      const std::string name = what.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
      if (!AddButton(press, name)) {
        REXLOG_WARN("ps5 input script: no button '{}' in '{}', left out", name, item);
        ok = false;
      }
      if (plus == std::string::npos) break;
      start = plus + 1;
    }
    if (ok) presses.push_back(press);
  }
  return presses;
}

struct Script {
  std::vector<Press> presses;
  uint64_t start_ms = 0;
  uint32_t packet = 0;
  uint16_t last_buttons = 0xFFFF;
};

Script* GetScript() {
  static Script* script = [] {
    const std::string text = REXCVAR_GET(ps5_input_script);
    if (text.empty()) return static_cast<Script*>(nullptr);
    auto* s = new Script;
    s->presses = Parse(text);
    REXLOG_INFO("ps5 input script: {} presses: {}", s->presses.size(), text);
    return s;
  }();
  return script;
}

void Store16(uint8_t* base, uint32_t address, uint16_t v) {
  v = __builtin_bswap16(v);
  std::memcpy(base + address, &v, 2);
}
uint16_t Load16(const uint8_t* base, uint32_t address) {
  uint16_t v;
  std::memcpy(&v, base + address, 2);
  return __builtin_bswap16(v);
}
void Store32(uint8_t* base, uint32_t address, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + address, &v, 4);
}

int16_t Stick(int16_t real, int16_t scripted) { return scripted ? scripted : real; }

}  // namespace

// XInputGetStateEx(user r3, flags r4, XINPUT_STATE* r5) -> r3 (0: connected).
// XINPUT_STATE: packet number (4 bytes), then buttons (2), the triggers (1, 1)
// and the sticks (2 each: LX LY RX RY), big-endian.
extern "C" void __imp__XInputGetStateEx_82A36098(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void XInputGetStateEx_82A36098(PPCContext& __restrict ctx, uint8_t* base) {
  Script* script = GetScript();
  const uint32_t user = ctx.r3.u32, state = ctx.r5.u32;
  __imp__XInputGetStateEx_82A36098(ctx, base);
  if (!script || (user & 0xFF) != 0 || !state) return;
  const uint64_t now = rex::ps5diag::NowMs();
  if (!script->start_ms) script->start_ms = now;
  const uint64_t t = now - script->start_ms;
  const bool connected = ctx.r3.u32 == 0;
  if (!connected) std::memset(base + state, 0, 16);
  uint16_t buttons = Load16(base, state + 4);
  uint8_t lt = base[state + 6], rt = base[state + 7];
  int16_t lx = int16_t(Load16(base, state + 8)), ly = int16_t(Load16(base, state + 10));
  int16_t rx = int16_t(Load16(base, state + 12)), ry = int16_t(Load16(base, state + 14));
  for (Press& p : script->presses) {
    if (t < p.at_ms || t >= p.until_ms) continue;
    if (!p.logged) {
      p.logged = true;
      REXLOG_INFO("ps5 input script: {:.1f} s: {} (frame {})", double(t) / 1000.0, p.text,
                  rex::ps5diag::Get(rex::ps5diag::kSwaps));
    }
    buttons |= p.buttons;
    lt = std::max(lt, p.left_trigger);
    rt = std::max(rt, p.right_trigger);
    lx = Stick(lx, p.lx);
    ly = Stick(ly, p.ly);
    rx = Stick(rx, p.rx);
    ry = Stick(ry, p.ry);
  }
  Store16(base, state + 4, buttons);
  base[state + 6] = lt;
  base[state + 7] = rt;
  Store16(base, state + 8, uint16_t(lx));
  Store16(base, state + 10, uint16_t(ly));
  Store16(base, state + 12, uint16_t(rx));
  Store16(base, state + 14, uint16_t(ry));
  if (!connected) {
    // (no controller: this one; its packet number moves when its state does)
    if (buttons != script->last_buttons || lx || ly || rx || ry || lt || rt) ++script->packet;
    Store32(base, state, script->packet);
    ctx.r3.u64 = 0;
  }
  script->last_buttons = buttons;
}
