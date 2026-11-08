#pragma once
#include <string>
#include "esphome/core/stub_runtime.h"

namespace esphome {

namespace setup_priority {
static const float DATA = 600.0f;
}

class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual float get_setup_priority() const { return 0.0f; }
};

// Base for entities: carries the object id hash used as a preference key.
class EntityBase {
 public:
  void set_object_id_hash(uint32_t h) { object_id_hash_ = h; }
  uint32_t get_object_id_hash() const { return object_id_hash_; }

 protected:
  uint32_t object_id_hash_{0x12345678};
};

}  // namespace esphome
