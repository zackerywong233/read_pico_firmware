# AGENTS.md

面向 AI agent 的仓库入口。人类也可读。产品概述、硬件表、引脚、编译命令见 [README.md](README.md)。

Agent entry for this tree. Humans can read it too. Product overview, hardware, pinout and flash commands live in [README.md](README.md).

新人（人或 agent）上手看 [docs/ONBOARDING.md](docs/ONBOARDING.md)；接手进行中的任务先看 [docs/HANDOFF.md](docs/HANDOFF.md)。

Newcomers (human or agent): read [docs/ONBOARDING.md](docs/ONBOARDING.md). Taking over an in-flight task: read [docs/HANDOFF.md](docs/HANDOFF.md) first.

## 构建 / Build

需要 ESP-IDF v6.1。`components/read_pico/read_pico_flash_hpm.c` 依赖 v6 才有的 `esp_flash_chips/spi_flash_override.h`。

Requires ESP-IDF v6.1. `components/read_pico/read_pico_flash_hpm.c` needs `esp_flash_chips/spi_flash_override.h` (v6 only).

```
idf.py set-target esp32s3
idf.py build
```

- `sdkconfig.defaults`：真机 120 MHz flash / PSRAM 时序，依赖板上 Zbit 型号。/ Board timing for 120 MHz flash / PSRAM; depends on the Zbit part.
- `sdkconfig.ci`：默认时序，只验证能否编过。/ Default timing; compile-only CI check.
- **开发环境约定**：当前开发机未安装本地 ESP-IDF，不要在本机搜索或运行 `idf.py`；本地先跑 `tools/` 下的主机回归测试，固件完整编译通过推送到 GitHub Actions 构建验证。/ **Dev machine note**: ESP-IDF is not installed locally; do not probe or run `idf.py` locally. Run host regression tests under `tools/` locally and push to GitHub Actions for full firmware builds.

不要在没有对应 flash 的机器上把 CI 配置当产品默认。/ Do not ship the CI timing as the product default.

## 目录职责 / Layout

