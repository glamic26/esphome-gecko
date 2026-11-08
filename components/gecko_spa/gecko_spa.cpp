#include "gecko_spa.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include <ctime>
#include <cctype>
#include <cstring>
#include <cmath>

namespace esphome {
namespace gecko_spa {

static const char *const TAG = "gecko_spa";

// GO keep-alive message
const uint8_t GeckoSpa::GO_MESSAGE[15] = {
    0x17, 0x00, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x47, 0x4F  // "GO"
};

void GeckoSpa::setup() {
  ESP_LOGI(TAG, "GeckoSpa starting");
  if (reset_pin_) {
    reset_pin_->setup();
    reset_pin_->digital_write(true);  // RST is active LOW, keep HIGH
  }
  restore_notifications_();
}

void GeckoSpa::restore_notifications_() {
  // Fester Schlüssel, damit der Cache unabhängig von Entity-Namen bleibt
  notification_pref_ = global_preferences->make_preference<NotificationCache>(
      fnv1_hash("gecko_spa_notifications"), true);

  NotificationCache cache{};
  if (!notification_pref_.load(&cache)) {
    ESP_LOGI(TAG, "No cached reminders in flash");
    return;
  }

  text_sensor::TextSensor *sensors[4] = {rinse_filter_sensor_, clean_filter_sensor_,
                                         change_water_sensor_, spa_checkup_sensor_};
  for (int i = 0; i < 4; i++) {
    // Nur gültige Einträge im Format YYYY-MM-DD übernehmen
    cache.dates[i][sizeof(cache.dates[i]) - 1] = '\0';
    const char *d = cache.dates[i];
    if (strlen(d) != 10 || d[4] != '-' || d[7] != '-')
      continue;
    strcpy(notification_date_[i], d);
    ESP_LOGI(TAG, "Restored reminder %d from flash: %s", i + 1, d);
    if (sensors[i])
      sensors[i]->publish_state(d);
  }
}

void GeckoSpa::loop() {
  // Handle non-blocking reset pulse completion (100ms)
  if (reset_in_progress_ && (millis() - reset_start_time_ > 100)) {
    if (reset_pin_) {
      reset_pin_->digital_write(true);  // Release reset (HIGH)
    }
    reset_in_progress_ = false;
    ESP_LOGI(TAG, "Arduino reset complete");
  }

  // Read UART lines from Arduino proxy
  while (available()) {
    char c = read();
    if (c == '\n' || c == '\r') {
      if (uart_pos_ > 0) {
        uart_buffer_[uart_pos_] = '\0';
        process_proxy_message(uart_buffer_);
        uart_pos_ = 0;
      }
    } else if (uart_pos_ < sizeof(uart_buffer_) - 1) {
      uart_buffer_[uart_pos_++] = c;
    }
  }

  // Check connection timeout (1 minute without I2C traffic)
  if (millis() - last_i2c_time_ > 60000) {
    if (connected_) {
      connected_ = false;
      if (connected_sensor_)
        connected_sensor_->publish_state(false);
      ESP_LOGW(TAG, "Spa connection lost (timeout)");
      reset_retry_count_ = 0;
      reset_arduino();
    } else if (!reset_in_progress_) {
      // Not connected and not mid-reset: retry with backoff
      // Retry intervals: 30s, 60s, 120s, then every 120s (max 5 retries before giving up)
      // FORK: no bare min(); it only exists through Arduino.h (DECISIONS.md #9)
      uint32_t backoff = 30000UL * (1u << (reset_retry_count_ < 2 ? reset_retry_count_ : 2));
      if (reset_retry_count_ < 5 && (millis() - reset_start_time_ > backoff)) {
        ESP_LOGW(TAG, "Arduino recovery retry %d/%d (backoff %ds)",
                 reset_retry_count_ + 1, 5, backoff / 1000);
        reset_arduino();
      }
    }
  }

  // Send GO keep-alive every 23 seconds (triggers handshake sequence)
  if (millis() - last_go_send_time_ > 23000) {
    last_go_send_time_ = millis();
    send_i2c_message(GO_MESSAGE, 15);
    ESP_LOGD(TAG, "Sent GO keep-alive");
  }
}

void GeckoSpa::send_light_command(bool on) {
  uint8_t cmd[20] = {
      0x17, 0x0A, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x06, 0x46, config_version_, status_version_,
      0x01, 0x33, (uint8_t)(on ? 0x01 : 0x00), 0x00};
  cmd[19] = calc_checksum(cmd, 20);
  send_i2c_message(cmd, 20);
  ESP_LOGI(TAG, "Sent light %s command", on ? "ON" : "OFF");
}

void GeckoSpa::send_circ_command(bool on) {
  uint8_t cmd[20] = {
      0x17, 0x0A, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x06, 0x46, config_version_, status_version_,
      0x01, 0x6B, (uint8_t)(on ? 0x01 : 0x00), 0x00};
  cmd[19] = calc_checksum(cmd, 20);
  send_i2c_message(cmd, 20);
  ESP_LOGI(TAG, "Sent circ %s command", on ? "ON" : "OFF");
}

void GeckoSpa::send_pump1_command(uint8_t state) {
  // P1 function ID: 0x03
  // State: 0=OFF, 2=ON/HIGH (P1 uses 0x02 for ON, not 0x01)
  uint8_t state_val = (user_demand_state_ & 0xFC) | ((state == 0) ? 0x00 : 0x02);

  uint8_t cmd[20] = {
      0x17, 0x0A, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x06, 0x46, config_version_, status_version_,
      0x01, 0x03, state_val, 0x00};
  cmd[19] = calc_checksum(cmd, 20);
  send_i2c_message(cmd, 20);
  ESP_LOGI(TAG, "Sent P1 state=%d command (val=0x%02X)", state, state_val);
}

void GeckoSpa::send_pump2_command(uint8_t state) {
  // P2 function ID: 0x03
  // State: 0=OFF, 8=ON/HIGH (P2 uses 0x08 for ON, not 0x01)
  uint8_t state_val = (user_demand_state_ & 0xF3) | ((state == 0) ? 0x00 : 0x08);

  uint8_t cmd[20] = {
      0x17, 0x0A, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x06, 0x46, config_version_, status_version_,
      0x01, 0x03, state_val, 0x00};
  cmd[19] = calc_checksum(cmd, 20);
  send_i2c_message(cmd, 20);
  ESP_LOGI(TAG, "Sent P2 state=%d command (val=0x%02X)", state, state_val);
}

void GeckoSpa::send_pump3_command(uint8_t state) {
  // P3 function ID: 0x03
  // State: 0=OFF, 32=ON/HIGH (P3 uses 0x20 for ON, not 0x01)
  uint8_t state_val = (user_demand_state_ & 0xCF) | ((state == 0) ? 0x00 : 0x20);

  uint8_t cmd[20] = {
      0x17, 0x0A, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x06, 0x46, config_version_, status_version_,
      0x01, 0x03, state_val, 0x00};
  cmd[19] = calc_checksum(cmd, 20);
  send_i2c_message(cmd, 20);
  ESP_LOGI(TAG, "Sent P3 state=%d command (val=0x%02X) [EXPERIMENTAL]", state, state_val);
}

void GeckoSpa::send_pump4_command(uint8_t state) {
  // P4 function ID: 0x03
  // State: 0=OFF, 128=ON/HIGH (P4 uses 0x80 for ON, not 0x01)
  uint8_t state_val = (user_demand_state_ & 0x3F) | ((state == 0) ? 0x00 : 0x80);

  uint8_t cmd[20] = {
      0x17, 0x0A, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x06, 0x46, config_version_, status_version_,
      0x01, 0x03, state_val, 0x00};
  cmd[19] = calc_checksum(cmd, 20);
  send_i2c_message(cmd, 20);
  ESP_LOGI(TAG, "Sent P4 state=%d command (val=0x%02X) [EXPERIMENTAL]", state, state_val);
}


void GeckoSpa::send_program_command(uint8_t prog) {
  if (prog > 4)
    return;
  uint8_t cmd[18] = {
      0x17, 0x0B, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x04, 0x4E, 0x03, 0xD0,
      prog, 0x00};
  cmd[17] = calc_checksum(cmd, 18);
  send_i2c_message(cmd, 18);
  ESP_LOGI(TAG, "Sent program %d command", prog);
}

void GeckoSpa::send_temperature_command(float temp_c) {
  if (temp_c < 8.0 || temp_c > 40.0)
    return;
  // Use full 16 bit information
  uint16_t temp_raw = (uint16_t)(temp_c * 18.0);

  // Get high and low byte
  uint8_t high_byte = (uint8_t)((temp_raw >> 8) & 0xFF);
  uint8_t low_byte = (uint8_t)(temp_raw & 0xFF);
  uint8_t cmd[21] = {
      0x17, 0x0A, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x07, 0x46, config_version_, status_version_,
      0x00, 0x01, high_byte, low_byte, 0x00};
  cmd[20] = calc_checksum(cmd, 21);
  send_i2c_message(cmd, 21);
  ESP_LOGI(TAG, "Sent temperature %.1f command (raw=%02X, high=%02X, low=%02X)", temp_c, temp_raw, high_byte, low_byte);
}

void GeckoSpa::send_datetime_command(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second) {
  // Basic validation to prevent invalid values
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59)
    return;
  
