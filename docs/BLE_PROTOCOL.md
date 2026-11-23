# 小纸 Pico · BLE 配套控制协议 / Read Pico BLE Companion Protocol

面向 iOS 配套客户端（CoreBluetooth）的轻量蓝牙控制通道规范。本通道用于设备发现、状态广播、配对码鉴权、免切热点 Wi-Fi 配网、远程拉起/停止局域网传书、一键对时、系统设置与 Agent/定时看板配置、应用切换、网页直达以及短文本直传；电子书、字体、锁屏大图和 OTA 升级仍经由局域网 HTTP 通道（见 [HTTP_API.md](HTTP_API.md)）完成。

Lightweight Bluetooth Low Energy (BLE) control protocol for companion iOS apps (CoreBluetooth). It handles discovery, status beacons, optional PIN authentication, hotspot-free Wi-Fi provisioning, remote LAN transfer start/stop, time sync, settings/Agent/Ambient configuration, app navigation, URL push, and short memos. Bulk books, fonts, lock-screen images, and OTA updates stay on LAN HTTP ([HTTP_API.md](HTTP_API.md)).

---

## 1. 广播与发现 / Advertising & Discovery

- **设备名 / Local Name**：`ReadPico-XXXX`（`XXXX` 为蓝牙 MAC 末 2 字节大写十六进制）。
- **统一设备标识 / Unified Device ID**：`status.device_id`（基础 MAC 12 位大写十六进制）与局域网 mDNS `id` TXT 及 `GET /info` 的 `device_id` 完全一致，用于 iOS 端合并同一设备的蓝牙与局域网连接。
- **主广播包（Advertising Data）**：
  - Flags：`LE General Discoverable | BR/EDR Not Supported`
  - Complete 128-bit Service UUID：`72656164-7069-636f-0001-000000000000`
  - iOS 可直接按该 UUID 过滤扫描：`centralManager.scanForPeripherals(withServices: [CBUUID(string: "72656164-7069-636f-0001-000000000000")])`。
- **扫描响应包（Scan Response Data）**：
  - Complete Local Name：`ReadPico-XXXX`
  - Manufacturer Specific Data（9 字节）：

| 字节偏移 | 长度 | 字段 | 说明 |
| --- | --- | --- | --- |
| `0..1` | 2 B | `company_id` | 固定 `0xFFFF`（小端 `FF FF`） |
| `2` | 1 B | `proto_ver` | 固定 `0x01` |
| `3` | 1 B | `flags` | bit0: `charging`；bit1: `transfer_active`；bit2: `wifi_configured`；bit3: `sd_mounted`；bit4: `pin_required`（局域网或 BLE 任一开启配对码）；bit5: `battery_valid` |
| `4` | 1 B | `battery_soc` | 电量百分比 `0..100`；`0xFF` 表示未知 |
| `5` | 1 B | `transfer_mode` | `0` = 空闲，`1` = AP 热点，`2` = STA 已有 Wi-Fi |
| `6..8` | 3 B | `fw_semver` | `major`, `minor`, `patch`（如 `v0.1.23` -> `0, 1, 23`） |

---

## 2. GATT 服务与特征 / GATT Services & Characteristics

### 2.1 标准 Device Information Service (`0x180A`)

| 特征 | UUID | 属性 | 值 |
| --- | --- | --- | --- |
| Manufacturer Name | `0x2A29` | Read | `MindReset` |
| Model Number | `0x2A24` | Read | `Read Pico (RDP-G01-W)` |
| Firmware Revision | `0x2A26` | Read | 当前固件版本（如 `v0.1.23`） |
| Software Revision | `0x2A28` | Read | `Pico OS` |

### 2.2 标准 Battery Service (`0x180F`)

| 特征 | UUID | 属性 | 值 |
| --- | --- | --- | --- |
| Battery Level | `0x2A19` | Read, Notify | 1 字节无符号整数 `0..100` |

### 2.3 Read Pico Companion Service (`72656164-7069-636f-0001-000000000000`)

| 特征 | UUID | 属性 | 用途 |
| --- | --- | --- | --- |
| **Device Status** | `72656164-7069-636f-0001-000000000001` | Read, Notify | 只读 UTF-8 JSON 设备与传输状态快照；关键状态变化自动推送 |
| **Command RX** | `72656164-7069-636f-0001-000000000002` | Write, Write Without Response | iOS 向设备发送 JSON 指令（支持单包裸 JSON 与分包封装） |
| **Response / Event TX** | `72656164-7069-636f-0001-000000000003` | Notify | 设备返回指令响应 JSON 及异步事件（如 `transfer_status`） |

> 建议连接后立即订阅 `Response / Event TX`、`Device Status` 和 `Battery Level` 三个通知特征。

---

## 3. 分包帧格式 / Framing & Chunking

单条消息 UTF-8 JSON 载荷上限 **4096 字节**。RX 与 TX 采用统一双模式封装：

