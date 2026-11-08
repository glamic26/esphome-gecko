// Replay and unit tests for the gecko_spa component.
// Build and run: tests/run.sh
//
// Fixtures in tests/fixtures are recorded bus traffic (inYT, config/status
// version 65), sanitised with tools/sanitise_capture.py. Expected values in
// these tests were decoded from the fixtures independently of the component.

#include "harness.h"
#include "../components/gecko_spa/gecko_spa.cpp"

using namespace test;
namespace p = esphome::gecko_spa::protocol;

static const float SET_POINT = 37.5f;      // raw 0x02A3
static const float ECONOMY_SET = 26.39f;   // raw 0x01DB, effective set point in economy programs

static bool any_current_below(const climate::Climate &c, float limit) {
  for (auto &s : c.published)
    if (std::isfinite(s.current) && s.current < limit)
      return true;
  return false;
}

// ---- protocol helpers -------------------------------------------------------

TEST(embedded_layouts_are_known) {
  uint16_t off = 0, len = 0;
  CHECK(p::find_embedded_status(353, off, len));
  CHECK(off == 208 && len == 145);
  CHECK(p::find_embedded_status(390, off, len));
  CHECK(off == 228 && len == 162);
  CHECK(!p::find_embedded_status(145, off, len));
  CHECK(!p::find_embedded_status(208, off, len));
}

TEST(spa_initiated_parts_are_recognised) {
  const uint8_t status_part[16] = {0x17, 0x09, 0, 0, 0, 0x17, 0x0A, 0x01, 0, 0x01, 0, 0, 0x40, 0xC7, 0x41, 0x41};
  const uint8_t schedule_part[16] = {0x17, 0x09, 0, 0, 0, 0x17, 0x0B, 0x00, 0, 0x01, 0, 0, 0x40, 0x4E, 0x00, 0x00};
  CHECK(!p::is_spa_initiated_part(status_part, 78));
  CHECK(p::is_spa_initiated_part(schedule_part, 78));
  CHECK(!p::is_spa_initiated_part(schedule_part, 15));  // short frames are handled elsewhere
}

TEST(temperature_plausibility) {
  CHECK(p::set_point_plausible(0x02A3));
  CHECK(!p::set_point_plausible(0));
  CHECK(p::water_temp_plausible(0x029A));
  CHECK(!p::water_temp_plausible(0));  // reported just after power-up
  CHECK(!p::water_temp_plausible(1000));
}

// ---- C1: status from the combined config+status message --------------------

TEST(restart_temperatures_from_first_combined_message) {
  Rig r;
  auto fr = load_fixture("restart_steady.rx");
  // Frames 0-9 hold the first combined message (frame 9 is its last part);
  // the first standalone status block arrives at frame 28.
  r.replay(fr, 0, 10);
  CHECK(!r.climate->published.empty());
  CHECK_NEAR(r.climate->target_temperature, SET_POINT, 0.05f);
  CHECK_NEAR(r.climate->current_temperature, SET_POINT, 0.05f);
  CHECK(r.count_log("Status from config fallback") == 1);
  CHECK(r.count_log("Embedded status rejected") == 0);
}

TEST(combined_message_every_handshake) {
  Rig r;
  auto fr = load_fixture("restart_steady.rx");
  r.replay(fr);
  // 13 combined messages in this capture, all accepted.
  CHECK(r.count_log("Status from config fallback") == 13);
  CHECK(r.count_log("Embedded status rejected") == 0);
}

// ---- C2: program reported by the spa is saved and restored ------------------

TEST(program_changes_are_published_and_saved) {
  Rig r;
  auto fr = load_fixture("program_changes.rx");
  r.replay(fr);
  const std::vector<std::string> expected = {"Super Energy", "Weekend", "Away", "Standard"};
  CHECK(r.program.published == expected);
  CHECK(stub::pref_save_count() == 4);
}

TEST(program_restored_after_esp_restart) {
  {
    Rig r;
    auto fr = load_fixture("program_changes.rx");
    r.replay(fr, 0, 43);  // up to and including the Weekend report (frame 42)
    CHECK(r.program.state == "Weekend");
  }
  RigOptions keep;
  keep.clear_flash = false;
  Rig after(keep);
  CHECK(after.program.has_state());
  CHECK(after.program.state == "Weekend");
}