  // Recalculate Year if necessary
  if (year >= 2000) {
    year -= 2000;
  }
  
  // Automatically calculate the weekday using Sakamoto's algorithm
  // Result matches Gecko: 0 = Sunday, 1 = Monday, ..., 5 = Friday, 6 = Saturday
  int full_year = year + 2000;
  int y = full_year - (month < 3);
  static int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  uint8_t weekday = (uint8_t)((y + y/4 - y/100 + y/400 + t[month-1] + day) % 7);

  // Prepare all time and date bytes dynamically
  uint8_t year_byte    = year;
  uint8_t day_byte     = day;
  uint8_t month_byte   = month;
  uint8_t hour_byte    = hour;
  uint8_t minute_byte  = minute;
  uint8_t second_byte  = second; // DYNAMIC: Now uses the actual seconds passed to the function

  // Construct the 22-byte command array (Indices 0 to 21)
  uint8_t cmd[] = {
      0x17, 0x0A, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00, // Source & Destination (0-9)
      0x00, 0x00, 0x00, 0x00, 0x08, 0x4B,             // Protocol & Function Code (10-13)
      year_byte,                                      // Byte 14: Year from 2000
      month_byte,                                     // Byte 15: Month
      day_byte,                                       // Byte 16: Day
      weekday,                                        // Byte 17: Day of week (0=Sun)
      hour_byte,                                      // Byte 18: Hour (hex, 24h format)
      minute_byte,                                    // Byte 19: Minutes (hex)
      second_byte,                                    // Byte 20: Seconds (hex)
      0x00                                            // Byte 21: Checksum placeholder
  };

  // Calculate CRC-8 checksum for the full 22-byte packet
  cmd[21] = calc_checksum(cmd, 22);

  // Send the packet via I2C
  send_i2c_message(cmd, 22);

