#pragma once
#include <cstring>
#include <vector>
#include "esphome/core/stub_runtime.h"

namespace esphome {

// In-memory equivalent of ESPHome's preference object: values survive a
// simulated reboot (a new component instance) because the store is global.
class ESPPreferenceObject {
 public:
  ESPPreferenceObject() = default;
  explicit ESPPreferenceObject(uint32_t key) : key_(key), valid_(true) {}

  template<typename T> bool save(const T *src) {
    if (!valid_)
      return false;
    auto &slot = stub::pref_store()[key_];
    slot.assign(reinterpret_cast<const uint8_t *>(src), reinterpret_cast<const uint8_t *>(src) + sizeof(T));
    stub::pref_save_count()++;
    return true;
  }
  template<typename T> bool load(T *dest) {
    if (!valid_)
      return false;
    auto it = stub::pref_store().find(key_);
    if (it == stub::pref_store().end() || it->second.size() != sizeof(T))
      return false;
    std::memcpy(dest, it->second.data(), sizeof(T));
    return true;
  }

 private:
  uint32_t key_{0};
  bool valid_{false};
};

class ESPPreferences {
 public:
  template<typename T> ESPPreferenceObject make_preference(uint32_t type, bool in_flash = false) {
    (void) in_flash;
    return ESPPreferenceObject(type);
  }
};

inline ESPPreferences *global_preferences = new ESPPreferences();

}  // namespace esphome
