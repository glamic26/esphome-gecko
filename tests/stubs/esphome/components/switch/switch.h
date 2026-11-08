#pragma once
#include <vector>
#include "esphome/core/component.h"

namespace esphome {
namespace switch_ {

class Switch : public EntityBase {
 public:
  virtual ~Switch() = default;
  void publish_state(bool s) {
    state = s;
    published.push_back(s);
  }
  bool state{false};
  std::vector<bool> published;

 protected:
  virtual void write_state(bool state) = 0;

 public:
  void turn(bool s) { write_state(s); }
};

}  // namespace switch_
}  // namespace esphome