| 路径 / Path | 职责 / Role |
| --- | --- |
| `main/app_main.c` | 开机装配：板级 init、读设置、定 VCOM、开机图、字体，然后交给 `app_loop`。/ Boot wiring, then `app_loop`. |
| `main/ble_service.*` | 轻量 BLE 配套控制桥接：在主循环串行执行配网、远程传书、对时、设置与短文本指令。/ Lightweight BLE companion bridge executing provisioning, remote transfer, time, settings and memos on the main loop. |
| `main/reading_stats.c` | 持久化累计阅读时间、最近摘录与最近图书进度。/ Persist cumulative reading time, saved excerpt and recent book progress. |
| `main/app/app.h` | `app_desc_t` / `app_ctx_t` / `app_redraw_t` 契约。/ Page contract. |
| `main/app/app_loop.c` | 与页面无关的事件循环：触摸去抖、三键短/长按与 8 层导航栈、顶边下滑快捷控制中心、切页、刷屏、底栏按键说明带、锁屏、SD 字体延迟加载。/ Shared event loop with 3-key short/long dispatch, 8-level nav stack, top pull-down Quick Control Menu, and key-hint bar. |
| `main/app/app_registry.c` | 全部 24 个应用与工具的 3 页九宫格（9 + 9 + 6）顺序表及桌面/引导入口。/ Flat 24-app order across 3 pages of 3×3 grid (9 + 9 + 6) plus Home/Onboarding entries. |
| `main/app/app_config.h` | 均衡 / ALL_DU 刷新档位。/ Refresh profile. |
| `main/ui/` | `ui_kit` 排版常量与绘制原语；`ui_icons` 24 格线框图标库；`ui_menu` 屏下三键命中与按键说明带；`ui_quick_menu` 顶部下滑快捷控制中心。/ Shared drawing, wireframe icons, physical key-hint bar, and top pull-down Quick Control Menu. |
| `main/files_ops.*` `main/feeds_store.*` | TF 卡文件管理安全护栏与 OPDS/RSS 订阅源独立 NVS 持久化及 XML 解析。/ TF file-manager guardrails and OPDS/RSS feed store + XML parser. |
| `main/font/` | stb_truetype 字形缓存。`fallback.h` 是生成位图，勿手改。/ Glyph cache. Do not edit `fallback.h` by hand. |
| `main/factory/` | 设备功能自检与出厂 VCOM 标定。/ Device self-test and factory VCOM. |
| `main/display.*` `sleep.*` `settings.*` | 刷屏封装、睡眠/锁屏、NVS 设置。/ Present helpers, sleep/lock, NVS. |
| `components/read_pico/` | 板级 BSP：I2C、EPD 板定义与扫描时序、TF 卡、蜂鸣器、flash HPM。/ Board BSP. |
| `components/read_pico_ble/` | 独立 NimBLE 外设组件：ReadPico 广播信标、DIS/BAS、配套控制 GATT 与分包重组。/ Standalone NimBLE peripheral: ReadPico beacon, DIS/BAS, companion GATT and chunk framing. |
| `components/read_pico_search/` | 离线拼音/首字母/英文书名匹配，设备和网页共用；字表由工具生成。/ Shared offline pinyin, initials and English matching with a generated table. |
| `components/read_pico_pmu/` | CW32L010 协议主机端。线格式在 `read_pico_pmu_protocol.h`。/ PMU host. |
| `components/epdiy/` | 上游裁剪 fork（LCD 路径）。改动清单在其 LICENSE。不要为注释去改它。/ Trimmed upstream fork. Leave it alone for comment work. |
| `components/continuous_du/` | 连续 DU，触摸跟手。/ Continuous DU for finger tracking. |
| `components/cst836u/` `sc7a20h/` `fca9555/` `sy7636a/` | 芯片驱动。/ Chip drivers. |
| `components/e0470_epaper_waveform/` | 面板波形与裁剪。`waveforms/*.h` 是数据表。/ Panel waveforms. |
| `components/pwm_audio/` | PWM 音频（蜂鸣器底层之一）。/ PWM audio helper. |
| `docs/CHANGELOG.md` | 按版本简述对用户可见的功能变化与修复。/ Brief user-visible changes and fixes by version. |
| `docs/BLE_PROTOCOL.md` | 面向 iOS 配套客户端的 BLE GATT UUID、分包与 JSON 控制协议规范。/ BLE GATT UUID, framing and JSON control protocol specification for iOS companion apps. |
| `docs/HTTP_API.md` | 面向 iOS 配套客户端的局域网 HTTP API v2 与 mDNS 发现规范。/ Companion LAN HTTP API v2 and mDNS discovery specification. |
| `docs/WEB_FLASHER.md` | 浏览器刷写的发布、部署、使用与排障说明。/ Browser flashing release, deployment, use and troubleshooting. |
| `web/` | ESP Web Tools 安装页与固件清单；固件镜像和页面同源部署。/ ESP Web Tools installer and manifest; deploy the firmware image on the same origin as the page. |
| `.github/workflows/release-firmware.yml` | 版本标签触发产品配置构建、合并镜像、GitHub Release 与 Pages 发布。/ Tag-triggered product build, merged image, GitHub Release and Pages deployment. |

功能操作见三份 README；浏览器刷写流程见 `docs/WEB_FLASHER.md`；页面契约以本文及 `main/app/app.h` 为准。/ See the READMEs for feature usage and `docs/WEB_FLASHER.md` for browser flashing; this file and `main/app/app.h` define the page contract.

### 文档交付边界 / Documentation delivery boundary

对外交付功能实现、可复跑测试、功能说明及必要的维护契约。功能变化只在 `docs/CHANGELOG.md` 按版本轻量记录；代码演进由 Git 管理，不另建逐任务变更账本。
Deliver implementation, reproducible tests, feature documentation and necessary maintenance contracts. Summarize feature changes only in `docs/CHANGELOG.md`; Git owns code history, not per-task ledgers.

