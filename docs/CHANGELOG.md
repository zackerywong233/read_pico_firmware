# 版本变更 / Changelog

## v1.0.3 · 2026-11-23

- 修复图书「纯净」刷新档位静置约 1 分钟后背景逐渐泛黑、透出前几页文字残影的问题：在 `display` 新增 `update_display_full_with()`，将图书阅读的 `GC16` 翻页（纯净模式每页、均衡/极速周期清屏与切章）及全刷路径改用原厂完整 48 相直流平衡波形 `E0470_FULL_WAVEFORM`，消除裁剪版 `GC16` 连续触发时白底（`15→15`）累积 `+6` 相直流偏置导致的电荷回弹与前页透字，并补充主机显示与波形直流平衡回归测试。
  Fix book reader background darkening and prior-page ghost bleed after ~1 minute idle in Clean refresh mode: add `update_display_full_with()` in `display` and route book `GC16` page turns (Clean every turn, Balanced/Fast periodic clean, chapter turns) and full refreshes through the unmodified 48-phase DC-balanced `E0470_FULL_WAVEFORM` instead of the asymmetrically trimmed `GC16` LUT (+6 net white phases on `15→15`), with host display and DC-balance regression tests.

## v1.0.2 · 2026-11-23

- 图书书架排版与分页交互修复：修复顶部扫描告警（如「部分目录或文件不可读，请检查后重扫」）与首行书籍及「最近阅读」卡片重叠的问题；在 `ui_kit` 新增禁用态按钮原语 `ui_draw_button_disabled()`，书架/目录/管理页首末页（含仅有一页时）的「上一页」「下一页」自动置灰且不可点击、不再触发无效刷屏。
  Fix Books shelf notice layout and inactive pagination buttons: reserve top notice space so scan warnings never overlap the first book row or Recent Reading card; add `ui_draw_button_disabled()` in `ui_kit` so inactive Prev/Next buttons on single-page or boundary pages render grayed out and ignore presses/taps without triggering redraws.
- 首页页码指示与阅读翻页精简：首页底部当前页指示由黑色实心方块改为黑色实心圆圈（非当前页同步为同心空心圆）；移除易残留黑色竖条纹的波前翻页动效及其「排版与显示」「设置 → 显示与阅读」中的相关选项，阅读翻页统一按刷新档位直走标准 `GC16` / `GL16` / `DU`。
  Home indicator and reader page-turn cleanup: draw the active home page dot as a filled black circle (with concentric hollow circles for inactive pages) instead of a square; remove the wavefront page-turn animation that left vertical black stripes along with its Typography and Display Settings toggles so reader turns always use clean `GC16` / `GL16` / `DU` refreshes.

## v1.0.1 · 2026-11-23