1. **单包裸 JSON（首字节 `0x7B` = `'{'`）**：当整条 JSON 字节数 `<= (ATT_MTU - 3)` 时直接收发，无需包头。
2. **分包二进制封装（首字节 `0x01`）**：当整条 JSON 字节数 `> (ATT_MTU - 3)` 时，每片携带 6 字节包头 + UTF-8 切片：

| 字节 | 字段 | 说明 |
| --- | --- | --- |
| `0` | `magic` | 固定 `0x01` |
| `1` | `flags` | `bit 0` = `START (0x01)`，`bit 1` = `END (0x02)`（单帧可同时置位 `0x03`） |
| `2` | `seq` | 分片序号，从 `0` 开始逐片 `+1`（按 256 回绕） |
| `3` | `msg_id` | 消息编号 `0..255`，同一消息所有分片一致 |
| `4..5` | `total_len` | 完整 JSON 总字节数（小端 `uint16`，范围 `1..4096`） |
| `6..N` | `payload` | 当前分片的 UTF-8 字节切片 |

- 重组超时：同一消息连续分片间隔超过 **5 秒**或断连时自动丢弃未完成缓存。

---

## 4. 指令集（共 28 条）/ JSON Commands

所有指令为 UTF-8 JSON 对象，支持可选整数 `id`（`0..2147483647`）用于客户端请求-响应关联，必选字符串 `cmd`。
响应统一包含 `ok`（布尔值）、原 `id` 与 `cmd`；失败时附带 `code` 和 `error`。

### 4.1 连通、状态与蓝牙配对码鉴权

- **`ping`**（免鉴权）
  - 请求：`{"id":1,"cmd":"ping"}`
  - 响应：`{"id":1,"cmd":"ping","ok":true,"ble_api":1,"uptime_ms":123456}`
- **`status`**（免鉴权）
  - 请求：`{"id":2,"cmd":"status"}`
  - 响应：`{"id":2,"cmd":"status","ok":true,"status":{...}}`
  - `status` 字段：`ble_api`, `api_version`, `device_id`, `name`, `firmware_version`, `active_page`, `auth_required`, `authorized`, `battery_soc`, `battery_mv`, `charging`, `sd_mounted`, `flash_available`, `flash_free_bytes`（已挂载时返回）, `sd_available`, `sd_free_bytes`（已挂载时返回）, `wifi_configured`, `wifi_count`, `transfer_active`, `transfer_mode`, `transfer_state`, `network_ready`, `url`, `mdns_host`, `ap_ssid`/`wifi_ssid`, `pin_required`, `time_valid`, `timezone_min`。
- **`auth_verify`**（免鉴权）
  - 当设备「设置 → 网络与时间 → 蓝牙配对码」开启时（`status.auth_required == true`），除 `ping` / `status` / `auth_verify` 外的其余 25 条指令须先调用本指令校验设备上显示的 6 位随机配对码；否则返回 `code: "auth_required"`。
  - 连续 3 次错误冷却 3 秒并返回 `code: "rate_limited"`；BLE 断开连接后自动失效。
  - 请求：`{"id":3,"cmd":"auth_verify","pin":"482910"}`
  - 响应：`{"id":3,"cmd":"auth_verify","ok":true,"authorized":true,"auth_required":true}`

### 4.2 BLE 免切热点 Wi-Fi 配网

- **`wifi_list`**：列出已保存 Wi-Fi SSID（最多 5 组，永不回显密码）。
- **`wifi_scan`**：扫描附近 2.4 GHz 网络（最多 16 个去重结果，按 RSSI 降序；Wi-Fi 忙时返回 `code: "busy"`）。
- **`wifi_save`**：`{"id":5,"cmd":"wifi_save","ssid":"HomeWiFi","password":"yourpassword"}`（密码空串表示开放网络，否则 8..63 位可见 ASCII）。
- **`wifi_forget`**：`{"id":6,"cmd":"wifi_forget","ssid":"HomeWiFi"}` 或 `{"id":6,"cmd":"wifi_forget","all":true}`。

### 4.3 远程拉起与停止局域网传书（BLE -> Wi-Fi 切换）

- **`transfer_start`**：`{"id":7,"cmd":"transfer_start","mode":"sta"|"ap"|"auto"}`（默认已配网走 `sta`，否则走 `ap`）。
  - 为避免 ESP32-S3 上 Wi-Fi 与 BLE 同时运行导致片内内存/射频竞争，设备回复 `ok: true` 后约 120 ms 切入传书页并自动暂停 BLE；STA 模式下 iOS 端应通过 mDNS（`_http._tcp` / `readpico.local`，TXT 含 `id`/`ble`）发现局域网服务（热点模式固定 `http://192.168.4.1`），传完书后可调用局域网 `POST /transfer/stop` 或在设备端退出传书页，BLE 将自动恢复广播。
- **`transfer_stop`**：空闲时停止局域网传书并返回原页面（上传或 OTA 进行中返回 `code: "busy"`；进入传书页暂停 BLE 时请改用局域网 `POST /transfer/stop`）。

### 4.4 远程操控与网页直达

