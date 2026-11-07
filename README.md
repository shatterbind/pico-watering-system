# pico-watering-system

Simple Raspberry Pi Pico W automatic watering controller example. Includes wiring diagrams (ASCII + Mermaid) and sample MicroPython code to read a soil moisture sensor and drive a pump through a relay.

## Parts
- Raspberry Pi Pico W
- Relay module (5V) or MOSFET + flyback diode if using DC pump
- Small water pump (use appropriate voltage/current supply)
- Soil moisture sensor (analog)
- External 5V power supply for pump/relay
- Jumper wires, breadboard

## Wiring (summary)
- Never drive the pump directly from the Pico. Use a relay or transistor and an external power supply.
- Connect grounds: Pico GND and external power supply GND must be common.

Example pin mapping used in the sample code:
```
Pico W GP15  -> Relay input (active LOW or HIGH depending on module)
Pico W GP26  -> Soil moisture analog input (ADC0)
Pico W GND   -> Common ground
External 5V   -> Pump and relay VCC (relay VCC may need 5V)
Relay GND     -> Common ground
Pump +        -> External 5V
Pump -        -> Relay NO or transistor MOSFET drain
```

## Wiring table
| Component         | Pico pin / Supply       |
|------------------:|:-----------------------|
| Relay IN          | GP15                   |
| Relay VCC         | +5V (external)         |
| Relay GND         | GND (common)           |
| Pump +            | +5V (external)         |
| Pump -            | Relay NO               |
| Soil sensor Vout  | GP26 (ADC0)            |
| Soil sensor VCC   | 3.3V (or sensor spec)  |
| Soil sensor GND   | GND (common)           |

## ASCII wiring diagram
```
[Pico W]                 [Relay module]         [Pump]      [Power]
    GP15  --- Relay IN
    GP26  --- Soil Vout (ADC)
    3V3   --- Soil VCC
    GND  ------------------------ GND ---------- GND
                                                         Relay VCC --- +5V (external)
Pump + ----------------------- +5V (external)
Pump - ---------------- Relay NO (or transistor)
```

## Mermaid circuit overview (rendered where supported)
```mermaid
flowchart LR
    Pico[Pico W]
    Relay[Relay Module]
    Pump[Water Pump]
    Sensor[Soil Moisture Sensor]
    PS[External 5V Supply]

    Pico -->|GP15| Relay
    Pico -->|GP26 (ADC)| Sensor
    Sensor -->|VCC| Pico
    Pico -->|GND| Relay
    PS -->|+5V| Relay
    PS -->|+5V| Pump
    Relay -->|NO| Pump
    Pico -->|GND common| PS
```

## MicroPython example
Save as `main.py` on the Pico W. Adjust pin numbers to match your wiring and relay logic (active LOW vs HIGH).
```python
from machine import Pin, ADC
import time

RELAY_PIN = 15      # GP15
SOIL_ADC_PIN = 26   # GP26 / ADC0
WATER_THRESHOLD = 2000  # calibrate for your sensor (0-65535 on RP2 port)

relay = Pin(RELAY_PIN, Pin.OUT)
soil = ADC(SOIL_ADC_PIN)

def pump_on():
        # if your relay is active LOW, write 0 to turn on
        relay.value(0)

def pump_off():
        relay.value(1)

def read_soil():
        return soil.read_u16()

try:
        pump_off()
        while True:
                moisture = read_soil()
                print("Soil:", moisture)
                if moisture < WATER_THRESHOLD:
                        print("Soil dry -> watering")
                        pump_on()
                        time.sleep(5)  # pump run time (seconds)
                        pump_off()
                time.sleep(10)
except KeyboardInterrupt:
        pump_off()
```

## Safety & notes
- Verify relay module voltage requirements (some require 5V VCC and a 3.3V logic input; others may not).
- Use flyback diodes for inductive loads, fuses, and proper insulation for water and mains.
- Calibrate WATER_THRESHOLD for your soil sensor and environment.
- Test with the pump disconnected from water to verify operation before plumbing.

That's all — adjust pins and timing to your needs.