/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：独立 NimBLE 外设传输层：广播 ReadPico-XXXX 与厂商状态信标，提供标准 DIS/BAS 与配套控制 GATT 服务。
 *
 * English: Standalone NimBLE peripheral transport: advertises ReadPico-XXXX with a status beacon and exposes standard DIS/BAS plus the companion control GATT service.
 *
 * 冻结：不依赖页面或文件系统；所有写命令通过回调移交给上层主循环处理，不在此组件内访问 I2C 或大文件。
 * Frozen: No page or filesystem dependency; hand write commands to the caller's main loop and never access I2C or bulk files here.
 */
#include "read_pico_ble.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "read_pico_ble_framing.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "read_pico_ble";

#define BLE_DEFAULT_MTU 23U
#define BLE_MAX_ATT_CHUNK 512U
#define BLE_STATUS_READ_MAX 512U
#define BLE_NOTIFY_RETRIES 8

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_host_stopped_sem;
static SemaphoreHandle_t s_tx_mutex;

static bool s_running;
static bool s_stopping;
static bool s_synced;
static uint8_t s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_tx_msg_id;

static read_pico_ble_callbacks_t s_cbs;
static read_pico_ble_status_t s_status;
static ble_adv_beacon_t s_adv_beacon;
static uint8_t s_mfg_data[READ_PICO_BLE_MFG_DATA_LEN];

static char *s_rx_buf;
static ble_rx_reassembler_t s_rx;

static uint16_t s_status_val_handle;
static uint16_t s_rsp_val_handle;
static uint16_t s_battery_val_handle;

static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(READ_PICO_BLE_UUID128_BYTES(0x00));
static const ble_uuid128_t s_chr_status_uuid = BLE_UUID128_INIT(READ_PICO_BLE_UUID128_BYTES(0x01));
static const ble_uuid128_t s_chr_cmd_uuid = BLE_UUID128_INIT(READ_PICO_BLE_UUID128_BYTES(0x02));
static const ble_uuid128_t s_chr_rsp_uuid = BLE_UUID128_INIT(READ_PICO_BLE_UUID128_BYTES(0x03));

static const ble_uuid16_t s_dis_svc_uuid = BLE_UUID16_INIT(READ_PICO_BLE_DIS_SVC_UUID);
static const ble_uuid16_t s_dis_mfr_uuid = BLE_UUID16_INIT(READ_PICO_BLE_DIS_MFR_UUID);
static const ble_uuid16_t s_dis_model_uuid = BLE_UUID16_INIT(READ_PICO_BLE_DIS_MODEL_UUID);
static const ble_uuid16_t s_dis_fw_uuid = BLE_UUID16_INIT(READ_PICO_BLE_DIS_FW_REV_UUID);
static const ble_uuid16_t s_dis_sw_uuid = BLE_UUID16_INIT(READ_PICO_BLE_DIS_SW_REV_UUID);

static const ble_uuid16_t s_bas_svc_uuid = BLE_UUID16_INIT(READ_PICO_BLE_BAS_SVC_UUID);
static const ble_uuid16_t s_bas_lvl_uuid = BLE_UUID16_INIT(READ_PICO_BLE_BAS_LEVEL_UUID);

static int gap_event_cb(struct ble_gap_event *event, void *arg);
static void start_advertising_locked_or_not(void);

static void *ble_alloc(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

static void build_device_name(char out[READ_PICO_BLE_NAME_MAX]) {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
        (void)esp_read_mac(mac, ESP_MAC_BASE);
    }
    snprintf(out, READ_PICO_BLE_NAME_MAX, "ReadPico-%02X%02X", mac[4], mac[5]);
}

static void refresh_mfg_bytes(void) {
    ble_adv_beacon_t beacon;
    portENTER_CRITICAL(&s_lock);
    beacon = s_adv_beacon;
    portEXIT_CRITICAL(&s_lock);
    ble_adv_mfg_encode(&beacon, s_mfg_data);
}

