// tests/test_usage_model.c —— 看板纯逻辑层的宿主测试。
//
// 覆盖：wire golden vector、字段校验的每条拒绝路径、字符串边界、
// 时钟外推、新鲜度判定，以及 BLE 链路状态机的全部转移。
#include <assert.h>
#include <string.h>

#include "usage_model.h"

// 与 tools/usage_bridge.py 的 struct 布局共享的 golden vector。
// 两侧任一改动都必须让本测试失败。
#define GEN_UNIX      1788400000u   // 2026-09-03 前后
#define KABOO_SAMPLED 1788399900u
#define CLAUDE_SAMPLE 1788399800u
#define FIVE_RESETS   1788420000u
#define SEVEN_RESETS  1788829200u

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)(v & 0xFFFFFFFFu));
    put_le32(p + 4, (uint32_t)(v >> 32));
}

// 构造一份合法 payload；各用例在此基础上只改要测的字段。
static void build_valid(uint8_t *wire)
{
    memset(wire, 0, USAGE_WIRE_SIZE);
    wire[0] = USAGE_WIRE_VERSION;
    wire[1] = USAGE_FLAG_KABOO_VALID | USAGE_FLAG_CLAUDE_VALID;
    put_le32(wire + 2, GEN_UNIX);
    put_le16(wire + 6, (uint16_t)(int16_t)480);   // UTC+8
    put_le32(wire + 8, KABOO_SAMPLED);
    put_le32(wire + 12, CLAUDE_SAMPLE);
    put_le32(wire + 16, 144625529u);              // today tokens
    put_le64(wire + 20, 2245178539ull);           // week tokens
    put_le64(wire + 28, 9145403458ull);           // month tokens (> 32 bit)
    put_le64(wire + 36, 57892473518ull);          // all tokens (> 32 bit)
    put_le32(wire + 44, 74u);                     // today cents
    put_le32(wire + 48, 283774u);                 // week cents
    put_le32(wire + 52, 827886u);                 // month cents
    put_le32(wire + 56, 3665536u);                // all cents
    memcpy(wire + 60, "claude opus 5", 13);
    wire[84] = 13;                                // five hour pct
    wire[85] = 25;                                // seven day pct
    put_le32(wire + 86, FIVE_RESETS);
    put_le32(wire + 90, SEVEN_RESETS);
}

static void test_golden_vector(void)
{
    uint8_t wire[USAGE_WIRE_SIZE];
    build_valid(wire);

    usage_snapshot_t s;
    assert(usage_model_decode(wire, sizeof wire, 5000000, GEN_UNIX, &s) == USAGE_DECODE_OK);

    assert(s.version == USAGE_WIRE_VERSION);
    assert(s.generated_unix == GEN_UNIX);
    assert(s.tz_offset_minutes == 480);
    assert(s.kaboo_sampled_unix == KABOO_SAMPLED);
    assert(s.claude_sampled_unix == CLAUDE_SAMPLE);
    assert(s.today_tokens == 144625529u);
    assert(s.week_tokens == 2245178539ull);
    // 超过 32 位的窗口值必须完整还原，验证 le64 解码。
    assert(s.month_tokens == 9145403458ull);
    assert(s.all_tokens == 57892473518ull);
    assert(s.today_cost_cents == 74u);
    assert(s.month_cost_cents == 827886u);
    assert(s.five_hour_pct == 13);
    assert(s.seven_day_pct == 25);
    assert(s.five_hour_resets_unix == FIVE_RESETS);
    assert(s.seven_day_resets_unix == SEVEN_RESETS);
    assert(strcmp(s.top_model, "claude opus 5") == 0);
    assert(s.received_monotonic_us == 5000000);
}