  // Log the final sent command to the serial console
  ESP_LOGI(TAG, "Successfully synced DateTime (With Seconds): %02d.%02d. (Weekday=%d) %02d:%02d:%02d", 
           day, month, weekday, hour, minute, second);
}

void GeckoSpa::request_status() {
  write_str("PING\n");
}

void GeckoSpa::reset_arduino() {
  if (!reset_pin_) {
    ESP_LOGW(TAG, "Reset pin not configured");
    return;
  }
  if (reset_in_progress_) {
    ESP_LOGD(TAG, "Reset already in progress");
    return;
  }
  ESP_LOGI(TAG, "Resetting Arduino (attempt %d)", reset_retry_count_ + 1);
  arduino_ready_ = false;
  reset_pin_->digital_write(false);  // Pull LOW to reset
  reset_start_time_ = millis();
  reset_in_progress_ = true;
  reset_retry_count_++;
}

uint8_t GeckoSpa::calc_checksum(const uint8_t *data, uint8_t len) {
  uint8_t xor_val = 0;
  for (uint8_t i = 0; i < len - 1; i++) {
    xor_val ^= data[i];
  }
  return xor_val;
}

void GeckoSpa::send_i2c_message(const uint8_t *data, uint8_t len) {
  write_str("TX:");
  for (uint8_t i = 0; i < len; i++) {
    char hex[3];
    sprintf(hex, "%02X", data[i]);
    write_str(hex);
  }
  write_str("\n");
}

uint8_t GeckoSpa::hex_to_byte(char high, char low) {
  auto nibble = [](char c) -> uint8_t {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    return 0;
  };
  return (nibble(high) << 4) | nibble(low);
}

void GeckoSpa::process_proxy_message(const char *msg) {
  // FORK: raw proxy lines move to VERBOSE with log_status_changes (DECISIONS.md #5)
  if (log_status_changes_) {
    ESP_LOGV(TAG, "Proxy: %s", msg);
  } else {
    ESP_LOGD(TAG, "Proxy: %s", msg);
  }

  // RX:<len>:<hex>
  if (strncmp(msg, "RX:", 3) == 0) {
    const char *p = msg + 3;
    int len = atoi(p);

    // Find the colon after length
    while (*p && *p != ':')
      p++;
    if (*p == ':')
      p++;

    // Reject truncated or malformed proxy frames before accessing the buffer.
    // The Arduino proxy sends a maximum of 128 bytes per RX line.
    if (len <= 0 || len > 128 || strlen(p) != static_cast<size_t>(len) * 2) {
      ESP_LOGW(TAG, "Invalid proxy RX frame length: %d", len);
      return;
    }
    uint8_t data[128];
    for (int i = 0; i < len; i++) {
      if (!isxdigit(static_cast<unsigned char>(p[i * 2])) ||
          !isxdigit(static_cast<unsigned char>(p[i * 2 + 1]))) {
        ESP_LOGW(TAG, "Invalid hex data in proxy RX frame");
        return;
      }
      data[i] = hex_to_byte(p[i * 2], p[i * 2 + 1]);
    }

    process_i2c_message(data, static_cast<uint8_t>(len));
  } else if (strcmp(msg, "READY") == 0) {
    ESP_LOGI(TAG, "Arduino proxy ready");
    arduino_ready_ = true;
  } else if (strcmp(msg, "I2C_PROXY:V1") == 0) {
    ESP_LOGI(TAG, "Arduino proxy version 1");
  } else if (strcmp(msg, "TX:OK") == 0) {
    ESP_LOGD(TAG, "I2C TX acknowledged");
  } else if (strcmp(msg, "PONG") == 0) {
    ESP_LOGD(TAG, "Proxy ping OK");
  }
}

void GeckoSpa::process_i2c_message(const uint8_t *data, uint8_t len) {
  // Any I2C message means we're connected
  last_i2c_time_ = millis();
  if (!connected_) {
    connected_ = true;
    reset_retry_count_ = 0;
    if (connected_sensor_)
      connected_sensor_->publish_state(true);
    ESP_LOGI(TAG, "Spa connected (I2C traffic detected)");
  }

  // Log standalone messages as FULL-RX (not continuation parts of multi-part messages)
  // Continuation flag is byte[9]: 0x01 = more coming
  bool is_continuation = (len >= 10 && data[9] == 0x01);
  if (!is_continuation && msg_buffer_len_ == 0 && len > 0) {
    log_hex_dump_(data, len);
  }

  // GO message (15 bytes, ends with "GO") - just log it
  if (len == 15 && data[13] == 0x47 && data[14] == 0x4F) {
    ESP_LOGD(TAG, "Received GO message from spa");
    return;
  }

  // Handshake acknowledgment response
  static const uint8_t ACK_MESSAGE[15] = {
      0x17, 0x0A, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02
  };

  // 33-byte config file message - parse XML filename and send acknowledgment
  if (len == 33) {
    // Extract XML filename from bytes 16-28 (null-terminated string)
    char xml_name[16];
    int pos = 0;
    for (int i = 16; i < 29 && data[i] != 0 && pos < 15; i++) {
      xml_name[pos++] = (char)data[i];
    }
    xml_name[pos] = '\0';

    ESP_LOGI(TAG, "Handshake XML: %s", xml_name);

    // Parse version number from filename (e.g., inYT_C82.xml -> 82)
    // Look for _C or _S followed by digits
    const char *ver_ptr = strstr(xml_name, "_C");
    if (ver_ptr == nullptr) ver_ptr = strstr(xml_name, "_S");
    if (ver_ptr != nullptr) {
      int version = atoi(ver_ptr + 2);

      // Publish to appropriate sensor and store version
      if (strstr(xml_name, "_C") != nullptr) {
        config_version_ = version;
        if (config_version_sensor_)
          config_version_sensor_->publish_state(xml_name);
        ESP_LOGI(TAG, "Config version: %d", config_version_);
      } else if (strstr(xml_name, "_S") != nullptr) {
        status_version_ = version;
        if (status_version_sensor_)
          status_version_sensor_->publish_state(xml_name);
        ESP_LOGI(TAG, "Status version: %d", status_version_);

        // Select appropriate offsets based on status version
        if (status_version_ <= 50) {
          log_offsets_ = &GECKO_LOG_OFFSETS_V50;
          ESP_LOGI(TAG, "Using v50 log offsets");
        } else {
          log_offsets_ = &GECKO_LOG_OFFSETS_V51;
          ESP_LOGI(TAG, "Using v51+ log offsets");
        }
      }
    }

    send_i2c_message(ACK_MESSAGE, 15);
    return;
  }

  // 22-byte clock message - parse time and send acknowledgment
  if (len == 22 && data[13] == 0x4B) {  // 0x4B = 'K'
    // Time format: [15]=Day [16]=Month [17]=DayOfWeek [18]=Hour [19]=Min [20]=Sec
    uint8_t day = data[15];
    uint8_t month = data[16];
    uint8_t hour = data[18];
    uint8_t minute = data[19];
    uint8_t second = data[20];

    ESP_LOGD(TAG, "Spa clock: %02d/%02d %02d:%02d:%02d", day, month, hour, minute, second);

    if (spa_time_sensor_) {
      char time_str[20];
      snprintf(time_str, sizeof(time_str), "%02d/%02d %02d:%02d:%02d", day, month, hour, minute, second);
      spa_time_sensor_->publish_state(time_str);
    }

    send_i2c_message(ACK_MESSAGE, 15);
    return;
  }

  // 15-byte "LO" message - handshake complete
  if (len == 15 && data[13] == 0x4C && data[14] == 0x4F) {
    ESP_LOGI(TAG, "Received LO message - handshake complete");
    return;
  }

  // Notification message (77 bytes with byte[6]=0x0B)
  if (len == 77 && data[6] == 0x0B) {
    ESP_LOGD(TAG, "77-byte notification message");
    parse_notification_message(data);
    return;
  }

  // Program status (18 bytes)
  if (len == 18) {
    ESP_LOGI(TAG, "18-byte msg: [1]=%02X [16]=%02X", data[1], data[16]);
    uint8_t prog = data[16];
    if (prog <= 4 && prog != program_id_) {
      program_id_ = prog;
      ESP_LOGI(TAG, "Program from spa: %d", prog);
      // FORK: also save it, so it survives an ESP restart (DECISIONS.md #2)
      if (program_select_)
        program_select_->set_reported_program(prog);
    }
    return;
  }

  // Multi-part message handling using byte[9] as continuation flag
  // byte[9] == 0x01: more parts coming, byte[9] == 0x00: last part
  // Header is 16 bytes: [0-13]=protocol header + [14-15]=5251("RQ" frame marker)
  // Stripping 16 bytes aligns payload with geckolib struct offsets
  // Only concatenate messages with byte[1]=0x09 (config/status type)
  static const int HEADER_LEN = 16;

  // FORK: frames from the spa-initiated stream (byte 6 = 0x0B) are interleaved
  // with config/status parts and are not used here. Appending them corrupts the
  // message being assembled (DECISIONS.md #7).
  if (protocol::is_spa_initiated_part(data, len)) {
    ESP_LOGV(TAG, "Spa-initiated frame (%d bytes, type 0x%02X) not used", len, data[13]);
    return;
  }

  if (len >= HEADER_LEN && data[1] == 0x09) {
    bool more_coming = (data[9] == 0x01);

    // Add this part to buffer (strip 16-byte header including frame marker)
    int payload_start = HEADER_LEN;
    int payload_len = len - payload_start;
    if (payload_len > static_cast<int>(sizeof(msg_buffer_) - msg_buffer_len_)) {
      ESP_LOGW(TAG, "Multi-part message exceeds buffer; dropping frame");
      msg_buffer_len_ = 0;
      return;
    }
    memcpy(msg_buffer_ + msg_buffer_len_, data + payload_start, payload_len);
    msg_buffer_len_ += payload_len;

    if (more_coming) {
      ESP_LOGD(TAG, "Message part (%d bytes), more coming. Buffer now %d bytes", len, msg_buffer_len_);
      return;
    }

    // Last part received - log complete message in FULL-RX format
    log_hex_dump_(msg_buffer_, msg_buffer_len_);

    // Check message type by size
    // ~162 bytes = status-only message (3 parts: 78+78+54 - 3*16 headers)
    // ~390 bytes = config+status message (8 parts with log section starting at offset 230)

    // We expect status message to be around 150 bytes long, but it varies per spa pack model
    // and software version.  The length will be consistent per pack, so once we've detected
    // it we can assume that all future messages at that length are status messages.  Assume
    // that anything between MIN_STATUS_MSG_LEN and 170 bytes long, with a 0 byte at offset 1,
    // is a status message if we don't yet know the correct length.
    if (status_msg_len_ == 0) {
      if ((msg_buffer_len_ >= MIN_STATUS_MSG_LEN) &&
          (msg_buffer_len_ <= 170) &&
          (msg_buffer_[1] == 0x00)) {
        ESP_LOGI(TAG, "Auto-detect %d as the standard status message length", msg_buffer_len_);
        status_msg_len_ = msg_buffer_len_;
      }
    }

    // Check message type by size
    // 162 bytes = status-only message (3 parts: 78+78+54 - 3*16 headers)
    // ~390 bytes = config+status message (8 parts with log section starting at offset 230)
    if ((msg_buffer_len_ == status_msg_len_) &&
        (status_msg_len_ != 0) &&
        (msg_buffer_[1] == 0x00)) {
      // Status-only message (162 bytes)
      ESP_LOGI(TAG, "Status msg (%db): [3]=%02X [5]=%02X [21-24]=%02X%02X%02X%02X [53]=%02X",
               msg_buffer_len_,
               msg_buffer_[3], msg_buffer_[5],
               msg_buffer_[21], msg_buffer_[22], msg_buffer_[23], msg_buffer_[24], msg_buffer_[53]);
      parse_status_message(msg_buffer_, msg_buffer_len_);
    } else if (msg_buffer_len_ >= 300 && msg_buffer_len_ <= 400) {
      // Config+status message (~390 bytes)
      // Config section has +2 byte offset (geckolib offset N → message byte N+2)
      static const int CFG_OFFSET = 2;  // Config struct offset

      // Parse config section (with +2 offset from geckolib struct definitions)
      // Geckolib offsets → message bytes: N → N+2
      uint8_t config_num = msg_buffer_[CFG_OFFSET + 0];
      uint16_t setpoint_raw = (msg_buffer_[CFG_OFFSET + 1] << 8) | msg_buffer_[CFG_OFFSET + 2];
      float setpoint_c = setpoint_raw / 18.0f;
      uint8_t filt_freq = msg_buffer_[CFG_OFFSET + 3];
      uint8_t temp_units = msg_buffer_[CFG_OFFSET + 33];  // 0=F, 1=C
      uint8_t time_format = msg_buffer_[CFG_OFFSET + 34]; // 0=NA, 1=AmPm, 2=24h
      uint8_t pump_timeout = msg_buffer_[CFG_OFFSET + 54];
      uint8_t light_timeout = msg_buffer_[CFG_OFFSET + 55];
      uint8_t econ_type = msg_buffer_[CFG_OFFSET + 70];   // 0=Standard, 1=Night
      uint8_t customer_id = msg_buffer_[CFG_OFFSET + 111];
      uint8_t num_zones = msg_buffer_[CFG_OFFSET + 127];
      uint8_t silent_mode = msg_buffer_[CFG_OFFSET + 157]; // 0=NA, 1=OFF, 2=ECONOMY, 3=SLEEP, 4=NIGHT

      static const char* time_fmt_str[] = {"NA", "AmPm", "24h"};
      static const char* silent_str[] = {"NA", "OFF", "ECONOMY", "SLEEP", "NIGHT"};
      static const char* econ_str[] = {"Standard", "Night"};

      ESP_LOGI(TAG, "Config: Ver=%d Setpoint=%.1f%s FiltFreq=%d TimeFormat=%s",
               config_num, setpoint_c, temp_units == 1 ? "C" : "F", filt_freq,
               time_format < 3 ? time_fmt_str[time_format] : "?");
      ESP_LOGI(TAG, "Config: PumpTimeout=%dmin LightTimeout=%dmin EconType=%s",
               pump_timeout, light_timeout,
               econ_type < 2 ? econ_str[econ_type] : "?");
      ESP_LOGI(TAG, "Config: CustomerID=%d Zones=%d SilentMode=%s",
               customer_id, num_zones,
               silent_mode < 5 ? silent_str[silent_mode] : "?");

      // FORK: publish the user's set point. Unlike the status set point it does
      // not drop while an economy program is active (DECISIONS.md #6).
      if (protocol::set_point_plausible(setpoint_raw) &&
          (!std::isfinite(set_point_) || fabsf(setpoint_c - set_point_) > 0.01f)) {
        set_point_ = setpoint_c;
        ESP_LOGI(TAG, "Set point (config): %.1f", set_point_);
        if (set_point_sensor_)
          set_point_sensor_->publish_state(set_point_);
      }

      // A combined config+status message ends with a full status block. Do
      // not require a previous standalone status message: after a restart,
      // only combined messages may be received for a long time.
      // FORK: known layouts come from a table (390/162 upstream, 353/145 for
      // inYT v65) instead of the single 390-byte case (DECISIONS.md #1).
      uint16_t embedded_offset = 0;
      uint16_t embedded_len = 0;
      if (protocol::find_embedded_status(msg_buffer_len_, embedded_offset, embedded_len)) {
        const uint8_t *status = &msg_buffer_[embedded_offset];
        const GeckoLogOffsets &off = *log_offsets_;
        const int target_pos = off.realSetPointG - 254;
        const int actual_pos = off.displayedTempG - 254;
        if (target_pos < 0 || actual_pos < 0 ||
            target_pos + 1 >= embedded_len ||
            actual_pos + 1 >= embedded_len) {
          ESP_LOGW(TAG, "Embedded status offsets out of bounds");
          msg_buffer_len_ = 0;
          return;
        }
        const uint16_t target_raw = (uint16_t(status[target_pos]) << 8) | status[target_pos + 1];
        const uint16_t actual_raw = (uint16_t(status[actual_pos]) << 8) | status[actual_pos + 1];
        const float target_c = target_raw / 18.0f;
        const float actual_c = actual_raw / 18.0f;
        // Structural markers and plausible temperatures for this spa model.
        // These checks prevent accidental decoding of unrelated config tails.
        // An actual temperature of 0 is accepted here (the spa reports 0 just
        // after power-up); parse_status_message() ignores it.
        if (status[0] == 0x01 && status[1] == 0x00 &&
            target_c >= 8.0f && target_c <= 40.0f &&
            actual_c >= 0.0f && actual_c <= 45.0f) {
          if (status_msg_len_ == 0) {
            ESP_LOGI(TAG, "Embedded status found before standalone status (%d bytes)", embedded_len);
          }
          ESP_LOGI(TAG, "Status from config fallback: target=%.1f actual=%.1f", target_c, actual_c);
          parse_status_message(status, embedded_len);
        } else {
          ESP_LOGW(TAG, "Embedded status rejected: marker=%02X/%02X target=%.1f actual=%.1f",
                   status[0], status[1], target_c, actual_c);
        }
      } else if (status_msg_len_ != 0 && msg_buffer_len_ > status_msg_len_) {
        // Preserve the original path for other pack variants, with bounds check.
        const int status_offset = msg_buffer_len_ - status_msg_len_;
        if (status_offset > 0 && msg_buffer_[status_offset - 1] == 0x3B &&
            msg_buffer_[status_offset + 1] == 0x00) {
          parse_status_message(&msg_buffer_[status_offset], status_msg_len_);
        }
      }

    }

    // Reset buffer for next message
    msg_buffer_len_ = 0;
    return;
  }

  // Short messages (< 11 bytes) - log them
  if (len > 2) {
    char hex_str[64];
    int pos = 0;
    for (int i = 0; i < len && pos < 60; i++) {
      pos += sprintf(hex_str + pos, "%02X", data[i]);
    }
    ESP_LOGI(TAG, "Short msg (%d bytes): %s", len, hex_str);
  }
}

// FORK: hex dump of a received message. Moves to VERBOSE when
// log_status_changes is set (DECISIONS.md #5).
void GeckoSpa::log_hex_dump_(const uint8_t *data, int len) {
  // Split into 32 bytes per line (64 hex characters)
  const int CHUNK_BYTES = 32;
  char hex_str[68];
  if (log_status_changes_) {
    ESP_LOGV(TAG, "FULL-RX:%d bytes", len);
  } else {
    ESP_LOGD(TAG, "FULL-RX:%d bytes", len);
  }
  for (int offset = 0; offset < len; offset += CHUNK_BYTES) {
    int chunk_len = (len - offset < CHUNK_BYTES) ? (len - offset) : CHUNK_BYTES;
    int pos = 0;
    for (int i = 0; i < chunk_len; i++) {
      pos += sprintf(hex_str + pos, "%02X", data[offset + i]);
    }
    if (log_status_changes_) {
      ESP_LOGV(TAG, "  %03d: %s", offset, hex_str);
    } else {
      ESP_LOGD(TAG, "  %03d: %s", offset, hex_str);
    }
  }
}

// FORK: logs the bytes that changed since the previous status block and
// remembers this one. Returns true if anything changed (DECISIONS.md #5).
bool GeckoSpa::log_status_diff_(const uint8_t *data, uint16_t len) {
  if (len > sizeof(last_status_))
    len = sizeof(last_status_);
  if (last_status_len_ != len) {
    memcpy(last_status_, data, len);
    last_status_len_ = len;
    ESP_LOGI(TAG, "Status block (%d bytes) received", len);
    return true;
  }
  char buf[160];
  int pos = 0;
  int changes = 0;
  for (uint16_t i = 0; i < len; i++) {
    if (data[i] == last_status_[i])
      continue;
    changes++;
    if (pos < static_cast<int>(sizeof(buf)) - 16)
      pos += snprintf(buf + pos, sizeof(buf) - pos, " [%u]%02X>%02X", i, last_status_[i], data[i]);
  }
  memcpy(last_status_, data, len);
  if (changes == 0)
    return false;
  ESP_LOGI(TAG, "Status change (%d bytes):%s%s", changes, buf, pos >= static_cast<int>(sizeof(buf)) - 16 ? " ..." : "");
  return true;
}

void GeckoSpa::parse_status_message(const uint8_t *data, uint16_t len) {
  // Convert geckolib offset to message byte: byte = geckolib_offset - 254
  // This accounts for the +2 byte misalignment between geckolib structs and actual message
  auto toB = [](uint16_t geckoOffset) -> uint16_t { return geckoOffset - 254; };

  // Get offsets from version-specific struct
  const GeckoLogOffsets &off = *log_offsets_;

  // Calculate message byte positions
  uint16_t b_hours = toB(off.hours);
  uint16_t b_quietState = toB(off.quietState);
  uint16_t b_udP1 = toB(off.udP1);
  uint16_t b_deviceStatus = toB(off.deviceStatus);
  uint16_t b_p1 = toB(off.p1);
  uint16_t b_udLi = toB(off.udLi);
  uint16_t b_realSetPoint = toB(off.realSetPointG);
  uint16_t b_displayedTemp = toB(off.displayedTempG);
  uint16_t b_lockMode = toB(off.lockMode);
  uint16_t b_packType = toB(off.packType);
  uint16_t b_udPumpTime = toB(off.udPumpTime);

  // === Decode all fields from geckolib-compatible offsets ===

  // Hours counter
  uint8_t hours = data[b_hours];

  // QuietState: 0=NOT_SET, 1=DRAIN, 2=SOAK, 3=OFF
  uint8_t quietState = data[b_quietState];
  static const char* quiet_str[] = {"NOT_SET", "DRAIN", "SOAK", "OFF"};

  // User demand P1-P4 (2-bit fields in UdP1 byte)
  user_demand_state_ = data[b_udP1];
  uint8_t udP1 = (user_demand_state_ >> 0) & 0x03;  // bits 0-1
  uint8_t udP2 = (user_demand_state_ >> 2) & 0x03;  // bits 2-3
  uint8_t udP3 = (user_demand_state_ >> 4) & 0x03;  // bits 4-5
  uint8_t udP4 = (user_demand_state_ >> 6) & 0x03;  // bits 6-7
  static const char* pump_ud_str[] = {"OFF", "LO", "HI", "?"};

  // Device status byte (CP, BL, Heater, Waterfall)
  uint8_t devStatus = data[b_deviceStatus];
  bool cp_on = (devStatus >> 2) & 0x01;      // bit 2: CP (circulation pump)
  bool bl_on = (devStatus >> 1) & 0x01;      // bit 1: BL (blower)
  bool heater_on = (devStatus >> 5) & 0x01;  // bit 5: MSTR_HEATER
  bool waterfall = (devStatus >> 7) & 0x01;  // bit 7: Waterfall

  // P1-P4 device status (2-bit fields)
  uint8_t p1_raw = data[b_p1];
  uint8_t p1_state = (p1_raw >> 0) & 0x03;  // bits 0-1: 0=OFF, 1=HIGH, 2=LOW
  uint8_t p2_state = (p1_raw >> 2) & 0x03;  // bits 2-3
  uint8_t p3_state = (p1_raw >> 4) & 0x03;  // bits 4-5
  uint8_t p4_state = (p1_raw >> 6) & 0x03;  // bits 6-7
  static const char* pump_state_str[] = {"OFF", "HIGH", "LOW", "?"};

  // Light user demand
  uint8_t udLi = data[b_udLi];

  // Lock mode: 0=UNLOCK, 1=PARTIAL, 2=FULL
  uint8_t lockMode = data[b_lockMode];
  static const char* lock_str[] = {"UNLOCK", "PARTIAL", "FULL"};

  // Pack type
  uint8_t packType = data[b_packType];
  static const char* pack_str[] = {"Unknown", "inXE", "MasIBC", "MIA", "DJS4", "inClear", "inXM", "K600", "inTerface", "inTouch", "inYT"};

  // Pump timer countdown
  uint8_t pumpTime = data[b_udPumpTime];

  // Temperature (word values, big-endian)
  uint16_t target_raw = (data[b_realSetPoint] << 8) | data[b_realSetPoint + 1];
  uint16_t actual_raw = (data[b_displayedTemp] << 8) | data[b_displayedTemp + 1];
  float target_temp = target_raw / 18.0f;
  float actual_temp = actual_raw / 18.0f;

  // === Log decoded status (geckolib format) ===
  // FORK: with log_status_changes, only when the status block changed.
  const bool log_decoded = !log_status_changes_ || log_status_diff_(data, len);
  if (log_decoded) {
    ESP_LOGI(TAG, "Status[v%d]: Hours=%d QuietState=%s LockMode=%s PackType=%s",
             status_version_, hours,
             quietState < 4 ? quiet_str[quietState] : "?",
             lockMode < 3 ? lock_str[lockMode] : "?",
             packType < 11 ? pack_str[packType] : "?");

    ESP_LOGI(TAG, "Status: Temp=%.1f/%.1f°C Heater=%s CP=%s BL=%s Waterfall=%s",
             target_temp, actual_temp,
             heater_on ? "ON" : "OFF",
             cp_on ? "ON" : "OFF",
             bl_on ? "ON" : "OFF",
             waterfall ? "ON" : "OFF");

    ESP_LOGI(TAG, "Status: P1=%s P2=%s P3=%s P4=%s PumpTimer=%dmin",
             pump_state_str[p1_state], pump_state_str[p2_state],
             pump_state_str[p3_state], pump_state_str[p4_state],
             pumpTime);

    ESP_LOGI(TAG, "Status: UdP1=%s UdP2=%s UdP3=%s UdP4=%s UdLi=%s",
             pump_ud_str[udP1], pump_ud_str[udP2],
             pump_ud_str[udP3], pump_ud_str[udP4],
             udLi ? "ON" : "OFF");
  }

  // === Update internal state and entities ===

  // Derive states for Home Assistant entities
  bool new_standby = (quietState == 0x03);  // OFF in QuietState means standby
  bool new_circ = cp_on;              // Circulation pump from device status (CP)
  bool new_waterfall = waterfall;     // Waterfall from device status
  bool new_blower = bl_on;            // Blower from device status (BL)
  bool new_heating = heater_on;       // Heater from device status
  bool new_light = (udLi != 0);       // Light from UdLi (user demand light)

  // P1-P4 individual pump states (0=OFF, 1=HIGH, 2=LOW)
  uint8_t new_p1 = p1_state;
  uint8_t new_p2 = p2_state;
  uint8_t new_p3 = p3_state;
  uint8_t new_p4 = p4_state;

  float new_target = target_temp;
  float new_actual = actual_temp;
  // Plausible ranges, same as for the embedded status in config messages
  bool target_valid = (target_raw != 0 && target_temp >= 8.0f && target_temp <= 40.0f);
  bool actual_valid = (actual_raw != 0 && actual_temp >= 0.0f && actual_temp <= 45.0f);
  if ((!target_valid || !actual_valid) && log_decoded) {
    ESP_LOGW(TAG, "Implausible temperature ignored: target=%.1f (raw=%u) actual=%.1f (raw=%u)",
             target_temp, target_raw, actual_temp, actual_raw);
  }

  // On first status message, publish all states
  bool first = !first_status_received_;
  if (first) {
    first_status_received_ = true;
    ESP_LOGI(TAG, "First status received, publishing all states");
  }

  // Update entities on change (or first message)
  if (first || new_light != light_state_) {
    light_state_ = new_light;
    ESP_LOGI(TAG, "Light: %s", light_state_ ? "ON" : "OFF");
    if (light_switch_)
      light_switch_->publish_state(light_state_);
  }

  if (first || new_circ != circ_state_) {
    circ_state_ = new_circ;
    ESP_LOGI(TAG, "Circulation: %s", circ_state_ ? "ON" : "OFF");
    if (circ_switch_)
      circ_switch_->publish_state(circ_state_);
    // FORK: read-only circulation sensor (DECISIONS.md #3)
    if (circulation_sensor_)
      circulation_sensor_->publish_state(circ_state_);
  }

  if (first || new_waterfall != waterfall_state_) {
    waterfall_state_ = new_waterfall;
    ESP_LOGI(TAG, "Waterfall: %s", waterfall_state_ ? "ON" : "OFF");
    if (waterfall_sensor_)
      waterfall_sensor_->publish_state(waterfall_state_);
  }

  if (first || new_blower != blower_state_) {
    blower_state_ = new_blower;
    ESP_LOGI(TAG, "Blower: %s", blower_state_ ? "ON" : "OFF");
    if (blower_sensor_)
      blower_sensor_->publish_state(blower_state_);
  }

  if (first || new_heating != heating_state_) {
    heating_state_ = new_heating;
    ESP_LOGI(TAG, "Heating: %s", heating_state_ ? "ON" : "OFF");
    update_climate_state();
    // FORK: read-only heater sensor (DECISIONS.md #3)
    if (heater_sensor_)
      heater_sensor_->publish_state(heating_state_);
  }

  if (first || new_standby != standby_state_) {
    standby_state_ = new_standby;
    ESP_LOGI(TAG, "Standby: %s", standby_state_ ? "ON" : "OFF");
    if (standby_sensor_)
      standby_sensor_->publish_state(standby_state_);
  }

  // Update LockMode sensor
  if (first || lockMode != lock_mode_) {
    lock_mode_ = lockMode;
    if (lock_mode_sensor_) {
      lock_mode_sensor_->publish_state(lockMode < 3 ? lock_str[lockMode] : "?");
    }
  }

  // Update PackType sensor
  if (first || packType != pack_type_) {
    pack_type_ = packType;
    if (pack_type_sensor_) {
      pack_type_sensor_->publish_state(packType < 11 ? pack_str[packType] : "?");
    }
  }

  // Update PumpTimer sensor
  if (first || pumpTime != pump_timer_) {
    pump_timer_ = pumpTime;
    if (pump_timer_sensor_) {
      pump_timer_sensor_->publish_state(pumpTime);
    }
  }

  // Only update temperature if valid data was received
  bool temp_changed = false;
  if (target_valid && (!std::isfinite(target_temp_) ||
      fabsf(new_target - target_temp_) > 0.1f)) {
    target_temp_ = new_target;
    temp_changed = true;
  }

  if (actual_valid && (!std::isfinite(actual_temp_) ||
      fabsf(new_actual - actual_temp_) > 0.1f)) {
    actual_temp_ = new_actual;
    temp_changed = true;
  }

  if (temp_changed) {
    ESP_LOGI(TAG, "Temp: target=%.1f actual=%.1f",
             target_temp_, actual_temp_);
    update_climate_state();
  }

  // Update P1-P4 pump states (all controllable switches)
  if (first || new_p1 != pump1_state_) {
    pump1_state_ = new_p1;
    if (pump1_switch_)
      pump1_switch_->publish_state(pump1_state_ != 0);
    // FORK: read-only pump sensors, on for any speed (DECISIONS.md #3)
    if (pump1_sensor_)
      pump1_sensor_->publish_state(pump1_state_ != 0);
  }
  if (first || new_p2 != pump2_state_) {
    pump2_state_ = new_p2;
    if (pump2_switch_)
      pump2_switch_->publish_state(pump2_state_ != 0);
    if (pump2_sensor_)
      pump2_sensor_->publish_state(pump2_state_ != 0);
  }
  if (first || new_p3 != pump3_state_) {
    pump3_state_ = new_p3;
    if (pump3_switch_)
      pump3_switch_->publish_state(pump3_state_ != 0);
  }
  if (first || new_p4 != pump4_state_) {
    pump4_state_ = new_p4;
    if (pump4_switch_)
      pump4_switch_->publish_state(pump4_state_ != 0);
  }
}

void GeckoSpa::update_climate_state() {
  if (!climate_)
    return;

  // Keine ungültigen Temperaturwerte an ESPHome/MQTT veröffentlichen.
  // Erst veröffentlichen, wenn beide Temperaturen gültig sind.
  if (!std::isfinite(target_temp_) ||
      !std::isfinite(actual_temp_)) {
    return;
  }

  climate_->target_temperature = target_temp_;
  climate_->current_temperature = actual_temp_;

  // FORK: with report_cool_mode: false the spa is always shown in heat mode
  // and the action follows the heater only. The spa has no cooling, so water
  // above the set point is idle, not cooling (DECISIONS.md #4).
  const bool cool = climate_->get_report_cool_mode() && target_temp_ < actual_temp_;

  // Betriebsmodus anhand der Temperaturen bestimmen.
  if (cool) {
    climate_->mode = climate::CLIMATE_MODE_COOL;
  } else {
    climate_->mode = climate::CLIMATE_MODE_HEAT;
  }

  // Aktuelle Heiz-/Kühlaktion bestimmen.
  if (heating_state_) {
    climate_->action = climate::CLIMATE_ACTION_HEATING;
  } else if (cool) {
    climate_->action = climate::CLIMATE_ACTION_COOLING;
  } else {
    climate_->action = climate::CLIMATE_ACTION_IDLE;
  }

  // Gültigen Climate-Zustand veröffentlichen.
  climate_->publish_state();
}

int GeckoSpa::days_since_2000(int day, int month, int year) {
  // Calculate days since Jan 1, 2000
  struct tm tm = {};
  tm.tm_year = 100 + year;  // years since 1900, so 2000 = 100
  tm.tm_mon = month - 1;    // 0-based month
  tm.tm_mday = day;
  time_t target = mktime(&tm);

  struct tm epoch = {};
  epoch.tm_year = 100;  // 2000
  epoch.tm_mon = 0;     // January
  epoch.tm_mday = 1;
  time_t base = mktime(&epoch);

  return (int)((target - base) / 86400);
}

void GeckoSpa::parse_notification_message(const uint8_t *data) {
  // Notification entries start at byte 16, each entry is 6 bytes:
  // [ID] [DD] [MM] [YY] [INTERVAL_LO] [INTERVAL_HI]
  // ID: 0x01=Rinse Filter, 0x02=Clean Filter, 0x03=Change Water, 0x04=Spa Checkup
  bool cache_changed = false;
  for (int i = 0; i < 4; i++) {
    int offset = 16 + (i * 6);
    uint8_t id = data[offset];
    uint8_t reset_day, reset_month, reset_year;
    if (notif_date_format_ == NotifDateFormat::D_M_Y) {
      reset_day = data[offset + 1];
      reset_month = data[offset + 2];
      reset_year = data[offset + 3];  // 2-digit year
    } else {
      reset_year = data[offset + 1];  // 2-digit year
      reset_month = data[offset + 2];
      reset_day = data[offset + 3];
    }
    uint16_t interval = data[offset + 4] | (data[offset + 5] << 8);

    if (id == 0 || id > 4 || interval == 0) {
      ESP_LOGW(TAG, "Reminder entry=%d skipped (id=%u interval=%u)", i, id, interval);
      continue;
    }

    // Calculate due date: reset_date + interval days
    struct tm reset_tm = {};
    reset_tm.tm_year = 100 + reset_year;  // years since 1900
    reset_tm.tm_mon = reset_month - 1;     // 0-based month
    reset_tm.tm_mday = reset_day + interval;  // mktime normalizes this
    mktime(&reset_tm);  // Normalize the date

    // Format as ISO date string (YYYY-MM-DD)
    char date_str[12];
    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d",
             1900 + reset_tm.tm_year, reset_tm.tm_mon + 1, reset_tm.tm_mday);

    text_sensor::TextSensor *sensor = nullptr;
    switch (id) {
      case 0x01:
        sensor = rinse_filter_sensor_;
        break;
      case 0x02:
        sensor = clean_filter_sensor_;
        break;
      case 0x03:
        sensor = change_water_sensor_;
        break;
      case 0x04:
        sensor = spa_checkup_sensor_;
        break;
    }

    // Publish state if it has changed
    if (strcmp(date_str, notification_date_[id - 1]))
    {
      ESP_LOGI(TAG, "Publish changed notification %d : %s", id, date_str);
      if (sensor) {
        sensor->publish_state(date_str);
      }
      strcpy(notification_date_[id - 1], date_str);
      cache_changed = true;
    }
  }

