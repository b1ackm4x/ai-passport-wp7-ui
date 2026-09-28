// main/usage_model.c —— 看板数据的纯逻辑层实现。
//
// 约束：本文件不得 include 任何 ESP-IDF / LVGL / FreeRTOS 头文件。
// tools/validate.sh 会用宿主编译器直接编译它和对应 test。
#include "usage_model.h"

#include <string.h>

// wire 字段偏移。与 tools/usage_bridge.py 的 struct 格式一一对应，
// 任一侧改动都必须同步改另一侧并更新 golden vector 测试。
#define OFF_VERSION            0
#define OFF_FLAGS              1
#define OFF_GENERATED          2
#define OFF_TZ_OFFSET          6
#define OFF_KABOO_SAMPLED      8
#define OFF_CLAUDE_SAMPLED    12
#define OFF_TODAY_TOKENS      16
#define OFF_WEEK_TOKENS       20
#define OFF_MONTH_TOKENS      28
#define OFF_ALL_TOKENS        36
#define OFF_TODAY_CENTS       44
#define OFF_WEEK_CENTS        48
#define OFF_MONTH_CENTS       52
#define OFF_ALL_CENTS         56
#define OFF_TOP_MODEL         60
#define OFF_FIVE_HOUR_PCT     84
#define OFF_SEVEN_DAY_PCT     85
#define OFF_FIVE_HOUR_RESETS  86
#define OFF_SEVEN_DAY_RESETS  90