static void test_rejects_bad_input(void)
{
    uint8_t wire[USAGE_WIRE_SIZE];
    usage_snapshot_t s;

    // 长度必须精确匹配，短包与长包都拒绝。
    build_valid(wire);
    assert(usage_model_decode(wire, USAGE_WIRE_SIZE - 1, 0, GEN_UNIX, &s) == USAGE_DECODE_BAD_LENGTH);
    assert(usage_model_decode(wire, USAGE_WIRE_SIZE + 1, 0, GEN_UNIX, &s) == USAGE_DECODE_BAD_LENGTH);
    assert(usage_model_decode(NULL, USAGE_WIRE_SIZE, 0, GEN_UNIX, &s) == USAGE_DECODE_BAD_LENGTH);

    // 旧版 v1 与未来版本都要拒绝，不能只测一个方向。
    build_valid(wire);
    wire[0] = USAGE_WIRE_VERSION - 1;
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_BAD_VERSION);
    build_valid(wire);
    wire[0] = USAGE_WIRE_VERSION + 1;
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_BAD_VERSION);

    // 保留位必须为 0，否则说明对端用了本固件不认识的协议扩展。
    build_valid(wire);
    wire[1] |= 0x80;
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_RESERVED_FLAG);

    build_valid(wire);
    wire[84] = 101;
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_BAD_PERCENT);

    build_valid(wire);
    wire[85] = 255;
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_BAD_PERCENT);

    build_valid(wire);
    put_le16(wire + 6, (uint16_t)(int16_t)-721);
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_BAD_TIMEZONE);

    build_valid(wire);
    put_le16(wire + 6, (uint16_t)(int16_t)841);
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_BAD_TIMEZONE);

    // 边界值本身应当被接受。
    build_valid(wire);
    put_le16(wire + 6, (uint16_t)(int16_t)-720);
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_OK);
    build_valid(wire);
    put_le16(wire + 6, (uint16_t)(int16_t)840);
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_OK);
    build_valid(wire);
    wire[84] = 100;
    wire[85] = 100;
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_OK);

    // 1970 之类的损坏 epoch 必须拒绝，否则时间页会显示假时刻。
    build_valid(wire);
    put_le32(wire + 2, 100u);
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_BAD_EPOCH);

    build_valid(wire);
    put_le32(wire + 8, 100u);
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_BAD_EPOCH);

    // The generated timestamp is bounded against the receiver's trusted clock;
    // a far-future payload must not be allowed to poison time synchronization.
    build_valid(wire);
    put_le32(wire + 2, GEN_UNIX + USAGE_EPOCH_SKEW_MAX + 1u);
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_BAD_EPOCH);
}

// 记录解码器在"基准时钟可信 / 不可信"两种输入下的契约，usage_link 正是靠它
// 才能从落后的时钟里恢复：未同步时传 0，让唯一能纠正时钟的那一包落地。
//
// 注意这里锁不住调用方：本测试直接调 usage_model_decode，不经过 on_chr_write。
// 若有人把 usage_link.c 里的 time_sync_clock_trusted() 门去掉、改回无条件
// time(NULL)，下面的断言不会有任何变化，永久失同步的 bug 会悄悄回来。
// usage_link 依赖 NimBLE 与系统时钟，目前没有宿主机替身可测。
static void test_stale_clock_recovery(void)
{
    uint8_t wire[USAGE_WIRE_SIZE];
    usage_snapshot_t s;

    // 固件构建于 9 天前并在今天刷机：Mac 推来的"现在"比设备自以为的"现在"晚 9 天。
    const uint32_t stale_reference = GEN_UNIX - 9u * 86400u;
    build_valid(wire);
    assert(usage_model_decode(wire, sizeof wire, 0, stale_reference, &s) ==
           USAGE_DECODE_BAD_EPOCH);

    // 同一包在"尚无可信基准"下必须被接受，否则时钟再也回不来。
    build_valid(wire);
    assert(usage_model_decode(wire, sizeof wire, 0, 0u, &s) == USAGE_DECODE_OK);
    assert(s.generated_unix == GEN_UNIX);

    // 一旦真正同步过，对离谱未来值的防护仍然有效。
    build_valid(wire);
    put_le32(wire + 2, GEN_UNIX + USAGE_EPOCH_SKEW_MAX + 1u);
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) ==
           USAGE_DECODE_BAD_EPOCH);
}

static void test_top_model_boundary(void)
{
    uint8_t wire[USAGE_WIRE_SIZE];
    usage_snapshot_t s;

    // 发送方填满 24 字节且不留 NUL —— 接收侧必须强制截断。
    build_valid(wire);
    memset(wire + 60, 'A', USAGE_TOP_MODEL_CAP);
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_OK);
    assert(strlen(s.top_model) == USAGE_TOP_MODEL_CAP - 1);
    assert(s.top_model[USAGE_TOP_MODEL_CAP - 1] == '\0');

    // 空模型名是合法的（kaboo 尚无数据时）。
    build_valid(wire);
    memset(wire + 60, 0, USAGE_TOP_MODEL_CAP);
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_OK);
    assert(s.top_model[0] == '\0');
}

