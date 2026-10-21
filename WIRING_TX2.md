# Wiring: TX2 (tilt and water node)

Board: ESP32 DOIT DevKit V1. Pin numbers are GPIO numbers, not the silkscreen D-numbers.

TX2 reads ground tilt from an MPU6050 over I2C and water distance from an HC-SR04 sonar.

## MPU6050

| MPU6050 | ESP32 |
|---------|-------|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO21 |
| SCL | GPIO22 |

## HC-SR04

| HC-SR04 | ESP32 |
|---------|-------|
| VCC | 3.3V |
| GND | GND |
| TRIG | GPIO5 |
| ECHO | GPIO18 |

## Diagram

```
   MPU6050                ESP32               HC-SR04
  +-------+            +---------+          +--------+
  | VCC   |------------| 3V3     |----------| VCC    |
  | GND   |------------| GND     |----------| GND    |
  | SDA   |------------| GPIO21  |          |        |
  | SCL   |------------| GPIO22  |          |        |
  +-------+            | GPIO5   |----------| TRIG   |
                       | GPIO18  |----------| ECHO   |
                       +---------+          +--------+
```

## Reset

Hold **GPIO13** for 3 seconds on boot to wipe all saved settings.

## Notes

- The HC-SR04 is powered from 3.3V, which keeps the ECHO pin at a safe level for the ESP32. If you power it from 5V instead, put a voltage divider on ECHO.
- SDA is on 21 and SCL is on 22 here, the opposite of TX1. That is how the working build is wired, so keep it that way unless you change the firmware too.
- Sonar readings are only accepted between 2 cm and 400 cm.
- Power the board through USB or the VIN pin.
