<p align="right"><strong>简体中文</strong> · <a href="UPSTREAM.md">English</a></p>

# 上游来源与移植

- 来源：`ZyoungInc/JC4880P443C_BSP` 的 `wp7` 分支，提交
  `9d1743a9444a8d1fe9226e24c7f2b930e22f8612`。
- 原版界面文件 `main/main.c` 复制为 `main/wp7_ui.c`，保留原 SPDX 声明。
  仓库默认 `main` 分支运行的是 `lv_demo_widgets()`，不是 WP7 界面。
- 保留原版的 WP7 页面对象、主题、列表、设置页、转场和 NVS 配置。
- 用 Passport ESP32-C3 的 ST7789P3 SPI 彩屏、内部 RAM 中的 40 行 LVGL
  缓冲和三颗 ADC 按键替换原 ESP32-P4 的 MIPI-DSI、PSRAM、触摸屏 BSP。
- 为 240 × 320 屏调整字号、磁贴尺寸、设置页纵向比例和默认蓝色主题；
  增加实体键焦点边框和按键事件转换。
- 将 LVGL 刷新周期从 33 ms 调到原项目使用的 15 ms，动画速度和快速动画
  继续由原项目的设置页控制。
- 将 LVGL 专用内存从 64 KB 增至 96 KB；原配置下设置页退出时绘制任务
  会卡住，增大内存后原版退场动画在首页和应用列表路径均通过设备测试。
- Passport 设置页以 16/22 像素字体的实际行高安排文字框，并缩小主题色块；
  避免原比例缩放后文字被滑块或色块覆盖。
- 将数字磁贴与演示列表替换为实际应用名称；新增 Passport 功能页和板载
  CW2017 电量计读取。AI 用量入口尚未接入账户数据。
- 原版是界面演示，不是手机操作系统；Passport 版为五个应用列表项新增了
  对应功能页。

## 代码来源与许可边界

- `components/passport_bsp/` 从 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)
  的 `components/bsp/` 精简、适配而来；核对时的基础提交为
  `1051209d807fb26f943236b7e02281f13d39bc90`。原项目的 MIT 许可全文保留在
  [`LICENSES/FoloToy-MIT.txt`](LICENSES/FoloToy-MIT.txt)。
- `main/wp7_ui.c` 的上游原文件保留了 `SPDX-License-Identifier: Apache-2.0`；
  对应许可全文在 [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt)。
  上游 `wp7` 分支没有仓库根目录的 `LICENSE` 文件，因此不能把这个文件级标注
  延伸为整个上游仓库的许可声明。
- BSP 中显示和按键源文件保留了原来的 `trae_card` 移植说明；此仓库没有附带
  另一份 `trae_card` 源码。LVGL 和 Espressif 组件由组件管理器获取，不入库。
- 本仓库新增代码的整体授权尚未确定。上述许可副本用于保存现有来源的声明，
  不等于为整个新仓库选定许可证。
