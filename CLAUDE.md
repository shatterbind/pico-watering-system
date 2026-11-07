# CLAUDE.md

C firmware (Pico SDK 2.2.0) for a Raspberry Pi Pico 2 (RP2350) plant watering controller. All application logic is in `main.c`.

## Architecture
- Duty-cycled. Each DS3231 alarm wakes the chip from the `powman` power-off state on GP15, and the wake is a **full reboot**, so `main()` runs once per wake and then powers off. Nothing in RAM survives between wakes.
- Core 1 (`core1_entry`) receives the humidity value as a float bit-cast through the multicore FIFO, runs the pump, then pushes `1` back. Core 0 drives the e-paper display at the same time and waits for that `1` before powering off.
- The RTC is set only when it is behind the compile time. Setting it unconditionally would roll the clock back on every wake.
- `USE_RELAY_GND_ON` is a value (`true`/`false`), so test it with `#if`, never `#ifdef`. The pump pin is latched to "off" before `gpio_set_dir` so the relay does not glitch on at boot.

## Dependencies (git submodules in `lib/`, by the same author)
- `lib/pico-ds3231`: DS3231 RTC driver (I2C0, SDA 21 / SCL 20)
- `lib/pico-epd-driver`: e-paper driver. The project uses `common`, `epd_2_13` and `canvas`. Its pins are defined in `epd_2_13.h` (DC 8, CS 9, CLK 10, MOSI 11, RST 12, BUSY 13).
- After cloning, run `git submodule update --init`. To move them to the latest upstream, run `git submodule update --remote --merge && git add lib` (see the README).

## Build
`cmake -S . -B build -G Ninja && cmake --build build`. This needs the Pico VS Code extension toolchain in `~/.pico-sdk`. Do not edit the "DO NOT EDIT" block in `CMakeLists.txt`.

## Conventions
- "pomp" means pump; keep the existing identifiers.
- 2-space indent, clang-format style.
- There are no tests. Hardware behavior can only be verified on the device.