- 修复点击「图书」在提示“初始化内置存储…”后闪退回桌面（主任务栈溢出重启）：将书架封面绘制 `draw_book_cover_box()` 中 14 KB 缩略图缓冲及 EPUB 封面提取 `book_epub_extract_cover()` 的路径与 XML 缓冲从主任务栈移至 PSRAM 复用/分配，并将 `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 提升至 `24576`，同时补充主机栈帧门禁与 EPUB 封面提取回归测试。
  Fix Books crashing back to Home after “Initializing internal storage…”: move the 14 KB cover thumbnail buffer in `draw_book_cover_box()` and the path/XML buffers in `book_epub_extract_cover()` off the main task stack into PSRAM, raise `CONFIG_ESP_MAIN_TASK_STACK_SIZE` to `24576`, and add stack-frame and EPUB cover extraction regression tests.

## v1.0.0 · 2026-11-23

- 屏下三键导航、8 层返回栈与 24 格三页九宫格桌面重构：彻底移除旧全屏应用列表（`ui_menu`）与二级「系统工具」聚合页，将全部 24 个应用与工具配齐极简线框图标平铺为 3 页 3×3 宫格（9 + 9 + 6）并统一顶部状态栏（时间 · 小纸 Pico · 电量）；取消右下角方框菜单把手，底栏触控区恢复全宽 604 px，最底部新增三等分屏下按键说明带；多页列表短按 `KEY1` 在第 2+ 页翻上一页、第 1 页返回，长按 `KEY1` 跨页直接返回上级，任意页长按 `KEY2`（500 ms）全局回桌面第 1 页（首页无动作防误锁）；全系统去除冗余尾部英文。
  Navigation, 3-key contract & 24-app 3-page grid launcher: remove the full-screen `ui_menu` list and `app_system_tools` hub, arrange all 24 apps with minimalist wireframe icons across 3 pages of 3×3 grids (9 + 9 + 6) with a unified top status bar, remove the bottom-right menu handle so touch action bars reclaim the full 604 px width above a 3-column physical key-hint strip, add an 8-level navigation stack, map multi-page short `KEY1` to Prev/Back and long `KEY1` to direct Back, map long `KEY2` (500 ms) globally to Home Page 1 (no-op on Home Page 1), and strip redundant trailing English labels system-wide.
- 阅读器、中文排版与长按文本摘录升级：新增顶部状态栏（时间 · 书名 · 电量）与底部信息栏（章节 + 进度）双开关 HUD（双关即真全屏白纸阅读）；新增基于 `continuous_du` 的波前融合翻页（`关闭` / `波前极速` [默认] / `波前+润色`）；精修中文避头尾规则、2–3 码点连体标点保护、非段末行温和齐行、可配段间距与段首两格缩进；正文长按（500 ms）触发智能选句摘录（支持前扩句 / 后扩句 / 选整段 / 点选另一句连选与保存），并在正文渲染时为已有摘录绘制下划线回显。
  Reader HUD, wavefront turns, Chinese typography & long-press excerpts: add independent top/bottom HUD toggles (both off = pure fullscreen), add `continuous_du` wavefront page-turn animation (`off` / `fast wavefront` [default] / `wavefront + polish`), refine Chinese kinsoku, 2–3 codepoint punctuation-group protection, gentle justification, paragraph gap and 2-em indent, and replace body long-press with smart sentence highlighting/expansion/saving plus saved-excerpt underline echo.
- 书架封面缓存、锁屏当前书封面与 CBZ 漫画原生连翻：移除「继续上次阅读？」阻塞弹窗改为书架顶部常驻「最近阅读」卡片，支持 3×2 封面网格与紧凑列表一键切换，自动提取 EPUB/CBZ 封面并落盘 `.covers` 缩略图与 `lock_current.4bpp` 极速锁屏缓存；锁屏样式新增 `当前书封面`（`saver_style = 3`，含精装纸书与内置图兜底）；CBZ 漫画走全屏原图连翻路径并内置 4×4 Bayer 16 级灰阶有序抖动与下一页后台预解。
  Bookshelf cover grid, lockscreen book cover & native CBZ manga mode: replace the blocking resume modal with a permanent top Recent Reading card, add 1-tap 3×2 cover grid / compact list toggle backed by EPUB/CBZ cover extraction and `.covers` caching, add Current Book Cover lockscreen style (`saver_style = 3`) using fast `lock_current.4bpp` with paperback fallback, and add native fullscreen CBZ manga reading with 4×4 Bayer dithering, manga left/right paging, and next-page pre-decoding.
- 新增 TF 卡文件管理独立应用与 OPDS 1.2 / RSS 2.0 订阅书库：新增 `app_files` 与 `files_ops` 模块，支持目录树浏览、排序、隐藏项开关、新建目录、同卷移动、迭代栈非空目录删除、图片预览设壁纸、文本预览及五项安全护栏（生效字体回退、书架联动清理、防路径穿越、512 项上限、拔卡中断）；新增公共流式 XML 解析底座 `xml_tok`、独立命名空间 `rp_feeds` 的 `feeds_store`（密码绝不回显）、完整 `app_feeds` 页面（RSS/Atom 分页正文阅读与存书、OPDS 多级目录/分页与流式下载到书架并立即阅读、`/sdcard/feeds/` 离线缓存），并在传书网页与 HTTP API 新增 `GET / POST / DELETE /feeds`。
  Standalone TF file manager & OPDS 1.2 / RSS 2.0 feeds: add `app_files` + `files_ops` (browsing, sorting, hidden toggle, mkdir, rename move, iterative tree delete, image preview/wallpaper, text preview, and all 5 safety guardrails), plus shared `xml_tok`, dedicated `rp_feeds` `feeds_store` (redacted credentials), full `app_feeds` (RSS/Atom reading & save-to-shelf, OPDS catalog browsing & streaming book download with instant open, offline cache under `/sdcard/feeds/`), and `GET / POST / DELETE /feeds` in Transfer HTTP API and web UI.

## v0.1.29 · 2026-11-22

- 修复自定义锁屏壁纸不满屏与四周留边问题：新增 `book_image_decode_cover()`，在 PNG、基线 JPEG 与回退解码路径统一按屏幕全尺寸（`684 × 1216`）执行等比铺满并居中裁切，替代原先受限于电子书插图上限（`648 × 1000`）的解码与仅缩不放绘制逻辑，使任意比例与分辨率的锁屏图片都能无白边铺满整屏并保留清晰度。
  Fix borders around custom lock-screen wallpapers: add `book_image_decode_cover()` across PNG, baseline JPEG and fallback decoders to aspect-fill and center-crop directly to full screen size (`684 × 1216`) instead of decoding inside the `648 × 1000` book-illustration cap without upscaling, eliminating top/bottom/left/right margins while preserving sharpness.

## v0.1.28 · 2026-11-22

- 统一全系统屏下三键（KEY1 / KEY2 / KEY3）交互规范：重构主循环按键状态机为“短按抬起提交、长按 500 ms 触发且不重复触发短按”；普通页面与子页统一短按 `KEY1` 返回 / `KEY2` 整屏 GC16 强刷 / `KEY3` 开关应用菜单，正文阅读态（图书阅读、网页正文、Agent 正文、内置阅读）统一短按 `KEY1` 上一页 / `KEY2` 整屏 GC16 强刷 / `KEY3` 下一页；任意页面及菜单内长按 500 ms 统一为 `KEY1` 直达桌面 / `KEY2` 整屏强刷 / `KEY3` 开关菜单。
  Unify physical key behavior across Pico OS: rework key dispatch in the event loop so short presses commit on release and 500 ms holds fire once without triggering short press; regular pages and subviews map short `KEY1` Back / `KEY2` full GC16 refresh / `KEY3` App Menu, active reading views (Books, Web Reader, Agent Inbox, Built-in Reading) map short `KEY1` Prev / `KEY2` full GC16 refresh / `KEY3` Next, and 500 ms holds globally map `KEY1` Home / `KEY2` full refresh / `KEY3` App Menu everywhere.
- 多页面交互与逻辑修复：修复传书页「管理已保存网络」未渲染正确视图及 STA 模式本地地址文字重叠；修复图书横屏阅读点击区间仍按竖屏宽度计算导致无法点中区开关工具条；修复 Agent 历史收件列表行距错位、点选条目无法退出列表及图片后无法翻阅历史文本的问题；为网页阅读断网态补齐「重新连接」按钮、正文页展示收藏/存书反馈并提示空收藏夹；设置页改为仅在 OTA 检查/下载期间保持唤醒并在离页时取消进行中的 OTA；专注时钟倒计时改为分钟对齐刷新并补齐休息结束提示音；菜单点选当前页关闭时统一走整屏 GC16 强刷。
  Multi-page logic & UX fixes: fix Transfer “Manage Saved Wi-Fi” rendering the home view and STA `.local` caption overlap; fix landscape tap zones in Books using portrait width so center taps toggle the toolbar properly; fix Agent history row hit-testing, item selection closing the list, and paging text history after images; add a Reconnect button when Web Reader is offline, show bookmark/save notices in article view, and show an empty-bookmarks notice; keep Settings awake only during active OTA and cancel OTA on exit; align Focus countdown redraws to minute boundaries with a break-end chime; use full GC16 when closing the menu onto the current page.

## v0.1.27 · 2026-11-22

- 根治 Wi-Fi/BLE 共存导致的联网与扫描失败：改为按需分时独占射频。进入传书、网页阅读、Agent 收件及启动 Wi-Fi OTA 前自动暂停 BLE，离开页面或 OTA 结束后自动恢复广播（不改写设置里的蓝牙开关）；BLE 收到 `transfer_start` / `webopen` / 切入联网页指令时先回包并延迟 120 ms 再切换，避免吞掉蓝牙响应；新增局域网 `POST /transfer/stop` 接口，便于 iOS 配套应用传书完成后远程退出传书页并恢复蓝牙重连。
  Root-fix Wi-Fi/BLE coexistence failures by time-sharing the radio on demand: pause BLE while Transfer, Web Reader, Agent Inbox or Wi-Fi OTA is active and resume automatically afterward without altering the Settings switch; defer BLE-triggered transitions to Wi-Fi pages by 120 ms so the OK notification flushes first; add LAN `POST /transfer/stop` so the iOS companion can end Transfer remotely and trigger automatic BLE reconnection.

## v0.1.26 · 2026-11-22

- 修复触屏配网“扫描失败”反复出现：扫描前自动暂停空闲 BLE 并在结束后恢复（已连接时共存失败再暂停重试一次）；扫描对瞬态 `ESP_ERR_WIFI_STATE`/超时重试、将结果取尽的 `ESP_FAIL` 视为结束、清理失败不再覆盖已成功的结果；失败提示附带错误码，串口日志输出失败阶段与内部堆余量；调大内部 DMA 预留并缩减 Wi-Fi 静态缓冲与 BLE 活动数以腾出内部内存。
  Fix repeated on-device Wi-Fi scan failures: pause idle BLE around scans (retry once with a pause when connected), retry transient `ESP_ERR_WIFI_STATE`/timeout, treat exhausted-list `ESP_FAIL` as end of results, keep successful results despite cleanup errors, show an error code on failure, log stage and internal heap on serial, and free internal RAM via a larger DMA reserve, fewer Wi-Fi static buffers and fewer BLE activities.

## v0.1.25 · 2026-11-22

- Wi-Fi OTA 检查与可靠性修复：当 `api.github.com` 限流（HTTP 403/429）或异常时，自动回退通过 `github.com/.../releases/latest` 302 跳转获取最新版本号并拉取 `.sha256` 校验文件；细化屏幕与串口错误原因（区分未配置 Wi-Fi、超时、限流、HTTP 状态与连接错误）；修复重定向上下文 `rctx` 在 HTTPS 读取完成前被释放的生命周期隐患；OTA 联网期间由主循环自动暂停 BLE 并在结束后恢复，避免无线电与内存竞争。
  Wi-Fi OTA check & reliability fixes: automatically fall back to the `github.com/.../releases/latest` 302 redirect plus `.sha256` checksum download when `api.github.com` is rate-limited (HTTP 403/429) or fails; surface specific on-screen and serial error causes (missing Wi-Fi, timeout, rate limit, HTTP status, connect failure); fix redirect context `rctx` lifetime so it outlives the active HTTPS client; pause BLE on the main loop during active OTA checks/downloads and resume automatically afterward.
- 传书双存储入参修复：解除 `read_pico_transfer_start()` 对 `is_flash = true` 与 `font_dir` / `sd_root` 并存的拒绝，确保挂载 TF 卡同时切换图书上传至内置 Flash 时传书服务正常启动；同时修正该模式下 TF 字体上传改按 TF 卡可用空间校验。
  Transfer dual-storage fix: allow `read_pico_transfer_start()` to accept `is_flash = true` together with mounted TF card `font_dir` and `sd_root`, and check TF card free space when uploading fonts while book uploads target internal Flash.

## v0.1.24 · 2026-11-22

- 轻量蓝牙控制通道（BLE / NimBLE）：新增独立 `read_pico_ble` 组件与 `ble_service` 桥接层（仅外设单连接、PSRAM 缓冲、Wi-Fi/BLE 软件共存），面向 iOS CoreBluetooth 配套应用提供 `ReadPico-XXXX` 广播与状态信标、标准 DIS (`0x180A`) / BAS (`0x180F`) 以及自定义控制 GATT 服务（支持单包裸 JSON 与 4 KB 分包重组）。
  Lightweight BLE companion channel (NimBLE): add standalone `read_pico_ble` component and `ble_service` bridge (peripheral-only single link, PSRAM allocation, Wi-Fi/BLE software coexistence) exposing `ReadPico-XXXX` advertising/status beacon, standard DIS (`0x180A`) / BAS (`0x180F`), and custom companion GATT service with raw JSON and 4 KB chunked framing for iOS CoreBluetooth apps.
- iOS 蓝牙与局域网配套能力闭环：支持经 BLE 可选 6 位配对码鉴权（`auth_verify`，含防爆破冷却与断连重置）、免切热点扫描/保存/遗忘最多 5 组 Wi-Fi、远程拉起/停止局域网传书并异步推送 IP/URL 切换事件、无 Wi-Fi 一键对时与时区同步、设备设置读写（`ble_enabled`/`ble_pin_required`，严禁 VCOM/PMIC 写操作）、离线配置 Agent（MQTT）与定时看板、远程打开网页/切换应用/强刷/锁屏、看板查询以及短文本/看板备忘直传；统一 BLE 与局域网 `device_id`（基础 MAC）与 `ble_name`（含 mDNS TXT `id`/`ble`），解耦内置存储模式下 TF 卡锁屏图/便签/字体/看板访问，`POST /webopen` 在传书页自动跳入网页阅读，`POST /webclip` 新增 JSON 剪藏；「设置 → 网络与时间」提供蓝牙与配对码独立开关，详见 `docs/BLE_PROTOCOL.md` 与 `docs/HTTP_API.md`。
  iOS BLE & LAN companion polish: add optional 6-digit BLE PIN auth (`auth_verify` with rate-limiting and disconnect reset), hotspot-free Wi-Fi scan/save/forget (up to 5 networks), remote LAN transfer start/stop with async IP/URL handoff notifications, offline time/timezone sync, device settings read/patch (`ble_enabled`/`ble_pin_required`, never VCOM/PMIC writes), offline Agent/Ambient configuration, remote `webopen`/`open_app`/`refresh_screen`/`lock`, dashboard stats query, and direct short-text/dashboard memos; unify `device_id` (base MAC) and `ble_name` across BLE, `GET /info` and mDNS TXT (`id`/`ble`); decouple mounted TF card screensaver/sketches/fonts/dashboard endpoints from internal-flash book upload mode; auto-route `POST /webopen` from Transfer to Web Reader and accept JSON on `POST /webclip`; expose BLE and BLE-PIN toggles in Settings → Network & Time (see `docs/BLE_PROTOCOL.md` and `docs/HTTP_API.md`).

- 书架双向同步、网页收藏与专注时钟伴侣接口：HTTP `GET /books` 附带逐书阅读进度（`has_progress`/`percent`/`chapter`/`byte_off`/`order`）并支持 `?download=1` 原文件流式下载；`/notes` 新增 `GET ?format=json` 结构化书签/摘录列表与 `DELETE` 按条/清空删除；抽取公共 `web_bookmarks` 模块并在 HTTP（`/webmarks`）与 BLE（`webmarks_get/add/del`）开放最多 20 条网页收藏同步；HTTP（`/focus`）与 BLE（`focus_get/start/stop`）开放专注时钟状态查询与前台番茄钟远程启停；BLE 更新看板文字时在「我的看板」页即时重载刷新。
  Bookshelf sync, web bookmarks & focus companion APIs: enrich HTTP `GET /books` with per-book reading progress (`has_progress`/`percent`/`chapter`/`byte_off`/`order`) and `?download=1` file streaming; add structured JSON notes (`GET /notes?format=json`) and per-item/full deletion (`DELETE /notes`); extract shared `web_bookmarks` storage exposed over HTTP (`/webmarks`) and BLE (`webmarks_get/add/del`); expose Focus Pomodoro status and foreground remote start/stop over HTTP (`/focus`) and BLE (`focus_get/start/stop`); reload Personal Dashboard immediately when BLE updates `/sdcard/dashboard.txt`.

## v0.1.22 · 2026-11-21

- OTA 升级可靠性修复：Wi-Fi OTA 与局域网 `PUT /ota` 改用 `OTA_WITH_SEQUENTIAL_WRITES` 按扇区按需擦除，消除已建连下载前的整片擦除阻塞；GitHub Releases 跨主机 302 重定向改为捕获 `Location` 后重建干净 HTTPS 客户端并扩大收发缓冲（`16 KB / 4 KB`）；补齐完整流接收校验、有界超时重试、分阶段中文错误提示与详细错误日志。
  OTA reliability fixes: switch Wi-Fi OTA and LAN `PUT /ota` to `OTA_WITH_SEQUENTIAL_WRITES` to eliminate full-partition erase stalls on active HTTP streams; rebuild a clean HTTPS client with larger buffers (`16 KB / 4 KB`) across GitHub Releases cross-host 302 redirects; add complete-stream verification, bounded `EAGAIN` retries, stage-specific status messages, and error logging.

## v0.1.21 · 2026-11-21

- 配套客户端局域网接口（`api_version: 2`）：扩充 `GET /info`（固件版本、双存储余量、能力开关与缓存电池状态），新增锁屏图片管理（`GET/PUT/DELETE /screensaver`）、手写便签同步（`GET/DELETE /sketches`）、设备设置读写（`GET/PATCH /settings`）、空闲切换上传存储目标（`POST /storage`）、手机一键对时与时区同步（`GET/POST /time`）、字体列举/删除/切换（`GET/DELETE/POST /fonts`）以及个人看板文字与阅读统计（`GET/PUT/DELETE /dashboard`）。
  Companion LAN APIs (`api_version: 2`): expand `GET /info` (firmware version, dual-storage capacity, capability flags and cached battery status), and add lock-screen image management (`GET/PUT/DELETE /screensaver`), sketch sync (`GET/DELETE /sketches`), device settings (`GET/PATCH /settings`), idle storage switching (`POST /storage`), phone time/timezone sync (`GET/POST /time`), font listing/deletion/selection (`GET/DELETE/POST /fonts`), and dashboard note/reading stats (`GET/PUT/DELETE /dashboard`).
- 局域网安全、发现与 OTA 增强：mDNS `_http._tcp` 广播 `device` / `api` / `ver` / `mac` / `pin` TXT 记录；传书页展示会话 6 位随机配对码并新增可选强制鉴权开关（`POST /auth/verify` 与 `X-Pico-Pin` 校验）；`PUT /ota` 支持可选 `X-Pico-SHA256` 端到端校验、屏幕实时进度展示并消除 HTTPD 线程 I2C 竞争。
  LAN security, discovery & OTA improvements: publish `device` / `api` / `ver` / `mac` / `pin` mDNS TXT records; show a 6-digit session PIN on the Transfer page with optional enforcement (`POST /auth/verify` and `X-Pico-Pin`); add optional `X-Pico-SHA256` verification, live on-screen progress, and race-free cached PMU battery checks to `PUT /ota`.
- 发布产物补全：发布流水线现同步上传 `pico-os-merged.bin`、`pico-os-app.bin` 与 `pico-os-app.bin.sha256`，同时支持浏览器 USB 刷机与 Wi-Fi / 局域网 OTA 升级。
  Release artifacts: publish `pico-os-app.bin` and `pico-os-app.bin.sha256` alongside `pico-os-merged.bin` so both browser USB flashing and Wi-Fi / LAN OTA updates work out of the box.

## v0.1.20 · 2026-11-21

- 桌面与应用菜单：将“小纸 Pico 宫格桌面”加入一级菜单首位，并在应用菜单底栏新增“回桌面”按钮，可从任意页直接返回开机宫格首页。
  Launcher & App Menu: add the Pico OS grid home to the top of the root app menu and add a Home button to the menu bottom bar so any page can return directly to the boot grid.
- 随手便签：修复触控坐标未旋转导致的落点偏移与无法拖动书写问题，改用连续 DU 实现跟手滑动输入与笔画插值，修正竖屏 PNG 导出方向并补齐带边框工具栏与状态反馈。
  Sketch: fix unrotated touch coordinate offset and tap-only input by tracking continuous drags with continuous DU and stroke interpolation, exporting upright portrait PNGs, and adding bordered toolbar controls with status feedback.
- 固件版本识别：移除 `sdkconfig.defaults` / `sdkconfig.ci` 中写死的 `CONFIG_APP_PROJECT_VER="v0.0.0"`，并在 `CMakeLists.txt` 中于标签构建时自动采用 `GITHUB_REF_NAME`（本地回退 `git describe`），使概览页与 Wi-Fi OTA 正确识别固件版本。
  Firmware version: remove hardcoded `CONFIG_APP_PROJECT_VER="v0.0.0"` from `sdkconfig.defaults` and `sdkconfig.ci`, and read `GITHUB_REF_NAME` in `CMakeLists.txt` on tagged CI builds (falling back to `git describe` locally) so Overview and Wi-Fi OTA report the actual version.
- 统一时钟与手动调时：开机从 PMU RTC 恢复主机时钟，专注时钟与我的看板共用同一时间源；新增通用年月日时分与时区手动校准界面，可在专注时钟、我的看板与设置页快速进入并同步写入主板 RTC。
  Unified clock & manual time setup: restore the host clock from the PMU RTC at boot so Focus Clock and Dashboard share one time source, and add a shared date/time/timezone editor accessible from Focus Clock, Dashboard and Settings that persists to the PMU RTC.
- 设置应用：修复手势按下返回整页刷新导致主循环立即取消点击手势的缺陷（改为局部 DU 按下态），并为网络与时间、显示与阅读、睡眠与屏保、电源与提示、设备与系统工具、系统更新提供独立子页。
  Settings: fix gesture press returning full-page redraws that cancelled taps before release (switched to local DU press feedback), and provide dedicated sub-pages for Network & Time, Display & Reading, Sleep & Saver, Power & Alerts, Device & Tools, and System Update.

## v0.1.18 · 2026-11-21

- 设置与个人看板：修复“设置”页选项点击后仅高亮无法进入或调整的问题（改用抬手手势提交），修正三等分触控区间与计划唤醒时区显示；修复个人看板与定时看板按钮越界/重叠。
  Settings & Dashboard: fix Settings rows highlighting without activating by migrating to release-to-commit gestures, fix 3-way row split and timezone display, and fix Dashboard / Scheduled Dashboard button bounds and overlap.
- 设备自检与 PMU：改为同主版本下限语义版本校验，兼容 `1.1.0` 及以上同主版本 PMU 固件。
  Self-test & PMU: use same-major lower-bound semver checks so PMU firmware `1.1.0` and newer `1.x` versions are accepted.
- 联网、OTA 与底层修复：补齐网页阅读与 GitHub OTA 的 3xx 重定向跟随与超时重试，迁移 OTA SHA-256 至 ESP-IDF v6.1 PSA Crypto API；修复锁屏图重复解码、专注/便签/订阅页清屏与菜单把手、定时看板时区与 SNTP 停网并发竞态。
  Networking, OTA & Core fixes: follow 3xx redirects and retry `EAGAIN` in Web Reader and GitHub OTA, migrate OTA SHA-256 to ESP-IDF v6.1 PSA Crypto API, remove double decoding for saver images, restore page clear/menu handle on Focus/Sketch/Feeds, fix scheduled dashboard timezone, and guard SNTP teardown.

## v0.1.17 · 2026-11-21

- 图书：书架、传书网页与删除接口支持 `.md` 和 `.cbz`。Markdown 标题可按章节导航，清理常见行内标记；CBZ 图片按自然文件名排序，并以每 20 页分组。
  Books: add `.md` and `.cbz` to the shelf, transfer page and deletion API. Markdown headings provide chapter navigation with common inline markers cleaned; CBZ images sort naturally and group into chapters of twenty pages.
- 新增随手便签，可用三档笔粗和橡皮绘制、撤销、清空，导出灰度 PNG 或设为图片锁屏。
  Add Sketch with three brush sizes, eraser, undo, clear, grayscale PNG export and image lock-screen selection.
- 新增桌面时钟与番茄钟，支持 15/25/45 分钟专注、5 分钟休息、静音提示与 NVS 完成计数。
  Add a desk clock and pomodoro timer with 15/25/45-minute focus sessions, 5-minute breaks, mute-aware alerts and an NVS completion count.
- 定时看板：新增独立 `rp_ambient` NVS 配置，可选 15/30/60/120/360/720/1440 分钟刷新周期、小时级免打扰时段，以及 HTTP JSON、HTTP 图片或本地看板来源。深睡前按配置设置 PMU 单次闹钟；`wake_reason == 6` 时在自检续跑与 VCOM 标定之外执行刷新、GC16 推屏并重新设置下一次闹钟。局域网传书页新增配置表单，鉴权头不回显。
  Scheduled dashboard: add separate `rp_ambient` NVS settings for 15/30/60/120/360/720/1440-minute intervals, hourly quiet periods, and HTTP JSON, HTTP image or local dashboard sources. Deep sleep arms a one-shot PMU alarm; `wake_reason == 6` refreshes and presents with GC16 outside self-test resume and VCOM setup, then arms the next alarm. The local transfer page configures it without echoing the authorization header.
- HTTP 看板规范：JSON 卡片接受可选字符串 `title`、`subtitle`、`body`、`footer`（单字段最多 512 字节）；图片为 HTTP(S) PNG/JPEG，下载上限 2 MiB。可设置请求 `Authorization` 头；仅接收 2xx 响应，联网和读取共用 25 秒超时预算。
  HTTP dashboard format: JSON cards accept optional string fields `title`, `subtitle`, `body` and `footer` (up to 512 bytes each); images are HTTP(S) PNG/JPEG with a 2 MiB download cap. An `Authorization` request header can be configured; only 2xx responses are accepted, within a shared 25-second network and fetch budget.
- PMU：新增 `read_pico_pmu_time_sync()`、`read_pico_pmu_alarm_set()` 与 `read_pico_pmu_alarm_get()`，封装小端载荷，并按协议检查闹钟模式与秒数范围。
  PMU: add `read_pico_pmu_time_sync()`, `read_pico_pmu_alarm_set()` and `read_pico_pmu_alarm_get()` helpers for little-endian payloads and protocol-defined alarm mode and seconds bounds.
- Agent：消息格式为 `{"type":"text","title":"标题","text":"正文","id":"msg_123","actions":["已读","批准"],"sound":true}`，或 `{"type":"image","url":"https://…","id":"msg_124","actions":["已读"],"sound":true}`。`id` 最多 64 字节，最多 4 个动作标签、每项最多 24 字节。设备保留最近 20 条消息；动作回执发到 `<topic>/reply`，QoS 1，JSON 字段为 `id`、`action`、`ts`。提示音最长 175 ms，遵循全局静音并延后到页面推屏后播放。
  Agent: messages use `{"type":"text","title":"title","text":"body","id":"msg_123","actions":["Read","Approve"],"sound":true}` or `{"type":"image","url":"https://…","id":"msg_124","actions":["Read"],"sound":true}`. `id` is limited to 64 bytes, with up to four action labels of 24 bytes each. The device retains its latest 20 messages. Replies publish QoS 1 JSON fields `id`, `action` and `ts` to `<topic>/reply`. The soft cue lasts at most 175 ms, follows global mute and runs after page presentation.

## v0.1.16 · 2026-11-21

- 网络：Wi-Fi 凭据迁移到 v2，兼容旧版单网络并支持最多五组；STA 扫描已保存网络并优先连接信号最强者。传书网页和设备页可查看、添加、覆盖或遗忘网络；STA 服务提供 `http://readpico.local` 与 HTTP mDNS 广播。
  Networking: migrate Wi-Fi credentials to v2 with v1 single-network compatibility and up to five saved networks; scan and prefer the strongest saved AP. Manage networks from the transfer webpage or device, and advertise the STA service at `http://readpico.local` over mDNS.