计划、检查点、过程审查、验收结果和调试日志在 `docs/local/` 或被忽略的构建目录内部闭环，不加入提交。长期有效的行为与限制整理进对应功能文档，不要求读者查内部计划。只有需要交接未完功能或责任时才按 `docs/HANDOFF.md` 提供最小必要摘要；本机账本仍不入库。
Keep plans, checkpoints, review records, acceptance results and debug logs in ignored `docs/local/` or build directories. Put lasting behavior and limits in feature documentation without dependencies on internal plans. Use `docs/HANDOFF.md` for the minimal necessary handoff of unfinished work or responsibility; the local ledger remains untracked.

### 应用页与系统工具 / App pages and system tools

每个文件只导出 `app_desc_t`。全部 24 个应用按 `app_registry.c` 平铺在 3 页九宫格桌面中。

One file, one `app_desc_t`. All 24 apps are arranged in `app_registry.c` across the 3-page 3×3 launcher grid.

| 文件 / File | 功能 / Role | 依赖 / Depends on |
| --- | --- | --- |
| `app_pico_home.c` | Pico OS 3 页九宫格桌面（24 格线框图标与实时顶栏状态栏）。/ 3-page 3×3 launcher grid with wireframe icons and live status bar. | `ui_kit`, `ui_icons` |
| `app_onboarding.c` | 五步交互式初始使用引导（首次开机自动进入，也可从设置页重看）。/ 5-step interactive first-run onboarding guide (also reopenable from Settings). | `settings`, `time_sync`, `ui_time_edit`, `ui_kit` |
| `app_book.c` | 一级应用：TXT / EPUB / MD / CBZ 书架（最近阅读卡片、3×2 封面网格/列表）、波前翻页、双开关 HUD、中文精排、长按文本摘录与 CBZ 漫读。/ Reader, cover shelf, wavefront turns, HUD, Chinese typography, long-press excerpts, CBZ manga mode. | `book_*`, `continuous_du`, `ttf_font` |
| `app_transfer.c` | 一级应用：设备热点/已有 WiFi 传书、TF 字体上传与网页配置（含 `/feeds` 订阅源）。/ WiFi book/font transfer and LAN web UI (including `/feeds`). | `read_pico_transfer`, `book_store`, `feeds_store` |
| `app_web.c` | 一级应用：静态抓取 HTTP/HTTPS 网页正文并开放同网剪藏入口。/ Root app: static HTTP/HTTPS page reader and local browser-clip endpoint. | `read_pico_transfer`, `html_text`, `book_layout`, `esp_http_client` |
| `app_feeds.c` | 一级应用：OPDS 1.2 / RSS 2.0 / Atom 1.0 订阅书库（正文阅读、目录分页、流式下书与离线缓存）。/ OPDS 1.2 / RSS 2.0 / Atom 1.0 feeds, streaming download and offline cache. | `feeds_store`, `xml_tok`, `html_text`, `book_layout`, `read_pico_transfer` |
| `app_files.c` | 一级应用：TF 卡文件管理（浏览、排序、隐藏项、图片预览/设壁纸、文本预览、移入书架/字体与安全删除）。/ Standalone TF card file manager with safety guardrails. | `files_ops`, `book_image`, `book_store`, `read_pico_sd` |
| `app_sketch.c` | 一级应用：手写便签与 PNG 导出。/ Root app: handwritten sketch pad with PNG export. | `settings`, `read_pico_sd`, `ui_kit`, `ui_menu` |
| `app_focus.c` | 一级应用：桌面时钟与前台番茄钟。/ Root app: desk clock and foreground pomodoro timer. | `settings`, `reading_stats`, `read_pico_pmu`, `read_pico_buzzer` |
| `app_agent.c` | 一级应用：活动期间订阅 MQTT/TLS；接收分页文字和 HTTPS 锁屏图片。/ Root app: subscribe to MQTT/TLS while active; receive paginated text and HTTPS lock-screen images. | `read_pico_transfer`, `book_layout`, `book_image`, `esp-mqtt` |
| `app_home.c` | 设备概览：I2C 普查、识别码、电池、构建时间。/ Device and system status. | `read_pico`, `read_pico_pmu` |
| `app_personal.c` | 我的看板与锁屏主题（含当前书封面）。/ Personal dashboard and lock-screen theme (including current book cover). | `settings`, `reading_stats`, `read_pico_pmu`, `read_pico_sd` |
| `app_selftest.c` | 设备自检：设备探活与 PMU 命令 ACK 自检。/ Device probe and PMU command self-test. | `pmu_selftest` |
| `app_font_pick.c` | 字体管理：预览并切换内置或 TF 卡字体。/ Preview and switch built-in or TF card fonts. | `ttf_font`, `settings`, `read_pico_sd` |
| `app_settings.c` | 统一设备设置（含阅读顶/底栏、翻页效果、锁屏封面与 Wi-Fi OTA）。/ Unified settings and Wi-Fi OTA. | `settings`, `ota_update`, `ui_gesture` |
| 其余 `app_*.c` / Other `app_*.c` | 桌面第 2–3 页中的刷新、阅读、触摸、传感器、电源、睡眠、存储与 IOE 页面。/ Refresh, reading, touch, sensor, power, sleep, storage and IOE pages on launcher pages 2–3. | varies |

