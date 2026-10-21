// SPDX-License-Identifier: Apache-2.0

#include "magpie_ble.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "magpie_ble";

/*
 * UUIDs are written in little-endian byte order. They match the companion's
 * 7a3d0001-9c4e-4b8a-9f31-6d6167706965 service family (see docs/protocol.md
 * for the canonical form).
 */
#define MAGPIE_UUID_TAIL                                                     \
    0x65, 0x69, 0x70, 0x67, 0x61, 0x6d, 0x31, 0x9f, 0x8a, 0x4b, 0x4e, 0x9c

static const ble_uuid128_t kServiceUuid =
    BLE_UUID128_INIT(MAGPIE_UUID_TAIL, 0x01, 0x00, 0x3d, 0x7a);
static const ble_uuid128_t kIndexUuid =
    BLE_UUID128_INIT(MAGPIE_UUID_TAIL, 0x02, 0x00, 0x3d, 0x7a);
static const ble_uuid128_t kSubUuid =
    BLE_UUID128_INIT(MAGPIE_UUID_TAIL, 0x03, 0x00, 0x3d, 0x7a);

#define CONNECT_TIMEOUT_MS 8000
#define ATT_POLL_STEP_MS 20
#define SCAN_RESTART_DELAY_MS 2000
#define NOTIF_MAX 192

/* One signal result shared with the ATT callbacks of the calling task. */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    uint16_t svc_start;
    uint16_t svc_end;
    int status;
    bool done;
} att_result_t;

struct magpie_ble_source {
    magpie_ble_config_t config;
    volatile bool host_synced;
    volatile bool connected;
    volatile bool discovered;
    volatile bool scan_done;
    volatile uint16_t conn_handle;
    uint16_t index_handle;
    uint16_t sub_handle;
    /* Last INDEX notification, handed to the store on the next refresh. */
    char notif[NOTIF_MAX];
    volatile size_t notif_len;
    volatile bool notif_pending;
};

/* One central per device: the source instance lives here, so callers only
 * need the magpie_source_t vtable. */
static magpie_ble_source_t s_ble;
static magpie_ble_source_t *s_active;
static bool s_host_started;

static void scan_start(magpie_ble_source_t *self);
static int gap_event(struct ble_gap_event *event, void *arg);

/* ---- host lifecycle -------------------------------------------------- */

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_sync(void)
{
    magpie_ble_source_t *self = s_active;
    if (!self) {
        return;
    }
    uint8_t own_addr_type = 0;
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        ESP_LOGE(TAG, "no usable BLE identity address");
        return;
    }
    if (self->config.mtu >= 23) {
        (void)ble_att_set_preferred_mtu(self->config.mtu);
    }
    self->host_synced = true;
    ESP_LOGI(TAG, "NimBLE synced (preferred MTU %u, own addr type %u)",
             (unsigned)self->config.mtu, (unsigned)own_addr_type);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset (%d)", reason);
    if (s_active) {
        s_active->host_synced = false;
        s_active->connected = false;
        s_active->discovered = false;
    }
}

/* ---- ATT callbacks --------------------------------------------------- */

static int attr_len(const struct os_mbuf *om)
{
    return OS_MBUF_PKTLEN(om);
}

static int copy_attr(const struct os_mbuf *om, char *buf, size_t cap)
{
    const int total = attr_len(om);
    if (total < 0 || (size_t)total + 1 > cap) {
        return -1;
    }
    if (os_mbuf_copydata(om, 0, total, buf) != 0) {
        return -1;
    }
    buf[total] = '\0';
    return total;
}

static int on_read(uint16_t conn_handle, const struct ble_gatt_error *error,
                   struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    att_result_t *result = arg;
    if (error->status == 0 && attr) {
        const int len = copy_attr(attr->om, result->buf, result->cap);
        if (len < 0) {
            result->status = BLE_HS_EMSGSIZE;
        } else {
            result->len = (size_t)len;
            result->status = 0;
        }
    } else {
        result->status = error->status;
    }
    result->done = true;
    return 0;
}

static int on_write(uint16_t conn_handle, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    att_result_t *result = arg;
    result->status = error->status;
    result->done = true;
    return 0;
}

static int on_chr(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn_handle;
    magpie_ble_source_t *self = arg;
    if (error->status == BLE_HS_EDONE) {
        self->discovered = self->index_handle != 0 && self->sub_handle != 0;
        ESP_LOGI(TAG, "discovered INDEX=0x%04x SUB=0x%04x (%s)",
                 (unsigned)self->index_handle, (unsigned)self->sub_handle,
                 self->discovered ? "ready" : "characteristics missing");
        return 0;
    }
    if (error->status != 0 || !chr) {
        return 0;
    }
    if (ble_uuid_cmp(&chr->uuid.u, &kIndexUuid.u) == 0) {
        self->index_handle = chr->val_handle;
    } else if (ble_uuid_cmp(&chr->uuid.u, &kSubUuid.u) == 0) {
        self->sub_handle = chr->val_handle;
    }
    return 0;
}

