# CH224A I2C wiring for ESP32-C3

This firmware is modified for the CH224A I2C trigger board shown in the project screenshots.

## Wiring

| CH224A board | ESP32-C3 |
|---|---|
| SD / SDA | GPIO 5 |
| SL / SCL | GPIO 6 |
| PG | GPIO 7 |
| 3V | 3V3 |
| GND | GND |

Do **not** connect the CH224A USB-C VBUS/output to an ESP32 GPIO.

## Voltage control

The firmware writes CH224A register `0x0A`:

- `0` = 5V
- `1` = 9V
- `2` = 12V
- `3` = 15V

The firmware automatically probes I2C address `0x23` first and `0x22` second.

`PG` is active-low and is reported in the HTTP/BLE status as `powerGood`.

## Important

The selected voltage is a **USB-PD request**, not a guarantee that the charger can supply that PDO. The charger must advertise/support the requested voltage. The firmware reports `powerGood` so the app can distinguish a successful negotiation from a failed/not-ready negotiation.

This version no longer drives the old 9V/12V/15V selector pads with ESP32 GPIOs.
