#pragma once
#include <cmath>
#include <vector>
#include "esphome/core/component.h"

namespace esphome {
namespace sensor {

class Sensor : public EntityBase {
 public:
  void publish_state(float s) {
    state = s;
    published.push_back(s);
  }
  float state{NAN};
  std::vector<float> published;
};

}  // namespace sensor
}  // namespace esphome
