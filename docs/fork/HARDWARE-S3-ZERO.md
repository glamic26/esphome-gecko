# Hardware: Waveshare ESP32-S3-Zero

The architecture is upstream's: an Arduino Nano (ATmega328P, 5 V) acts as the
I2C proxy on the spa bus and talks to the ESP32 over UART. This page covers
the differences when the ESP32 is a Waveshare ESP32-S3-Zero. Example
configuration: [`esphome/spa-controller-s3-zero.yaml`](../../esphome/spa-controller-s3-zero.yaml).

## Parts

| Part | Notes |
|---|---|
| Waveshare ESP32-S3-Zero | 4 MB flash; native USB only (no USB-serial chip) |
| Arduino Nano or clone, ATmega328P, 5 V | flashed with `arduino/arduino-i2c-proxy-atmega328p.hex` |
| Resistors 2.2 kΩ and 4.7 kΩ | voltage divider, Nano TX to ESP32 RX |
| Jumper wires | to the spa's communication port |

Both boards are powered from USB. Do not power anything from the spa's port.

## Wiring

| ESP32-S3-Zero | Arduino Nano | Notes |
|---|---|---|
| GPIO5 (UART TX) | D0 (RX) | direct |
| GPIO6 (UART RX) | D1 (TX) | through the divider below |
| GPIO7 | RST | direct; lets the ESP32 reset the Nano |
| GND | GND | common ground |

| Arduino Nano | Spa communication port |
|---|---|
| A4 (SDA) | pin 3 (SDA) |
| A5 (SCL) | pin 2 (SCL) |
| GND | pin 8 (GND) |

### Voltage divider

```
Nano D1 (TX) ---[2.2 kΩ]---+--- ESP32 GPIO6
                           |
                        [4.7 kΩ]
                           |
                          GND
```

5 V × 4.7 / (2.2 + 4.7) ≈ 3.4 V at the ESP32 input, below its 3.6 V maximum.
The ratio is the same as upstream's 2.7 kΩ / 5.6 kΩ.

The ESP32's 3.3 V output into the Nano's D0 is read as high by the 5 V
ATmega328P without level shifting, as in upstream.

## Spa communication port

Pinout verified on an IN.YE-3 (H3.0) pack: SCL on pin 2, SDA on pin 3, GND on
pin 8, +5 V on pin 4 and +12 V on pin 6. Odd-numbered pins are in one row and
even-numbered pins in the other; the board is printed with the numbers of some
pins (for example 1, 7 and 8). Upstream's picture `pictures/spa_pinouts.png`
shows the connector.

Pins 4 and 6 carry power. Nothing should be connected to them; measuring them
is a simple way to confirm the pin numbering before connecting:

1. Spa powered, multimeter on DC volts (20 V range).
2. Black probe on pin 8 (GND).
3. Red probe on pin 4: about 5 V. On pin 6: about 12 V.

If the readings do not match, do not connect: the numbering is different on
your board.

## Power rules

- Switch the spa off at its isolator before connecting or disconnecting the
  wires to its communication port.
- Never remove power from the Nano while it is wired to a powered spa. An
  unpowered ATmega328P can hold the bus lines low through its input
  protection diodes and disturb communication with the keypad.
- With the spa off and the bridge powered, the Nano's I2C writes fail and the
  ESP32 retries resetting it with increasing back-off. This is expected and
  stops once the spa is back.

## Flashing

- **Nano:** `avrdude -v -patmega328p -carduino -P<port> -b115200 -D -Uflash:w:arduino-i2c-proxy-atmega328p.hex:i`
  (57600 for boards with the old bootloader), as in upstream.
- **ESP32-S3-Zero:** select the board `esp32-s3-devkitc-1` with variant
  `esp32s3`. For the first USB flash, if the board is not detected, hold
  BOOT while connecting USB. Later updates can be made over Wi-Fi.
