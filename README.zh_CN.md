<p align="right"><strong>简体中文</strong> · <a href="README.md">English</a></p>

# AI Passport · WP7 磁贴界面

面向 **FoloToy AI Passport** 的独立 ESP-IDF 应用。它把 [ZyoungInc 的 WP7 风格 LVGL 界面](https://github.com/ZyoungInc/JC4880P443C_BSP/tree/wp7)移植到 Passport 的 ESP32-C3、240 × 320 ST7789P3 彩屏与三颗实体按键。项目直接使用并适配上游的页面对象、设置项和转场；Passport 原来的终端应用不在本仓库中。

![Passport 整机显示 WP7 UI Settings 的效果图](assets/passport-wp7-settings-hero.png)

*主图由设备外观图与 UI Settings 实机截图合成，用于展示装机效果。*

## 功能与现状

- 两列彩色磁贴、应用列表、原版设置页进入／退场动画；亮度、深色模式、主题色、动画速度和快速动画保存到 NVS。
- **Clock**：按键手动设置时间；设备断电后需重新设置，未设置时状态栏显示 `--:--`。
- **Battery**：读取板载 CW2017 电量计的百分比与电压；读取失败时显示不可用。
- **Stopwatch**：开始、暂停、记圈与重置；退出页面后仍继续计时。
- **Focus**：可选 5／15／25／45 分钟计时；退出页面后仍继续计时。
- **AI Usage**：预留入口，目前没有连接账户或额度接口，也不会显示虚构用量。
- 状态栏的 Wi-Fi 和电池图标沿用界面样式，不代表实际连接状态或实时电量；实际电量请看 Battery 页面。

目标硬件是 **ESP32-C3、8 MB Flash、无 PSRAM、ST7789P3 240 × 320 SPI 屏、三键 ADC 输入** 的 FoloToy AI Passport。其他 ESP32 板卡需要调整 `components/passport_bsp` 中的引脚、显示与输入驱动。

## 实机截图

| 首页磁贴 | UI Settings |
| :---: | :---: |
| ![Passport 实机首页的六块彩色磁贴](screenshots/wp7-home.png) | ![Passport 实机的 UI Settings 页面](screenshots/wp7-settings.png) |

这两张 240 × 320 PNG 由 Passport 的 USB 串口从实际显示刷新数据生成，保留原始屏幕布局与像素。

## 按键

| 页面 | 上／下短按 | 上／下长按 | 确认短按 | 确认长按 |
| --- | --- | --- | --- | --- |
| 磁贴 | 选择磁贴 | 长按下打开应用列表 | 打开选中的磁贴；未选中时打开列表 | 打开列表 |
| 应用列表 | 选择项目 | — | 打开项目 | 返回磁贴 |
| UI Settings | 选择控件 | — | 修改控件 | 返回，播放原版退场动画 |
| Clock | 加 1 小时／加 1 分钟 | 加 6 小时／加 10 分钟 | — | 返回 |
| Stopwatch | 记圈／暂停时重置 | — | 开始／暂停 | 返回 |
| Focus | 暂停时换时长／重置 | — | 开始／暂停 | 返回 |

选中项有细边框提示，因为 Passport 没有触摸屏。

## 编译与写入

1. 按 [Espressif 的 ESP32-C3 安装指南](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32c3/get-started/index.html)安装并激活 **ESP-IDF 5.5.3**。在终端运行 `idf.py --version` 确认版本。
2. 在本仓库根目录编译：

   ```sh
   idf.py set-target esp32c3
   idf.py build
   ```

   首次构建会由组件管理器下载 LVGL、`esp_lvgl_port` 和 `button`；版本记录在 `dependencies.lock`。成功后应用镜像是 `build/passport_wp7.bin`。
3. 用数据线连接 Passport，确认目标串口和设备分区布局与本仓库的 `partitions.csv` 相符。macOS 端口通常是 `/dev/cu.usbmodemXXXX`，Linux 通常是 `/dev/ttyACM0`，Windows 为 `COM` 端口。把以下命令中的 `PORT` 换成实际端口：

   ```sh
   idf.py -p PORT app-flash
   idf.py -p PORT monitor
   ```

   `app-flash` 只更新应用分区，不写入引导程序、分区表或 NVS。监视器用 `Ctrl+]` 退出。若串口被其他程序占用，先关闭该程序。

本仓库的分区表针对已验证的 Passport 布局。其他批次或已经改过分区的设备，应先核对布局；不能仅凭屏幕和芯片型号就刷入。完整 `idf.py flash` 还会写入引导程序和分区表，请只在专用开发板或确认需要该布局时使用。`build/`、`managed_components/` 与本机 `sdkconfig` 均不入库，构建配置以 `sdkconfig.defaults` 为准。

## 从实体屏幕截图

固件通过 USB 串口提供只读截图命令。让设备停在目标页面后运行：

```sh
python tools/capture_wp7.py --port PORT \
  --output screenshots/capture.png --wait-for-enter
```

看到 `Serial ready` 后确认页面，再按回车。脚本从显示器实际刷新的 RGB565 区域重建 PNG，ESP32-C3 不需要另分配整屏帧缓冲。Python 环境需要 `pyserial`（ESP-IDF 环境已包含；单独使用脚本可安装 `tools/requirements.txt`）。

## 源码与致谢

| 项目 | 在这里的用途 | 来源／授权信息 |
| --- | --- | --- |
| [ZyoungInc/JC4880P443C_BSP `wp7`](https://github.com/ZyoungInc/JC4880P443C_BSP/tree/wp7) | `main/wp7_ui.c` 的 WP7 页面、主题、动画和设置结构 | 基于提交 `9d1743a`；原文件标注 `SPDX-License-Identifier: Apache-2.0`；见 [移植记录](UPSTREAM.zh_CN.md) |
| [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) | `components/passport_bsp` 的 Passport 显示、按键、I²C、电量计代码 | 原仓库 MIT License；本仓库为精简与适配版本 |
| [LVGL](https://github.com/lvgl/lvgl)、[ESP-IDF](https://github.com/espressif/esp-idf)、[esp_lvgl_port](https://components.espressif.com/components/espressif/esp_lvgl_port) | 图形、系统与显示移植依赖 | 通过 ESP-IDF 组件管理器获取；不将下载的组件代码入库 |

本仓库**尚未选择整体项目许可证**。上述许可证说明仅适用于对应来源文件，不能理解为整个仓库已经采用 Apache-2.0 或 MIT；正式公开发布前需要确定新增代码的授权并复核上游许可。具体移植改动见 [UPSTREAM.zh_CN.md](UPSTREAM.zh_CN.md)。