static void test_clock(void)
{
    uint8_t wire[USAGE_WIRE_SIZE];
    build_valid(wire);

    usage_snapshot_t s;
    assert(usage_model_decode(wire, sizeof wire, 1000000, GEN_UNIX, &s) == USAGE_DECODE_OK);

    uint32_t now = 0;
    // 未同步时绝不给出时刻 —— UI 据此显示 "--:--"。
    assert(!usage_model_now_unix(&s, false, 1000000, &now));

    assert(usage_model_now_unix(&s, true, 1000000, &now));
    assert(now == GEN_UNIX);

    // 外推 90 秒。
    assert(usage_model_now_unix(&s, true, 91000000, &now));
    assert(now == GEN_UNIX + 90);

    // 单调时钟倒退（deep sleep 重启）必须判为不可信。
    assert(!usage_model_now_unix(&s, true, 500000, &now));

    // UTC+8：GEN_UNIX=1788400000 → UTC 01:46:40 → 本地 09:46:40
    usage_wall_clock_t wall;
    assert(usage_model_wall_clock(&s, true, 1000000, &wall));
    assert(wall.hour == 9);
    assert(wall.minute == 46);
    assert(wall.second == 40);
    // 同一时刻的公历日期，验证 civil_from_days。
    assert(wall.year == 2026);
    assert(wall.month == 9);
    assert(wall.day == 3);
    assert(wall.weekday == 4);   // 2026-09-03 是星期四

    // 跨小时进位：+800 秒 → 10:00:00
    assert(usage_model_wall_clock(&s, true, 1000000 + 800LL * 1000000, &wall));
    assert(wall.hour == 10);
    assert(wall.minute == 0);
    assert(wall.second == 0);

    // 跨日进位：再推 14 小时 14 分 → 次日 00:00
    assert(usage_model_wall_clock(&s, true,
                                  1000000 + (800LL + 50400LL) * 1000000, &wall));
    assert(wall.day == 4);
    assert(wall.hour == 0);
    assert(wall.weekday == 5);

    assert(!usage_model_wall_clock(&s, false, 1000000, &wall));

    assert(usage_model_sync_age_seconds(&s, true, 1000000) == 0);
    assert(usage_model_sync_age_seconds(&s, true, 121000000) == 120);
    assert(usage_model_sync_age_seconds(&s, false, 121000000) == -1);
}

static void test_freshness(void)
{
    // 采样时刻在 ttl 内 → 新鲜。
    assert(usage_model_source_fresh(1000, 1300, 600));
    assert(usage_model_source_fresh(1000, 1600, 600));   // 边界
    assert(!usage_model_source_fresh(1000, 1601, 600));

    // 从未采样过。
    assert(!usage_model_source_fresh(0, 1300, 600));

    // 采样时刻在未来说明两端时钟不一致，不能当新鲜。
    assert(!usage_model_source_fresh(2000, 1300, 600));

    // 配额窗口已翻篇 → 百分比是过期读数。
    assert(usage_model_quota_expired(1000, 1000));
    assert(usage_model_quota_expired(999, 1000));
    assert(!usage_model_quota_expired(1001, 1000));
    assert(usage_model_quota_expired(0, 1000));

    assert(usage_model_seconds_until(1600, 1000) == 600);
    assert(usage_model_seconds_until(1000, 1000) == 0);
    assert(usage_model_seconds_until(900, 1000) == 0);
}

