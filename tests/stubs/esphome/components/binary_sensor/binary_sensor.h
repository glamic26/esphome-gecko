#pragma once
#include <vector>
#include "esphome/core/component.h"

namespace esphome {
namespace binary_sensor {

class BinarySensor : public EntityBase {
 public:
  void publish_state(bool s) {
    state = s;
    has_state = true;
    published.push_back(s);
  }
  bool state{false};
  bool has_state{false};
  std::vector<bool> published;
};

}  // namespace binary_sensor
}  // namespace esphome