- 设置与电源：新增一级“设置 Settings”入口，集中提供网络与时区、阅读显示、睡眠屏保、充电指示灯、低电提醒和全局静音设置；启动时同步 PMU 电源配置，阅读中低电时保存进度并提示。
  Settings and power: add a root Settings page for network/timezone, reader display, sleep/saver, charge LED, low-battery alerts and global mute; sync PMU settings at boot and save progress with a low-battery prompt while reading.
- 网页阅读：支持最多 20 条 NVS 收藏、直接存到书架，以及手机通过 `/webopen` 推送 HTTP(S) 地址后在设备自动打开。
  Web reading: add up to 20 NVS bookmarks, save extracted pages to the shelf, and open HTTP(S) URLs pushed from a phone through `/webopen`.

## v0.1.15 · 2026-11-21

- 阅读刷新：新增纯净/均衡/极速三档，支持每页 GC16、GL16 定稿和 DU 及周期性 GC16 清残影；工具栏保留强刷和长按 KEY2 手动清残影。
  Reader refresh: add clean, balanced and fast profiles with per-page GC16, GL16 or DU and periodic GC16 cleanup; keep toolbar full refresh and long KEY2 ghost clearing.
- 排版：英文整词优先折行、中文标点避头尾；新增可持久化行距、边距和可变字体字重调节。
  Typography: keep English words together when possible and apply Chinese kinsoku; add persistent line spacing, margins and variable-font weight controls.