  // Nur bei tatsächlicher Änderung in den Flash schreiben (schont den Speicher)
  if (cache_changed) {
    NotificationCache cache{};
    memcpy(cache.dates, notification_date_, sizeof(cache.dates));
    if (notification_pref_.save(&cache)) {
      ESP_LOGI(TAG, "Reminders saved to flash");
    } else {
      ESP_LOGW(TAG, "Saving reminders to flash failed");
    }
  }
}

// GeckoSpaClimate implementation
void GeckoSpaClimate::setup() {
  this->mode = climate::CLIMATE_MODE_HEAT;
  this->action = climate::CLIMATE_ACTION_IDLE;
  this->target_temperature = NAN;
  this->current_temperature = NAN;

  // Die MQTT-Komponente von ESPHome veröffentlicht die Zieltemperatur beim
  // Verbindungsaufbau ohne NAN-Prüfung (Payload "nan"). Daher die zuletzt
  // gespeicherte, plausible Zieltemperatur aus dem Flash wiederherstellen.
  auto restored = this->restore_state_();
  if (restored.has_value() && std::isfinite(restored->target_temperature) &&
      restored->target_temperature >= 8.0f && restored->target_temperature <= 40.0f) {
    this->target_temperature = restored->target_temperature;
    ESP_LOGI(TAG, "Restored target temperature: %.1f", this->target_temperature);
  } else {
    ESP_LOGW(TAG, "No plausible target temperature stored, waiting for spa status");
  }
}

