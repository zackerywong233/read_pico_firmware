# 版本变更 / Changelog

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