static void test_link_state_machine(void)
{
    usage_link_action_t action;
    usage_link_state_t st;

    // host 就绪 → 开始广播。
    st = usage_model_link_step(USAGE_LINK_IDLE, USAGE_LINK_EV_SYNC, &action);
    assert(st == USAGE_LINK_ADVERTISING);
    assert(action == USAGE_LINK_ACTION_ADVERTISE);

    // 连接成功后不再广播（controller 已自动停止）。
    st = usage_model_link_step(st, USAGE_LINK_EV_CONNECT_OK, &action);
    assert(st == USAGE_LINK_CONNECTED);
    assert(action == USAGE_LINK_ACTION_NONE);

    // 这条是关键：断连必须重启广播，否则设备永久不可再被发现。
    st = usage_model_link_step(st, USAGE_LINK_EV_DISCONNECT, &action);
    assert(st == USAGE_LINK_ADVERTISING);
    assert(action == USAGE_LINK_ACTION_ADVERTISE);

    // 连接失败同样要恢复广播。
    st = usage_model_link_step(USAGE_LINK_ADVERTISING,
                               USAGE_LINK_EV_CONNECT_FAIL, &action);
    assert(st == USAGE_LINK_ADVERTISING);
    assert(action == USAGE_LINK_ACTION_ADVERTISE);

    // 已连接状态下的 ADV_COMPLETE 是正常现象，不应重启广播。
    st = usage_model_link_step(USAGE_LINK_CONNECTED,
                               USAGE_LINK_EV_ADV_COMPLETE, &action);
    assert(st == USAGE_LINK_CONNECTED);
    assert(action == USAGE_LINK_ACTION_NONE);

    // 广播态下的 ADV_COMPLETE（超时）需要续广播。
    st = usage_model_link_step(USAGE_LINK_ADVERTISING,
                               USAGE_LINK_EV_ADV_COMPLETE, &action);
    assert(st == USAGE_LINK_ADVERTISING);
    assert(action == USAGE_LINK_ACTION_ADVERTISE);

    // 关停后吸收一切事件，绝不重启广播 —— 否则 teardown 等不到静默。
    st = usage_model_link_step(USAGE_LINK_CONNECTED, USAGE_LINK_EV_SHUTDOWN, &action);
    assert(st == USAGE_LINK_STOPPING);
    assert(action == USAGE_LINK_ACTION_NONE);

    st = usage_model_link_step(USAGE_LINK_STOPPING, USAGE_LINK_EV_DISCONNECT, &action);
    assert(st == USAGE_LINK_STOPPING);
    assert(action == USAGE_LINK_ACTION_NONE);

    st = usage_model_link_step(USAGE_LINK_STOPPING, USAGE_LINK_EV_ADV_COMPLETE, &action);
    assert(st == USAGE_LINK_STOPPING);
    assert(action == USAGE_LINK_ACTION_NONE);

    st = usage_model_link_step(USAGE_LINK_STOPPING, USAGE_LINK_EV_SYNC, &action);
    assert(st == USAGE_LINK_STOPPING);
    assert(action == USAGE_LINK_ACTION_NONE);
}

static void test_single_claude_window(void)
{
    uint8_t wire[USAGE_WIRE_SIZE];
    usage_snapshot_t s;

    // Claude Code 只在窗口活跃时才报它；实测出现过只有 seven_day 的情况。
    // 那时 five_hour 的 reset 是 0，若两个窗口共用一个 valid 位，整包会因
    // 这个 0 epoch 被拒，连同一包里有效的 Kaboo 数据一起丢掉。
    build_valid(wire);
    wire[1] = USAGE_FLAG_KABOO_VALID | USAGE_FLAG_SEVEN_DAY;
    put_le32(wire + 86, 0);          // five_hour reset 缺失
    wire[84] = 0;                    // five_hour pct 缺失
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_OK);
    assert(s.flags & USAGE_FLAG_KABOO_VALID);      // Kaboo 必须存活
    assert(s.flags & USAGE_FLAG_SEVEN_DAY);
    assert(!(s.flags & USAGE_FLAG_FIVE_HOUR));
    assert(s.seven_day_pct == 25);
    assert(s.today_tokens == 144625529u);

    // 反向：只有 five_hour。
    build_valid(wire);
    wire[1] = USAGE_FLAG_KABOO_VALID | USAGE_FLAG_FIVE_HOUR;
    put_le32(wire + 90, 0);          // seven_day reset 缺失
    wire[85] = 0;
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_OK);
    assert(s.flags & USAGE_FLAG_FIVE_HOUR);
    assert(!(s.flags & USAGE_FLAG_SEVEN_DAY));
    assert(s.five_hour_pct == 13);

    // 声明了某个窗口却给出损坏的 reset，仍然必须拒绝 —— 逐窗口校验不等于
    // 放弃校验。
    build_valid(wire);
    wire[1] = USAGE_FLAG_KABOO_VALID | USAGE_FLAG_SEVEN_DAY;
    put_le32(wire + 90, 100u);       // seven_day reset 早于 2020
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_BAD_EPOCH);

    // 两个窗口都不声明：Kaboo 仍应通过。
    build_valid(wire);
    wire[1] = USAGE_FLAG_KABOO_VALID;
    put_le32(wire + 86, 0);
    put_le32(wire + 90, 0);
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_OK);
    assert(!(s.flags & USAGE_FLAG_CLAUDE_VALID));
    assert(s.today_tokens == 144625529u);

    // 新的保留位掩码：bit3 及以上仍必须为 0。
    build_valid(wire);
    wire[1] |= 0x08;
    assert(usage_model_decode(wire, sizeof wire, 0, GEN_UNIX, &s) == USAGE_DECODE_RESERVED_FLAG);
}

int main(void)
{
    test_golden_vector();
    test_rejects_bad_input();
    test_stale_clock_recovery();
    test_top_model_boundary();
    test_single_claude_window();
    test_clock();
    test_freshness();
    test_link_state_machine();
    return 0;
}
