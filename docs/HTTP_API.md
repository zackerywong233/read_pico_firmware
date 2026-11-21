# 小纸 Pico · 局域网伴侣 HTTP API 规范（v2）/ Read Pico Companion LAN HTTP API (v2)

面向 iOS 配套客户端与局域网工具的 HTTP 接口规范。传输服务在设备进入「传书 Transfer」页（或经 BLE `transfer_start` 远程拉起）后启动，监听 80 端口，支持 **STA（已有 Wi-Fi）** 与 **SoftAP（设备热点 `ReadPico-XXXX`，密码 `readpico`，地址 `http://192.168.4.1`）** 两种模式。

LAN HTTP API specification (`api_version: 2`) for companion iOS apps and local clients. The server runs on port 80 while the Transfer page is active (or launched remotely via BLE `transfer_start`), supporting both STA and SoftAP (`ReadPico-XXXX` / `readpico` / `http://192.168.4.1`).

---

## 1. 服务发现与统一设备关联 / Discovery & Device Identity

STA 模式下广播 mDNS 主机名 `readpico.local` 与 `_http._tcp` 80 端口服务，TXT 记录如下：

| TXT 键 | 示例 | 说明 |
| --- | --- | --- |
| `device` | `readpico` | 固定产品标识 |
| `id` | `A1B2C3D4E5F6` | 统一设备 ID（基础 MAC 12 位大写十六进制，与 BLE `status.device_id` 一致） |
| `ble` | `ReadPico-E5F8` | 对应的 BLE 广播名，用于关联 CoreBluetooth 外设 |
| `api` | `2` | 伴侣 HTTP API 版本 |
| `ver` | `v0.1.23` | 当前运行固件版本 |
| `mac` | `E5F6` | Wi-Fi STA MAC 末 2 字节 |
| `pin` | `0` / `1` | 当前会话是否强制校验 6 位配对码 |

---

## 2. 通用请求规则与鉴权 / Request Rules & Authentication

- **Host / Origin 校验**：所有管理接口要求 HTTP `Host` 头匹配设备 IP 或 `readpico.local`（可选 `:80`）；携带 `Origin` 时须同源。iOS 端使用 `URLSession` 直连设备 IP 或 `readpico.local` 即可（建议对局域网禁用系统代理干扰）。
- **单并发写入锁**：固件同一时间只允许一个写操作（上传、删书、锁屏图、字体、看板写入、OTA）。并发写请求会返回 `409 Conflict` 或 `503 Service Unavailable`，客户端应串行队列调度。
- **可选 6 位配对码（PIN）**：当 `GET /info` 返回 `pin_required: true`（或 mDNS `pin=1`）时，所有变更/敏感接口须携带请求头 `X-Pico-Pin: <6位数字>`；连续 3 次错误冷却 3 秒并返回 `429 Too Many Requests`。可通过 `POST /auth/verify`（请求头 `X-Pico-Pin` 或正文 `{"pin":"123456"}`）提前校验。
- **TF 卡能力解耦**：只要 TF 卡已挂载，即使当前图书上传目标切到内置 7 MB Flash，`/screensaver`、`/sketches`、`/fonts`、`PUT /dashboard` 仍可正常访问。

---

## 3. 路由总览（共 41 条）/ Endpoints

### 3.1 设备信息与鉴权

| 方法与路径 | 鉴权 | 说明 |
| --- | --- | --- |
| `GET /info` | 否 | 返回 `api_version`、`firmware_version`、`build_date/time`、`device_id`、`ble_name`、`ble_supported`、`ble_enabled`、`ble_pin_required`、`active_storage`、`free_bytes`、`file_limit`、`flash_available/free_bytes`、`sd_available/free_bytes`、`can_switch_storage`、`mode`、`network_ready`、`url`、`mdns_host`、`wifi_configured`、`wifi_ssid`、`wifi_ssids`、`clip_token`、`pin_required`、能力开关（含 `book_download_enabled`、`book_progress_enabled`、`notes_json_enabled`、`webmarks_enabled`、`focus_enabled`）与缓存电池状态（`battery_soc`、`battery_mv`、`charging`、`charge_state`） |
| `POST /auth/verify` | 校验 PIN | 请求头 `X-Pico-Pin` 或 JSON `{"pin":"123456"}`，返回 `{"ok":true,"pin_required":...}` |
| `POST /storage` | PIN | 请求 `{"target":"flash"|"sd"}`；空闲时切换图书上传目标并重启服务 |
| `POST /transfer/stop` | PIN | 空闲时返回 `{"ok":true,"stopping":true}`，约 120 ms 后停止局域网传书并退出传书页以自动恢复 BLE 广播；上传中返回 `409` |

