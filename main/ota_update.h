/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * GitHub Release OTA 工作流与可取消的备用分区写入。
 * GitHub Release OTA checks and cancellable writes to the inactive slot.
 * 冻结：只在完整校验后切换启动分区；失败或取消不触碰当前槽位。
 * Frozen: Select the boot slot only after full verification; failures leave the running slot untouched.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "ota_policy.h"

typedef enum {
    OTA_UPDATE_IDLE,
    OTA_UPDATE_CHECKING,
    OTA_UPDATE_AVAILABLE,
    OTA_UPDATE_CURRENT,
    OTA_UPDATE_DOWNLOADING,
    OTA_UPDATE_DONE,
    OTA_UPDATE_ERROR,
    OTA_UPDATE_CANCELLED,
} ota_update_state_t;

typedef struct {
    ota_update_state_t state;
    unsigned progress_percent;
    char current_version[32];
    char latest_version[32];
    char message[96];
} ota_update_status_t;

/// 启动 GitHub Releases 检查任务。/ Start the GitHub Releases check task.
bool ota_update_check_start(void);
/// 启动经过 SHA-256 校验的 OTA 下载任务。/ Start a SHA-256 verified OTA download.
bool ota_update_install_start(void);
/// 请求停止当前下载并安全中止 OTA。/ Request cancellation and abort the OTA safely.
void ota_update_cancel(void);
/// 复制一致状态快照。/ Copy a consistent status snapshot.
void ota_update_get_status(ota_update_status_t *out);
/// 比较 v-prefixed numeric versions; returns -1, 0 or 1. / Compare v-prefixed numeric versions.
/// Parse a GitHub release response and retrieve asset URL and SHA-256. / Parse release JSON metadata.
bool ota_update_parse_release(const char *json, char version[32], char url[512], char checksum_url[512], char sha256_hex[65]);