- 阅读方向与手势：支持竖屏、横屏和稳定 1.5 秒的自动旋转，以及关闭、晃动和轻敲翻页；阅读页退出时恢复屏幕和传感器默认状态。
  Reading orientation and gestures: support portrait, landscape and auto-rotation after 1.5 seconds of stability, plus off, shake and tap page turning; restore display and sensor defaults when leaving the reader.
- 笔记与搜索：书签和摘录可导出 UTF-8 Markdown；传书网页可下载有笔记图书的 Markdown；支持当前章节搜索并跳转到匹配位置。
  Notes and search: export bookmarks and excerpts as UTF-8 Markdown, download note files for books with notes on the transfer page, and search the loaded chapter with direct result navigation.

## v0.1.14 · 2026-11-21

- 看板：增加“继续阅读”直达书架和 PMU 设备时间同步操作；概览页明确标注当前固件版本。
  Dashboard: add a direct Continue Reading entry to the shelf and a PMU clock-sync action; label the current firmware version clearly on Overview.
- 字体：把字体管理加入一级应用菜单，方便查找和切换 TF 卡字体。
  Fonts: add Font Management to the root app menu so SD-card fonts are easier to find and switch.
- 自检：身份检查失败时显示 PMU 读取错误、设备 ID、协议版本或固件版本原因；后续依赖步骤仍显示跳过。
  Self-test: report PMU read, device ID, protocol version or firmware version details for identity failures; dependent checks remain skipped.