static int apply_scan_response(void) {
    char name[READ_PICO_BLE_NAME_MAX];
    portENTER_CRITICAL(&s_lock);
    memcpy(name, s_status.device_name, sizeof(name));
    portEXIT_CRITICAL(&s_lock);

    refresh_mfg_bytes();
    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.name = (const uint8_t *)name;
    rsp_fields.name_len = (uint8_t)strlen(name);
    rsp_fields.name_is_complete = 1;
    rsp_fields.mfg_data = s_mfg_data;
    rsp_fields.mfg_data_len = sizeof(s_mfg_data);
    return ble_gap_adv_rsp_set_fields(&rsp_fields);
}

static void start_advertising_locked_or_not(void) {
    bool allow = false;
    portENTER_CRITICAL(&s_lock);
    allow = s_running && !s_stopping && s_synced && s_conn_handle == BLE_HS_CONN_HANDLE_NONE;
    portEXIT_CRITICAL(&s_lock);
    if (!allow) return;

    struct ble_hs_adv_fields adv_fields;
    memset(&adv_fields, 0, sizeof(adv_fields));
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.uuids128 = &s_svc_uuid;
    adv_fields.num_uuids128 = 1;
    adv_fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        portENTER_CRITICAL(&s_lock);
        s_status.state = READ_PICO_BLE_ERROR;
        s_status.last_error = ESP_FAIL;
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    rc = apply_scan_response();
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed: %d", rc);
        portENTER_CRITICAL(&s_lock);
        s_status.state = READ_PICO_BLE_ERROR;
        s_status.last_error = ESP_FAIL;
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // 160..200 ms 广播间隔：兼顾 iOS 秒级发现与待机功耗。
    // 160..200 ms advertising interval: balances fast iOS discovery with idle power.
    adv_params.itvl_min = 256;
    adv_params.itvl_max = 320;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
        portENTER_CRITICAL(&s_lock);
        s_status.state = READ_PICO_BLE_ERROR;
        s_status.last_error = ESP_FAIL;
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    portENTER_CRITICAL(&s_lock);
    s_status.state = READ_PICO_BLE_ADVERTISING;
    s_status.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "advertising as %s", s_status.device_name);
}