新增应用：把页面加入 `app_registry.c` 的 24 格列表（并配对应 `ui_icons` 图标）。不要改 `app_loop.c`。

Add apps to `app_registry.c` (with a matching `ui_icons` glyph). Leave `app_loop.c` alone.

## `app_desc_t` 契约 / Contract

定义在 [`main/app/app.h`](main/app/app.h)。主循环按返回值刷屏，见 [`main/app/app_loop.c`](main/app/app_loop.c) 的 `app_present()`。

Defined in [`main/app/app.h`](main/app/app.h). The loop presents via `app_present()` in [`main/app/app_loop.c`](main/app/app_loop.c).

| 返回值 / Return | 主循环做什么 / Loop does |
| --- | --- |
| `APP_REDRAW_NONE` | 不刷。/ No update. |
| `APP_REDRAW_AREA` | 只推 `area_hint()`。回调必须先画好 fb。模式是 `APP_DYNAMIC_REFRESH_MODE`（DU）。/ Push `area_hint()` only. Caller paints fb first. |
| `APP_REDRAW_PAGE` | 调 `render()`，整页 `APP_PAGE_REFRESH_MODE`（均衡档是 GL16）。/ `render()` then page mode. |
| `APP_REDRAW_FULL` | 调 `render()`；`APP_PAGE_FORCE_FULL` 时整屏 GC16，否则仍走页模式。/ `render()`; GC16 when force-full is on. |
| `APP_REDRAW_DONE` | 页面自己已经刷完。/ Page already presented. |

回调纯度：

