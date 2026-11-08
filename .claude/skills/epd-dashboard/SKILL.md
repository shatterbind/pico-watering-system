---
name: epd-dashboard
description: Preview and change the 2.13" e-paper dashboard of pico-watering-system without hardware or Python. Renders what the firmware draws (template + text from main.c defines) to a PNG, and edits the background template (watering_template.h + .png) pixel-exactly. Use when changing text, positions, fonts, cards or icons on the display, when the user sends a photo of the display with layout problems, or before telling the user a layout change is done.
---

# E-paper dashboard: preview and template edits

The display is 122×250, 2 bits per pixel (BLACK=0, WHITE=1, YELLOW=2, RED=3). The background comes from `lib/pico-epd-driver/src/canvas/images/watering_template.h`, with `assets/watering_template.png` as its image. `main.c` draws all text and values on top of it. Scripts are Node.js with no dependencies (Python is not installed).

## Preview what the firmware draws

```bash
node .claude/skills/epd-dashboard/scripts/preview.js --state watered     # or optimal / error
node .claude/skills/epd-dashboard/scripts/preview.js --state error --vsys usb --out build/preview_error.png
```

Options: `--moisture 34 --temp 21.1 --vsys 3.92|usb --time "11-08 01:16" --scale 3 --out <png>`. Default output `build/dashboard_preview.png` (3× scale). Then Read the PNG to look at it.

- It reads all `SLOT_*`, `LABEL_*`, `*_TEXT`, font, bar, battery and interval defines from `main.c`, so define changes show up directly.
- It **mirrors** the drawing code in `main.c` (dashboard part of `main()`, `draw_battery`, `draw_temperature`, `draw_text_centered`). When that code changes (new element, different color, new helper), update `preview.js` in the same change.
- It prints the same width warnings as the firmware (status wider than `STATUS_MAX_WIDTH`, title too wide).
- Render the states the change affects (usually `watered` and `error`, which has the widest status text) before reporting.
- A preview is not the real panel. Say so; the user confirms with a photo of the display.

## Edit the background template

```bash
S=.claude/skills/epd-dashboard/scripts
node $S/template.js show 120 145            # ASCII rows (x 0..121); optional x range: show 0 14 60 121
node $S/template.js bbox 8 155 38 198       # where an icon/text actually is
node $S/template.js clear 10 29 39 35       # white out a region
node $S/template.js move 12 124 109 131 0 13  # move a region by (dx, dy)
node $S/template.js check                   # PNG == header?
```

`clear` and `move` rewrite the header bytes in place (keeping its `// Row N` comments and uppercase hex) and regenerate the PNG from the header, so both stay identical. Always finish with `check`.

Workflow for a layout change:
1. `show`/`bbox` the area, and include a pixel or two of margin. Card borders are 1 px with rounded corners (radius 4); don't cut them.
2. `clear`/`move` in the template.
3. Update `lib/pico-epd-driver/scripts/generate_watering_template.py` to the same coordinates, so a later regeneration doesn't undo it.
4. Update the matching defines in `main.c` (`SLOT_*`, `LABEL_*`, `MOISTURE_BAR_*`, `BATTERY_*`) and the card geometry list in `CLAUDE.md`.
5. `preview.js`, look at the result, then build (build-firmware skill).

## Text and fonts

- Fonts: `FONT_SMALL`/`FONT_MEDIUM`/`FONT_LARGE` = Tahoma 8/12/16 (proportional). Capitals start 1 px below `y` and are 8/12/16 px high. A font must be enabled in `CANVAS_SELECT_FONTS` in `CMakeLists.txt`, otherwise its text is silently not drawn.
- Widths: `textWidth()` in `scripts/lib.js` gives the same result as `canvas_get_text_width()`. Space is 3/4/5 px; there is no degree glyph (drawn as a ring).
- Use `draw_text` (no background) on card areas; `draw_text_with_bg` fills the whole glyph cell and can cut into neighbors.
- Keep 1–2 px between text and card borders or the moisture bar; leave ~2 px between stacked lines.

## Committing

The template and fonts live in the `lib/pico-epd-driver` submodule: commit there first (and push before the user clones elsewhere), then commit the new submodule pointer in this repo.
