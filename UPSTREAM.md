<p align="right"><a href="UPSTREAM.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Upstream and port

- Source: `ZyoungInc/JC4880P443C_BSP`, `wp7` branch, commit
  `9d1743a9444a8d1fe9226e24c7f2b930e22f8612`.
- Original UI file: `main/main.c`, copied to `main/wp7_ui.c`; its SPDX header
  remains in place. The repository's default `main` branch runs
  `lv_demo_widgets()` and is not the WP7 source.
- Preserved: original WP7 screen objects, themes, list and settings pages,
  transitions, and NVS state.
- Replaced: ESP32-P4 MIPI-DSI/PSRAM/touch BSP with Passport ESP32-C3
  ST7789P3 SPI display, two 20-line internal-RAM LVGL buffers, and three ADC
  keys.
- Adjusted for 240 × 320: font sizes, grid tile size, settings vertical scale,
  and default blue theme. Added a key focus outline and key-to-UI action bridge.
- Changed LVGL's refresh period from 33 ms to the upstream 15 ms. Animation
  speed and fast animations remain controlled by the upstream settings page.
- Increased LVGL's memory pool from 64 KiB to 96 KiB. The original settings
  exit animation completes from both tiles and the app list with this pool.
- Sized Passport settings labels to their 16/22 px font line heights and made
  the palette smaller so sliders and swatches no longer cover text.
- Slider and switch knobs stay inside their controls at both ends of the
  range. They were centred on the indicator's end, so at the minimum and
  maximum half a knob hung outside the control and its focus outline. The
  settings rows are slightly tighter so the last switch and its outline fit
  above the bottom of the 320 px screen.
- Replaced numbered tiles and demo list entries with named applications; added
  Passport pages and the onboard CW2017 battery gauge. The former AI Usage
  entry is now two pages, Kaboo token usage and Claude quota, fed over
  Bluetooth LE. They take the first two home tiles; Focus moved to the app
  list only.
- The original demo is not a phone OS; the Passport build adds functional
  pages for the six non-settings app rows.
- App pages open and close with the upstream UI Settings transitions from
  their tile or list row instead of appearing at once. Each page is a title
  and full-width rows that the transitions move like the settings controls;
  quota bars fill once the page is in. At the default speed a page opens in
  0.9 to 1.1 s and closes in 1.1 to 1.2 s. The key focus outline is hidden
  while any page opens.
- Transitions overlap neighboring items: each starts a quarter unit after the
  previous one (an eighth with fast animations) instead of after it finishes,
  and the clicked-tile and title phases are shorter. At the default speed UI
  Settings opens in about 1.3 s instead of 3.4 s and the app list in 0.8 s
  instead of 2.4 s. Keys are ignored while a transition runs.
- The app-list length used by transitions is fixed. Upstream recounted it from
  the live list page: after the list-to-tiles transition destroyed the list it
  counted 9 rows instead of 7, stepped back into the slide-out phase and
  accessed deleted rows.
- Keys report a click on release instead of after the double-click window, a
  hold is 0.8 s, and OK sinks the selected tile or row while held using the
  upstream touch press animation. Settings are written to NVS one second after
  the last change instead of on every press.

## Source and license boundaries

- `main/usage_model.*`, `main/usage_link.*`, `tools/usage_bridge.py` and
  `tests/test_usage_model.c` come from
  [klchai/ai-passport-liquid-glass-ui](https://github.com/klchai/ai-passport-liquid-glass-ui)
  (MIT License, FoloToy; same text as `LICENSES/FoloToy-MIT.txt`). The only
  change is in `usage_link.c`: the time base for payload validation is the
  last packet accepted during this boot instead of that project's `time_sync`
  module, so Wi-Fi and SNTP are not linked.

- `components/passport_bsp/` is a reduced, adapted copy of
  [`components/bsp/` from FoloToy/ai-passport](https://github.com/FoloToy/ai-passport).
  The checked base revision was `1051209d807fb26f943236b7e02281f13d39bc90`.
  Its MIT License is preserved in [`LICENSES/FoloToy-MIT.txt`](LICENSES/FoloToy-MIT.txt).
- The upstream file adapted into `main/wp7_ui.c` carries
  `SPDX-License-Identifier: Apache-2.0`; the license text is in
  [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt). The upstream `wp7`
  branch does not contain a root `LICENSE` file, so that file-level header
  should not be described as a repository-wide license grant.
- Display and button sources in the BSP retain their original `trae_card`
  porting comments; no separate `trae_card` source is vendored here. LVGL and
  Espressif components are fetched by the component manager and are not committed.
- A license for this repository's newly written code has not been selected.
  The copies under `LICENSES/` preserve source notices; they do not license
  this entire new repository.
