#pragma once
#include <deque>
#include <string>
#include "esphome/core/stub_runtime.h"

namespace esphome {
namespace uart {

class UARTComponent {};

// Serial link to the Arduino proxy. Tests push the proxy's text lines into
// `rx` and read what the component sent from `tx`.
class UARTDevice {
 public:
  void set_uart_parent(UARTComponent *) {}
  int available() { return static_cast<int>(rx.size()); }
  int read() {
    if (rx.empty())
      return -1;
    char c = rx.front();
    rx.pop_front();
    return static_cast<unsigned char>(c);
  }
  void write_str(const char *s) { tx += s; }

  std::deque<char> rx;
  std::string tx;
};

}  // namespace uart
}  // namespace esphome
