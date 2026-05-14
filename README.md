# espressopost

Standalone ESP32-S3 firmware for an espresso-grind helper. Reads ambient
climate, logs per-shot feedback (time delta + 1–5 quality stars), and
learns a per-preset grind adjustment from local data. Offline-first.

## Status

**Step 1 of 10 — hardware bringup.** Display + capacitive touch up
under LVGL 9; one screen with a centered number adjustable by an arc
around the rim. No climate sensor, no storage, no model yet.

The full build order lives in the kickoff brief. Each step ends in a
runnable device state; this is the first.

## Hardware target

Waveshare ESP32-S3-Touch-AMOLED-1.75 (the round 466 × 466 AMOLED with
case).

- **MCU:** ESP32-S3R8 (8 MB OPI PSRAM, 16 MB QIO flash)
- **Display:** CO5300 AMOLED over QSPI (SPI2_HOST)
- **Touch:** CST9217 over I²C (shared bus with PMIC/RTC/IMU)
- **PMIC:** AXP2101 (display rail gated by PMIC defaults — works at
  power-on without configuration; we'll drive it later for sleep)
- **RTC:** PCF85063 (wall-clock time source for shot records)
- **IMU:** QMI8658 (planned wake-on-motion)
- **Audio:** ES7210 dual-mic codec (unused in v1)
- **Expansion:** 8-pin header (3 GPIO + 1 UART) — planned BME280
  climate sensor lives here

Pin map is verbatim from Waveshare's reference: see
[`main/board_pins.hpp`](main/board_pins.hpp).

## Toolchain

- ESP-IDF ≥ v5.5, installed at `~/esp/esp-idf`
- LVGL 9.x, pulled by the component manager
- C++17

### System prerequisites

ESP-IDF expects `cmake` and `ninja` on PATH — it doesn't bundle them. On macOS:

```sh
brew install cmake ninja
```

### One-time IDF install (if `idf.py` isn't on PATH)

```sh
mkdir -p ~/esp && cd ~/esp
git clone --recursive --depth 1 -b v5.5 https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3                     # ~10 min, ~3 GB; toolchain → ~/.espressif/
```

`~/.zshrc` defines a `get_idf` alias that puts `idf.py` and the xtensa
toolchain on PATH for the current shell. Open a fresh terminal after
install so the alias is loaded.

## Build

```sh
get_idf                                  # makes idf.py available in this shell
cd /Users/itaykeren/Code/GitHub/espressopost

# First time only:
idf.py set-target esp32s3

# Pull managed components (LVGL, CO5300 driver, CST9217 driver):
idf.py reconfigure

idf.py build
```

If clangd in your editor shows red squigglies on `esp_err_t`, `lv_*`, etc.
before the first build — that's expected; clangd has no project context
until `idf.py reconfigure` generates `build/compile_commands.json`. After
the first reconfigure the editor diagnostics clear. Clangd will still
warn about a few xtensa-gcc-only flags (`-mlongcalls`, `-fstrict-volatile-bitfields`,
etc.); those are cosmetic and don't affect the build — to silence them,
add a `.clangd` file with `CompileFlags: { Remove: [-mlongcalls, ...] }`
when it gets annoying.

### Changing build config after first build

`sdkconfig.defaults` is only consulted when there's no `sdkconfig` to
load from. After the initial build, edits to `sdkconfig.defaults` will
**not** take effect on rebuild. To apply changes:

- Quick: `rm sdkconfig && idf.py build` — regenerates from defaults.
- Interactive: `idf.py menuconfig` — changes write directly to `sdkconfig`.

## Flash & monitor

```sh
# Find the port: ls /dev/cu.usbmodem* (macOS) or /dev/ttyUSB* (Linux).
idf.py -p /dev/cu.usbmodem<N> flash monitor
```

Exit the monitor with `Ctrl+]`.

## What to verify on this build

1. Boot log shows: `display: display + LVGL ready (466x466, 50-line partial buffer)` and `touch: CST9217 touch ready`.
2. Screen shows the number **50** centered on a black background.
3. An amber arc traces three-quarters of the rim with a gap at the
   bottom; a knob sits where the arc meets the indicator.
4. Dragging the knob around the rim with a finger changes the number
   smoothly between 0 and 100.
5. No tearing / artifacts at the arc edges as you drag.

If any of these fail, the most likely culprits in order:

- **Black screen, no log:** wrong toolchain (sourced an older IDF), or
  `idf.py set-target esp32s3` was skipped.
- **Log says display ready but screen stays black:** AXP2101 isn't
  enabling the LCD rail (Waveshare's PMIC defaults usually do this, but
  if your unit shipped with non-default PMIC state, we'll need to
  initialize the PMIC explicitly — flag this and we'll add it).
- **Screen shows but no touch:** wrong CST9217 address (verify `0x5A`
  against your unit's schematic), or `kI2cSda`/`kI2cScl` pins differ on
  a revision.
- **Drag works on phantom location only:** swap_xy / mirror flags need
  flipping in `touch.cpp::init_panel`.

## What's deliberately NOT here yet

- AXP2101 PMIC driver (board powers up with sensible defaults; we'll
  add this when we implement sleep/wake).
- AMOLED burn-in mitigation (dim/sleep policy is still an open decision).
- PCF85063 RTC, QMI8658 IMU drivers.
- BME280 climate sensor.
- LittleFS, shot records, presets.
- The model.
- The Report screen and idle screen.

Each is its own step in the brief's build order. See the project's
memory notes for design decisions already locked in.

## Layout

```
.
├── CMakeLists.txt              top-level project
├── partitions.csv              custom: nvs + factory (4 MB) + littlefs (~12 MB)
├── sdkconfig.defaults          PSRAM, flash, partition table, FreeRTOS tick
├── main/
│   ├── app_main.cpp            init display → init touch → show bringup screen
│   ├── board_pins.hpp          verbatim from Waveshare's reference repo
│   ├── idf_component.yml       managed deps: lvgl, CO5300, CST9217
│   └── CMakeLists.txt
└── components/
    ├── display/                CO5300 QSPI + LVGL display binding + LVGL task
    ├── touch/                  CST9217 I²C + LVGL pointer indev
    └── ui/                     bringup screen (will be replaced as real
                                screens land in later steps)
```

Components for `sensors/`, `storage/`, `model/`, `presets/`, `grinder/`
are intentionally NOT scaffolded yet — they'll be added in their
respective build-order steps so the tree only contains live code.