TEST(first_report_of_default_program_is_saved) {
  // Nothing in flash yet; the spa reports Standard, which is also the default
  // index. It must still be saved so it can be restored.
  {
    Rig r;
    auto fr = load_fixture("power_up.rx");
    r.replay(fr);
    CHECK(r.program.state == "Standard");
    CHECK(stub::pref_save_count() == 1);
  }
  RigOptions keep;
  keep.clear_flash = false;
  Rig after(keep);
  CHECK(after.program.state == "Standard");
}

TEST(same_program_reported_again_does_not_write_flash) {
  Rig r;
  auto fr = load_fixture("power_up.rx");
  r.replay(fr);
  int saves = stub::pref_save_count();
  // Replaying the same capture reports Standard again.
  r.replay(fr);
  CHECK(stub::pref_save_count() == saves);
}

// ---- C3: read-only output sensors ------------------------------------------

TEST(pump_sensors_follow_panel_presses) {
  Rig r;
  auto fr = load_fixture("restart_steady.rx");
  r.replay(fr);
  // Pump 1 on at frame 28, off at 31; pump 2 on at 58, off at 73.
  const std::vector<bool> p1 = {false, true, false};
  CHECK(r.pump1.published == p1);
  CHECK(r.pump2.published == p1);
  CHECK(!r.heater.state);
}

TEST(heater_and_circulation_after_power_up) {
  Rig r;
  auto fr = load_fixture("power_up.rx");
  r.replay(fr);
  CHECK(r.heater.has_state && r.heater.state);
  CHECK(r.circulation.has_state && r.circulation.state);
  const std::vector<bool> heater = {false, true};
  CHECK(r.heater.published == heater);
}

// ---- power-up: temperature reads 0 until valid -----------------------------

TEST(zero_temperature_after_power_up_is_not_published) {
  Rig r;
  auto fr = load_fixture("power_up.rx");
  r.replay(fr);
  CHECK(!any_current_below(*r.climate, 1.0f));
  CHECK_NEAR(r.climate->current_temperature, 37.0f, 0.05f);
}

TEST(power_up_with_fresh_bridge) {
  // Bridge and spa start together: nothing known, start-up burst included.
  Rig r;
  auto fr = load_fixture("power_up.rx");
  r.replay(fr, 8);  // skip the combined message sent before the power cut
  CHECK(r.count_log("exceeds buffer") == 0);
  CHECK(r.count_log("Auto-detect 145") == 1);
  CHECK(r.program.state == "Standard");
  CHECK_NEAR(r.climate->current_temperature, 37.0f, 0.05f);
  CHECK(!any_current_below(*r.climate, 1.0f));
}

// ---- C4: cool mode reporting -----------------------------------------------

TEST(economy_set_point_reports_cool_by_default) {
  Rig r;
  auto fr = load_fixture("program_changes.rx");
  r.replay(fr, 0, 14);  // Super Energy: effective set point 26.4, water 37.5
  CHECK_NEAR(r.climate->target_temperature, ECONOMY_SET, 0.05f);
  CHECK(r.climate->mode == climate::CLIMATE_MODE_COOL);
  CHECK(r.climate->action == climate::CLIMATE_ACTION_COOLING);
  CHECK(r.climate->traits().modes.count(climate::CLIMATE_MODE_COOL) == 1);
}

TEST(report_cool_mode_false_keeps_heat_mode) {
  RigOptions o;
  o.report_cool_mode = false;
  Rig r(o);
  auto fr = load_fixture("program_changes.rx");
  r.replay(fr, 0, 14);
  CHECK_NEAR(r.climate->target_temperature, ECONOMY_SET, 0.05f);
  CHECK(r.climate->mode == climate::CLIMATE_MODE_HEAT);
  CHECK(r.climate->action == climate::CLIMATE_ACTION_IDLE);
  CHECK(r.climate->traits().modes.size() == 1);
  for (auto &s : r.climate->published)
    CHECK(s.mode == climate::CLIMATE_MODE_HEAT && s.action != climate::CLIMATE_ACTION_COOLING);
}