static int on_svc(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *service, void *arg)
{
    (void)conn_handle;
    att_result_t *result = arg;
    if (error->status == BLE_HS_EDONE) {
        if (result->svc_start == 0) {
            result->status = BLE_HS_ENOENT;
        }
        result->done = true;
        return 0;
    }
    if (error->status != 0 || !service) {
        result->status = error->status;
        result->done = true;
        return 0;
    }
    result->svc_start = service->start_handle;
    result->svc_end = service->end_handle;
    result->status = 0;
    return 0;
}

/* Blocks the calling task until the ATT procedure reports completion. */
static int att_wait(att_result_t *result, uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (!result->done && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(ATT_POLL_STEP_MS));
        waited += ATT_POLL_STEP_MS;
    }
    return result->done ? result->status : BLE_HS_ETIMEOUT;
}

/* ---- GAP ------------------------------------------------------------- */

static void scan_start(magpie_ble_source_t *self)
{
    uint8_t own_addr_type = 0;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        return;
    }
    struct ble_gap_disc_params params = {0};
    params.filter_duplicates = 1;
    params.passive = 1;
    params.itvl = 0;
    params.window = 0;
    self->scan_done = false;
    const int rc = ble_gap_disc(own_addr_type,
                                self->config.scan_ms ? self->config.scan_ms
                                                     : 4000,
                                &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "scan start failed (%d)", rc);
    }
}

/* Looks for the display service UUID (or the configured name) in an
 * advertisement without relying on the host's parsed-field copy. */
static bool adv_matches(const struct ble_gap_disc_desc *disc,
                        const magpie_ble_config_t *config)
{
    const uint8_t *data = disc->data;
    const uint8_t len = disc->length_data;
    const size_t prefix_len = strlen(config->name_prefix);
    for (uint8_t offset = 0; offset + 1 < len;) {
        const uint8_t field_len = data[offset];
        if (field_len == 0 || offset + 1 + field_len > len) {
            break;
        }
        const uint8_t type = data[offset + 1];
        const uint8_t *payload = &data[offset + 2];
        const uint8_t payload_len = (uint8_t)(field_len - 1);
        if ((type == BLE_HS_ADV_TYPE_COMP_UUIDS128 ||
             type == BLE_HS_ADV_TYPE_INCOMP_UUIDS128) &&
            payload_len >= 16) {
            for (uint8_t i = 0; i + 16 <= payload_len; i += 16) {
                if (memcmp(&payload[i], &kServiceUuid.value[0], 16) == 0) {
                    return true;
                }
            }
        }
        if (prefix_len && type == BLE_HS_ADV_TYPE_COMP_NAME &&
            payload_len >= prefix_len &&
            memcmp(payload, config->name_prefix, prefix_len) == 0) {
            return true;
        }
        offset = (uint8_t)(offset + 1 + field_len);
    }
    return false;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    magpie_ble_source_t *self = s_active;
    (void)arg;
    if (!self) {
        return 0;
    }
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        if (!adv_matches(&event->disc, &self->config)) {
            return 0;
        }
        if (ble_gap_disc_active()) {
            (void)ble_gap_disc_cancel();
        }
        uint8_t own_addr_type = 0;
        if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
            return 0;
        }
        const int rc = ble_gap_connect(own_addr_type, &event->disc.addr,
                                       CONNECT_TIMEOUT_MS, NULL, gap_event, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "connect failed (%d)", rc);
        }
        return 0;
    }
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            self->conn_handle = event->connect.conn_handle;
            self->connected = true;
            ESP_LOGI(TAG, "connected (handle %u)",
                     (unsigned)event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "connection failed (%d)", event->connect.status);
            self->connected = false;
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected (reason 0x%x)", event->disconnect.reason);
        self->connected = false;
        self->discovered = false;
        self->index_handle = 0;
        self->sub_handle = 0;
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.attr_handle != self->index_handle) {
            return 0;
        }
        {
            const int len = copy_attr(event->notify_rx.om, self->notif,
                                      sizeof(self->notif));
            if (len > 0) {
                self->notif_len = (size_t)len;
                self->notif_pending = true;
            }
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "ATT MTU %u", (unsigned)event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        self->scan_done = true;
        return 0;
    default:
        return 0;
    }
}

