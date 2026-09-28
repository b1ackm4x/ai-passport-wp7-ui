// main/usage_link.c —— 常驻 BLE GATT server，接收 Mac 推来的看板数据。
//
// 设计要点：
//  - 本模块是 NimBLE 的唯一 owner。仓库中不得存在第二处 nimble_port_init()，
//    ESP32-C3 controller 只有 IDLE 态可以 init，二次调用必然失败。
//  - GATT 写回调运行在 NimBLE host 任务，**绝不触碰 LVGL**。数据经
//    mutex + generation 一次性发布，UI 侧轮询取副本。
//  - 快照有 USAGE_WIRE_SIZE 字节，volatile 无法保证原子发布，必须走锁。
#include "usage_link.h"

#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "usage_link";
static const char *DEVICE_NAME = "FoloPassport";

// 128-bit 自定义 UUID。主广播里放 service UUID 便于 Mac 精确过滤，
// 完整设备名放 scan response —— legacy adv 只有 31 字节，两者塞不下。
//
// Service: 6b1d0001-5f9a-4c33-9a1e-2f8b7c4d5e60
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0x60, 0x5e, 0x4d, 0x7c, 0x8b, 0x2f, 0x1e, 0x9a,
    0x33, 0x4c, 0x9a, 0x5f, 0x01, 0x00, 0x1d, 0x6b);

// Characteristic: 6b1d0002-5f9a-4c33-9a1e-2f8b7c4d5e60
static const ble_uuid128_t CHR_UUID = BLE_UUID128_INIT(
    0x60, 0x5e, 0x4d, 0x7c, 0x8b, 0x2f, 0x1e, 0x9a,
    0x33, 0x4c, 0x9a, 0x5f, 0x02, 0x00, 0x1d, 0x6b);

static SemaphoreHandle_t s_mutex;
static usage_snapshot_t  s_snapshot;
static uint32_t          s_generation;      // 每次成功提交自增
static bool              s_have_snapshot;
// Last accepted packet, read and written only on the NimBLE host task.
static usage_snapshot_t  s_trusted;
static bool              s_have_trusted;

static usage_link_state_t s_state = USAGE_LINK_IDLE;
static atomic_uint_fast16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static atomic_uint_fast16_t s_mtu;
static uint8_t  s_addr_type;
static bool     s_started;

static int gap_event(struct ble_gap_event *event, void *arg);

// 广播恢复重试。advertise() 失败时不能只打日志：GAP 事件是恢复的唯一入口，
// 而没有开始广播就永远不会再有 ADV_COMPLETE/CONNECT，设备会静默地永久不可
// 发现，只能靠重启救回来。
//
// 重试用 NimBLE 自己的 callout，而不是 esp_timer：callout 到期后把事件投进
// host 默认事件队列，回调在 NimBLE host task 上执行——与 on_sync / gap_event
// 同一上下文。这样 (1) 设地址、设广播数据、起广播这些要等 HCI ACK 的调用不会
// 卡住 esp_timer task；(2) s_state / s_identity_ready 只在 host task 上读写，
// 不需要跨任务同步。
#define ADV_RETRY_DELAY_MS 2000

static struct ble_npl_callout s_adv_retry;
// ble_npl_callout_stop() 只停 timer；一个已经到期、已经排进事件队列的重试事件
// 仍会执行。on_reset 递增 epoch，排队时记下当时的 epoch，回调发现不一致就作废，
// 这样 reset 之前排队的重试不会在新一轮 sync 之前抢跑。若 reset 之后 on_sync
// 又排了一次重试，两个 epoch 会相同——那种情况靠 timer 是否仍在跑来区分，
// 见 adv_retry_cb。
static uint32_t s_reset_epoch;
static uint32_t s_retry_epoch;
// on_sync 里的地址准备（ensure_addr / infer_auto）是起广播的前置条件，而它失败
// 同样不会有任何后续 GAP 事件再来触发。所以它和广播失败共用这条重试通道：
// 未就绪时先补地址，补上后再经状态机起广播。
static bool s_identity_ready;
static bool advertise_now(void);
static bool prepare_identity(void);
static void link_event(usage_link_event_t event);

static void schedule_adv_retry(void)
{
    if (s_state == USAGE_LINK_STOPPING) return;
    s_retry_epoch = s_reset_epoch;
    // reset 会先停掉已排队的那次，所以重复调用是幂等的。
    if (ble_npl_callout_reset(&s_adv_retry,
                              ble_npl_time_ms_to_ticks32(ADV_RETRY_DELAY_MS))
        != BLE_NPL_OK) {
        ESP_LOGE(TAG, "无法排队广播重试；设备可能保持不可发现");
    }
}

