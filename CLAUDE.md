# CLAUDE.md

C firmware (Pico SDK 2.2.0) for a Raspberry Pi Pico 2 (RP2350) plant watering controller. All application logic is in `main.c`.

## Architecture
- Duty-cycled. Each DS3231 alarm wakes the chip from the `powman` power-off state on GP15, and the wake is a **full reboot**, so `main()` runs once per wake and then powers off. RAM does not survive between wakes; POWMAN `scratch[6..7]` does (it holds the last pump run time, in minutes).
- Core 0 decides whether to water. Core 1 (`core1_entry`) receives the pump run time in ms (0 = don't run) through the multicore FIFO, runs the pump, then pushes `1` back. Core 0 refreshes the e-paper display at the same time and waits for that `1` before powering off.
- The pump fails safe and does not run on a sensor fault (low raw value or large sample spread), an RTC read failure, an alarm flag that won't clear (that would cause a wake loop), or if it ran within `MIN_POMP_INTERVAL_MIN`.
- The RTC is set only if its OSF (oscillator stop) flag is set or it is behind the compile time. Setting it unconditionally would roll the clock back on every wake. The alarm flags are cleared early so INT/SQW releases the wake line.
- `USE_RELAY_GND_ON` is a value (`true`/`false`), so test it with `#if`, never `#ifdef`. The pump pin is initialized first in `main()` and latched to "off" before `gpio_set_dir`.
- Size the canvas with `EPD_2_13_CANVAS_SIZE`, never `W*H/4`: rows are byte-padded (31 bytes × 250 rows).

## Dependencies (git submodules in `lib/`, by the same author)
- `lib/pico-ds3231`: DS3231 RTC driver on I2C0. GP20 = SDA and GP21 = SCL (the hardware fixes these roles). Every I2C call has a timeout (`DS3231_I2C_TIMEOUT_US`).
- `lib/pico-epd-driver`: e-paper driver. The project uses `common`, `epd_2_13` and `canvas`. Its pins are defined in `epd_2_13.h` (DC 8, CS 9, CLK 10, MOSI 11, RST 12, BUSY 13). The busy-wait functions return `false` on timeout. Fonts are opt-in through `CANVAS_SELECT_FONTS` in `CMakeLists.txt`; add a `USE_FONT_*` there before using a new font.
- After cloning, run `git submodule update --init`. To move them to the latest upstream, run `git submodule update --remote --merge && git add lib` (see the README).

## Build
`cmake -S . -B build -G Ninja && cmake --build build`. This needs the Pico VS Code extension toolchain in `~/.pico-sdk`. Do not edit the "DO NOT EDIT" block in `CMakeLists.txt`.

## Conventions
- "pomp" means pump; keep the existing identifiers.
- 2-space indent, clang-format style. The libraries use 4 spaces.
- There are no tests. Hardware behavior can only be verified on the device.