/* ---- source vtable --------------------------------------------------- */

static int ble_open(magpie_source_t *source)
{
    magpie_ble_source_t *self = source->impl;
    if (!self) {
        return MAGPIE_SOURCE_ERR_ARG;
    }
    if (!self->host_synced) {
        /* The host task may still be starting; wait one scan window. */
        uint32_t waited = 0;
        while (!self->host_synced && waited < 5000) {
            vTaskDelay(pdMS_TO_TICKS(50));
            waited += 50;
        }
        if (!self->host_synced) {
            return MAGPIE_SOURCE_ERR_LINK;
        }
    }
    if (self->connected && self->discovered) {
        return MAGPIE_SOURCE_OK;
    }

    self->connected = false;
    self->discovered = false;
    self->index_handle = 0;
    self->sub_handle = 0;
    scan_start(self);

    const uint32_t scan_ms = self->config.scan_ms ? self->config.scan_ms : 4000;
    const uint32_t deadline_ms = scan_ms + CONNECT_TIMEOUT_MS + 1000;
    const uint32_t step_ms = 50;
    for (uint32_t waited = 0;
         waited < deadline_ms && !self->connected && !self->scan_done;
         waited += step_ms) {
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }
    if (!self->connected) {
        ESP_LOGW(TAG, "no sender found within %ums", (unsigned)deadline_ms);
        if (ble_gap_disc_active()) {
            (void)ble_gap_disc_cancel();
        }
        return MAGPIE_SOURCE_ERR_LINK;
    }

    /* Discover the display service and then its two characteristics. */
    att_result_t svc = {.buf = NULL, .cap = 0, .status = 0, .done = false};
    const int svc_rc = ble_gattc_disc_svc_by_uuid(self->conn_handle,
                                                  &kServiceUuid.u, on_svc,
                                                  &svc);
    if (svc_rc != 0) {
        ESP_LOGW(TAG, "service discovery failed (%d)", svc_rc);
        return MAGPIE_SOURCE_ERR_LINK;
    }
    const int svc_status = att_wait(&svc, 4000);
    if (svc_status != 0 || svc.svc_start == 0) {
        ESP_LOGW(TAG, "display service missing (%d)", svc_status);
        (void)ble_gap_terminate(self->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return MAGPIE_SOURCE_ERR_LINK;
    }
    const int chr_rc = ble_gattc_disc_all_chrs(self->conn_handle, svc.svc_start,
                                               svc.svc_end, on_chr, self);
    if (chr_rc != 0) {
        ESP_LOGW(TAG, "characteristic discovery failed (%d)", chr_rc);
        return MAGPIE_SOURCE_ERR_LINK;
    }
    /* on_chr marks `discovered` at BLE_HS_EDONE. */
    for (uint32_t waited = 0; waited < 4000; waited += ATT_POLL_STEP_MS) {
        if (self->discovered) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(ATT_POLL_STEP_MS));
    }
    if (!self->discovered) {
        ESP_LOGW(TAG, "display characteristics missing");
        (void)ble_gap_terminate(self->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return MAGPIE_SOURCE_ERR_LINK;
    }

    /* Subscribe to INDEX so the store can refresh without polling. */
    const int sub_rc = ble_gattc_write_flat(
        self->conn_handle, (uint16_t)(self->index_handle + 1),
        (const uint8_t[]){0x01, 0x00}, 2, NULL, NULL);
    if (sub_rc != 0) {
        ESP_LOGW(TAG, "INDEX subscribe failed (%d); polling instead", sub_rc);
    }
    return MAGPIE_SOURCE_OK;
}

static void ble_close(magpie_source_t *source)
{
    /* Keep the connection: the next refresh reuses it. Backoff is driven by
     * the arbiter, which calls this only when the link is unusable. */
    (void)source;
}

static int ble_read_index(magpie_source_t *source, char *buf, size_t cap)
{
    magpie_ble_source_t *self = source->impl;
    if (!self || !self->connected || !self->index_handle) {
        return MAGPIE_SOURCE_ERR_LINK;
    }
    if (self->notif_pending) {
        self->notif_pending = false;
        const size_t len = self->notif_len < cap - 1 ? self->notif_len
                                                     : cap - 1;
        memcpy(buf, self->notif, len);
        buf[len] = '\0';
        return (int)len;
    }
    att_result_t result = {.buf = buf, .cap = cap, .status = 0, .done = false};
    const int rc = ble_gattc_read(self->conn_handle, self->index_handle, on_read,
                                  &result);
    if (rc != 0) {
        return MAGPIE_SOURCE_ERR_LINK;
    }
    const int status = att_wait(&result, 4000);
    if (status != 0) {
        return status == BLE_HS_ETIMEOUT ? MAGPIE_SOURCE_ERR_TIMEOUT
                                         : MAGPIE_SOURCE_ERR_LINK;
    }
    return (int)result.len;
}

static int ble_read_sub(magpie_source_t *source, uint8_t slot, char *buf,
                        size_t cap)
{
    magpie_ble_source_t *self = source->impl;
    if (!self || !self->connected || !self->sub_handle) {
        return MAGPIE_SOURCE_ERR_LINK;
    }
    /* Select the slot, then read it: the characteristic holds one slot. */
    const uint8_t selection = slot;
    att_result_t write = {.status = 0, .done = false};
    int rc = ble_gattc_write_flat(self->conn_handle, self->sub_handle,
                                  &selection, 1, on_write, &write);
    if (rc != 0) {
        return MAGPIE_SOURCE_ERR_LINK;
    }
    const int write_status = att_wait(&write, 4000);
    if (write_status != 0) {
        return write_status == BLE_HS_ETIMEOUT ? MAGPIE_SOURCE_ERR_TIMEOUT
                                               : MAGPIE_SOURCE_ERR_LINK;
    }

    att_result_t read = {.buf = buf, .cap = cap, .status = 0, .done = false};
    rc = ble_gattc_read(self->conn_handle, self->sub_handle, on_read, &read);
    if (rc != 0) {
        return MAGPIE_SOURCE_ERR_LINK;
    }
    const int status = att_wait(&read, 4000);
    if (status != 0) {
        return status == BLE_HS_ETIMEOUT ? MAGPIE_SOURCE_ERR_TIMEOUT
                                         : MAGPIE_SOURCE_ERR_LINK;
    }
    return (int)read.len;
}

static bool ble_wait_changed(magpie_source_t *source, uint32_t timeout_ms)
{
    magpie_ble_source_t *self = source->impl;
    if (!self) {
        return false;
    }
    const uint32_t step = 20;
    for (uint32_t waited = 0; waited < timeout_ms; waited += step) {
        if (self->notif_pending) {
            return true;
        }
        if (!self->connected) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(step));
    }
    return self->notif_pending;
}