- `render()`：纯绘制。禁止 I2C 写、蜂鸣、睡眠、改设置。KEY2 整屏强刷会复用它。/ Paint only. No I2C writes, buzzer, sleep, or settings. KEY2 reuses it.
- `on_enter()`：上电、唤醒传感器、拉一次数据。/ Power-up and first sample.
- `on_media_lost()` 可选：主循环检测已挂载卡失效时调用，先停止文件/后台消费者，不画屏、不重挂载；返回后主循环回退字体并重绘。/ Optional mounted-media-loss callback: stop file/background consumers without drawing or remounting; the loop then falls back fonts and redraws.
- `on_exit()`：掉电、停传感器。/ Power-down.
- `present()`：自定义推屏。返回 true 表示已经刷过，主循环不再推。/ Custom present; true means done.
- `on_touch()` / `on_gesture()` / `on_key()` / `on_tick()`：可有副作用，用返回值要刷屏。/ Side effects OK; return the redraw.
- `on_key()` 默认在抬起时只收 `UI_KEY_1`（返回/取消）；`owns_keys` 阅读页在菜单关闭时可额外接收 `UI_KEY_3`（下一页），边界无动作时须置 `ctx->key_handled = true`。未消费的 `KEY1` / `KEY3` 回退主循环全局返回 / 菜单；`KEY2` 始终为全局整屏 GC16 强刷；任意页面 500ms 长按统一由主循环执行 `KEY1` 回桌面 / `KEY2` 强刷 / `KEY3` 开关菜单且不触发短按。/ Short presses commit on release: `on_key()` receives `UI_KEY_1` (back/cancel) by default, and reading `owns_keys` pages may also receive `UI_KEY_3` (next page), setting `ctx->key_handled = true` on boundary no-ops. Unhandled `KEY1`/`KEY3` fall back to global Back/Menu; `KEY2` is always global full GC16 refresh; 500ms holds are global everywhere (`KEY1` Home / `KEY2` Refresh / `KEY3` Menu) without firing short press.
- `on_gesture()` 可选；提供后不再接收 `on_touch()`。主循环负责识别与全局中断取消，页面解释动作。/ Optional gesture callback replaces `on_touch`; the loop recognizes and cancels, the page interprets.

标志：

- `holds_pmu`：长时间独占 PMU（自检）。主循环的锁屏按键轮询让路。/ Page owns the PMU; lock-key poll yields.
- `owns_keys`：允许阅读类页面在正文态额外接收短按 `KEY3` 翻页；`KEY2` 强刷、全局 500ms 长按与菜单把手始终由主循环统一处理。/ Allow reading pages to handle short `KEY3` next-page outside the menu; `KEY2` refresh, global 500ms holds and the menu handle remain global.
- 页面切换与菜单打开/返回统一走整屏 GC16；图书每次翻页整屏 GC16。/ App switches, menu open/return and each book turn use full-screen GC16.

`app_ctx_t.request_app` 非空时，主循环在本轮末尾切页；`request_menu` 请求打开根菜单。同时设置时切页优先。/ At tick end, `request_app` switches pages and `request_menu` opens the root menu; a page request takes priority.

## 术语表 / Glossary

| 术语 / Term | 中文 | English |
| --- | --- | --- |
| DU | 快速差分刷新，只驱动变化像素，灰阶少、残影多。 | Fast differential update. Drives changed pixels only. |
| GC16 | 16 级灰度全像素刷新。区域内白像素先压黑再擦白，刷新处一条黑带。 | 16-gray full-pixel update. Whites go black then white; a black flash. |
| GL16 | 16 级灰度，白底保持，不先压黑。 | 16-gray update that keeps an already-white background. |
| 跟随 DU / FOLLOW DU | 开机生成的 8 帧短波形，一次刷新连 diff 带扫描，用来跟手数字或读数。 | Boot-built 8-frame short waveform: one refresh does diff + scan. Used for live digits. |
| 连续 DU / continuous DU | 每轮只扫一个相位，软件记每像素剩余相位。帧率与黑度解耦。 | One DU phase per scan; software tracks leftover phases per pixel. |
| 跟手 | 手指或传感器在动时用 DU/连续 DU 追画面。 | Live tracking with DU / continuous DU while the finger or sensor moves. |
| 定稿 | 动作停下后用 GC16/GL16 整页（或指定区）清残影。 | Settle after motion with GC16/GL16 to clear ghosting. |
| 把手 | 底栏菜单按钮或 KEY3。打开/关闭全屏一级菜单，不交给当前页。 | Menu handle (bottom-right or KEY3). Toggles the root menu; not dispatched to the page. |
| 冻结 / Frozen | 文件头里的产品决策。agent 不得改行为去“优化”它。 | Product decisions in the file banner. Do not “improve” them. |
| 均衡配置 | `APP_REFRESH_BALANCED`：整页 GL16，周期 GC16 压灰底，动态区 DU。 | Default refresh profile: GL16 pages, periodic GC16, DU for motion. |
| ALL_DU | 实验档：整机都走 DU，只看速度。 | Experimental profile: everything is DU. |
| 锁屏 | 电源键短按后的锁页。浅睡按键或拿起回原页。 | Lock face after a short power-key press. |
| 软睡 | `SOFT_SLEEP`：PMU 拉低 EN，再短按开机，不是硬复位。 | Soft sleep: PMU drops EN; a short press powers on without a hard reset. |
| VCOM | 面板公共电压（绝对值 mV）。出厂写入 PMU，主机只读。 | Panel common voltage (absolute mV). Factory-written in the PMU; host is read-only. |