/* ---- GATT 回调 / GATT callbacks ---- */
static int dis_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint16_t uuid16 = ble_uuid_u16(ctxt->chr->uuid);
    const char *text = "";
    switch (uuid16) {
        case READ_PICO_BLE_DIS_MFR_UUID:
            text = "MindReset";
            break;
        case READ_PICO_BLE_DIS_MODEL_UUID:
            text = "Read Pico (RDP-G01-W)";
            break;
        case READ_PICO_BLE_DIS_FW_REV_UUID: {
            const esp_app_desc_t *app = esp_app_get_description();
            text = (app && app->version[0]) ? app->version : "unknown";
            break;
        }
        case READ_PICO_BLE_DIS_SW_REV_UUID:
            text = "Pico OS";
            break;
        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(ctxt->om, text, (uint16_t)strlen(text)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int bas_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t level = 0;
    if (s_cbs.on_battery_read) level = s_cbs.on_battery_read(s_cbs.ctx);
    if (level > 100U) level = 100U;
    return os_mbuf_append(ctxt->om, &level, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int companion_status_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                      struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    char *buf = ble_alloc(BLE_STATUS_READ_MAX);
    if (!buf) return BLE_ATT_ERR_INSUFFICIENT_RES;
    size_t len = 0;
    if (s_cbs.on_status_read) {
        len = s_cbs.on_status_read(buf, BLE_STATUS_READ_MAX, s_cbs.ctx);
    } else {
        len = (size_t)snprintf(buf, BLE_STATUS_READ_MAX, "{\"ble_api\":%u}", (unsigned)READ_PICO_BLE_API_VERSION);
    }
    if (len >= BLE_STATUS_READ_MAX) len = BLE_STATUS_READ_MAX - 1U;
    int rc = os_mbuf_append(ctxt->om, buf, (uint16_t)len);
    free(buf);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int companion_cmd_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
    if (om_len == 0U || om_len > BLE_MAX_ATT_CHUNK) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t flat[BLE_MAX_ATT_CHUNK];
    uint16_t copied = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, flat, sizeof(flat), &copied) != 0 || copied != om_len) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    size_t msg_len = 0;
    int64_t now_ms = esp_timer_get_time() / 1000LL;
    ble_rx_result_t res = ble_rx_feed(&s_rx, flat, copied, now_ms, &msg_len);
    if (res == BLE_RX_ERROR) {
        ESP_LOGW(TAG, "invalid BLE command frame len=%u", (unsigned)copied);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (res == BLE_RX_COMPLETE) {
        portENTER_CRITICAL(&s_lock);
        ++s_status.rx_count;
        portEXIT_CRITICAL(&s_lock);
        if (s_cbs.on_command) {
            s_cbs.on_command(s_rx.buf, msg_len, s_cbs.ctx);
        }
    }
    return 0;
}

static int companion_rsp_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_dis_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_dis_mfr_uuid.u,
                .access_cb = dis_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &s_dis_model_uuid.u,
                .access_cb = dis_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &s_dis_fw_uuid.u,
                .access_cb = dis_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &s_dis_sw_uuid.u,
                .access_cb = dis_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_bas_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_bas_lvl_uuid.u,
                .access_cb = bas_access_cb,
                .val_handle = &s_battery_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_chr_status_uuid.u,
                .access_cb = companion_status_access_cb,
                .val_handle = &s_status_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &s_chr_cmd_uuid.u,
                .access_cb = companion_cmd_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_chr_rsp_uuid.u,
                .access_cb = companion_rsp_access_cb,
                .val_handle = &s_rsp_val_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ---- GAP 与协议栈事件 / GAP and stack events ---- */
static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ble_rx_reset(&s_rx);
                portENTER_CRITICAL(&s_lock);
                s_conn_handle = event->connect.conn_handle;
                s_status.state = READ_PICO_BLE_CONNECTED;
                s_status.peer_connected = true;
                s_status.mtu = BLE_DEFAULT_MTU;
                s_status.status_notify_enabled = false;
                s_status.rsp_notify_enabled = false;
                s_status.battery_notify_enabled = false;
                portEXIT_CRITICAL(&s_lock);
                ESP_LOGI(TAG, "connected handle=%u", (unsigned)event->connect.conn_handle);
                if (s_cbs.on_conn_change) s_cbs.on_conn_change(true, s_cbs.ctx);
            } else {
                start_advertising_locked_or_not();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ble_rx_reset(&s_rx);
            portENTER_CRITICAL(&s_lock);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_status.peer_connected = false;
            s_status.mtu = BLE_DEFAULT_MTU;
            s_status.status_notify_enabled = false;
            s_status.rsp_notify_enabled = false;
            s_status.battery_notify_enabled = false;
            if (s_running && !s_stopping) s_status.state = READ_PICO_BLE_ADVERTISING;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "disconnected reason=%d", event->disconnect.reason);
            if (s_cbs.on_conn_change) s_cbs.on_conn_change(false, s_cbs.ctx);
            start_advertising_locked_or_not();
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            start_advertising_locked_or_not();
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            portENTER_CRITICAL(&s_lock);
            if (event->subscribe.attr_handle == s_status_val_handle) {
                s_status.status_notify_enabled = event->subscribe.cur_notify != 0;
            } else if (event->subscribe.attr_handle == s_rsp_val_handle) {
                s_status.rsp_notify_enabled = event->subscribe.cur_notify != 0;
            } else if (event->subscribe.attr_handle == s_battery_val_handle) {
                s_status.battery_notify_enabled = event->subscribe.cur_notify != 0;
            }
            portEXIT_CRITICAL(&s_lock);
            return 0;

        case BLE_GAP_EVENT_MTU:
            portENTER_CRITICAL(&s_lock);
            s_status.mtu = event->mtu.value >= BLE_DEFAULT_MTU ? event->mtu.value : BLE_DEFAULT_MTU;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "MTU updated: %u", (unsigned)event->mtu.value);
            return 0;

        default:
            return 0;
    }
}

static void on_stack_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE host reset reason=%d", reason);
    portENTER_CRITICAL(&s_lock);
    s_synced = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_status.peer_connected = false;
    s_status.status_notify_enabled = false;
    s_status.rsp_notify_enabled = false;
    s_status.battery_notify_enabled = false;
    portEXIT_CRITICAL(&s_lock);
}

static void on_stack_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
        portENTER_CRITICAL(&s_lock);
        s_status.state = READ_PICO_BLE_ERROR;
        s_status.last_error = ESP_FAIL;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        portENTER_CRITICAL(&s_lock);
        s_status.state = READ_PICO_BLE_ERROR;
        s_status.last_error = ESP_FAIL;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_synced = true;
    portEXIT_CRITICAL(&s_lock);
    start_advertising_locked_or_not();
}

