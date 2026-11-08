#pragma once
#include <algorithm>
#include <string>
#include "esphome/core/stub_runtime.h"

namespace esphome {

// Same algorithm as ESPHome's fnv1_hash (FNV-1, 32 bit).
inline uint32_t fnv1_hash(const std::string &str) {
  uint32_t hash = 2166136261UL;
  for (char c : str) {
    hash *= 16777619UL;
    hash ^= static_cast<uint8_t>(c);
  }
  return hash;
}

}  // namespace esphome
