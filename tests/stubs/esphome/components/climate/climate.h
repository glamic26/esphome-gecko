#pragma once
#include <cmath>
#include <cstdint>
#include <set>
#include <vector>
#include "esphome/core/component.h"

namespace esphome {
namespace climate {

enum ClimateMode : uint8_t { CLIMATE_MODE_OFF = 0, CLIMATE_MODE_HEAT_COOL, CLIMATE_MODE_COOL, CLIMATE_MODE_HEAT };
enum ClimateAction : uint8_t {
  CLIMATE_ACTION_OFF = 0,
  CLIMATE_ACTION_COOLING = 2,
  CLIMATE_ACTION_HEATING = 3,
  CLIMATE_ACTION_IDLE = 4,
};
enum ClimateFeature : uint32_t {
  CLIMATE_SUPPORTS_CURRENT_TEMPERATURE = 1 << 0,
  CLIMATE_SUPPORTS_ACTION = 1 << 2,
};

class ClimateTraits {
 public:
  void set_supported_modes(std::set<ClimateMode> m) { modes = m; }
  void add_feature_flags(uint32_t f) { flags |= f; }
  void set_visual_min_temperature(float v) { min_t = v; }
  void set_visual_max_temperature(float v) { max_t = v; }
  void set_visual_temperature_step(float v) { step = v; }
  void set_visual_current_temperature_step(float v) { cur_step = v; }
  std::set<ClimateMode> modes;
  uint32_t flags{0};
  float min_t{0}, max_t{0}, step{0}, cur_step{0};
};

class ClimateCall {
 public:
  ClimateCall &set_target_temperature(float t) {
    target_ = t;
    return *this;
  }
  const optional<float> &get_target_temperature() const { return target_; }

 private:
  optional<float> target_;
};

struct ClimateDeviceRestoreState {
  float target_temperature;
};

struct ClimateSnapshot {
  ClimateMode mode;
  ClimateAction action;
  float target;
  float current;
};

class Climate : public EntityBase {
 public:
  virtual ~Climate() = default;
  virtual ClimateTraits traits() = 0;
  virtual void control(const ClimateCall &call) = 0;

  void publish_state() { published.push_back({mode, action, target_temperature, current_temperature}); }

  ClimateMode mode{CLIMATE_MODE_OFF};
  ClimateAction action{CLIMATE_ACTION_OFF};
  float target_temperature{NAN};
  float current_temperature{NAN};
  std::vector<ClimateSnapshot> published;

  // Test hook: what restore_state_() returns.
  optional<ClimateDeviceRestoreState> restore_value;

 protected:
  optional<ClimateDeviceRestoreState> restore_state_() { return restore_value; }
};

}  // namespace climate
}  // namespace esphome
