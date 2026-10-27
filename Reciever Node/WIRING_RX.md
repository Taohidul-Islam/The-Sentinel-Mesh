# Wiring: Receiver

Board: ESP32 DOIT DevKit V1. Pin numbers are GPIO numbers, not the silkscreen D-numbers.

The receiver has an OLED screen, a buzzer, an alert LED and one button.

## Connections

| Part | ESP32 pin | Notes |
|------|-----------|-------|
| OLED SH1106 1.3" SDA | GPIO26 | I2C |
| OLED SH1106 1.3" SCL | GPIO27 | I2C |
| OLED VCC / GND | 3.3V / GND | |
| Buzzer | GPIO22 | Through an NPN transistor, pin goes to the base |
| Alert LED | GPIO23 | Through an NPN transistor, pin goes to the base |
| Button | GPIO19 | Cycles the OLED screens |
| Reset | GPIO13 | Hold on boot to wipe saved settings |
| Status LED | GPIO2 | Onboard LED |

## Diagram

```
   OLED SH1106            ESP32
  +----------+         +---------+
  | VCC      |---------| 3V3     |
  | GND      |---------| GND     |
  | SDA      |---------| GPIO26  |
  | SCL      |---------| GPIO27  |
  +----------+         |         |
                       | GPIO19  |---- Button ---- GND
                       | GPIO13  |---- Reset ----- GND
                       | GPIO2   |---- Status LED (onboard)
                       |         |
                       | GPIO22  |---- Buzzer driver (NPN base)
                       | GPIO23  |---- LED driver (NPN base)
                       +---------+
```

## Buzzer and LED driver

The buzzer and the LED are not driven straight from the ESP32 pin. Each one goes through its own NPN transistor, with the GPIO connected to the base.

```
                 3.3V / 5V
                    |
              +-----+------+
              | Buzzer/LED |
              +-----+------+
                    |
                    C
 GPIO ----[R]-----B   NPN
                    E
                    |
                   GND
```

Use one of these circuits for the buzzer and another for the LED.

## Notes

- The base resistor and transistor type are not in my notes. A common small NPN like a 2N2222 or BC547 with a base resistor of around 1k works for this, but check it against your own build.
- The buzzer supply voltage and the button and reset wiring (pull-up or pull-down) are also not recorded. The button and reset are shown going to GND, which is the usual way, but check against how your sketch sets the pin mode.
- Power the board through USB or the VIN pin.