static void nimble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
    if (s_host_stopped_sem) xSemaphoreGive(s_host_stopped_sem);
}

/* ---- 发送封装 / Notification helpers ---- */
static esp_err_t notify_packet_locked(uint16_t conn_handle, uint16_t val_handle,
                                      const uint8_t *data, size_t len) {
    for (int attempt = 0; attempt < BLE_NOTIFY_RETRIES; ++attempt) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
        if (!om) {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }
        // NimBLE 无论成功或失败均接管并释放 om，不得手动释放。
        // NimBLE always consumes om on success or failure; never free it manually.
        int rc = ble_gatts_notify_custom(conn_handle, val_handle, om);
        if (rc == 0) return ESP_OK;
        if (rc == BLE_HS_ENOMEM) {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }
        return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t send_framed_notify(uint16_t val_handle, bool need_rsp_sub,
                                    const char *json, size_t len) {
    if (!json || !len || len > READ_PICO_BLE_PAYLOAD_MAX) return ESP_ERR_INVALID_ARG;
    if (!s_tx_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
    uint16_t mtu = BLE_DEFAULT_MTU;
    bool subscribed = false;
    portENTER_CRITICAL(&s_lock);
    if (s_running && !s_stopping && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        conn_handle = s_conn_handle;
        mtu = s_status.mtu;
        subscribed = need_rsp_sub ? s_status.rsp_notify_enabled : s_status.status_notify_enabled;
    }
    portEXIT_CRITICAL(&s_lock);

    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || !subscribed) {
        xSemaphoreGive(s_tx_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    size_t chunks = ble_tx_chunk_count(len, mtu);
    if (chunks == 0U) {
        xSemaphoreGive(s_tx_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t msg_id = ++s_tx_msg_id;
    uint8_t pkt[BLE_MAX_ATT_CHUNK];
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < chunks; ++i) {
        size_t pkt_len = ble_tx_encode_chunk(json, len, mtu, msg_id, i, pkt, sizeof(pkt));
        if (pkt_len == 0U) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        err = notify_packet_locked(conn_handle, val_handle, pkt, pkt_len);
        if (err != ESP_OK) break;
        if (chunks > 1U && i + 1U < chunks) vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        ++s_status.tx_count;
        portEXIT_CRITICAL(&s_lock);
    }
    xSemaphoreGive(s_tx_mutex);
    return err;
}

/* ---- 公开 API / Public API ---- */
esp_err_t read_pico_ble_start(const read_pico_ble_callbacks_t *cbs) {
    if (s_running) return ESP_ERR_INVALID_STATE;

    if (!s_tx_mutex) {
        s_tx_mutex = xSemaphoreCreateMutex();
        if (!s_tx_mutex) return ESP_ERR_NO_MEM;
    }
    if (!s_host_stopped_sem) {
        s_host_stopped_sem = xSemaphoreCreateBinary();
        if (!s_host_stopped_sem) return ESP_ERR_NO_MEM;
    }
    while (xSemaphoreTake(s_host_stopped_sem, 0) == pdTRUE) {}

    if (!s_rx_buf) {
        s_rx_buf = ble_alloc(READ_PICO_BLE_PAYLOAD_MAX + 1U);
        if (!s_rx_buf) return ESP_ERR_NO_MEM;
    }
    ble_rx_init(&s_rx, s_rx_buf, READ_PICO_BLE_PAYLOAD_MAX);

    portENTER_CRITICAL(&s_lock);
    s_cbs = cbs ? *cbs : (read_pico_ble_callbacks_t){0};
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = READ_PICO_BLE_STARTING;
    s_status.mtu = BLE_DEFAULT_MTU;
    build_device_name(s_status.device_name);
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_synced = false;
    s_stopping = false;
    s_running = true;
    portEXIT_CRITICAL(&s_lock);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        portENTER_CRITICAL(&s_lock);
        s_running = false;
        s_status.state = READ_PICO_BLE_ERROR;
        s_status.last_error = err;
        portEXIT_CRITICAL(&s_lock);
        return err;
    }

    ble_hs_cfg.reset_cb = on_stack_reset;
    ble_hs_cfg.sync_cb = on_stack_sync;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
#if MYNEWT_VAL(BLE_SM_SC)
    ble_hs_cfg.sm_sc = 1;
#endif

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc == 0) rc = ble_svc_gap_device_name_set(s_status.device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT/GAP setup failed: %d", rc);
        (void)nimble_port_deinit();
        portENTER_CRITICAL(&s_lock);
        s_running = false;
        s_status.state = READ_PICO_BLE_ERROR;
        s_status.last_error = ESP_FAIL;
        portEXIT_CRITICAL(&s_lock);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "BLE started name=%s", s_status.device_name);
    return ESP_OK;
}

void read_pico_ble_stop(void) {
    if (!s_running) return;

    uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
    portENTER_CRITICAL(&s_lock);
    s_stopping = true;
    conn_handle = s_conn_handle;
    portEXIT_CRITICAL(&s_lock);

    if (ble_gap_adv_active()) (void)ble_gap_adv_stop();
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    int rc = nimble_port_stop();
    if (rc == 0 && s_host_stopped_sem) {
        if (xSemaphoreTake(s_host_stopped_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
            ESP_LOGW(TAG, "timed out waiting for NimBLE host task exit");
        }
    }
    (void)nimble_port_deinit();

    if (s_rx_buf) {
        free(s_rx_buf);
        s_rx_buf = NULL;
    }
    ble_rx_init(&s_rx, NULL, 0);

    portENTER_CRITICAL(&s_lock);
    s_running = false;
    s_stopping = false;
    s_synced = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_status.state = READ_PICO_BLE_STOPPED;
    s_status.peer_connected = false;
    s_status.mtu = BLE_DEFAULT_MTU;
    s_status.status_notify_enabled = false;
    s_status.rsp_notify_enabled = false;
    s_status.battery_notify_enabled = false;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "BLE stopped");
}

bool read_pico_ble_is_running(void) {
    bool running;
    portENTER_CRITICAL(&s_lock);
    running = s_running && !s_stopping;
    portEXIT_CRITICAL(&s_lock);
    return running;
}

void read_pico_ble_get_status(read_pico_ble_status_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t read_pico_ble_send_response(const char *json, size_t len) {
    return send_framed_notify(s_rsp_val_handle, true, json, len);
}

esp_err_t read_pico_ble_notify_status(const char *json, size_t len) {
    return send_framed_notify(s_status_val_handle, false, json, len);
}

esp_err_t read_pico_ble_notify_battery(uint8_t soc_percent) {
    if (soc_percent > 100U) soc_percent = 100U;
    if (!s_tx_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;

    uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
    bool subscribed = false;
    portENTER_CRITICAL(&s_lock);
    if (s_running && !s_stopping && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        conn_handle = s_conn_handle;
        subscribed = s_status.battery_notify_enabled;
    }
    portEXIT_CRITICAL(&s_lock);

    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE && subscribed) {
        err = notify_packet_locked(conn_handle, s_battery_val_handle, &soc_percent, 1U);
    }
    xSemaphoreGive(s_tx_mutex);
    return err;
}

void read_pico_ble_update_adv_state(const read_pico_ble_adv_state_t *state) {
    if (!state) return;
    ble_adv_beacon_t next = {
        .battery_soc = state->battery_soc,
        .battery_valid = state->battery_valid,
        .charging = state->charging,
        .transfer_active = state->transfer_active,
        .transfer_mode = state->transfer_mode,
        .wifi_configured = state->wifi_configured,
        .sd_mounted = state->sd_mounted,
        .pin_required = state->pin_required,
        .fw_major = state->fw_major,
        .fw_minor = state->fw_minor,
        .fw_patch = state->fw_patch,
    };

    bool adv_active = false;
    portENTER_CRITICAL(&s_lock);
    bool changed = memcmp(&s_adv_beacon, &next, sizeof(next)) != 0;
    s_adv_beacon = next;
    adv_active = s_running && !s_stopping && s_synced && s_conn_handle == BLE_HS_CONN_HANDLE_NONE;
    portEXIT_CRITICAL(&s_lock);

    if (changed && adv_active && ble_gap_adv_active()) {
        (void)apply_scan_response();
    }
}