- 书架、看板与桌面：恢复开机六格桌面，修正阅读页字体入口；看板可继续阅读、显示 UTC+8，并在 STA 联网后校准 PMU 时钟。
  Shelf, dashboard and launcher: restore the six-tile default home, fix the font page route, add Continue Reading, show UTC+8 and sync the PMU clock after STA connects.
- 网络阅读与网页剪藏：下载可取消并在退出前完成回收；网页解析容忍常见不规范标记，剪藏折叠 CRLF 并清理控制字符。
  Web reading and clipping: downloads cancel and join before network teardown; parsing tolerates malformed markup, and clipping folds CRLF and cleans control characters.
- Agent 与笔记：Agent 配置存入独立 NVS 并支持运行中热更新；图书笔记列表、系统工具导航与安装弹窗翻译修正。
  Agent and notes: store Agent configuration in dedicated NVS with live updates; fix note list layout, system-tool navigation and installer-dialog localization.

## 2026-11-20

- 本地内容可靠性：内置存储仅在检测到分区全擦除时自动初始化；已有内容遇到挂载错误时保留数据并报告不可用。TF 卡和内置存储同时可用时，可在传书页点按容量行切换图书上传目标，切换仅在上传空闲时重启服务；字体仍只上传至 TF 卡。Wi-Fi 覆盖遇到已提交文件留下的过期备份时可继续替换；字体配置仅在字体成功加载后更新。
  Local content reliability: auto-initialize internal storage only when the partition is erased; preserve existing data on mount errors. When TF and internal storage are both available, tap the capacity row to switch the book upload target; switching restarts the service only while idle, and fonts remain TF-only. Wi-Fi replacement can proceed past stale backups left beside committed files, and font settings update only after a successful font load.
