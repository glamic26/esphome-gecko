#pragma once
#include <vector>
#include "esphome/core/stub_runtime.h"

namespace esphome {

class GPIOPin {
 public:
  virtual ~GPIOPin() = default;
  virtual void setup() {}
  virtual void digital_write(bool value) {
    level = value;
    writes.push_back(value);
  }
  bool level{true};
  std::vector<bool> writes;
};

}  // namespace esphome