climate::ClimateTraits GeckoSpaClimate::traits() {
  auto traits = climate::ClimateTraits();
  if (report_cool_mode_) {
    traits.set_supported_modes({climate::CLIMATE_MODE_HEAT, climate::CLIMATE_MODE_COOL});
  } else {
    traits.set_supported_modes({climate::CLIMATE_MODE_HEAT});  // FORK: DECISIONS.md #4
  }
  
  traits.add_feature_flags(
      climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE |
      climate::CLIMATE_SUPPORTS_ACTION);

  traits.set_visual_min_temperature(8.0);
  traits.set_visual_max_temperature(40.0);
  traits.set_visual_temperature_step(0.5);
  traits.set_visual_current_temperature_step(0.5f);
  return traits;
}

void GeckoSpaClimate::control(const climate::ClimateCall &call) {
  if (call.get_target_temperature().has_value()) {
    float temp = *call.get_target_temperature();
    // Unplausible Sollwerte weder senden noch veröffentlichen
    if (!std::isfinite(temp) || temp < 8.0f || temp > 40.0f) {
      ESP_LOGW(TAG, "Rejected implausible target temperature: %.1f", temp);
      return;
    }
    this->target_temperature = temp;
    parent_->send_temperature_command(temp);
  }
  this->publish_state();
}

