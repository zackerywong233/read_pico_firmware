/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 双模式传书生命周期和跨任务状态；存储策略由调用方注入。
 * AP/STA transfer lifecycle and cross-task status; caller supplies storage policy.
 * 决策变更：书籍文件支持 TXT、EPUB、Markdown 与 CBZ；完整字体部署使用独立 TF 目录的 TTF。
 * Decision change: support TXT, EPUB, Markdown and CBZ books; complete TTF fonts use a separate TF directory.
 * 冻结：不依赖页面或图书实现。/ Frozen: No page or book implementation dependency.
 * 冻结：热点网页或停服后的设备触屏可管理最多五组网络；已有WiFi模式不接受远程凭据修改。
 * Frozen: AP webpage or stopped-service device UI manages up to five networks; STA rejects remote credential changes.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "read_pico_transfer_companion.h"
#include "read_pico_transfer_network.h"

#define READ_PICO_TRANSFER_PASSWORD "readpico"
#define READ_PICO_TRANSFER_URL "http://192.168.4.1"
#define READ_PICO_WEBOPEN_URL_MAX 256

typedef enum {
    READ_PICO_TRANSFER_MODE_AP = 0, ///< 设备热点 / Device hotspot
    READ_PICO_TRANSFER_MODE_STA, ///< 已有 WiFi / Existing WiFi
} read_pico_transfer_mode_t;

typedef enum {
    READ_PICO_TRANSFER_STOPPED, ///< 已停止 / Stopped
    READ_PICO_TRANSFER_STARTING, ///< 启动中 / Starting
    READ_PICO_TRANSFER_READY, ///< 等待上传 / Ready
    READ_PICO_TRANSFER_UPLOADING, ///< 接收中 / Receiving
    READ_PICO_TRANSFER_ERROR, ///< 网络或请求失败 / Network or request failed
} read_pico_transfer_state_t;

