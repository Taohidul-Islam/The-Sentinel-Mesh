# Wiring: TX1 (weather node)

Board: ESP32 DOIT DevKit V1. Pin numbers are GPIO numbers, not the silkscreen D-numbers.

TX1 reads temperature, humidity and pressure from a BME280 over I2C.

## Connections

| BME280 | ESP32 |
|--------|-------|
| VIN | 3.3V |
| GND | GND |
| SCL | GPIO21 |
| SDA | GPIO22 |

```
   BME280                 ESP32
  +-------+            +---------+
  | VIN   |------------| 3V3     |
  | GND   |------------| GND     |
  | SCL   |------------| GPIO21  |
  | SDA   |------------| GPIO22  |
  +-------+            +---------+
```

## Reset

Hold **GPIO13** for 3 seconds on boot to wipe all saved settings.

## Notes

- SCL is on 21 and SDA is on 22 here. That is the opposite of the usual ESP32 default, and the opposite of TX2. It is how the working build is wired, so keep it that way unless you change the firmware too.
- Power the board through USB or the VIN pin.
