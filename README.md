# Read Pico Firmware · Pico OS

**Languages:** [English](./README.md) | [简体中文](./README.zh-CN.md) | [日本語](./README.ja-JP.md)

[Contributing](CONTRIBUTING.md) · [Support](SUPPORT.md) · [Security](SECURITY.md) · [Code of Conduct](CODE_OF_CONDUCT.md)

[![License](https://img.shields.io/github/license/MindReset/read_pico_firmware?style=for-the-badge&logo=apache&logoColor=white)](LICENSE)
[![Build](https://img.shields.io/github/actions/workflow/status/MindReset/read_pico_firmware/build.yml?branch=main&style=for-the-badge&logo=githubactions&logoColor=white)](https://github.com/MindReset/read_pico_firmware/actions)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.1-E7352C?style=for-the-badge&logo=espressif&logoColor=white)
![Target](https://img.shields.io/badge/target-ESP32--S3-E7352C?style=for-the-badge&logo=espressif&logoColor=white)

Read Pico is an ESP32-S3 development board in the Read series with a 4.7" monochrome e-paper panel,
made by Shenzhen MindReset Technology Co., Ltd. This repository provides Pico OS, the firmware
that runs on the board, featuring a 3-page 24-app 3×3 grid launcher, a 5-step interactive first-run
Onboarding Guide, a top pull-down Quick Control Menu, and native apps for books, Wi-Fi/BLE companion
transfer, OPDS/RSS feeds, web reading, TF file management, sketch pad, Pomodoro clock, dashboard,
Agent inbox, and unified system settings.

The board support package, lightweight BLE companion service, PMU protocol host and chip drivers
remain reusable foundations for extending Pico OS.

Agent-facing layout, `app_desc_t` contract, glossary and comment style are in
[AGENTS.md](AGENTS.md).

## Documentation & More Devices

- [Official Read Pico documentation](https://dot.mindreset.tech/docs/read_0)
- [Dot Open Platform](https://github.com/MindReset/dot_open_platform): explore other Dot devices and projects, including Quote/0 hardware resources and Rand/0 local display integration, with firmware examples, pin maps, and enclosure files.

## Hardware

| Item | Specification |
| --- | --- |
| MCU | ESP32-S3, 16 MB flash and 8 MB Octal PSRAM, both at 120 MHz |
| Display | 4.7" monochrome e-paper, 1216 × 684, 16 gray levels, 16-bit parallel interface driven by the LCD peripheral |
| EPD power | SY7636A; PGOOD read through the IO expander |
| PMU | CW32L010 with a custom I2C protocol for battery, charging/discharging, indicator LED, RTC, alarms and power control |
| Touch | CST836U, two touch points, interrupt and deep-sleep wake |
| Accelerometer | SC7A20H, tap, orientation, free-fall and FIFO |
| IO expander | FCA9555, EPD control pins and card detection |
| Storage | TF card over 1-bit SDMMC; fonts loaded from the card |
| Other | Buzzer and three capacitive key zones |

## Build & Flash

For the first move to the dual OTA partition layout, install through the [browser flasher](https://zackerywong233.github.io/read_pico_firmware/) over USB and select erase flash. This rebuilds the partition table and 7 MB internal storage; files on the TF card remain untouched. Later versions can be installed over Wi-Fi from Settings → System Update. See [browser flashing](docs/WEB_FLASHER.md).

Requires ESP-IDF v6.1. `components/read_pico/read_pico_flash_hpm.c` depends on
`esp_flash_chips/spi_flash_override.h`, which is available only in v6.

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

The 120 MHz flash / PSRAM timing in `sdkconfig.defaults` depends on the flash part
installed on the board. CI uses `sdkconfig.ci` with default timing only to check
compilation; see [.github/workflows/build.yml](.github/workflows/build.yml).

If the device is not recognized after waking from sleep during development or
flashing:

1. Try a USB Type-A data cable.
2. Put the device to sleep and wake it again.
3. Reboot the board and retry.

The panel VCOM is calibrated at the factory and stored in the PMU. The firmware
reads it once at boot to configure the driver; it is neither stored locally nor
user-editable.

## Pico OS Apps & Interaction

On first boot, Pico OS opens a 5-step interactive Onboarding Guide covering timezone/clock calibration and core gestures before entering the 3-page 3×3 grid launcher (all 24 apps and hardware diagnostics laid out as `9 + 9 + 6` beneath a live clock and battery status bar). Pulling down from the top screen edge on any non-PMU-exclusive page (or tapping the Home status bar) opens the Quick Control Menu overlay to toggle BLE, global mute, charge LED, pickup wake, auto-sleep, and lock-screen saver style, adjust live reader font size, refresh profile, HUD bars, and orientation, or trigger full GC16 refresh, lock, and Wi-Fi transfer without leaving the current page.

| App | Content |
| --- | --- |
| Books | UTF-8 / GBK `TXT`, `EPUB`, `MD`, and `CBZ` manga from TF or internal storage; Recent Reading card, 3×2 cover grid / compact list, multi-condition filtering, 4 sort modes, pinyin search, and batch management; adjustable size, weight, line spacing, margins, paragraph gap, 2-em indent, and independent top/bottom HUD bars; long-press sentence excerpts and toolbar progress-bar seeking. |
| Transfer & Companion | Upload books and TF `.ttf` fonts over SoftAP or existing Wi-Fi via browser or Companion LAN HTTP API v2, paired with the `ReadPico-XXXX` BLE control channel for Wi-Fi-free time sync, Wi-Fi provisioning, reading progress/notes/stats backup & restore, web bookmarks, and feed configuration. |
| Web Reader & Feeds | Read statically extracted HTTP/HTTPS articles (with LAN browser clipping and synced bookmarks), browse multi-level OPDS 1.2 catalogs with streaming book downloads, and read cached RSS 2.0 / Atom 1.0 feeds. |
| Files, Sketch & Focus | Standalone TF card file manager with safety guardrails, image wallpaper preview, and text preview; continuous-DU handwritten sketch pad with upright PNG export; desk clock and 15/25/45-minute foreground Pomodoro timer. |
| Dashboard, Agent & Settings | Personal Dashboard with date/time, battery, recent book, 26-week reading heatmap, personal note, 5 lock-screen styles, and scheduled PMU wake sync; MQTT/TLS Agent Inbox for paginated text and HTTPS saver images; unified Settings with Network/Time, Display/Reading, Sleep/Saver, Power/Alerts, Onboarding replay, Reset Preferences, and Wi-Fi OTA. |
| Diagnostics (Pages 2–3) | Device Overview, Self-Test, Sleep/Wake, Storage & Buzzer, Font Picker, Power, EPD Refresh, Touch, Built-in Reading, Accelerometer, IMU Lab, PMU Protocol, Key Test, and IO Expander. |

Agent messages use one MQTT JSON object: `{"type":"text","title":"Title","text":"Body"}` or `{"type":"image","url":"https://example.com/image.jpg"}`. Configure the broker, username, password and exact topic on the device's local transfer page. The password is stored unencrypted on device and is never echoed by the webpage. Image delivery requires a mounted TF card and a valid HTTPS certificate.

Physical keys and the bottom touch key-hint strip share one unified OS contract: short `KEY2` runs a full-screen GC16 refresh and holding `KEY2` (500 ms) returns globally to Home Page 1; short `KEY1` / `KEY3` dispatch to the active page first (turning pages in readers and multi-page lists), with unhandled `KEY1` popping the 8-level navigation stack and holding `KEY1` (500 ms) returning directly to the parent page.

## Repository Layout

```text
main/
  app_main.c        Boot wiring, then app_loop
  app/              App interface (app.h), registry and event loop
  apps/             Pico OS apps and system tools, each exporting app_desc_t
  ui/               ui_kit drawing primitives and layout constants; ui_menu two-level menu
  font/             stb_truetype glyph cache
  factory/          Device self-test and factory VCOM calibration
components/
  read_pico/        Board BSP: I2C, EPD definition/timing, TF card, buzzer, flash HPM
  read_pico_pmu/    CW32L010 protocol host
  epdiy/            E-paper renderer, trimmed to the LCD peripheral path
  continuous_du/    Continuous DU with phases accumulated across scans for finger tracking
  cst836u/ sc7a20h/ fca9555/ sy7636a/    Chip drivers
  e0470_epaper_waveform/                 Panel waveform tables and trimming functions
  pwm_audio/        LEDC PWM audio, one buzzer backend
assets/             Image sources for main/assets/*.bin
tools/              Font and image conversion scripts
```

To add a root app, create a page in `main/apps/` and add it to
`main/app/app_registry.c`. Add hardware diagnostics to the tool table in
`main/apps/app_system_tools.c`. The main loop stays untouched.

## Pinout

| Function | GPIO |
| --- | --- |
| I2C SCL / SDA | 40 / 39 (400 kHz) |
| EPD data D0–D15 | 4–18, 45 |
| EPD XLE / XSTL / XCL / SPV / CKV | 3 / 46 / 21 / 47 / 48 |
| FCA9555 INT# | 41 (also a light-sleep wake source) |
| CST836U INT# | 43 |
| SC7A20H INT1 | 1 |
| TF card CLK / CMD / D0 | 38 / 42 / 44 |
| Buzzer | 2 |

EPD power enable, XOE, MODE, VCOM_EN, touch reset and card detection are on FCA9555
Port-0; see the pin table in [main/apps/app_ioe.c](main/apps/app_ioe.c).

## Transfer and limitations

AP and existing WiFi modes provide a QR code for the upload page. AP can switch between joining WiFi and opening the page. The device labels its build timestamp as UTC.

The transfer server runs only on its page and stops on exit. It uses local-network HTTP without separate login or TLS; peers on that network can manage books in the active upload storage, so use a trusted network. Saved WiFi credentials reside in device NVS and are not returned by public endpoints or logs. NVS/flash encryption is not enabled, so this does not provide physical-access protection.

Stopping transfer returns to the entry page or menu position. When a mounted TF card becomes unavailable, affected reading/transfer stops and fonts fall back; explicitly remount from the TF page after reinserting it. Removing a card during writes can damage the filesystem. EPUB opens text with image placeholders. Tap a placeholder to load that local JPEG/PNG in a separate, proportionally scaled preview; tap the preview, the return button, or any of the three keys to return to the same text position without repagination. Chapter opening, page turns and prepainting do not decode images. ZIP entries, manifest items and spine chapters are each limited to 32768; OPF/navigation metadata is limited to 4 MiB uncompressed and chapter/image reads to 2 MiB. The ZIP central directory has an 8 MiB limit; title storage has a 1 MiB budget, with numbered fallback titles beyond it. Entries above 4 MiB are rejected even when unused. These are independent memory/resource limits, so chapter count alone does not guarantee acceptance; ZIP64 and books over 32768 entries or chapters are unsupported. Externally replaced files or another card with the same path and file size may still match old reading progress. Pending retries after failed saves are not guaranteed to survive power loss.

Entering Books offers to resume the previous book; staying on the shelf does not open it (KEY1 cancels; tap “Continue” to resume). The reader parses metadata and the current chapter, then paginates the first two pages or through the saved position. Remaining pagination advances two pages at a time; other chapters load on demand. The page total shows “…” while incomplete; entering the previous chapter at its last page still requires paginating that chapter. Width measurement uses resident font tables without rasterizing a whole chapter. Opening and chapter loading retain waiting hints and visible failure reasons (memory, limits, unsupported format or invalid file).

Swipe page turns commit on release at 64 pixels (previously 120); movement beyond the 24-pixel tap tolerance but below the swipe threshold cancels, and an image-placeholder swipe turns the page without loading the image.

Illustration limits: baseline JPEG favors decode-time scaling with at most 2x enlargement to the display size, up to 16M source pixels and 8192 per side; PNG and progressive JPEG allow up to 1M source pixels and a 4 MiB decoder heap. Output fits 648x1000 grayscale pixels; transparent PNG uses a white background. Only the most recently viewed image in the current chapter is cached; closing a preview and reopening that same image reuses it. Missing, corrupt, oversized or unsupported images show an explanation on request and leave text readable. Repeated references to the same normalized EPUB resource path show “重复图片” and the earliest numbered section among chapters visited during this book opening (EPUB section order includes covers and front matter, so it can differ from printed chapter numbers). Opening image blocks immediately followed by a heading, and recognized references to those same resources, use a separate “标题图” label without the first-location line; other illustrations keep the visited-origin hint. This is not a scan of unread chapters or a claim about the first occurrence in the whole book; the history resets when the book closes. Identical content under different resource paths is not matched. JPEG/PNG references inside SVG wrappers work; pure SVG vectors, CSS backgrounds and remote images do not. See [decoder sources and licenses](main/book/vendor/README.md).

The built-in font covers the UI and demo text only. For external Chinese books, place a complete Chinese TTF in `fonts/` or `assets/fonts/` on the TF card and select it on the Fonts page. The default path is `fonts/ChillDuanSansVF.ttf`; for missing characters, check that the file exists and the selected font covers them. A complete font and its copyright notices are provided in the [TF deployment package](sdcard/README.md). The webpage also accepts fonts into `/sdcard/fonts`: up to 32 MiB per TTF with TrueType outlines, excluding OTF/CFF, TTC and WOFF. Capacity is checked first, replacements require confirmation, and interrupted or invalid uploads retain the old file. Stop transfer, then select the font on the device. Transfer temporarily uses the built-in font to avoid replacing an open font; the saved selection is retained.

See [Changelog](docs/CHANGELOG.md) for feature changes. Offline pinyin data comes from pypinyin under MIT; see the [component license and regeneration notes](components/read_pico_search/README.md).

## Acknowledgments & License

- Firmware: Apache-2.0, see [LICENSE](LICENSE).
- [epdiy](https://github.com/vroland/epdiy): e-paper timing and rendering. This is a
  trimmed fork for the board's LCD path, licensed under LGPL-3.0-or-later.
  Local changes are listed in [components/epdiy/LICENSE](components/epdiy/LICENSE).
- [stb_truetype](https://github.com/nothings/stb): glyph rasterization, public domain.
- [pwm_audio](https://github.com/espressif/esp-iot-solution/tree/master/components/audio/pwm_audio):
  Espressif LEDC PWM audio, trimmed copy, Apache-2.0. See
  [components/pwm_audio/LICENSE](components/pwm_audio/LICENSE).
- Panel waveform tables ship with the board as-is under Apache-2.0. See
  [components/e0470_epaper_waveform/LICENSE](components/e0470_epaper_waveform/LICENSE).
- The built-in font, `main/assets/builtin.ttf`, is a subset of the
  [ChillDuanSans](https://github.com/Warren2060/ChillDuanSans) variable font by
  Warren2060, generated by `tools/gen_builtin_font.py`. The unmodified full font
  and original license are bundled in `sdcard/fonts/`; the modified UI subset
  is named Read Pico UI. Both remain under SIL OFL-1.1. See [deployment notes](sdcard/README.md).

Thank you for your patience and support.

Shenzhen MindReset Technology Co., Ltd.
