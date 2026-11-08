#pragma once
#include <string>
#include <vector>
#include "esphome/core/component.h"

namespace esphome {
namespace select {

class Select : public EntityBase {
 public:
  virtual ~Select() = default;
  void publish_state(const std::string &s) {
    state = s;
    has_state_ = true;
    published.push_back(s);
  }
  void publish_state(const char *s) { publish_state(std::string(s)); }
  bool has_state() const { return has_state_; }
  std::string state;
  std::vector<std::string> published;

  void call_control(const std::string &v) { control(v); }

 protected:
  virtual void control(const std::string &value) = 0;
  bool has_state_{false};
};

}  // namespace select
}  // namespace esphome
