# CLAUDE.md

C firmware (Pico SDK 2.2.0) for a Raspberry Pi Pico 2 (RP2350) plant watering controller. All application logic is in `main.c`. README.md documents the defines and behavior for users; keep it in sync when they change.

## Architecture
- Duty-cycled. A DS3231 Alarm 1 wakes the chip from the `powman` power-off state (LOW level on GP15). The wake is a **full reboot**: `main()` runs once per wake, then powers off. Nothing survives in RAM between wakes.
- Boot order matters: the pump pin is set up first in `main()`, with `gpio_put(POMP_PIN, POMP_OFF_LEVEL)` **before** `gpio_set_dir`, otherwise an active-LOW relay pulses on at boot. `USE_RELAY_GND_ON` is a value (`true`/`false`): test it with `#if`, never `#ifdef`.
- RTC: `setup_rtc()` writes the compile time only when the oscillator-stop flag is set or the RTC is behind the compile time, and never after a failed read. Writing it unconditionally would roll the clock back on every wake.
- Schedule: `schedule_next_alarm()` arms Alarm 1 every boot for the current hour (rounded down) + `WAKE_INTERVAL_DAYS`/`WAKE_INTERVAL_HOURS`, date+h+m+s match, with an hourly fallback if the RTC can't be read. The interval must stay < 28 days (date match; `_Static_assert`). Alarm 2 is unused and disabled.
- Watering: `read_soil()` averages `SENSOR_SAMPLES` and flags a fault on low raw value or large spread. Core 0 decides once (`water = soil.ok && humidity < MIN_HUMIDITY`) and pushes the pump run time in ms (0 = off) to core 1 (`core1_entry`) through the multicore FIFO. Core 1 runs the pump and pushes `1` back; core 0 refreshes the display meanwhile and waits for that `1` before powering off. The display status must reflect what was done (`WATERED`, `OPTIMAL`, `SENSOR ERR`), not the intention.
- VSYS is read on GP29/ADC3 (×3 divider) **before** the pump starts. Battery bars use single-cell Li-ion thresholds; above `VSYS_USB_V` it shows `USB`.
- Pump guards in `decide_pomp()` (only when the soil is dry): no pump on a brown-out boot (`SKIP_POMP_AFTER_BROWNOUT`, status `SKIP: BOR`, prevents a pump-start → brown-out → reboot loop) and none below `MIN_POMP_VSYS_V` (status `LOW BATTERY`). VSYS and the reset reason are therefore read before the decision. No pump interval guard exists; the wake interval is the only limit.
- Reset reason: `read_reset_reason()` reads `powman_hw->last_swcore_pwrup` (1 = GPIO wake source 0 = normal alarm wake; 0 = chip reset) and `powman_hw->chip_reset` (`HAD_BOR`, `HAD_POR`, ...). It's printed on every boot and, if not a normal wake, shown as a short red code right-aligned on the footer time line (codes ≤ 3 letters: the time can end at x 84).

## Display
- Canvas: size with `EPD_2_13_CANVAS_SIZE` (or the equivalent `CANVAS_BUFFER_SIZE`), never `W*H/4`: rows are byte-padded (31 bytes × 250 rows, 2 bpp).
- The background is `lib/pico-epd-driver/src/canvas/images/watering_template.h` (+ `assets/watering_template.png`). It contains only card frames, icons, the bar frame and the battery outline, **no text**. All text is drawn by `main.c`.
- Card geometry (template y): header separator 20, plant 24–76, soil moisture 81–164 (bar 137–144, ticks at x 44/76), temperature 170–201 (thermometer 174–196), footer 207–246. The `SLOT_*` / `LABEL_*` / `MOISTURE_BAR_*` / `BATTERY_*` defines must match the template; when moving something, update the template PNG, the header and `scripts/generate_watering_template.py` together.
- Fonts: one family (driver Tahoma bitmap) via `FONT_SMALL`/`FONT_MEDIUM`/`FONT_LARGE` (Tahoma 8/12/16). Capitals start 1 px below `y` and are 8/12/16 px high. Fonts are opt-in: each needs `USE_FONT_*` in `CANVAS_SELECT_FONTS` in `CMakeLists.txt`, or the text is silently not drawn. No degree glyph (drawn as a ring in `draw_temperature`).
- Use `draw_text` (no background) on the white card areas; `draw_text_with_bg` fills the whole glyph cell and cuts into neighboring text or borders. Exceptions: the header title (it covers the template's red dot) and the battery text.
- Check text fits: `canvas_get_text_width()`. Status text ≤ `STATUS_MAX_WIDTH` (73 px), header title ≤ ~58 px.
- Python isn't installed on the dev machine: edit the template with the `epd-dashboard` skill's `template.js` (2 bits per pixel, MSB-first, BLACK=0 WHITE=1 YELLOW=2 RED=3, 31 bytes per row). It keeps the PNG and header in sync and the header's format (`// Row N` comments, uppercase hex).

## Dependencies (git submodules in `lib/`, by the same author)
- `lib/pico-ds3231`: DS3231 RTC driver on I2C0. GP20 = SDA and GP21 = SCL (the hardware fixes these roles; the `I2C_SCL_PIN`/`I2C_SDA_PIN` names in `main.c` are swapped but both pins just get the I2C function). Every I2C call has a timeout (`DS3231_I2C_TIMEOUT_US`).
- `lib/pico-epd-driver`: e-paper driver (`common`, `epd_2_13`, `canvas`), fonts, dashboard template and generator script. Pins in `epd_2_13.h` (DC 8, CS 9, CLK 10, MOSI 11, RST 12, BUSY 13). Busy-wait functions return `false` on timeout.
- Both submodules are currently checked out on their `fixes` branch, while `.gitmodules` tracks `main`. Commit and push inside a submodule before committing its new pointer here.
- After cloning: `git submodule update --init`. To move to the latest upstream: `git submodule update --remote --merge && git add lib`.

## Build
`cmake -S . -B build -G Ninja && cmake --build build`. Needs the Pico VS Code extension toolchain in `~/.pico-sdk` (SDK 2.2.0, GCC 14_2_Rel1, picotool 2.2.0-a4). In a plain shell, put `~/.pico-sdk/cmake/v4.3.4/bin` and `~/.pico-sdk/ninja/v1.13.2` on `PATH`. Do not edit the "DO NOT EDIT" block in `CMakeLists.txt`.

## Project skills (`.claude/skills/`)
- `build-firmware`: build with the right `PATH` and report warnings.
- `epd-dashboard`: `scripts/preview.js` renders what `main.c` draws (reads its defines; mirrors its drawing code, so update it with that code), `scripts/template.js` shows/clears/moves template regions and checks PNG == header.

## Conventions
- "pomp" means pump; keep the existing identifiers.
- 2-space indent, LLVM style from `.clang-format`. Format with the clang-format bundled in the VS Code C/C++ extension (`~/.vscode/extensions/ms-vscode.cpptools-*/LLVM/bin/clang-format.exe -i main.c`). `lib/.clang-format` disables formatting for the submodules, which use 4 spaces and their own brace style.
- There are no tests. Hardware behavior can only be verified on the device; the user checks layout by photographing the real display.