// GeckoSpaSwitch implementation
void GeckoSpaSwitch::write_state(bool state) {
  if (switch_type_ == "light") {
    parent_->send_light_command(state);
  } else if (switch_type_ == "circulation") {
    parent_->send_circ_command(state);
  } else if (switch_type_ == "pump1") {
    parent_->send_pump1_command(state ? 1 : 0);  // 1=HIGH, 0=OFF
  } else if (switch_type_ == "pump2") {
    parent_->send_pump2_command(state ? 1 : 0);  // EXPERIMENTAL
  } else if (switch_type_ == "pump3") {
    parent_->send_pump3_command(state ? 1 : 0);  // EXPERIMENTAL
  } else if (switch_type_ == "pump4") {
    parent_->send_pump4_command(state ? 1 : 0);  // EXPERIMENTAL
  }
  // State will be published when spa confirms the change
}

// GeckoSpaSelect implementation
void GeckoSpaSelect::setup() {
  // Initialize preferences
  this->pref_ = global_preferences->make_preference<uint8_t>(this->get_object_id_hash());

  // Restore saved state on boot
  if (this->pref_.load(&this->saved_index_) && this->saved_index_ < 5) {
    this->saved_valid_ = true;  // FORK: DECISIONS.md #2
    static const char *prog_names[] = {"Away", "Standard", "Energy", "Super Energy", "Weekend"};
    this->publish_state(prog_names[this->saved_index_]);
    ESP_LOGI("gecko_spa", "Restored program state: %s (index %d)", prog_names[this->saved_index_], this->saved_index_);
  }
}

void GeckoSpaSelect::control(const std::string &value) {
  uint8_t prog = 1;
  if (value == "Away")
    prog = 0;
  else if (value == "Standard")
    prog = 1;
  else if (value == "Energy")
    prog = 2;
  else if (value == "Super Energy")
    prog = 3;
  else if (value == "Weekend")
    prog = 4;

  // Save the state
  this->saved_index_ = prog;
  this->saved_valid_ = this->pref_.save(&this->saved_index_);

  parent_->send_program_command(prog);
  this->publish_state(value);
}

// FORK: the spa reports its program only at power-up and when it changes, so
// save it; setup() then restores it after an ESP restart (DECISIONS.md #2).
// Saving only on change keeps flash writes rare.
void GeckoSpaSelect::set_reported_program(uint8_t prog) {
  const char *name = protocol::program_name(prog);
  if (name == nullptr)
    return;
  if (!this->saved_valid_ || prog != this->saved_index_) {
    this->saved_index_ = prog;
    this->saved_valid_ = this->pref_.save(&this->saved_index_);
    ESP_LOGI("gecko_spa", "Saved program reported by spa: %s", name);
  }
  this->publish_state(name);
}

}  // namespace gecko_spa
}  // namespace esphome