TEST(report_cool_mode_false_shows_heating) {
  RigOptions o;
  o.report_cool_mode = false;
  Rig r(o);
  auto fr = load_fixture("power_up.rx");
  r.replay(fr);
  CHECK(r.climate->action == climate::CLIMATE_ACTION_HEATING);
}

// ---- C5: change-only status logging ----------------------------------------

TEST(default_logging_is_unchanged) {
  Rig r;
  auto fr = load_fixture("restart_steady.rx");
  r.replay(fr);
  CHECK(r.count_log("Proxy: RX:", 'D') == fr.size());
  CHECK(r.count_log("FULL-RX:", 'D') > 0);
  CHECK(r.count_log("FULL-RX:", 'V') == 0);
  // 20 standalone + 13 embedded status blocks, each decoded and logged. The
  // first one arrives before the version handshake, so it is logged as v0.
  CHECK(r.count_log("Status[v", 'I') == 33);
  CHECK(r.count_log("Status[v0]", 'I') == 1);
}

TEST(log_status_changes_logs_only_changes) {
  RigOptions o;
  o.log_status_changes = true;
  Rig r(o);
  auto fr = load_fixture("restart_steady.rx");
  r.replay(fr);
  CHECK(r.count_log("Proxy: RX:", 'D') == 0);
  CHECK(r.count_log("Proxy: RX:", 'V') == fr.size());
  CHECK(r.count_log("FULL-RX:", 'D') == 0);
  // First block plus one per change: pump 1 on/off, pump 2 on/off, 16 light changes.
  CHECK(r.count_log("Status[v", 'I') == 21);
  CHECK(r.count_log("Status change", 'I') == 20);
  // Entity updates are not affected by the logging option.
  CHECK(r.light_sw.published.size() == 17);
}

// ---- C7: set point from the config message ---------------------------------

TEST(set_point_sensor_ignores_economy_drop) {
  Rig r;
  auto fr = load_fixture("program_changes.rx");
  r.replay(fr);
  // Climate target followed the economy programs; the set point did not.
  bool saw_economy = false;
  for (auto &s : r.climate->published)
    if (std::fabs(s.target - ECONOMY_SET) < 0.05f)
      saw_economy = true;
  CHECK(saw_economy);
  CHECK(r.set_point.published.size() == 1);
  CHECK_NEAR(r.set_point.state, SET_POINT, 0.05f);
}

// ---- C8: spa-initiated stream kept out of reassembly -----------------------

TEST(program_schedule_burst_does_not_corrupt_reassembly) {
  Rig r;
  auto fr = load_fixture("program_changes.rx");
  r.replay(fr);
  CHECK(r.count_log("exceeds buffer") == 0);
  // Five combined messages complete (frames 21, 59, 116, 131, 143). The one
  // ending at frame 116 has the program schedule burst interleaved with its
  // parts; with a shared buffer it would not reassemble to 353 bytes.
  size_t seen = 0;
  bool at_116 = false;
  Rig r2;
  r2.replay(fr, 0, SIZE_MAX, [&](size_t i) {
    size_t n = r2.count_log("Status from config fallback");
    if (n != seen && i == 116)
      at_116 = true;
    seen = n;
  });
  CHECK(seen == 5);
  CHECK(at_116);
  // The message the spa cut short (frame 91) is ignored, not misread.
  CHECK(r.count_log("Embedded status rejected") == 0);
}

TEST(spa_initiated_parts_logged_at_verbose_only) {
  Rig r;
  auto fr = load_fixture("program_changes.rx");
  r.replay(fr);
  CHECK(r.count_log("Spa-initiated frame", 'V') == 58);
  CHECK(r.count_log("Short msg (") == r.count_log("Short msg (", 'I'));
}

// ---- runner -----------------------------------------------------------------

int main(int argc, char **argv) {
  stub::log_echo() = argc > 1 && std::string(argv[1]) == "-v";
  int run = 0;
  for (auto &t : Registry::get().tests) {
    int before = failures();
    std::printf("%s\n", t.first.c_str());
    t.second();
    run++;
    if (failures() != before)
      std::printf("  -> FAILED\n");
  }
  std::printf("%d tests, %d failed checks\n", run, failures());
  return failures() == 0 ? 0 : 1;
}