bool magpie_ble_source_connected(void)
{
    return s_ble.connected && s_ble.discovered;
}

int magpie_ble_source_take_notification(char *buf, size_t cap)
{
    magpie_ble_source_t *self = &s_ble;
    if (!self->notif_pending || !buf || cap == 0) {
        return 0;
    }
    self->notif_pending = false;
    const size_t len = self->notif_len < cap - 1 ? self->notif_len : cap - 1;
    memcpy(buf, self->notif, len);
    buf[len] = '\0';
    return (int)len;
}

esp_err_t magpie_ble_source_init(const magpie_ble_config_t *config,
                                 magpie_source_t *out_source)
{
    if (!config || !out_source) {
        return ESP_ERR_INVALID_ARG;
    }
    magpie_ble_source_t *self = &s_ble;
    memset(self, 0, sizeof(*self));
    self->config = *config;
    self->config.name_prefix[sizeof(self->config.name_prefix) - 1] = '\0';
    if (self->config.scan_ms == 0) {
        self->config.scan_ms = 4000;
    }
    if (self->config.mtu < 23) {
        self->config.mtu = 247;
    }
    s_active = self;

    if (!s_host_started) {
        esp_err_t err = nimble_port_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
            s_active = NULL;
            return err;
        }
        ble_hs_cfg.sync_cb = on_sync;
        ble_hs_cfg.reset_cb = on_reset;
        /* Central only: no GATT server, no security manager pairing store. */
        nimble_port_freertos_init(host_task);
        s_host_started = true;
    }

    memset(out_source, 0, sizeof(*out_source));
    out_source->name = "ble";
    out_source->open = ble_open;
    out_source->close = ble_close;
    out_source->read_index = ble_read_index;
    out_source->read_sub = ble_read_sub;
    out_source->wait_changed = ble_wait_changed;
    out_source->impl = self;
    return ESP_OK;
}

void magpie_ble_source_deinit(magpie_source_t *source)
{
    magpie_ble_source_t *self = &s_ble;
    if (self->connected) {
        (void)ble_gap_terminate(self->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        self->connected = false;
    }
    if (source) {
        source->impl = NULL;
        source->open = NULL;
        source->close = NULL;
        source->read_index = NULL;
        source->read_sub = NULL;
        source->wait_changed = NULL;
    }
    if (s_active == self) {
        s_active = NULL;
    }
}