- **`webopen`**：`{"id":8,"cmd":"webopen","url":"https://example.com/article"}`；排队 URL 并切入「网页阅读」页在连上已保存 Wi-Fi 后自动抓取打开。
- **`open_app`**：`{"id":9,"cmd":"open_app","app":"home"|"books"|"transfer"|"web"|"sketch"|"focus"|"agent"|"personal"|"settings"}`；远程切换设备当前应用页（自检/OTA/上传中返回 `code: "busy"`）。
- **`refresh_screen`**：`{"id":10,"cmd":"refresh_screen"}`；对当前页触发一次整屏 GC16 强刷清残影。
- **`lock`**：`{"id":11,"cmd":"lock"}`；触发与短按侧边电源键一致的全局锁屏/睡眠流程（先回包 `ok: true`，约 100 ms 后进入锁屏并自动断开 BLE）。

### 4.5 对时、系统设置、Agent 与定时看板配置

- **`time_sync`**：`{"id":12,"cmd":"time_sync","unix_sec":1795190000,"timezone_min":480}`；无需 Wi-Fi 同步主机时钟与主板 PMU RTC，当前在专注时钟或个人看板页时自动重绘刷新。
- **`settings_get`** / **`settings_patch`**：读写设备设置快照（含 `ble_enabled`、`ble_pin_required`、`saver_style` 0..3，与 HTTP `/settings` 共享原子校验，拒绝 `vcom` 与 `font_path`）。
  - 请求：`{"id":13,"cmd":"settings_patch","settings":{"book_px":52,"muted":true}}`
- **`agent_get`** / **`agent_set`**：离线配置 MQTT/TLS Agent（密码永不回显；`{"clear":true}` 清除配置；不传 `password` 且 `clear_password` 不为 `true` 时保留原密码）。
  - 请求：`{"id":14,"cmd":"agent_set","uri":"mqtts://broker.example.com:8883","username":"u","password":"p","topic":"pico/inbox"}`
- **`ambient_get`** / **`ambient_set`**：离线配置定时看板（`enabled`、`interval_min` 为 15/30/60/120/360/720/1440、`quiet_start_h`、`quiet_end_h`、`source_type`、`url`、`auth_header`、`clear_auth`；鉴权头不回显）。

### 4.6 短文本与个人看板直传（无需 Wi-Fi）

- **`memo_send`**：
  - 书架短文本（`target: "clip"`，默认）：`{"id":15,"cmd":"memo_send","target":"clip","title":"购物清单","text":"牛奶\n鸡蛋"}`（上限 3584 字节 UTF-8，可选 `overwrite: true`，保存至 TF 卡或内置 7 MB 存储并刷新书架）。
  - 看板备忘（`target: "dashboard"`）：`{"id":16,"cmd":"memo_send","target":"dashboard","text":"今日待办\n晚上八点阅读"}`（需挂载 TF 卡，最多 4 行、1024 字节，空串清空）。
- **`dashboard_get`**：读取看板文字、累计阅读时长、最近图书与最近摘录（`memo_send` 更新看板文字时，若当前停留在「我的看板」页将立即重新加载并重绘）。

### 4.7 网页收藏夹与专注时钟（无需 Wi-Fi）

- **`webmarks_get`**：读取最多 20 条网页收藏（`{"count":N,"max":20,"items":[{"title":"...","url":"..."}]}`）。
- **`webmarks_add`**：`{"id":17,"cmd":"webmarks_add","title":"文档","url":"https://example.com"}`；同 URL 更新标题，新 URL 满 20 条返回 `code: "limit_reached"`。
- **`webmarks_del`**：`{"id":18,"cmd":"webmarks_del","url":"https://example.com"}`；按 URL 删除收藏。
- **`focus_get`**：读取专注时钟状态（`running`、`phase`、`duration_min`、`remaining_sec`、`completed_count`）。
- **`focus_start`**：`{"id":19,"cmd":"focus_start","minutes":25}`（支持 `15` / `25` / `45`，默认 `25`）；自动切入「专注时钟」页并在前台启动番茄钟。
- **`focus_stop`**：停止当前专注倒计时。

---

## 5. 电源、睡眠与安全边界 / Power, Sleep & Security Boundaries

- **设备端开关**：在「设置 → 网络与时间」可独立开关「蓝牙 BLE」与「蓝牙配对码」（开启配对码时状态行实时显示 6 位随机码），对应 NVS 键 `ble_en`（默认开）和 `ble_pin`（默认关）。
- **睡眠协调**：进入浅睡、软睡或关机前自动停止 NimBLE 协议栈；浅睡唤醒后按设置恢复广播。定时看板唤醒（`wake_reason == 6`）与自检续跑不启动蓝牙。
- **Wi-Fi / BLE 共存**：启用 `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`，NimBLE 缓冲优先分配在 PSRAM（`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y`）。
- **硬约束**：永不暴露面板 VCOM 或 SY7636A 寄存器写入；Wi-Fi 密码、MQTT 密码、订阅源密码、定时看板鉴权头与 6 位配对码永不经公开接口回显。