typedef struct {
    read_pico_transfer_mode_t mode; ///< 网络模式，默认热点 / Network mode, default AP
    const char *root_dir; ///< 已挂载根目录，start 内复制 / Mounted root, copied by start
    const char *font_dir; ///< 可选 TF 字体目录；调用方须暂停 SD 字体读取至 stop 返回 / Optional TF font directory; caller must suspend SD font reads until stop returns
    bool is_flash; ///< 内置存储标志 / Internal storage flag
    size_t file_limit; ///< 单文件上限，零表示不限 / Per-file limit, zero means unlimited
    uint64_t (*free_bytes_cb)(void *ctx); ///< 查询可用字节 / Query available bytes
    void *free_bytes_ctx; ///< 回调上下文，stop 前有效 / Callback context valid until stop
    esp_err_t (*file_changed_cb)(const char *path); ///< 文件提交或删除后清除进度；失败不回滚文件 / Clear progress after commit or deletion; failure never rolls back the file
    void (*sta_ip_cb)(void); ///< STA 首次取得 IP 后调用 / Called after STA receives an IP address
    void (*service_poll_cb)(void); ///< 传输服务轮询时调用 / Called during transfer service polling
    void (*network_stop_cb)(void); ///< 停止网络前调用 / Called before stopping networking
    const char *sd_root; ///< 可选 TF 卡根目录（如 /sdcard），用于锁屏图片与手写便签 / Optional TF card root (e.g. /sdcard) for screensaver and sketches
    bool (*storage_info_cb)(read_pico_storage_info_t *out, void *ctx); ///< 可选：查询双存储状态与余量 / Optional: query flash/SD availability and free bytes
    bool (*storage_switch_cb)(bool prefer_flash, void *ctx); ///< 可选：请求主循环空闲切换存储目标 / Optional: request an idle storage-target switch on the owner task
    void *storage_ctx; ///< 存储回调上下文 / Context for storage callbacks
    bool (*settings_get_cb)(read_pico_device_settings_t *out, void *ctx); ///< 可选：读取设备设置快照 / Optional: read device settings snapshot
    esp_err_t (*settings_set_cb)(const read_pico_device_settings_t *next, void *ctx); ///< 可选：保存设备设置快照 / Optional: persist device settings snapshot
    void *settings_ctx; ///< 设置回调上下文 / Context for settings callbacks
    bool pin_required; ///< 是否强制要求 6 位配对码 / Require 6-digit PIN for sensitive/mutating HTTP requests
    bool (*time_get_cb)(read_pico_time_info_t *out, void *ctx); ///< 可选：读取设备时间与时区 / Optional: read device clock and timezone
    esp_err_t (*time_set_cb)(uint32_t unix_sec, bool has_tz, int16_t tz_min, void *ctx); ///< 可选：同步设备时间与时区 / Optional: sync device clock and timezone
    void *time_ctx; ///< 时间回调上下文 / Context for time callbacks
    esp_err_t (*font_select_cb)(const char *path, void *ctx); ///< 可选：切换当前字体路径，空串表示内置字体 / Optional: select active font path; empty selects built-in
    void *font_ctx; ///< 字体回调上下文 / Context for font callbacks
    bool (*dashboard_stats_cb)(read_pico_dashboard_stats_t *out, void *ctx); ///< 可选：读取累计阅读时长、最近图书与摘录 / Optional: read reading stats, recent book and excerpt
    esp_err_t (*dashboard_stats_set_cb)(const read_pico_stats_restore_t *in, read_pico_dashboard_stats_t *out, void *ctx); ///< 可选：恢复累计阅读统计、最近图书与摘录 / Optional: restore cumulative reading stats, recent book and excerpt
    void *dashboard_ctx; ///< 看板回调上下文 / Context for dashboard callbacks
    bool (*book_progress_cb)(const char *path, uint32_t size, read_pico_book_progress_t *out, void *ctx); ///< 可选：查询单本图书阅读进度 / Optional: query reading progress for one book
    esp_err_t (*book_progress_set_cb)(const char *path, uint32_t size, const read_pico_book_progress_t *in, bool set_last, read_pico_book_progress_t *out, void *ctx); ///< 可选：恢复单本图书阅读进度 / Optional: restore reading progress for one book
    void *book_progress_ctx; ///< 进度回调上下文 / Context for book progress callback
    esp_err_t (*book_notes_set_cb)(const char *path, uint32_t size, const read_pico_book_note_item_t *items, size_t count, void *ctx); ///< 可选：原子覆盖恢复单本图书书签与摘录 / Optional: atomically restore bookmarks and excerpts for one book
    void *book_notes_ctx; ///< 笔记回调上下文 / Context for book notes callback
    size_t (*webmarks_list_cb)(read_pico_webmark_t *out, size_t cap, void *ctx); ///< 可选：列出网页收藏 / Optional: list web bookmarks
    esp_err_t (*webmarks_upsert_cb)(const char *title, const char *url, void *ctx); ///< 可选：添加或更新网页收藏 / Optional: upsert one web bookmark
    esp_err_t (*webmarks_delete_cb)(const char *url, void *ctx); ///< 可选：删除网页收藏 / Optional: delete one web bookmark
    void *webmarks_ctx; ///< 网页收藏回调上下文 / Context for web bookmark callbacks
    size_t (*feeds_list_cb)(read_pico_feed_source_t *out, size_t cap, void *ctx); ///< 可选：列出脱敏订阅源 / Optional: list redacted feed sources
    esp_err_t (*feeds_upsert_cb)(const char *name, const char *url, uint8_t kind, const char *auth_user, const char *auth_pass, bool clear_auth, void *ctx); ///< 可选：添加或更新订阅源 / Optional: upsert one feed source
    esp_err_t (*feeds_delete_cb)(const char *url, void *ctx); ///< 可选：删除订阅源 / Optional: delete one feed source
    void *feeds_ctx; ///< 订阅源回调上下文 / Context for feed source callbacks
    bool (*focus_get_cb)(read_pico_focus_status_t *out, void *ctx); ///< 可选：读取专注时钟状态 / Optional: read focus timer status
    esp_err_t (*focus_ctrl_cb)(bool start, int minutes, void *ctx); ///< 可选：远程启动或停止专注时钟 / Optional: start or stop focus timer remotely
    void *focus_ctx; ///< 专注时钟回调上下文 / Context for focus timer callbacks
    bool (*stop_request_cb)(void *ctx); ///< 可选：经局域网 HTTP 请求主循环延迟退出传书页 / Optional: request main loop to stop Transfer and exit over LAN HTTP
    void *stop_request_ctx; ///< 远程停止回调上下文 / Context for stop request callback
} read_pico_transfer_cfg_t;

typedef struct {
    read_pico_transfer_state_t state; ///< 当前状态 / Current state
    read_pico_transfer_mode_t mode; ///< 网络模式 / Network mode
    char url[64]; ///< 可访问地址，未联网为空 / Reachable address, empty when offline
    char mdns_host[64]; ///< mDNS 主机名，不含 .local / mDNS hostname without .local
    char wifi_ssid[33]; ///< 已保存网络名称，不含密码 / Saved network name, never a password
    char wifi_ssids[5][33]; ///< 已保存网络名，不含密码 / Saved network names, never passwords
    uint8_t wifi_count; ///< 已保存网络数 / Number of saved networks
    bool wifi_configured; ///< 存在有效凭据 / Valid credentials saved
    bool network_ready; ///< 已取得可用网络地址 / Usable network address acquired
    unsigned sta_count; ///< 连接数 / Station count
    char ssid[33]; ///< 热点名称 / AP name
    char pin[7]; ///< 本次传输会话的 6 位配对码（仅屏幕显示，不经公开接口回显）/ 6-digit session PIN (on-screen only, never echoed by public APIs)
    bool pin_required; ///< 当前会话是否强制校验配对码 / Whether PIN auth is enforced in this session
    char cur_name[121]; ///< 完整 UTF-8 文件名 / Complete UTF-8 filename
    size_t cur_bytes; ///< 已接收字节 / Received bytes
    size_t cur_total; ///< 本次总字节 / Request total bytes
    unsigned done_count; ///< 成功提交数 / Successfully committed files
    unsigned changed_count; ///< 提交、删除或进度清理重试成功的次数 / Commits, deletions, or successful progress-cleanup retries
    esp_err_t last_error; ///< 最近错误 / Last error
} read_pico_transfer_status_t;

