#pragma once
// FORK: protocol helpers with no ESPHome dependencies, so they can be unit
// tested on the host (see tests/). See docs/fork/DECISIONS.md.

#include <cstdint>

namespace esphome {
namespace gecko_spa {
namespace protocol {

// Raw I2C frame header length; payloads of multi-part messages start here.
static const uint8_t FRAME_HEADER_LEN = 16;

// Byte 6 of a frame: 0x0A for config/status replies, 0x0B for messages the
// spa sends on its own (program, reminders, program schedule data).
static const uint8_t FRAME_CHANNEL_BYTE = 6;
static const uint8_t CHANNEL_SPA_INITIATED = 0x0B;

// True for a multi-part frame that belongs to the spa-initiated stream. These
// frames must not be appended to the config/status reassembly buffer: the spa
// interleaves them with config/status parts (for example the program schedule
// burst sent after a program change), and that burst has no final part.
inline bool is_spa_initiated_part(const uint8_t *data, uint8_t len) {
  return len >= FRAME_HEADER_LEN && data[1] == 0x09 && data[FRAME_CHANNEL_BYTE] == CHANNEL_SPA_INITIATED;
}

// Known combined config+status message layouts: total payload length and the
// length of the status block at its end.
struct CombinedLayout {
  uint16_t total_len;
  uint16_t status_len;
};
static const CombinedLayout COMBINED_LAYOUTS[] = {
    {390, 162},  // upstream reference pack
    {353, 145},  // inYT config/status version 65
};

// Looks up a combined layout by total length. Returns true and fills
// `status_offset`/`status_len` if the length is known.
inline bool find_embedded_status(uint16_t total_len, uint16_t &status_offset, uint16_t &status_len) {
  for (const auto &l : COMBINED_LAYOUTS) {
    if (l.total_len == total_len) {
      status_offset = static_cast<uint16_t>(l.total_len - l.status_len);
      status_len = l.status_len;
      return true;
    }
  }
  return false;
}

// Temperatures are sent as big-endian words in 1/18 °C.
inline float raw_to_celsius(uint16_t raw) { return raw / 18.0f; }

// A set point the spa can hold.
inline bool set_point_plausible(uint16_t raw) {
  float c = raw_to_celsius(raw);
  return raw != 0 && c >= 8.0f && c <= 40.0f;
}

// A water temperature reading. The spa reports 0 for some minutes after it is
// powered up, until its sensor reading is valid.
inline bool water_temp_plausible(uint16_t raw) {
  float c = raw_to_celsius(raw);
  return raw != 0 && c >= 0.0f && c <= 45.0f;
}

static const uint8_t NUM_PROGRAMS = 5;
inline const char *program_name(uint8_t id) {
  static const char *const NAMES[NUM_PROGRAMS] = {"Away", "Standard", "Energy", "Super Energy", "Weekend"};
  return id < NUM_PROGRAMS ? NAMES[id] : nullptr;
}

}  // namespace protocol
}  // namespace gecko_spa
}  // namespace esphome
