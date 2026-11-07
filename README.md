# pico-watering-system

Low-power automatic plant watering controller for the **Raspberry Pi Pico 2 (RP2350)**, written in C on the Pico SDK.

The board spends almost all its time powered off. A DS3231 RTC alarm wakes it. It then reads a capacitive soil-moisture sensor, runs the pump if the soil is too dry, refreshes a 2.13" e-paper display, and powers itself off again.

## How it works

1. **Wake.** The DS3231 alarm pulls its `INT/SQW` line low on `GP15`, and the RP2350 `powman` GPIO wake brings the chip out of the power-off state. This is a full reboot, so `main()` runs from the top.
2. **RTC.** The RTC time is set from the firmware compile time only if the RTC is behind it (new module or dead coin cell). Alarm 1 is (re)armed and interrupt mode is enabled.
3. **Measure.** It reads soil moisture on `ADC0 / GP26` and maps it to 0–100 % using `AIR_VALUE` and `WATER_VALUE`. It also reads the on-chip temperature sensor.
4. **Water.** The moisture value goes to **core 1**. If it is below `MIN_HUMIDITY`, the relay switches on for `TIME_POMP_ON` ms. Meanwhile core 0 redraws the e-paper display.
5. **Sleep.** Once the pump finishes and the alarm flags are cleared, the board enters the `powman` power-off state.

The e-paper display shows the last-update date and time, moisture in %, the raw ADC value and the chip temperature.

## Hardware

| Part | Notes |
|---|---|
| Raspberry Pi Pico 2 | `PICO_BOARD pico2` |
| DS3231 RTC module | I2C0, with a backup coin cell |
| 2.13" SPI e-paper | Driver: [pico-epd-driver](https://github.com/shatterbind/pico-epd-driver) |
| Capacitive soil-moisture sensor | Analog output |
| Relay module or MOSFET + flyback diode | Drives the pump |
| Small DC pump + external supply | Never power the pump from the Pico |

## Pinout

| Signal | Pico pin |
|---|---|
| Soil sensor AOUT | GP26 (ADC0) |
| Pump relay IN | GP22 |
| DS3231 SDA | GP21 |
| DS3231 SCL | GP20 |
| DS3231 INT/SQW (wake) | GP15 |
| EPD DC | GP8 |
| EPD CS | GP9 |
| EPD CLK | GP10 |
| EPD MOSI (DIN) | GP11 |
| EPD RST | GP12 |
| EPD BUSY | GP13 |

All grounds, including the pump supply's ground, must be common.

## Configuration (`main.c`)

| Define | Default | Meaning |
|---|---|---|
| `AIR_VALUE` / `WATER_VALUE` | `4095` / `1100` | Raw ADC readings with the sensor in dry air and in water. Calibrate these for your sensor (see `DEBUG`). |
| `MIN_HUMIDITY` | `40` % | Water when moisture is below this value. |
| `TIME_POMP_ON` | `5000` ms | How long the pump runs per wake. |
| `POMP_PIN` | `22` | Relay control pin. |
| `USE_RELAY_GND_ON` | `true` | `true`: the relay is active-LOW. `false`: it is active-HIGH. |
| `CONFIGURE_RTC` | `1` | Sets the RTC from compile time when the RTC is behind. |
| `CONFIGURE_ALARM_1` | `1` | Alarm 1: triggers when minutes and seconds match `00:00`, i.e. **once per hour**. |
| `CONFIGURE_ALARM_2` | `0` | Optional alarm 2 (minutes match). |
| `COMPILE_UPLOAD_OFFSET_SECONDS` | `6` | Covers the time between compiling and the board booting. |
| `DEBUG` | off | When enabled, the firmware loops forever and prints raw and % moisture over USB/UART. Use this to calibrate. |

## Building

The project uses the Raspberry Pi Pico VS Code extension's toolchain (SDK 2.2.0, ARM GCC 14.2).

```sh
git clone --recurse-submodules https://github.com/shatterbind/pico-watering-system
# or, in an existing clone:
git submodule update --init

cmake -S . -B build -G Ninja
cmake --build build
```

Flash `build/pico_watering_system.uf2` by copying it to the board in BOOTSEL mode, or run `picotool load -x`.

## Safety

- Use a relay or MOSFET with a flyback diode for the pump, and supply the pump separately.
- Check that your relay module accepts a 3.3 V logic input.
- Test with the pump out of the water first, and keep the electronics away from water.

## License

MIT. See [LICENSE](LICENSE).