- 阅读体验：底部进度条现在可直接点按跳转；跨章节跳转会继续分页到目标位置，避免落到只预排的前几页。
  Reading: tap the bottom progress track to seek directly. Cross-chapter seeking paginates through the target offset instead of stopping at the first pre-rendered pages.
- 书架与阅读：增加按图书保存的书签和整页摘录，笔记迁至图书目录旁的隐藏文件，不限制条目数或摘录长度，实际受存储空间和内存限制。旧固件 NVS 笔记会在首次打开时迁移。清阅读进度保留笔记，删除或替换图书时清理笔记。
  Bookshelf and reading: add per-book bookmarks and full-page excerpts in hidden sidecar files, with no item-count or excerpt-length cap beyond available storage and memory. Legacy NVS notes migrate on first open. Clearing resume progress preserves notes; deleting or replacing a book removes them.
- 续读：只有路径和文件大小都匹配的有效进度才显示续读确认；已删除或被替换的文件不会留下误导性的续读提示。
  Resume: show the confirmation only when path and file size match a valid progress record; missing or replaced files no longer leave a misleading prompt.
- 新增网页阅读：在设备输入 HTTP/HTTPS 地址，最多抓取 512 KiB 并按图书布局分页；同一 WiFi 下可用浏览器书签将网页正文剪藏到当前书架。静态提取不运行网页脚本、不加载 CSS 或图片，离开页面或设备锁屏后停止联网服务。
  Add Web Reader: enter an HTTP/HTTPS address on the device, fetch up to 512 KiB and paginate with the book layout. A browser bookmark on the same WiFi can clip page text into the current shelf. Extraction is static, with no scripts, CSS or images; networking stops when leaving the page or locking the device.