// 显式小端解码。刻意不做 cast —— 把 wire buffer 强转成 uint64_t* 会在
// RISC-V 上生成未对齐 word load。
static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p)
{
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

static bool epoch_plausible(uint32_t value, uint32_t reference,
                            uint32_t max_future_skew)
{
    if (value < USAGE_EPOCH_MIN) return false;
    // 允许略晚于授时基准（重置时刻本来就在未来），但不接受离谱的未来值。
    if (reference >= USAGE_EPOCH_MIN && value > reference &&
        (value - reference) > max_future_skew) {
        return false;
    }
    return true;
}

usage_decode_result_t usage_model_decode(const uint8_t *wire, size_t len,
                                         int64_t received_monotonic_us,
                                         uint32_t reference_unix,
                                         usage_snapshot_t *out)
{
    if (!wire || !out) return USAGE_DECODE_BAD_LENGTH;
    if (len != USAGE_WIRE_SIZE) return USAGE_DECODE_BAD_LENGTH;

    uint8_t version = wire[OFF_VERSION];
    if (version != USAGE_WIRE_VERSION) return USAGE_DECODE_BAD_VERSION;

    uint8_t flags = wire[OFF_FLAGS];
    if (flags & USAGE_FLAG_RESERVED_MASK) return USAGE_DECODE_RESERVED_FLAG;

    uint8_t five = wire[OFF_FIVE_HOUR_PCT];
    uint8_t seven = wire[OFF_SEVEN_DAY_PCT];
    if (five > 100 || seven > 100) return USAGE_DECODE_BAD_PERCENT;

    int16_t tz = (int16_t)get_le16(wire + OFF_TZ_OFFSET);
    if (tz < -720 || tz > 840) return USAGE_DECODE_BAD_TIMEZONE;

    uint32_t generated = get_le32(wire + OFF_GENERATED);
    if (!epoch_plausible(generated, reference_unix,
                         USAGE_EPOCH_SKEW_MAX)) return USAGE_DECODE_BAD_EPOCH;

    uint32_t kaboo_sampled = get_le32(wire + OFF_KABOO_SAMPLED);
    uint32_t claude_sampled = get_le32(wire + OFF_CLAUDE_SAMPLED);
    uint32_t five_resets = get_le32(wire + OFF_FIVE_HOUR_RESETS);
    uint32_t seven_resets = get_le32(wire + OFF_SEVEN_DAY_RESETS);

    // 只校验被 valid 位声明为有效的那部分时间戳；未使用的源允许为 0。
    if (flags & USAGE_FLAG_KABOO_VALID) {
        if (!epoch_plausible(kaboo_sampled, generated,
                             USAGE_EPOCH_SKEW_MAX)) return USAGE_DECODE_BAD_EPOCH;
    }
    // 每个 Claude 窗口独立校验：只有被声明有效的那个窗口的 reset 时刻需要
    // 合理。这样"只有 seven_day"的真实情况不会连带否掉整包。
    if (flags & USAGE_FLAG_CLAUDE_VALID) {
        if (!epoch_plausible(claude_sampled, generated,
                             USAGE_EPOCH_SKEW_MAX)) return USAGE_DECODE_BAD_EPOCH;
    }
    if (flags & USAGE_FLAG_FIVE_HOUR) {
        if (!epoch_plausible(five_resets, generated,
                             USAGE_RESET_SKEW_MAX)) return USAGE_DECODE_BAD_EPOCH;
    }
    if (flags & USAGE_FLAG_SEVEN_DAY) {
        if (!epoch_plausible(seven_resets, generated,
                             USAGE_RESET_SKEW_MAX)) return USAGE_DECODE_BAD_EPOCH;
    }

    memset(out, 0, sizeof(*out));
    out->version = version;
    out->flags = flags;
    out->generated_unix = generated;
    out->tz_offset_minutes = tz;
    out->kaboo_sampled_unix = kaboo_sampled;
    out->claude_sampled_unix = claude_sampled;
    out->today_tokens = get_le32(wire + OFF_TODAY_TOKENS);
    out->week_tokens = get_le64(wire + OFF_WEEK_TOKENS);
    out->month_tokens = get_le64(wire + OFF_MONTH_TOKENS);
    out->all_tokens = get_le64(wire + OFF_ALL_TOKENS);
    out->today_cost_cents = get_le32(wire + OFF_TODAY_CENTS);
    out->week_cost_cents = get_le32(wire + OFF_WEEK_CENTS);
    out->month_cost_cents = get_le32(wire + OFF_MONTH_CENTS);
    out->all_cost_cents = get_le32(wire + OFF_ALL_CENTS);
    out->five_hour_pct = five;
    out->seven_day_pct = seven;
    out->five_hour_resets_unix = five_resets;
    out->seven_day_resets_unix = seven_resets;
    out->received_monotonic_us = received_monotonic_us;

    // 发送方可能填满 24 字节而不留 NUL；无条件截断，避免后续按 C 字符串读越界。
    memcpy(out->top_model, wire + OFF_TOP_MODEL, USAGE_TOP_MODEL_CAP);
    out->top_model[USAGE_TOP_MODEL_CAP - 1] = '\0';

    return USAGE_DECODE_OK;
}

bool usage_model_now_unix(const usage_snapshot_t *snapshot, bool synced,
                          int64_t now_monotonic_us, uint32_t *out_unix)
{
    if (!snapshot || !synced || !out_unix) return false;

    int64_t elapsed_us = now_monotonic_us - snapshot->received_monotonic_us;
    // 单调时钟倒退说明发生了 deep sleep 重启或计数器复位，此时不可信。
    if (elapsed_us < 0) return false;

    *out_unix = snapshot->generated_unix + (uint32_t)(elapsed_us / 1000000);
    return true;
}

// 由天数序号还原公历年月日（Howard Hinnant 的 civil_from_days 算法）。
// 自己实现而不用 gmtime()，是为了让本文件保持零依赖、可宿主测试。
static void civil_from_days(int64_t days, int *year, int *month, int *day)
{
    // 把纪元移到 0000-03-01，使闰日落在周期末尾，省掉月份特判。
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;                          // [0, 146096]
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);       // [0, 365]
    int64_t mp = (5 * doy + 2) / 153;                            // [0, 11]，3 月为 0
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);                         // 还原为 1-12

    *year = (int)(y + (m <= 2 ? 1 : 0));
    *month = (int)m;
    *day = (int)d;
}

