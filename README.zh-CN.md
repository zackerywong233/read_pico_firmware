# 小纸 Pico 固件 · Pico OS

**语言:** [English](./README.md) | [简体中文](./README.zh-CN.md) | [日本語](./README.ja-JP.md)

[贡献指南](CONTRIBUTING.md) · [支持](SUPPORT.md) · [安全政策](SECURITY.md) · [行为准则](CODE_OF_CONDUCT.md)

[![License](https://img.shields.io/github/license/MindReset/read_pico_firmware?style=for-the-badge&logo=apache&logoColor=white)](LICENSE)
[![Build](https://img.shields.io/github/actions/workflow/status/MindReset/read_pico_firmware/build.yml?branch=main&style=for-the-badge&logo=githubactions&logoColor=white)](https://github.com/MindReset/read_pico_firmware/actions)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.1-E7352C?style=for-the-badge&logo=espressif&logoColor=white)
![Target](https://img.shields.io/badge/target-ESP32--S3-E7352C?style=for-the-badge&logo=espressif&logoColor=white)

小纸 Pico 是深圳思维重置科技有限公司旗下小纸 Read 系列的开发板，搭载 ESP32-S3 和 4.7 寸单色墨水屏。
本仓库提供随板运行的 Pico OS 固件，包含图书阅读、Wi-Fi 传书、设备看板与系统状态等原生应用。

设备自检位于一级应用菜单；刷新、触摸、传感器、电源、按键、TF 卡、字体与睡眠等底层诊断统一收纳在“系统工具”中。
板级支持包、PMU 协议主机端和芯片驱动仍可独立复用，也可作为开发者扩展 Pico OS 的基础。

面向 AI agent 的目录职责、`app_desc_t` 契约、术语表和注释规范见 [AGENTS.md](AGENTS.md)。

## 官方文档与更多设备

- [小纸 Pico 官方文档](https://dot.mindreset.tech/docs/read_0)
- [Dot Open Platform](https://github.com/MindReset/dot_open_platform)：探索更多可以动手玩的 Dot 设备与项目，包括 Quote/0 硬件资源和 Rand/0 本地显示集成，以及固件示例、引脚表和外壳文件。

## 硬件

| 项目 | 规格 |
| --- | --- |
| 主控 | ESP32-S3，16 MB flash，8 MB Octal PSRAM，二者均运行于 120 MHz |
| 屏幕 | 4.7 寸单色墨水屏，1216 × 684，16 级灰阶，16 bit 并口经 LCD 外设驱动 |
| 屏电源 | SY7636A，PGOOD 经 IO 扩展读回 |
| 电源管理 | CW32L010，自定义 I2C 协议：电池、充放电、指示灯、RTC、闹钟、开关机 |
| 触摸 | CST836U，两点触摸、中断与深睡唤醒 |
| 加速度计 | SC7A20H，敲击、朝向、自由落体、FIFO |
| IO 扩展 | FCA9555，屏控制脚与卡检测 |
| 存储 | TF 卡（1 bit SDMMC），字体从卡上加载 |
| 其它 | 蜂鸣器、三个电容按键区 |

## 浏览器内安装

发布新版本后，可在 [小纸 Pico 浏览器刷机页](https://zackerywong233.github.io/read_pico_firmware/) 直接安装固件。请使用最新版 Chrome 或 Edge，通过 USB 连接设备；首次安装时会显示是否擦除 ESP 内置 Flash 的选项。擦除会清除内置存储与设置，TF 卡文件不受影响。发布与刷写说明见 [浏览器刷机文档](docs/WEB_FLASHER.md)。

## 编译与烧写

需要 ESP-IDF v6.1。`components/read_pico/read_pico_flash_hpm.c` 依赖 v6 才提供的
`esp_flash_chips/spi_flash_override.h`。

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

`sdkconfig.defaults` 中的 120 MHz flash / PSRAM 时序依赖板上实际的 flash 型号。
CI 使用 `sdkconfig.ci` 换回默认时序，仅验证能否编译通过，见
[.github/workflows/build.yml](.github/workflows/build.yml)。

如果在固件开发或烧录时，设备从睡眠状态唤醒后无法被识别，请依次尝试：

1. 更换 USB Type-A（标准 USB）数据线。
2. 对设备重新执行一次睡眠和唤醒操作。
3. 重启开发板后重试。

面板公共电压（VCOM）在出厂时标定并写入 PMU。固件开机读取一次用于配置驱动，
不在本地保存，也不提供修改入口。

## Pico OS 应用

开机默认进入 Pico OS 桌面。一级应用菜单提供日常功能、设备状态、自检与“系统工具”入口；硬件调试页不再与阅读器功能平铺在一起。

| 一级应用 | 内容 |
| --- | --- |
| 图书 | TF 卡或内置存储的 UTF-8 / GBK TXT 与 EPUB；目录、字号、逐书进度、书架筛选/排序/搜索和管理；每次翻页整屏 GC16 清理残影。EPUB 支持 NCX 与 nav 目录。 |
| 传书 | 设备热点或已有 WiFi，通过浏览器上传 TXT/EPUB 与 TF 卡 TTF 字体；触屏配网、二维码连接、网页图书管理、确认替换/删除、取消与重试。离页断网。 |
| 设备状态 | 开机 I2C 在线检测、识别码、电池与充电状态、构建时间。 |
| 我的看板 | 可选日期、电池、最近阅读和 TF 卡个人文字；设置锁屏主题。 |
| 设备自检 | 探活与命令 ACK；需要断电的项目只在后台记录结果。 |
| 系统工具 | 进入下方硬件诊断与开发辅助页。 |

系统工具包含墨水屏刷新、内置阅读、触摸、加速度计及其诊断、电源与电池、PMU 协议、按键事件、睡眠唤醒、TF 卡与蜂鸣器、字体管理和扩展口。睡眠工具可检查深睡、浅睡或关机路径；默认锁屏深睡后短按侧键开机，显式关机仍需长按开机。

图书阅读三键 KEY1 / KEY2 / KEY3 对应上一页 / 工具条 / 下一页；工具条提供强刷。长按中键约 500 ms 可打开应用菜单。其他页面仍为 KEY2 整屏 GC16、KEY3 菜单。菜单项抬起提交，滑出可取消。

开机默认进入“我的看板”。在“看板与屏保”中开关日期、电量、最近阅读和个人信息卡片，并选择默认图案、最近摘录或 TF 卡图片作为锁屏内容。将最多 4 行 UTF-8 文字放入 TF 卡根目录 `dashboard.txt`；屏保图片命名为 `screensaver.jpg` 或 `screensaver.png`，编码文件不超过 2 MiB。PMU 时钟已同步时，看板显示 UTC 时间。

## 目录

```text
main/
  app_main.c        开机装配，随后交给 app_loop
  app/              app 接口（app.h）、注册表、事件循环
  apps/             Pico OS 应用与系统工具页，每页导出 app_desc_t
  ui/               ui_kit 绘制原语与布局常量、ui_menu 两层菜单
  font/             stb_truetype 字形缓存
  factory/          设备功能自检与出厂 VCOM 标定
components/
  read_pico/        板级 BSP：I2C、EPD 板定义与扫描时序、TF 卡、蜂鸣器、flash HPM
  read_pico_pmu/    CW32L010 协议主机端
  epdiy/            墨水屏渲染，裁剪至 LCD 外设路径
  continuous_du/    连续 DU：跨多轮累积相位，用于跟手
  cst836u/ sc7a20h/ fca9555/ sy7636a/    芯片驱动
  e0470_epaper_waveform/                 面板波形表与裁剪函数
  pwm_audio/        LEDC PWM 音频，蜂鸣器底层之一
assets/             图片素材（main/assets/*.bin 的来源）
tools/              字体与图片转换脚本
```

新增一级应用：在 `main/apps/` 新建页面并加入 `main/app/app_registry.c`；新增系统诊断页时，加入 `app_system_tools.c` 的工具表。主循环不需要改动。

## 引脚

| 功能 | GPIO |
| --- | --- |
| I2C SCL / SDA | 40 / 39（400 kHz） |
| EPD 数据 D0–D15 | 4–18, 45 |
| EPD XLE / XSTL / XCL / SPV / CKV | 3 / 46 / 21 / 47 / 48 |
| FCA9555 INT# | 41（同时作为浅睡唤醒源） |
| CST836U INT# | 43 |
| SC7A20H INT1 | 1 |
| TF 卡 CLK / CMD / D0 | 38 / 42 / 44 |
| 蜂鸣器 | 2 |

屏电源开关、XOE、MODE、VCOM_EN、触摸复位和卡检测位于 FCA9555 的 Port-0，
见 [main/apps/app_ioe.c](main/apps/app_ioe.c) 中的引脚表。

## 传书与使用限制

AP 与已有 WiFi 均提供传书网页二维码；热点页可切换连接 WiFi 与打开网页二维码。设备显示的构建时间统一标注 UTC。

传书服务仅在传书页运行，离页停止。它使用局域网 HTTP，没有独立登录或 TLS；同网设备可管理当前上传存储中的图书，请使用可信网络。保存的 WiFi 凭据位于设备 NVS，公开接口和日志不返回密码。本版未启用 NVS/flash 加密，不以此提供物理访问防护。

停止传书后返回进入前的页面或菜单位置。检测到已挂载TF卡失效时，设备停止相关阅读/传书并回退字体；插回后需在TF页显式重新挂载。写入时拔卡可能损坏文件系统。EPUB打开正文时只显示图片占位；点击后才加载本地 JPEG/PNG 并单独等比预览，点图片、返回按钮或三键任意一键回到原正文位置，不重新分页；开章、翻页和预绘制不解码图片；ZIP条目、资源清单项和章节各最多32768个，OPF/导航元数据解压后最多4 MiB，正文/图片读取最多2 MiB，ZIP中央目录最多8 MiB；标题总预算1 MiB，超出后用编号标题；即使未使用的ZIP条目也不能超过4 MiB。以上内存与资源限制独立于章节数，并非章数合规就一定能打开；不支持ZIP64或超过32768项/章的文件；外部换书或换卡产生同路径、同大小文件时，旧阅读进度可能仍被匹配。保存失败的待重试状态不能保证在断电后保留。

进入书架时询问是否继续上次阅读，选择“留在书架”不会打开图书；确认“继续阅读”才加载（KEY1取消、KEY3继续）。先解析目录与当前章节，再完成起始两页或续读位置；其余分页每次补两页，其他章节按需加载。分页未完成时页数显示“…”；向前跨章到上一章末页仍需完成该章分页。字宽测量使用驻留字体表，不提前生成整章字形位图。打开图书和加载章节时保留等待提示；失败显示可见原因（内存不足、超出限制、格式不支持或文件异常）。

滑动翻页在抬手时提交，距离阈值由120像素缩短到64像素；超过24像素轻点容差但不足64像素的短拖仍取消。在图片占位上滑动仍翻页，不触发图片加载。

插图范围：基线 JPEG 优先在解码时缩小，允许最多放大两倍到显示尺寸以减少计算，原图最多16M像素、单边8192；PNG及渐进 JPEG 原图最多1M像素，解码堆上限4 MiB。输出不超过648×1000灰度像素，透明 PNG 合成白底；只缓存当前章节最近查看的一幅图片，同图关闭后再次打开可复用；超限、缺失、损坏或不支持的图片在点击后说明原因，不中断正文阅读。同一归一化资源路径再次出现时显示“重复图片”和本次打开图书后已读章节中的最早节号（EPUB目录顺序包含封面、前言，可能不同于正文章号）。章首连续图片后紧接标题时，这些图片及已识别的同资源引用单独显示“标题图”，不重复显示首次位置；标题后的其他插图仍保留已读最早提示；不扫描未读章节，不声称是全书首次位置，关闭图书后记录清空；不同路径下的相同内容不作匹配；支持 SVG 包装中的 JPEG/PNG 引用，不绘制纯 SVG 矢量、CSS背景或外链图片。解码器来源与许可见 [第三方说明](main/book/vendor/README.md)。

内置字体仅覆盖界面和演示正文，阅读外部中文书籍请将完整中文 TTF 放到 TF 卡的 `fonts/` 或 `assets/fonts/`，再在“字体”页选用。默认字体路径为 `fonts/ChillDuanSansVF.ttf`；缺字时应检查卡上字体文件和所选字体的字形覆盖。仓库提供 [TF 部署字体与版权说明](sdcard/README.md)。也可通过 WiFi 网页的“上传字体”区上传到 `/sdcard/fonts`：单文件上限 32 MiB，仅支持带 TrueType 轮廓的 TTF（不支持 OTF/CFF、TTC、WOFF）。上传前检查容量，同名替换需确认，校验失败或中断保留旧字体；停止传书后在设备“字体”页选用。传书期间暂用内置字体，防止替换正在读取的字库；不会改变已保存的字体选择。

功能变化见 [版本变更](docs/CHANGELOG.md)。离线拼音字表来自 pypinyin（MIT），见 [组件许可与再生成说明](components/read_pico_search/README.md)。

## 致谢与许可

- 固件本体：Apache-2.0，见 [LICENSE](LICENSE)。
- [epdiy](https://github.com/vroland/epdiy)：墨水屏时序与渲染。本仓库为按本板
  LCD 路径裁剪的 fork，LGPL-3.0-or-later，改动清单见
  [components/epdiy/LICENSE](components/epdiy/LICENSE)。
- [stb_truetype](https://github.com/nothings/stb)：字形光栅化，公共领域。
- [pwm_audio](https://github.com/espressif/esp-iot-solution/tree/master/components/audio/pwm_audio)：
  Espressif LEDC PWM 音频。本仓库为裁剪副本，Apache-2.0，见
  [components/pwm_audio/LICENSE](components/pwm_audio/LICENSE)。
- 面板波形表随本板附带，按现状提供，Apache-2.0，见
  [components/e0470_epaper_waveform/LICENSE](components/e0470_epaper_waveform/LICENSE)。
- 内置字体 `main/assets/builtin.ttf` 由 `tools/gen_builtin_font.py` 从
  [ChillDuanSans](https://github.com/Warren2060/ChillDuanSans)（寒蝉端黑体，
  Warren2060，SIL OFL-1.1）可变字体子集化生成。完整原版字体和许可证随 `sdcard/fonts/` 提供；内置修改版名为 Read Pico UI。两者均适用 OFL-1.1，详见 [部署说明](sdcard/README.md)。

感谢各位开发者的耐心与支持。

深圳思维重置科技有限公司