// 在 NimBLE host task 上执行（见 s_adv_retry 的说明）。
static void adv_retry_cb(struct ble_npl_event *ev)
{
    (void)ev;
    if (s_state == USAGE_LINK_STOPPING) return;
    if (s_retry_epoch != s_reset_epoch) return;   // reset 之前排队的：作废
    // reset 之后若已重新排队，s_retry_epoch 被刷成新 epoch，上面拦不住 reset
    // 前入队的旧事件。但那时新 timer 一定还在跑（它到期后才会再投事件），而
    // 属于"自己这次到期"的事件运行时 timer 已经停了：timer 仍活跃 == 旧事件。
    if (ble_npl_callout_is_active(&s_adv_retry)) return;
    if (!s_identity_ready) {
        if (!prepare_identity()) {
            ESP_LOGW(TAG, "地址准备重试仍失败，%d 秒后再试",
                     ADV_RETRY_DELAY_MS / 1000);
            schedule_adv_retry();
            return;
        }
        link_event(USAGE_LINK_EV_SYNC);   // 经状态机起广播；再失败会重新排队
        return;
    }
    if (!advertise_now()) {
        ESP_LOGW(TAG, "广播重试仍失败，%d 秒后再试", ADV_RETRY_DELAY_MS / 1000);
        schedule_adv_retry();
    }
}

// 返回是否真的开始广播了。调用方必须处理 false。
static bool advertise_now(void)
{
    // 主广播：flags + 128-bit service UUID。名字放不下，见下面的 scan response。
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&SVC_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields 失败: %d", rc);
        return false;
    }

    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (const uint8_t *)DEVICE_NAME;
    rsp.name_len = strlen(DEVICE_NAME);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields 失败: %d", rc);
        return false;
    }

    // 可连接、一般可发现。这是与旧 demo_ble.c 的关键差别（那里是 CONN_MODE_NON）。
    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params,
                           gap_event, NULL);
    if (rc != 0) {
        // BLE_HS_EALREADY 说明已经在广播，不是故障。
        if (rc == BLE_HS_EALREADY) return true;
        ESP_LOGE(TAG, "adv_start 失败: %d", rc);
        return false;
    }
    return true;
}

static void advertise(void)
{
    if (!advertise_now()) schedule_adv_retry();
}

// 状态推进集中在这里：转移表是 usage_model 里的纯函数，已被 host test 覆盖。
static void link_event(usage_link_event_t event)
{
    usage_link_action_t action = USAGE_LINK_ACTION_NONE;
    s_state = usage_model_link_step(s_state, event, &action);
    if (action == USAGE_LINK_ACTION_ADVERTISE) advertise();
}

static int on_chr_write(struct ble_gatt_access_ctxt *ctxt)
{
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != USAGE_WIRE_SIZE) {
        ESP_LOGW(TAG, "payload 长度 %u，期望 %d", len, USAGE_WIRE_SIZE);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t wire[USAGE_WIRE_SIZE];
    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, wire, sizeof wire, &copied);
    if (rc != 0 || copied != USAGE_WIRE_SIZE) return BLE_ATT_ERR_UNLIKELY;

    // 解码与校验在锁外的局部变量上完成，临界区只做整体赋值。
    usage_snapshot_t decoded;
    // Only a packet accepted during this boot is a trusted time base. Before
    // that, pass 0 so the first packet is accepted after the minimal epoch
    // check; afterwards future-skew protection applies as before.
    uint32_t reference_unix = 0;
    if (s_have_trusted) {
        usage_model_now_unix(&s_trusted, true, esp_timer_get_time(),
                             &reference_unix);
    }
    usage_decode_result_t result =
        usage_model_decode(wire, sizeof wire, esp_timer_get_time(),
                           reference_unix, &decoded);
    if (result != USAGE_DECODE_OK) {
        ESP_LOGW(TAG, "payload 校验失败: %d（保留上一份快照）", (int)result);
        return BLE_ATT_ERR_UNLIKELY;
    }

    // Host-task-only copy used as the next packet's time base.
    s_trusted = decoded;
    s_have_trusted = true;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    s_snapshot = decoded;          // 数据与接收时刻属于同一次提交
    s_have_snapshot = true;
    s_generation++;
    xSemaphoreGive(s_mutex);

    return 0;
}

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) return on_chr_write(ctxt);
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def GATT_SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // 只开 WRITE（write-with-response）。低 MTU 时由 NimBLE 的
                // prepare/execute long write 汇聚成完整 USAGE_WIRE_SIZE 字节后再回调。
                .uuid = &CHR_UUID.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            atomic_store(&s_conn_handle, event->connect.conn_handle);
            uint16_t mtu = ble_att_mtu(event->connect.conn_handle);
            atomic_store(&s_mtu, mtu);
            ESP_LOGI(TAG, "已连接 handle=%u mtu=%u",
                     event->connect.conn_handle, mtu);
            link_event(USAGE_LINK_EV_CONNECT_OK);
        } else {
            // 连接失败后 controller 已停止广播，必须显式恢复。
            ESP_LOGW(TAG, "连接失败: %d", event->connect.status);
            link_event(USAGE_LINK_EV_CONNECT_FAIL);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "断开: %d", event->disconnect.reason);
        atomic_store(&s_conn_handle, BLE_HS_CONN_HANDLE_NONE);
        atomic_store(&s_mtu, 0);
        // 漏掉这条会导致断连后设备永久不可再被发现。
        link_event(USAGE_LINK_EV_DISCONNECT);
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        link_event(USAGE_LINK_EV_ADV_COMPLETE);
        break;

    case BLE_GAP_EVENT_MTU:
        atomic_store(&s_mtu, event->mtu.value);
        ESP_LOGI(TAG, "MTU 协商为 %u", event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

static bool prepare_identity(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr 失败: %d", rc);
        return false;
    }
    rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto 失败: %d", rc);
        return false;
    }
    s_identity_ready = true;
    return true;
}

