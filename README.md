<div align="center">

<img src="docs/assets/img/cover.jpg" alt="Mosaico Film" width="100%">

# Mosaico Film

**装进 ESP-Mosaico 的随身胶片暗房**

取景 · 拍摄 · 冲洗 · 重洗 · 分享

[![在线烧录](https://img.shields.io/badge/在线烧录-Mosaico_Ideas-ff5a1f?style=for-the-badge)](https://mosaico-ideas.espressif.com/firmware/1cc4d85f-a6cf-4f7d-af48-17ca3bd18bd8)
[![项目主页](https://img.shields.io/badge/项目主页-GitHub_Pages-ff9a3c?style=for-the-badge)](https://hkgood.github.io/mosaico-film/)
[![下载固件](https://img.shields.io/badge/下载固件-v1.0.0-c8371f?style=for-the-badge)](https://github.com/hkgood/mosaico-film/releases/latest)

[![Release](https://img.shields.io/github/v/release/hkgood/mosaico-film?style=flat-square&color=ff9a3c)](https://github.com/hkgood/mosaico-film/releases/latest)
[![License](https://img.shields.io/badge/license-Apache--2.0-f1e8d8?style=flat-square)](LICENSE)
![ESP32-S31](https://img.shields.io/badge/chip-ESP32--S31-ff5a1f?style=flat-square)
![ESP-GSP](https://img.shields.io/badge/UI-ESP--GSP_1.5.1-ff9a3c?style=flat-square)

**中文** · [English](README.en.md)

</div>

---

<table>
<tr>
<td width="54%"><img src="docs/assets/gif/film_flow.gif" alt="Mosaico Film 从拍摄到冲洗" width="100%"></td>
<td>

Mosaico Film 把 ESP-Mosaico 变成一台有取景器、快门、胶卷和暗房的数码胶片相机。

选择受经典旁轴和折叠式拍立得启发的机身，转动拨轮换胶卷、调曝光，再按下机身红色按键拍摄。照片在设备上本地冲洗、保存，还可以换一卷胶片重新冲洗。

| | |
| :-: | :-: |
| **8** 款胶片预设 | **2** 种相机体验 |
| **480×480** 自绘界面 | **本地** 冲洗与相册 |

</td>
</tr>
</table>

## 🎬 演示片

> 点击海报前往[项目主页](https://hkgood.github.io/mosaico-film/#film)播放完整演示片。

<p align="center">
  <a href="https://hkgood.github.io/mosaico-film/#film">
    <img src="docs/assets/video/mosaico_film_demo_poster.jpg" alt="Mosaico Film 演示片海报" width="760">
  </a>
</p>

## 📷 从取景到暗房

<p align="center"><img src="docs/assets/img/contact.jpg" alt="Mosaico Film 拍摄、冲洗与分享流程" width="900"></p>

1. **选机身**：从屏幕顶端下拉，在 M6 风格旁轴和 SX-70 风格拍立得之间切换。
2. **装胶卷**：转动拨轮选择胶卷；也可以摇一摇设备随机换卷。
3. **拍摄**：使用屏幕快门或机身红色 AI 按键，配合曝光补偿、漏光和日期戳。
4. **本地冲洗**：设备完成裁切、转正、滤镜、颗粒、暗角与相纸排版，再写入板载存储。
5. **重新冲洗**：保留原片，在暗房里换胶卷、调效果，生成新的版本。
6. **发送到手机**：选择照片后显示二维码；通过同一 Wi‑Fi 或设备热点在手机浏览器中下载。

## 🎞️ 八卷胶片

| 胶卷 | 风格 |
| --- | --- |
| **GOLD 200** | 暖金色调、饱满日光与柔和颗粒 |
| **SOFT 400** | 低反差、自然肤色和轻微暖影 |
| **VERDE 200** | 青绿色阴影与清透高光 |
| **CROSS X** | 高反差负冲、蓝色暗部与暖黄高光 |
| **SILVER 400** | 银盐黑白、明显颗粒与暗角 |
| **FADED 77** | 褪色暖调、抬高黑位的旧照片感 |
| **NIGHT 800T** | 钨丝灯夜景、冷调与红色光晕 |
| **PIXEL 8BIT** | 下采样和调色板量化的像素胶卷 |

取景器运行快速预览，成片则使用完整冲洗管线；同一随机种子会得到可复现的颗粒与漏光。

## 🧪 一台设备，两种相机

- **M6 风格旁轴**：4:3 横幅取景，胶卷计数、曝光拨轮与日期戳，适合连续拍摄。
- **SX-70 风格拍立得**：方形取景、即时相纸排版和机械拨杆反馈。
- **触觉与声音**：快门、拨盘、按钮和摇一摇都有对应的声音与马达反馈。
- **方向感知**：拍摄时记录设备方向，冲洗后自动把照片转正。

> M6、SX-70、Leica 与 Polaroid 是其各自权利人的商标。本项目独立开发，与这些公司无关联或背书关系。

## ⚡ 安装

**硬件**

- ESP-Mosaico 主板（ESP32-S31，16 MB Flash）
- 安装在左侧扩展位的 OV3640 相机模块

**在线烧录（推荐）**

1. 打开 Mosaico Ideas 上的 [Mosaico Film 官方烧录页](https://mosaico-ideas.espressif.com/firmware/1cc4d85f-a6cf-4f7d-af48-17ca3bd18bd8)。
2. 用 USB 连接设备，在「下载固件」区域选择「在线烧录」，按页面提示完成安装。
3. 烧录页同时提供 Iris 包和完整 BIN 下载；完整 BIN 用于首次安装，会覆盖设备上的固件与设置。

**通过 GitHub Release 安装**

1. 从 [v1.0.0 Release](https://github.com/hkgood/mosaico-film/releases/latest) 下载 `.irisfw`。
2. 在 ESP-Mosaico 工作区使用 `python mosaico.py iris system-update --bundle <package.irisfw>` 安装。
3. 空白或状态未验证的设备请先运行 `python mosaico.py recover`。

Mosaico Film 使用独立的 `assets` 分区和应用布局，因此首次安装必须使用完整的 `system-update`；Vibe Mode 的恢复固件与配置保持不变。

## 🧩 技术亮点

- **可移植 C 界面**：全部 UI 绘制到 480×480 RGB565 画布，通过 `film_port_t` 同时连接 PC 模拟器和设备服务。
- **本地暗房管线**：曲线、饱和度、分离色调、红晕、暗角、漏光和颗粒均在设备端完成；像素胶卷走独立量化分支。
- **异步拍摄链路**：取景、全尺寸冲洗、JPEG 编码和 NAND 写入分工执行，界面通过事件和进度保持响应。
- **无 App 分享**：设备在局域网或临时热点上提供带会话令牌的照片页面，手机扫码即可下载。
- **同一份代码可测试**：滤镜、暗房、相册和完整 UI 交互均可在主机上编译测试。

## 🛠️ 从源码构建

本仓库是一个 ESP-Mosaico 应用，需要克隆到
[`esp-mosaico-vibe`](https://github.com/esp-mosaico/esp-mosaico-vibe)
工作区的 `projects/mosaico_film`：

```sh
git clone https://github.com/esp-mosaico/esp-mosaico-vibe.git
cd esp-mosaico-vibe
git submodule update --init \
  submodule/esp-mosaico-utils \
  submodule/esp-mosaico-bsp
git clone https://github.com/hkgood/mosaico-film.git projects/mosaico_film

python mosaico.py project sim --project projects/mosaico_film --interactive
python mosaico.py iris system-update --project projects/mosaico_film
```

固件目标为 `esp32s31`，需使用工作区固定的 ESP-IDF 版本。主机测试、模块地图、媒体生成和设备更新注意事项见 [DEVELOPMENT.md](DEVELOPMENT.md)。

```text
main/          设备服务：相机、冲洗任务、存储、分享、反馈
components/    可移植 UI、滤镜、暗房、绘图、素材与二维码
pc/            PC 模拟器平台实现
host_test/     滤镜、暗房、相册和交互测试
ui/            ESP-GSP 场景与字体
promo/         Remotion 演示片工程
docs/          GitHub Pages 与精选媒体
```

## 📄 许可证与致谢

[Apache License 2.0](LICENSE) © 2026 Rocky。第三方组件、字体、照片与商标说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