### 3.2 图书、笔记、网页收藏与网页投递

| 方法与路径 | 鉴权 | 说明 |
| --- | --- | --- |
| `GET /books` | 否 | 支持 `?q=<关键词>&page=<页码>` 分页搜索（附带每本书的 `has_notes` 及阅读进度 `has_progress`/`percent`/`chapter`/`byte_off`/`order`）；`?name=<urlenc>` 查询单本详情与进度；`?name=<urlenc>&download=1` 流式下载图书原文件 |
| `PUT /upload?name=<urlenc>&overwrite=0\|1` | PIN | 流式上传 `.txt` / `.epub` / `.md` / `.cbz` 至当前活动存储；内置 Flash 单文件上限 1 MiB，中断自动清理 `.part` |
| `DELETE /books?name=<urlenc>` | PIN | 删除当前存储根目录图书并清理其进度、书签与摘录 |
| `POST /books?name=<urlenc>&action=retry_progress` | PIN | 重试清理删除/覆盖遗留的阅读进度记录 |
| `GET /notes?name=<urlenc>[&format=json]` | PIN | 默认流式下载 UTF-8 Markdown；加 `&format=json` 返回结构化书签/摘录 JSON 数组（含 `index`、`kind`、`chapter`、`offset`、`percent`、`text`） |
| `DELETE /notes?name=<urlenc>[&index=<idx>]` | PIN | 传入 `index` 原子删除指定单条书签/摘录；省略 `index` 清空该书全部笔记 |
| `GET / POST / DELETE /webmarks` | POST/DELETE 需 PIN | 同步网页收藏夹（最多 20 条，`read_pico` NVS）：`GET` 列出；`POST` JSON `{"title":"...","url":"https://..."}` 新增或更新标题；`DELETE` 支持 `?url=<urlenc>` 或 JSON `{"url":"..."}` |
| `POST /webclip` | 见下 | 将网页正文剪藏为当前书架 `.txt`（正文上限 64 KiB）。支持：<br/>1) `application/x-www-form-urlencoded`（浏览器书签：`token`、`title`、`url`、`text`，返回 HTML）<br/>2) `application/json`（iOS App：`{"title":"...","url":"https://...","text":"..."}`，可选 `token` + 遵循 `X-Pico-Pin`，返回 `{"ok":true,"name":"webclip_...txt"}`） |
| `POST /webopen` | PIN | JSON `{"url":"https://..."}`；设备停留在「传书页」或「网页阅读页」时自动切入网页阅读并抓取打开 |
| `GET / POST /focus` | POST 需 PIN | 专注时钟（番茄钟）协同：`GET` 返回 `running`、`phase`（`idle`/`focus`/`break`）、`duration_min`、`remaining_sec`、`completed_count`；`POST` JSON `{"action":"start","minutes":25}`（支持 15/25/45 分钟并切入前台专注页）或 `{"action":"stop"}` |

### 3.3 锁屏图片与手写便签（需挂载 TF 卡）

| 方法与路径 | 鉴权 | 说明 |
| --- | --- | --- |
| `GET /screensaver` | 否 | 返回当前生效锁屏图元数据（`exists`、`name`、`format`、`size`、`mtime`）；加 `?raw=1` 流式回读原图 |
| `PUT /screensaver?activate=0\|1` | PIN | 上传 PNG/JPEG 锁屏图片（上限 2 MiB，服务端校验文件头）；默认 `activate=1` 自动切换为图片锁屏并清理冲突旧图 |
| `DELETE /screensaver` | PIN | 清除 TF 卡自定义/Agent 锁屏图片 |
| `GET /sketches` | 否 | 无 `name` 时列出 `/sdcard/notes/sketch_*.png`；带 `?name=<urlenc>` 流式下载 PNG |
| `DELETE /sketches?name=<urlenc>` | PIN | 删除指定手写便签 PNG |