- 新增 Agent 收件：从应用菜单打开后，使用本机传书页配置的 MQTT/TLS Broker 与主题接收 JSON 文字或 HTTPS 图片；文字可分页阅读，图片保存至 TF 卡并设为锁屏屏保。仅在页面活动且设备已连接 WiFi 时联网；图片大小上限 2 MiB，屏保文件为 `agent_screensaver.jpg/png`。
  Add Agent Inbox: open it from the app menu and receive JSON text or HTTPS images through the MQTT/TLS broker and topic configured on the local transfer page. Text is paginated; images are saved to TF and selected as the lock-screen saver. Networking runs only while the page is active and WiFi is connected; images are capped at 2 MiB and stored as `agent_screensaver.jpg/png`.
- Pico OS 桌面改为六格布局，网页阅读、我的看板等日常应用可直接进入，同时保留原有离线入口与应用菜单。
  Expand the Pico OS launcher to six tiles for direct access to Web Reader, Dashboard and other daily apps while retaining offline navigation and the full app menu.
- 睡眠设置增加无操作自动睡眠时间：默认 2 分钟，可选 30 秒至 60 分钟或关闭；自动锁屏继续使用已选的浅睡、深睡或关机模式。
  Add an adjustable idle timeout before automatic sleep: 2 minutes by default, with options from 30 seconds to 60 minutes or off. Automatic lock continues to use the selected light, deep or power-off mode.
- 修复首页看板和系统工具新增中文缺少内置字形的问题；重新生成内置字体子集。
  Add the missing built-in glyphs for the new dashboard and System Tools Chinese text by regenerating the embedded font subset.
- 修正设备自检的失败级联：PMU 未就绪或探活前置项失败时，未运行步骤显示“跳过”；修正探活/命令分组边界并显示跳过计数。
  Prevent self-test failure cascades: show unexecuted checks as skipped when the PMU or a prerequisite probe fails, correct the probe/command grouping and include skipped counts.
- Pico OS 一级菜单现在突出图书、传书、看板、设备状态和自检；刷新、传感器、存储等开发诊断统一收进“系统工具”，不再把 Pico OS 桌面与硬件演示页并列呈现。
  The Pico OS root menu now foregrounds books, transfer, dashboard, device status and self-test. Refresh, sensor, storage and other development diagnostics are grouped under System Tools instead of appearing beside the launcher as peer demo pages.
- 锁屏默认改为 PMU 软睡眠，短按侧边电源键即可开机；显式关机档仍需长按开机。图书每次翻页及应用/菜单切换使用整屏 GC16，减少上一页残影。
  Default lock now uses PMU soft sleep, so a short power-key press boots the device; explicit power-off still requires a long press. Book turns and app/menu transitions use full-screen GC16 to reduce prior-page ghosting.
