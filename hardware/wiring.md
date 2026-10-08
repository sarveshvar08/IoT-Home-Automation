# Wiring Reference

All pins come directly from the firmware (`relayPins[]`, `switchPins[]`, `WIFI_LED`). See `circuit/circuit_diagram.png` and `circuit/schematic.png`.

> **Warning:** If your relays switch AC mains, the load side is dangerous. This page covers only the low-voltage control side. Load wiring, voltage and current ratings are **not specified in the source code**.

## Relay outputs (ESP32 → relay module)

| ESP32 Pin | Connected To | Purpose |
| --------- | ------------ | ------- |
| GPIO 23 | Relay IN1 | Appliance 1 (`light1`) |
| GPIO 22 | Relay IN2 | Appliance 2 (`light2`) |
| GPIO 21 | Relay IN3 | Appliance 3 (`light3`) |
| GPIO 19 | Relay IN4 | Appliance 4 (`light4`) |
| GPIO 18 | Relay IN5 | Appliance 5 (`light5`) |
| GPIO 5  | Relay IN6 | Appliance 6 (`light6`) |

Relays are treated as **active-LOW** (`RELAY_ACTIVE_LOW true`): the GPIO is driven LOW to turn a relay ON and HIGH to turn it OFF. For active-HIGH modules, set it to `false`.

## Manual switches (switch → ESP32)

| ESP32 Pin | Connected To | Purpose |
| --------- | ------------ | ------- |
| GPIO 32 | Switch 1 (other terminal → GND) | Manual control of Relay 1 |
| GPIO 33 | Switch 2 (other terminal → GND) | Manual control of Relay 2 |
| GPIO 25 | Switch 3 (other terminal → GND) | Manual control of Relay 3 |
| GPIO 26 | Switch 4 (other terminal → GND) | Manual control of Relay 4 |
| GPIO 27 | Switch 5 (other terminal → GND) | Manual control of Relay 5 |
| GPIO 14 | Switch 6 (other terminal → GND) | Manual control of Relay 6 |

The pins use `INPUT_PULLUP`, so no external resistors are needed. A pin reads HIGH when idle and LOW when the switch is closed. The wiring "one terminal to GPIO, other to GND" follows from this logic.

## Status LED

| ESP32 Pin | Connected To | Purpose |
| --------- | ------------ | ------- |
| GPIO 2 | `WIFI_LED` (onboard or external: not specified) | Blinks every 500 ms when connected to Arduino IoT Cloud; solid ON when disconnected |

## Power and ground

| Connection | Details |
| ---------- | ------- |
| ESP32 GND ↔ Relay GND ↔ Switch GND | Common ground (needed for signals) |
| Relay module VCC | Voltage **not specified in the source code**; follow your module's datasheet |
| ESP32 power input | **Not specified in the source code** |

## Behavior

- **Switch press** (debounced, 40 ms): toggles the matching relay. The switch toggles state rather than following its position, so momentary and maintained switches both work.
- **Cloud change** (`lightN` from dashboard, Google Assistant or Alexa): sets the matching relay to the requested state.
- **Either way**, the relay, the internal `relayState[]` and the cloud property are updated together.
- **At boot**, all relays start OFF. The code does not store state across power loss.
- **After Wi-Fi/cloud reconnect**, the ESP32 pushes its actual relay states to the cloud so the dashboard does not show stale values.

## Pin caveats (from the standard ESP32 pin map, not from the code)

- GPIO 2 (`WIFI_LED`) is an ESP32 boot-strapping pin, although a code comment says strapping pins are avoided. Keep any external circuit on it from pulling it HIGH at boot.
- GPIO 5 (Relay 6) is also a strapping pin and may briefly output a signal at boot. On active-LOW modules this can cause a brief relay click at power-up.
