// main/usage_model.h —— 看板数据的纯逻辑层。
//
// 本文件与 .c 刻意不依赖 ESP-IDF / LVGL / FreeRTOS，以便 host test 直接编译。
// wire 解码、字段校验、时钟外推和新鲜度判定都在这里，设备侧只做渲染。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Mac 侧一次 write 的字节数。协议固定长度，长度不符即整包拒绝。
// v2：在 all_time 之后插入 30 天窗口（tokens + cents），供 Kaboo 页轮换卡片。
#define USAGE_WIRE_SIZE      94
#define USAGE_WIRE_VERSION   2
#define USAGE_TOP_MODEL_CAP  24   // 含结尾 NUL

// flags 位定义。未列出的位保留，必须为 0。
//
// Claude 的两个配额窗口各有独立 valid 位：Claude Code 只在窗口处于活跃状态时
// 才发出它，实测出现过只有 seven_day 的情况。用一个位覆盖两个窗口时，缺失窗口
// 的 resets=0 会让整包 epoch 校验失败，连同一包里有效的 Kaboo 数据一起被拒。
#define USAGE_FLAG_KABOO_VALID   (1u << 0)
#define USAGE_FLAG_FIVE_HOUR     (1u << 1)
#define USAGE_FLAG_SEVEN_DAY     (1u << 2)
// 任一 Claude 窗口有效即视为该源可用（UI 用它决定显示数据还是"等待"态）。
#define USAGE_FLAG_CLAUDE_VALID  (USAGE_FLAG_FIVE_HOUR | USAGE_FLAG_SEVEN_DAY)
#define USAGE_FLAG_RESERVED_MASK (~(uint8_t)0x07)

// 解码后的快照。字段自然对齐，不是 wire 布局的镜像 —— 解码时逐字段读，
// 不允许把 wire buffer 直接 cast 成本结构体。
typedef struct {
    uint8_t  version;
    uint8_t  flags;
    uint32_t generated_unix;        // Mac 在 write 前一刻取的时刻，也是授时基准
    int16_t  tz_offset_minutes;
    uint32_t kaboo_sampled_unix;    // kaboo 数据的抓取时刻
    uint32_t claude_sampled_unix;   // claude 数据的抓取时刻
    uint32_t today_tokens;
    uint64_t week_tokens;
    uint64_t month_tokens;          // 30 天窗口
    uint64_t all_tokens;
    uint32_t today_cost_cents;
    uint32_t week_cost_cents;
    uint32_t month_cost_cents;
    uint32_t all_cost_cents;
    char     top_model[USAGE_TOP_MODEL_CAP];
    uint8_t  five_hour_pct;
    uint8_t  seven_day_pct;
    uint32_t five_hour_resets_unix;
    uint32_t seven_day_resets_unix;

    // 接收侧补充，不在 wire 上。
    int64_t  received_monotonic_us;  // 收到该包时的单调时钟
} usage_snapshot_t;

// 解码失败的原因。UI 需要区分"没收到过"和"收到但不合法"。
typedef enum {
    USAGE_DECODE_OK = 0,
    USAGE_DECODE_BAD_LENGTH,
    USAGE_DECODE_BAD_VERSION,
    USAGE_DECODE_RESERVED_FLAG,
    USAGE_DECODE_BAD_PERCENT,
    USAGE_DECODE_BAD_TIMEZONE,
    USAGE_DECODE_BAD_EPOCH,
} usage_decode_result_t;

// epoch 合理区间：早于 2020-01-01 或远晚于当前授时都视为损坏。
#define USAGE_EPOCH_MIN        1577836800u   // 2020-01-01T00:00:00Z
#define USAGE_EPOCH_SKEW_MAX   86400u        // 允许比 generated_unix 晚一天
#define USAGE_RESET_SKEW_MAX   (7u * 86400u) // 配额重置最多提前七天

// 解码并校验。任一字段不合法即整体拒绝，调用方应保留上一份有效快照。
// received_monotonic_us 由调用方传入（设备上是 esp_timer_get_time()）。
// reference_unix 为接收端最近可信的 UTC 秒；传 0 表示当前尚无可信基准，
// 此时仍执行最小 epoch 校验，但不执行未来偏移校验。
usage_decode_result_t usage_model_decode(const uint8_t *wire, size_t len,
                                         int64_t received_monotonic_us,
                                         uint32_t reference_unix,
                                         usage_snapshot_t *out);

// ---- 时钟 ----

// 由授时基准 + 单调时钟外推当前 UTC 秒。未同步时返回 false。
bool usage_model_now_unix(const usage_snapshot_t *snapshot, bool synced,
                          int64_t now_monotonic_us, uint32_t *out_unix);

// 拆成本地时间的时分秒。tz_offset_minutes 为同步时的固定偏移，不含 DST 规则。
typedef struct {
    int hour;      // 0-23
    int minute;    // 0-59
    int second;    // 0-59
    int year;      // 例如 2026
    int month;     // 1-12
    int day;       // 1-31
    int weekday;   // 0=周日 … 6=周六
} usage_wall_clock_t;

bool usage_model_wall_clock(const usage_snapshot_t *snapshot, bool synced,
                            int64_t now_monotonic_us, usage_wall_clock_t *out);

// ---- 新鲜度 ----

// 距上次收到 write 的秒数。未同步时返回 -1。
int64_t usage_model_sync_age_seconds(const usage_snapshot_t *snapshot,
                                     bool synced, int64_t now_monotonic_us);

// 某个数据源是否仍可信：valid 位已置、采样时刻不在未来、且未超过 ttl。
bool usage_model_source_fresh(uint32_t sampled_unix, uint32_t now_unix,
                              uint32_t ttl_seconds);

// 配额窗口是否已翻篇（resets_at 已过去 → 百分比是过期读数）。
bool usage_model_quota_expired(uint32_t resets_unix, uint32_t now_unix);

// 距重置还有多少秒；已过期返回 0。
uint32_t usage_model_seconds_until(uint32_t resets_unix, uint32_t now_unix);

// ---- BLE 链路状态机 ----
//
// 放在纯逻辑层是为了让 GAP 状态转移可被 host test 覆盖 —— 漏掉
// DISCONNECT→advertise 会导致断连后永久不可发现。

typedef enum {
    USAGE_LINK_IDLE = 0,
    USAGE_LINK_ADVERTISING,
    USAGE_LINK_CONNECTED,
    USAGE_LINK_STOPPING,
} usage_link_state_t;

typedef enum {
    USAGE_LINK_EV_SYNC = 0,        // host 就绪
    USAGE_LINK_EV_CONNECT_OK,
    USAGE_LINK_EV_CONNECT_FAIL,
    USAGE_LINK_EV_DISCONNECT,
    USAGE_LINK_EV_ADV_COMPLETE,
    USAGE_LINK_EV_SHUTDOWN,
} usage_link_event_t;

typedef enum {
    USAGE_LINK_ACTION_NONE = 0,
    USAGE_LINK_ACTION_ADVERTISE,   // 调用方需要（重新）启动广播
} usage_link_action_t;

// 纯状态转移：返回新状态，并通过 out_action 告知是否需要重启广播。
usage_link_state_t usage_model_link_step(usage_link_state_t state,
                                         usage_link_event_t event,
                                         usage_link_action_t *out_action);