bool usage_model_wall_clock(const usage_snapshot_t *snapshot, bool synced,
                            int64_t now_monotonic_us, usage_wall_clock_t *out)
{
    if (!out) return false;

    uint32_t utc = 0;
    if (!usage_model_now_unix(snapshot, synced, now_monotonic_us, &utc)) return false;

    // 固定偏移，不含 DST 规则；断线跨夏令时切换会偏一小时（已知限制）。
    int64_t local = (int64_t)utc + (int64_t)snapshot->tz_offset_minutes * 60;
    if (local < 0) return false;

    int64_t days = local / 86400;
    int64_t day_seconds = local % 86400;

    out->hour = (int)(day_seconds / 3600);
    out->minute = (int)((day_seconds % 3600) / 60);
    out->second = (int)(day_seconds % 60);
    // 1970-01-01 是星期四，故偏移 4。
    out->weekday = (int)((days + 4) % 7);
    civil_from_days(days, &out->year, &out->month, &out->day);
    return true;
}

int64_t usage_model_sync_age_seconds(const usage_snapshot_t *snapshot,
                                     bool synced, int64_t now_monotonic_us)
{
    if (!snapshot || !synced) return -1;
    int64_t elapsed_us = now_monotonic_us - snapshot->received_monotonic_us;
    if (elapsed_us < 0) return -1;
    return elapsed_us / 1000000;
}

bool usage_model_source_fresh(uint32_t sampled_unix, uint32_t now_unix,
                              uint32_t ttl_seconds)
{
    if (sampled_unix == 0) return false;
    // 采样时刻在未来说明两端时钟不一致，不能当作新鲜。
    if (sampled_unix > now_unix) return false;
    return (now_unix - sampled_unix) <= ttl_seconds;
}

bool usage_model_quota_expired(uint32_t resets_unix, uint32_t now_unix)
{
    if (resets_unix == 0) return true;
    return resets_unix <= now_unix;
}

uint32_t usage_model_seconds_until(uint32_t resets_unix, uint32_t now_unix)
{
    if (resets_unix <= now_unix) return 0;
    return resets_unix - now_unix;
}

usage_link_state_t usage_model_link_step(usage_link_state_t state,
                                         usage_link_event_t event,
                                         usage_link_action_t *out_action)
{
    usage_link_action_t action = USAGE_LINK_ACTION_NONE;
    usage_link_state_t next = state;

    // 关停中吸收一切事件，绝不重启广播，否则 teardown 永远等不到静默。
    if (state == USAGE_LINK_STOPPING) {
        if (out_action) *out_action = USAGE_LINK_ACTION_NONE;
        return USAGE_LINK_STOPPING;
    }

    switch (event) {
    case USAGE_LINK_EV_SHUTDOWN:
        next = USAGE_LINK_STOPPING;
        break;

    case USAGE_LINK_EV_SYNC:
        next = USAGE_LINK_ADVERTISING;
        action = USAGE_LINK_ACTION_ADVERTISE;
        break;

    case USAGE_LINK_EV_CONNECT_OK:
        next = USAGE_LINK_CONNECTED;
        break;

    // 连接失败后 controller 已停止广播，必须显式重启，否则再也连不上。
    case USAGE_LINK_EV_CONNECT_FAIL:
    case USAGE_LINK_EV_DISCONNECT:
        next = USAGE_LINK_ADVERTISING;
        action = USAGE_LINK_ACTION_ADVERTISE;
        break;

    case USAGE_LINK_EV_ADV_COMPLETE:
        // 已连接时的 ADV_COMPLETE 是正常的（连接会终止广播），不要重启。
        if (state == USAGE_LINK_CONNECTED) {
            next = USAGE_LINK_CONNECTED;
        } else {
            next = USAGE_LINK_ADVERTISING;
            action = USAGE_LINK_ACTION_ADVERTISE;
        }
        break;
    }

    if (out_action) *out_action = action;
    return next;
}
