# Protocol notes: inYT, config/status version 65

Observations from bus traffic recorded with this component on an inYT pack
(handshake files `inYT_C65.xml` and `inYT_S65.xml`) with an in.k500 keypad,
two single-speed jet pumps, a circulation pump, a heater and a light. Values
were checked against the keypad and against the pack's power draw measured on
its supply. Upstream's [I2C protocol section](../../README.md#i2c-protocol)
describes the framing; this page adds what differs or was not documented.

Unless stated otherwise, byte offsets are in the reassembled payload, after
the 16-byte header of each frame has been removed. Status offsets equal
geckolib's `inyt-log-65` offsets minus 254, as in upstream.

## Frame header

| Byte | Meaning |
|---|---|
| 1 | `0x09` for frames from the spa |
| 6 | Channel: `0x0A` config/status replies, `0x0B` spa-initiated messages |
| 9 | `0x01` more parts follow, `0x00` last part |
| 12 | Number of bytes after byte 13 (frame length minus 14) |
| 13 | Message type, for example `0xC7` config/status, `0x4E` program |

Every frame ends with a checksum: XOR of all preceding bytes.

## Config/status channel (`0x0A`)

| Message | Parts (raw frame sizes) | Payload |
|---|---|---|
| Status | 78 + 78 + 37 | 145 bytes |
| Config + status | 78 + 78 + 65 + 51 + 78 + 78 + 37 | 353 bytes |

The combined message is sent after every GO handshake. Bytes 0-207 are
config, bytes 208-352 are the full status block (marker `01 00`). Byte 207 is
`0xDE`.

The spa occasionally ends a combined message early (last-part flag on the
fourth part, 208 bytes). Such a message contains no status block and is
ignored.

### Status block (145 bytes)

| Byte | Meaning | Verified values |
|---|---|---|
| 0-1 | Marker | `01 00` |
| 2 | Hour of the pack clock (geckolib `Hours`) | 0-23 |
| 5 | Pump user demand, 2 bits per pump | pump 1 on `0x02`, pump 2 on `0x08` |
| 6 | Output flags | bit 5 heater, bit 2 circulation pump, bit 7 see below |
| 7 | Pump state, 2 bits per pump | pump 1 on `0x01`, pump 2 on `0x04` |
| 21-22 | Effective set point, 1/18 °C | drops during economy periods |
| 23-24 | Water temperature, 1/18 °C | `0` after power-up, see below |
| 49 | Pump timer, minutes | 20 after a pump starts |
| 51 | Light timer, minutes | 120 after the light is switched on |
| 53 | Light on | `0x00` / `0x01` |

Byte 6 bit 7 (upstream's "waterfall") follows the circulation pump on this
pack. Bytes 61, 135 and 144 change whenever an output changes; their meaning
is not confirmed.

Single-speed pumps report state 1 (geckolib "HIGH") when running. The state
in the status block follows the relay by up to about 3 s. The keypad press
that only wakes the panel produces no bus traffic.

The light's colour is selected inside the lamp (by switching its power) and
is not visible on the bus: every "on" status block is identical.

### Config (first 208 bytes of the combined message)

| Byte | Meaning |
|---|---|
| 3-4 | User set point, 1/18 °C. Does not change during economy periods. |

Upstream's config decoding gives consistent values: the set point, and pump
and light timeouts that match the timers seen in the status block.

## Spa-initiated channel (`0x0B`)

| Message | Size | Notes |
|---|---|---|
| Program | 18 bytes | byte 16 = program: 0 Away, 1 Standard, 2 Energy, 3 Super Energy, 4 Weekend |
| Reminders | 77 bytes | as upstream |
| Program schedule | 78-byte parts, type `0x4E` | 16 or more parts, no final part |
| Unknown | 21 bytes, type `0x52` | single frame, sent at start-up |

The program message is sent when the spa starts up and whenever the program
is changed at the keypad. It is not repeated otherwise and is not part of the
config message.

The program schedule burst follows every program change and spa power-up.
Its parts arrive interleaved with config/status parts. The component ignores
multi-part frames on this channel (see [DECISIONS.md #7](DECISIONS.md)).

## Economy periods

With Super Energy and Away selected, the effective set point (status bytes
21-22) dropped from 37.5 °C to 26.4 °C, 20 °F lower, while the config set
point stayed at 37.5 °C. With Energy and Weekend selected no drop was seen at
the time; economy periods depend on each program's schedule.

## Power-up sequence

Times from when the pack is powered:

| Time | Traffic |
|---|---|
| 0 s | Frames from other bus addresses, the program schedule burst, the program message, the reminder message, 21-byte type `0x52` frames |
| about 20 s | First handshake. The clock in this handshake (and status byte 2) reads 00:00; the next handshake has the correct time. |
| 20 s to about 2 min 20 s | Status blocks with water temperature `0`; circulation pump on, heater off |
| about 2 min 20 s | First valid water temperature; heater on |

A water temperature of `0` is not published (upstream behaviour, kept).