### 3.4 字体管理（需挂载 TF 卡）

| 方法与路径 | 鉴权 | 说明 |
| --- | --- | --- |
| `GET /fonts` | 否 | 无参数列出 `/sdcard/fonts/*.ttf` 与当前激活字体；带 `?name=<urlenc>` 检查是否存在 |
| `PUT /fonts?name=<urlenc>&overwrite=0\|1` | PIN | 上传 TrueType 字体（`.ttf`，上限 32 MiB，提交前校验 TTF 头） |
| `POST /fonts` | PIN | 切换当前字体：`{"builtin":true}` 或 `{"name":"MyFont.ttf"}`（退出传书页后加载生效） |
| `DELETE /fonts?name=<urlenc>` | PIN | 删除指定 TF 卡字体（若为当前字体则自动回退内置字体） |

### 3.5 系统设置、对时与个人看板

| 方法与路径 | 鉴权 | 说明 |
| --- | --- | --- |
| `GET /settings` | 否 | 读取全部可远程管理设置快照（不含 VCOM） |
| `PATCH /settings` | PIN | 原子校验并局部更新设置：`sleep_mode` (0..2)、`auto_sleep_sec` (0/30/60/120/300/600/900/1800/3600)、`pickup_wake`、`book_px` (36..72 步长 4)、`book_refresh` (0..2)、`book_line_height` (0..2)、`book_margin` (0..2)、`book_weight` (0 或 300..800 步长 100)、`book_rotation` (0..2)、`book_gesture` (0..2)、`saver_style` (0..2)、`dashboard_items` (0..15)、`timezone_min` (-720..840)、`charge_led`、`low_soc_notify_percent` (0 或 5..30 步长 5)、`muted`、`transfer_pin_required`、`ble_enabled`、`ble_pin_required`。未知键、越界值、`font_path` 与 `vcom` 均返回 `400` |
| `GET /time` | 否 | 读取设备 UTC 时间戳、有效标志与时区偏移分钟 |
| `POST /time` | PIN | JSON `{"unix_sec":1795190000,"timezone_min":480}`（`timezone_min` 可选），同步主机时钟与主板 PMU RTC |
| `GET /dashboard` | 否 | 返回看板文字（`/sdcard/dashboard.txt`）、`total_reading_sec`、`last_book` 与 `excerpt` |
| `PUT /dashboard` | PIN | JSON `{"note":"..."}`（合法 UTF-8，最多 4 行、1024 字节，需挂载 TF 卡） |
| `DELETE /dashboard` | PIN | 清空 `/sdcard/dashboard.txt` |

### 3.6 网络、Agent、定时看板与局域网 OTA

| 方法与路径 | 鉴权 | 说明 |
| --- | --- | --- |
| `POST /wifi` | PIN（仅 AP） | 仅热点模式允许：保存 Wi-Fi 凭据（最多 5 组，密码永不回显） |
| `DELETE /wifi[?ssid=<urlenc>]` | PIN（仅 AP） | 仅热点模式允许：遗忘单组或全部已保存 Wi-Fi |
| `GET / POST / DELETE /agent` | PIN | 读取/写入/清除 MQTT/TLS Agent 配置（`uri`、`username`、`password`、`topic`、`clear_password`；密码不回显） |
| `GET / POST /ambient` | PIN | 读取/写入定时看板配置（`enabled`、`interval_min`、`quiet_start_h`、`quiet_end_h`、`source_type`、`url`、`auth_header`、`clear_auth`；鉴权头不回显） |
| `PUT /ota` | PIN | 流式上传 ESP32-S3 应用固件（`pico-os-app.bin`）。前置校验电量 ≥ 20% 或充电中（否则返回 `409`）；支持可选请求头 `X-Pico-SHA256: <64位hex>` 端到端校验；按扇区顺序擦写至备用 OTA 槽位，校验通过后切换启动槽并在 1 秒后重启 |
