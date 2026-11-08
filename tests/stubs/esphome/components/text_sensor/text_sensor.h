#pragma once
#include <string>
#include <vector>
#include "esphome/core/component.h"

namespace esphome {
namespace text_sensor {

class TextSensor : public EntityBase {
 public:
  void publish_state(const std::string &s) {
    state = s;
    published.push_back(s);
  }
  std::string state;
  std::vector<std::string> published;
};

}  // namespace text_sensor
}  // namespace esphome
