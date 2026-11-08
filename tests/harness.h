#pragma once
// Replay harness: builds the real gecko_spa component against the stubs in
// tests/stubs and feeds it proxy lines from fixture files.

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../components/gecko_spa/gecko_spa.h"

namespace test {

using namespace esphome;
using namespace esphome::gecko_spa;

// ---- tiny test runner -------------------------------------------------------
struct Registry {
  std::vector<std::pair<std::string, std::function<void()>>> tests;
  static Registry &get() {
    static Registry r;
    return r;
  }
};
inline int &failures() {
  static int f = 0;
  return f;
}
struct Register {
  Register(const char *name, std::function<void()> fn) { Registry::get().tests.emplace_back(name, fn); }
};
#define TEST(name) \
  static void name(); \
  static ::test::Register reg_##name(#name, name); \
  static void name()
#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ::test::failures()++; \
    } \
  } while (0)
#define CHECK_NEAR(a, b, tol) CHECK(std::fabs((a) - (b)) <= (tol))

// ---- fixture loading --------------------------------------------------------
inline std::vector<std::string> load_fixture(const std::string &name) {
  std::vector<std::string> lines;
  std::ifstream in(std::string(FIXTURE_DIR) + "/" + name);
  if (!in) {
    std::printf("    cannot open fixture %s\n", name.c_str());
    failures()++;
    return lines;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("RX:", 0) == 0)
      lines.push_back(line);
  }
  return lines;
}

// ---- a spa controller with all entities attached ----------------------------
struct RigOptions {
  bool clear_flash{true};  // false = simulated ESP restart (flash kept)
  bool report_cool_mode{true};
  bool log_status_changes{false};
};

struct Rig {
  GeckoSpa spa;
  std::unique_ptr<GeckoSpaClimate> climate;
  GeckoSpaSelect program;
  switch_::Switch *light{nullptr};
  binary_sensor::BinarySensor connected, standby, waterfall, blower;
  binary_sensor::BinarySensor heater, circulation, pump1, pump2;
  text_sensor::TextSensor pack_type, cfg_ver, st_ver, spa_time;
  sensor::Sensor pump_timer, set_point;
  GPIOPin reset_pin;
  GeckoSpaSwitch light_sw;

  explicit Rig(RigOptions opt = {}) {
    if (opt.clear_flash) {
      stub::pref_store().clear();
      stub::pref_save_count() = 0;
    }
    stub::log_lines().clear();
    stub::now_ms() = 0;
    climate = std::make_unique<GeckoSpaClimate>(&spa);
    climate->set_report_cool_mode(opt.report_cool_mode);
    spa.set_log_status_changes(opt.log_status_changes);
    spa.set_heater_sensor(&heater);
    spa.set_circulation_sensor(&circulation);
    spa.set_pump1_sensor(&pump1);
    spa.set_pump2_sensor(&pump2);
    spa.set_set_point_sensor(&set_point);
    spa.set_climate(climate.get());
    spa.set_program_select(&program);
    program.set_parent(&spa);
    light_sw.set_parent(&spa);
    light_sw.set_switch_type("light");
    spa.set_light_switch(&light_sw);
    spa.set_connected_sensor(&connected);
    spa.set_standby_sensor(&standby);
    spa.set_waterfall_sensor(&waterfall);
    spa.set_blower_sensor(&blower);
    spa.set_pack_type_sensor(&pack_type);
    spa.set_config_version_sensor(&cfg_ver);
    spa.set_status_version_sensor(&st_ver);
    spa.set_spa_time_sensor(&spa_time);
    spa.set_pump_timer_sensor(&pump_timer);
    spa.set_reset_pin(&reset_pin);
    spa.setup();
    climate->setup();
    program.setup();
  }

  void feed(const std::string &line, uint32_t gap_ms = 20) {
    stub::now_ms() += gap_ms;
    for (char c : line)
      spa.rx.push_back(c);
    spa.rx.push_back('\n');
    spa.loop();
  }

  // Replays frames [from, to). Calls `after` after each frame if given.
  void replay(const std::vector<std::string> &frames, size_t from = 0, size_t to = SIZE_MAX,
              const std::function<void(size_t)> &after = nullptr) {
    for (size_t i = from; i < frames.size() && i < to; i++) {
      feed(frames[i]);
      if (after)
        after(i);
    }
  }

  size_t count_log(const char *needle, char level = 0) const {
    size_t n = 0;
    for (auto &l : stub::log_lines())
      if ((level == 0 || l.level == level) && l.text.find(needle) != std::string::npos)
        n++;
    return n;
  }
};

}  // namespace test