注释里不要再展开这些词。/ Do not re-explain these terms in comments.

## 硬约束 / Hard rules

- 对外产品文案：中文用“小纸 Pico”，英文和日文用“Read Pico”；Read/0 仅可作为内部代号保留。/ Public product copy: use “小纸 Pico” in Chinese and “Read Pico” in English and Japanese; reserve Read/0 for the internal codename.

`冻结 / Frozen:` 段落是产品决策，不是建议。改行为前必须先改这段，并说明为什么决策变了。

A `冻结 / Frozen:` block is a decision, not a hint. Change the behavior only after rewriting that block and saying why the decision changed.

- 面板 VCOM 存在 PMU 里。主机开机读一次，不写 ESP NVS，产品页不提供修改入口。标定页（`vcom_setup.c`）是未标定时的出厂拦住路径。/ VCOM lives in the PMU. Host reads once. No NVS copy. No product UI to edit it. `vcom_setup.c` is the factory gate when unset.
- 不要写 SY7636A 的 VCOM / VLDO / 放电 / 延时 / VCOMCTL。电源页只读。/ Do not write SY7636A VCOM / VLDO / discharge / delay / VCOMCTL. Power page is read-only.
- `sdkconfig.defaults` 的 120 MHz 时序依赖真机 Zbit flash。覆盖 HPM 表见 `read_pico_flash_hpm.c`。/ 120 MHz timing needs the onboard Zbit part. HPM override is `read_pico_flash_hpm.c`.
- 浏览器发布固件必须使用 `sdkconfig.defaults`，不得使用仅编译检查的 `sdkconfig.ci`；`web/manifest.json` 的 ESP32-S3 合并镜像路径须与部署产物保持一致。/ Browser releases must use `sdkconfig.defaults`, never compile-only `sdkconfig.ci`; keep the ESP32-S3 merged image path in `web/manifest.json` aligned with the deployed artifact.
- 不要改 `components/epdiy/**` 来“整理注释”。/ Do not touch `components/epdiy/**` for comment cleanup.
- 不要手改 `main/font/fallback.h`、`main/font/stb_truetype.h`、`components/e0470_epaper_waveform/waveforms/*.h`。/ Do not hand-edit generated or vendored tables.
- 不改 UI 字面量或日志字符串，除非任务明确要求。/ Do not change UI strings or log text unless asked.

## 注释规范 / Comment style

全仓中英双语。只改注释，不改逻辑。

Every comment is bilingual. Comments only; no logic changes.

文件头：

```
/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：这个文件负责什么、边界在哪。
 *
 * English: what this file owns and where it stops.
 *
 * 冻结：不得做的事。
 * Frozen: things this file must not do.
 */
```

- 头文件公开 API 与 struct 成员用 `///`。/ Public API and struct fields: `///`.
- `.c` 内部用 `//`。不要 `/** */`。/ `.c` internals: `//`. No `/** */`.
- 单行：`// 中文。/ English.`
- 超过一行：中文一行，英文紧跟一行。/ Multi-line: Chinese line, then English line.
- `enum` 成员短尾注：`///< 中文 / English`
- 大 `.c` 用 `/* ---- 区块名 / Section ---- */` 分区。/ Large `.c` files use section banners.
- 术语见表，不在注释里重复解释。/ Use the glossary; do not redefine terms.