- 修正浏览器刷机页的连接排查说明，移除 Read Pico 上不存在的 BOOT 键操作。
  Correct the browser flashing connection guidance; Read Pico has no physical BOOT button.

## 2026-11-16 · Pico OS

- 新增 Pico OS 桌面入口与第一阶段产品/视觉基线；保留原厂硬件驱动、图书、传输及诊断页面。
  Add the Pico OS launcher and Phase 1 product/visual baseline while retaining factory drivers, books, transfer and diagnostics.

按日期和作者简述对用户可见的功能变化；详细实现历史见 Git。使用方法见 [README](../README.zh-CN.md)。
User-visible changes by date and author; Git retains implementation history. See [README](../README.md) for usage.

## 2026-11-21

- 固件：16 MB Flash 改为双 4 MB OTA 槽，内置图书存储扩至 7 MB；启用启动回滚保护。首次迁移需通过 Web Flasher 擦除并整机刷写，之后可在设置中使用 Wi-Fi OTA，也支持传书网页上传固件。
  Firmware: use dual 4 MB OTA slots and expand internal book storage to 7 MB, with boot rollback protection. The first migration requires an erase and full Web Flasher install; later updates support Wi-Fi OTA in Settings or firmware upload from the transfer page.

## 2026-10-09

- 锁屏：未单独选择时默认关机（EN=0，长按约 1 秒开机）。已保存的浅睡或深睡不变。
  Power-key lock defaults to power-off when no mode is stored. A saved light or deep choice stays.
- 空闲 2 分钟自动锁屏，睡眠档仍按已选的浅睡、深睡或关机。传书页停留期间不计时。
  Lock after 2 minutes idle, using the saved light, deep, or power-off mode. The transfer page does not count idle time.

## 2026-09-28 · UNSaWEN

- 阅读：进入书架先确认是否续读；ZIP目录连续读取、字宽测量不生成位图，当前章先排起始/续读页，其余分页分批补齐。
  Confirm resume before opening the last book; read ZIP directories sequentially, measure advances without rasterizing, and paginate the initial/resume pages before completing the chapter incrementally.
- EPUB插图：默认占位，点击后单独预览本地JPEG/PNG，返回保持正文位置；标题前的章首图片及同资源单独标为标题图，其他重复图片提示本次已读章节中最早出现的位置，开章不再反复解码重复图片。
  Show image placeholders and load local JPEG/PNG previews only on tap, returning to the same text position; label opening title images and matching resources separately, retaining earliest-visited hints for other repeated illustrations and avoid decoding images on chapter opening.
- 手势：滑动翻页阈值由120缩短到64像素，保留抬手提交、轻点容差和短拖取消。
  Reduce swipe distance from 120 to 64 pixels while retaining release-to-commit, tap tolerance and short-drag cancellation.
- EPUB：ZIP 条目、资源项及章节上限提高到 32768，使用固定窗口扫描ZIP目录和紧凑索引查找，标题按实际长度保存，OPF/导航上限4 MiB；打开和排版显示等待提示，失败显示具体原因。
  Raise EPUB ZIP, manifest and spine limits to 32768, scan ZIP directories through a fixed window, use compact indexed lookup and actual-length titles, and allow 4 MiB OPF/navigation metadata; show parsing/layout wait hints and failure reasons.
- WiFi：TF 卡支持上传完整 TTF 字体，32 MiB 上限、同名确认、提交前结构校验及中断恢复；传书期间暂停卡上字体读取，离页恢复。
  Upload complete TTF fonts to TF over WiFi with a 32 MiB limit, confirmed replacement, pre-commit structural checks and interruption recovery; suspend card font reads during transfer and resume on exit.
- 部署：随仓库提供未修改的 ChillDuanSans VF v1.30、原 OFL-1.1 许可和来源校验；内置子集命名为 Read Pico UI，保留原版权与许可。
  Bundle unmodified ChillDuanSans VF v1.30, original OFL-1.1 terms and source verification; name the embedded subset Read Pico UI and retain attribution and licensing.

## 2026-09-27 · UNSaWEN

- EPUB 图片位置增加“[图片]”占位，避免纯图片章节显示为空白；暂不解码图片。
  Show “[图片]” placeholders at EPUB image positions so image-only chapters are visible; image decoding is not yet supported.

- 修复长篇 EPUB 因条目或章节超过 512 而无法打开的问题；上限提高到 2048，单资源解压上限仍为 2 MiB。
  Fix long EPUBs failing to open above 512 entries or chapters; raise the limit to 2048 while retaining the 2 MiB uncompressed resource limit.

- 图书阅读：支持 TF 卡或内置存储的 UTF-8/GBK TXT 与纯文本 EPUB，目录、逐书续读、字号、手势翻页及默认关闭的实验晃动翻页。
  Read UTF-8/GBK TXT and text-only EPUB from TF or internal storage, with a TOC, per-book resume, font sizes, gestures and optional experimental shake-to-turn.
- 图书管理：增加拼音/首字母/英文搜索、来源筛选、名称/最近阅读排序、详情，以及分别确认的单本/批量删除和清进度。
  Add pinyin/initials/English search, source filters, name/recent sorting, details and separately confirmed single/batch deletion or progress reset.
- WiFi 传书：支持设备热点和已有网络、触屏/网页配网、连接与网址二维码；网页管理当前存储，支持确认替换、取消和重试；停止后返回进入前的位置。
  Transfer over the device hotspot or an existing network, with touchscreen/web provisioning and connection/URL QR codes. Manage current storage in the browser, confirm replacements, cancel or retry uploads, and return to the entry location on stop.
- 可靠性：处理保存失败与部分成功重试、中文文件名和覆盖中断恢复；修复显示欠载后相位队列残留导致的死锁；检测 TF 失效后停止相关消费者并回退字体，插回后需显式重挂。
  Handle save failures, partial-operation retries, Chinese filenames and interrupted replacements. Fix a display queue deadlock after underrun; stop affected consumers and fall back fonts on TF loss, requiring explicit remount after reinsertion.