static void on_sync(void)
{
    s_identity_ready = false;
    if (!prepare_identity()) {
        // 不能只 return：这是 host ready 后唯一的入口，没有别的事件会再来。
        // 失败留给重试 timer，它会先补地址再经状态机起广播。
        schedule_adv_retry();
        return;
    }
    link_event(USAGE_LINK_EV_SYNC);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset: %d", reason);
    atomic_store(&s_conn_handle, BLE_HS_CONN_HANDLE_NONE);
    atomic_store(&s_mtu, 0);
    ble_npl_callout_stop(&s_adv_retry);   // 排队中的重试对新一轮 sync 没意义
    s_reset_epoch++;                      // 已经出队在路上的那次也作废
    s_identity_ready = false;    // 地址要在下一次 on_sync 重新推导
    s_state = USAGE_LINK_IDLE;   // 后续 on_sync 会重新起广播
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t usage_link_start(void)
{
    if (s_started) return ESP_ERR_INVALID_STATE;

    // NimBLE 需要 NVS。失败时不擦分区 —— cardid 等出厂数据不能被本模块碰。
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES &&
        err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "nvs_flash_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    // 从这里起任何失败都要把 host 拆回去：留下半初始化的 NimBLE 会让下一次
    // usage_link_start() 二次 nimble_port_init()。
    bool callout_ready = false;
    int rc;

    // 重试 callout 挂在 host 默认事件队列上，回调因此在 host task 上跑。
    rc = ble_npl_callout_init(&s_adv_retry, nimble_port_get_dflt_eventq(),
                              adv_retry_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "无法创建广播重试 callout: %d", rc);
        goto fail;
    }
    callout_ready = true;

    rc = ble_gatts_count_cfg(GATT_SVCS);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count_cfg 失败: %d", rc);
        goto fail;
    }
    rc = ble_gatts_add_svcs(GATT_SVCS);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add_svcs 失败: %d", rc);
        goto fail;
    }

    rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) ESP_LOGW(TAG, "device_name_set 失败: %d", rc);

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    s_started = true;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE 链路已启动，等待 Mac 推送");
    return ESP_OK;

fail:
    if (callout_ready) ble_npl_callout_deinit(&s_adv_retry);
    nimble_port_deinit();
    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;
    return ESP_FAIL;
}

bool usage_link_get(usage_snapshot_t *out, uint32_t *out_generation)
{
    if (!out || !s_mutex) return false;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    bool have = s_have_snapshot;
    if (have) {
        *out = s_snapshot;
        if (out_generation) *out_generation = s_generation;
    }
    xSemaphoreGive(s_mutex);
    return have;
}

bool usage_link_connected(void)
{
    return atomic_load(&s_conn_handle) != BLE_HS_CONN_HANDLE_NONE;
}

uint16_t usage_link_mtu(void)
{
    return (uint16_t)atomic_load(&s_mtu);
}