#define READ_PICO_AGENT_URI_MAX 192
#define READ_PICO_AGENT_USER_MAX 64
#define READ_PICO_AGENT_PASSWORD_MAX 128
#define READ_PICO_AGENT_TOPIC_MAX 160

typedef struct {
    char broker_uri[READ_PICO_AGENT_URI_MAX]; ///< mqtts:// TLS 地址 / TLS broker URI
    char username[READ_PICO_AGENT_USER_MAX]; ///< 可为空 / Optional
    char password[READ_PICO_AGENT_PASSWORD_MAX]; ///< 可为空；仅供本机调用，不进状态接口 / Optional; local caller only, never exposed in status APIs
    char topic[READ_PICO_AGENT_TOPIC_MAX]; ///< 精确订阅主题，不含通配符 / Exact subscription topic without wildcards
} read_pico_agent_config_t;

/// 同一控制任务串行启动/停止；重复启动返回状态错误。/ Serialize start/stop on one owner task; repeated start fails.
esp_err_t read_pico_transfer_start(const read_pico_transfer_cfg_t *cfg);
/// 等待请求退出并释放本组件资源；可重复调用。/ Join requests and release owned resources; repeatable.
void read_pico_transfer_stop(void);
/// 原子拒绝文件操作期间的停止；成功则已停服，由同一控制任务调用。/ Atomically refuse stopping during file operations; true means stopped. Owner task only.
bool read_pico_transfer_try_stop_if_idle(void);
/// 跨任务复制一致状态快照。/ Copy a consistent snapshot across tasks.
void read_pico_transfer_get_status(read_pico_transfer_status_t *out);
/// 控制任务每500ms驱动有界连接重试与15秒超时。/ Owner task polls every 500 ms for bounded retries and a 15 s timeout.
void read_pico_transfer_service_poll(void);
/// 只读最近优先的已保存SSID；未配置仍返回ESP_OK。/ Read the most recent saved SSID; unconfigured still returns ESP_OK.
esp_err_t read_pico_transfer_get_saved_wifi(char ssid[33], bool *configured);
/// 只读最多五个已保存SSID；不含密码。/ Read up to five saved SSIDs without passwords.
esp_err_t read_pico_transfer_get_saved_wifi_list(char ssids[5][33], size_t *count);
/// 读取独立 NVS 中的 Agent 配置；首次读取时兼容迁移旧文件。/ Read Agent configuration from dedicated NVS, migrating the legacy file on first access.
esp_err_t read_pico_transfer_get_agent_config(read_pico_agent_config_t *out);
/// 校验并写入独立 NVS 中的 Agent 配置；传 NULL 清除。/ Validate and store Agent configuration in dedicated NVS; pass NULL to clear.
esp_err_t read_pico_transfer_set_agent_config(const read_pico_agent_config_t *cfg);
/// 配置变更计数，用于运行中的 Agent 页面发现网页更新。/ Configuration revision for live Agent pages to detect browser updates.
uint32_t read_pico_transfer_agent_config_version(void);
/// 仅热点或停止状态可遗忘；上传中拒绝。/ Forget only while AP or stopped; rejected during upload.
esp_err_t read_pico_transfer_forget_wifi(void);
/// 单独遗忘SSID；仅热点或停止状态可调用。/ Forget one SSID while AP or stopped.
esp_err_t read_pico_transfer_forget_wifi_ssid(const char *ssid);
/// 停服后同步扫描2.4GHz网络；最多16个去重SSID，按信号降序，所有临时资源均释放。/ Scan synchronously while stopped; up to 16 unique SSIDs by descending RSSI; release all temporary resources.
esp_err_t read_pico_transfer_scan_wifi(read_pico_transfer_network_t out[READ_PICO_TRANSFER_SCAN_MAX], size_t *count);
/// 停服后保存设备输入的凭据，不自动连接；密码校验与网页相同。/ Save device-entered credentials while stopped without connecting; validation matches the webpage.
esp_err_t read_pico_transfer_save_wifi(const char *ssid, const char *password);
/// OTA 专用 STA 临时联网；返回前等待取得 IP，必须配对调用 stop。/ Temporarily connect STA for OTA and wait for an IP; pair with stop.
esp_err_t read_pico_transfer_ota_wifi_start(uint32_t timeout_ms);
/// 释放 OTA 专用 Wi-Fi 资源。/ Release the OTA-only Wi-Fi resources.
void read_pico_transfer_ota_wifi_stop(void);
/// 原子取出最近一次手机推送网址；没有新网址时返回 false。/ Take the latest phone-pushed URL; false means none is pending.
bool read_pico_transfer_take_webopen_url(char url[READ_PICO_WEBOPEN_URL_MAX]);
/// 清除当前会话配对码，下次启动服务时重新生成 6 位随机码。/ Clear session PIN so the next start generates a fresh 6-digit PIN.
void read_pico_transfer_reset_pin(void);
