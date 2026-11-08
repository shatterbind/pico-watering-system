---
name: build-firmware
description: Build the pico-watering-system firmware (build/pico_watering_system.uf2) from a plain shell with the Pico VS Code extension toolchain, and report compiler warnings and errors. Use when asked to build, compile, rebuild, check that the code compiles, or produce the .uf2 after changing main.c, CMakeLists.txt or a library in lib/.
---

# Build the firmware

The toolchain comes from the Raspberry Pi Pico VS Code extension in `~/.pico-sdk`. It isn't on `PATH` in a plain shell, so add it for the build command.

## Steps

1. Build (configures automatically if `build/` is missing or CMakeLists.txt changed):

   ```bash
   export PATH="$HOME/.pico-sdk/ninja/v1.13.2:$HOME/.pico-sdk/cmake/v4.3.4/bin:$PATH"
   [ -f build/build.ninja ] || cmake -S . -B build -G Ninja
   cmake --build build 2>&1 | grep -iE 'warning|error|undefined reference|cannot find' ; ls -la build/pico_watering_system.uf2
   ```

   If the versioned folders don't exist, list `~/.pico-sdk/cmake` and `~/.pico-sdk/ninja` and use what is there.

2. To see warnings in `main.c` again after a clean build, `touch main.c` and rebuild: Ninja only recompiles changed files.

3. Report: warnings/errors (with file:line), and the `.uf2` size and timestamp.

## Rules

- A successful build is **not** a hardware test. Never say a change "works" on the device; the user flashes it (hold BOOTSEL, copy the `.uf2`) and checks the real display.
- `CMakeLists.txt` pins SDK 2.2.0 / GCC 14_2_Rel1 / picotool 2.2.0-a4. Don't edit its "DO NOT EDIT" block.
- Link error `cannot find -l<name>`: a misspelled library in `target_link_libraries`.
- Text silently missing on the display: the font isn't enabled in `CANVAS_SELECT_FONTS` (`USE_FONT_TAHOMA_*`) in `CMakeLists.txt`.
