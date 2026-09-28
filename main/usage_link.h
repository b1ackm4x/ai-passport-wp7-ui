// main/usage_link.h —— 看板的 BLE 数据链路。
//
// 本模块是应用内 NimBLE 的**唯一 owner**：只 init 一次、常驻广播/服务，
// 页面切换不参与 BLE 生命周期。仓库里不得再有第二处 nimble_port_init()。
#pragma once

#include "usage_model.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// 启动 NimBLE 与 GATT server 并开始广播。重复调用返回 ESP_ERR_INVALID_STATE。
esp_err_t usage_link_start(void);

// 取一份快照副本。内部加锁复制，调用方拿到的是自洽的一帧。
//
// 返回 false 表示至今没有收到过任何合法 payload（UI 应显示未同步态）。
// out_generation 用于让 UI 跳过无变化的重绘。
bool usage_link_get(usage_snapshot_t *out, uint32_t *out_generation);

// 当前是否有 central 连着。注意：链路在线与数据有效是两件事 ——
// 断连后最后一份快照仍然保留，由 UI 用 sync age 决定如何呈现。
bool usage_link_connected(void);

// 已协商的 ATT MTU；未连接时返回 0。用于诊断低 MTU 路径。
uint16_t usage_link_mtu(void);
