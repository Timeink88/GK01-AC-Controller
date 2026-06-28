# GK01 AC Controller

基于吉联科技（GreeLink）GK01 开发板的万能红外空调控制器固件。

板子开机自动创建 WiFi 热点（AP 模式），手机连接后打开浏览器即可控制空调。支持多台设备协同工作，无需路由器或互联网。

## 功能

- **空调控制** — 内置 30+ 品牌空调协议（格力/华凌/美的/海尔/大金/三菱/松下等），选品牌调温度一键开机
- **自定义编码器** — 格力 YBOFB、华凌 WAHIN 使用自研编码器，绕过库兼容问题
- **Captive Portal** — 手机连接 WiFi 后自动弹出控制页面，无需手动输入地址
- **红外学习** — 捕获任意遥控器信号，命名保存
- **遥控面板** — 保存的学习按钮一键发送，支持重命名/删除
- **多设备协同** — 多台设备自动组网，手机控制一台主机，所有设备同步发射红外信号
- **AP 热点可配置** — 通过 WebUI 自定义热点名称和密码，从机自动适配
- **恢复出厂设置** — WebUI 一键恢复出厂，或长按硬件按键 5 秒重置
- **iOS 风格界面** — 手机端优化，添加到主屏幕即变成独立 App
- **零依赖** — 不需要电脑、路由器、互联网，板子自己就是热点

## 硬件

| 项目 | 参数 |
|------|------|
| 板子型号 | GK01（IR Mini V105 圆形 PCB） |
| 厂商 | 吉联科技 / GreeLink |
| 主控 | ESP-12F (ESP8266, 80MHz, 4MB Flash) |
| 红外发射 | 8× IR LED（GPIO14/D5） |
| 红外接收 | VS1838B（GPIO5/D1） |
| 指示灯 | 蓝色 GPIO4/D2, 红色 GPIO12/D6, 黄色 GPIO13/D7 |
| 按键 | GPIO13/D7（复用黄色 LED 引脚） |
| 供电 | Micro USB（仅供电）或焊盘 VCC/GND |
| 刷写接口 | 焊盘 TX/RX/GND/IO0，需 USB-TTL 转换器 |

## 快速开始

### 1. 编译固件

需要 [PlatformIO](https://platformio.org/)：

```bash
cd firmware
pio run -e rom0
```

编译产物在 `.pio/build/rom0/firmware.bin`。

### 2. 刷写固件

需要 USB-TTL 转换器（如 CH340），连接板子焊盘：

| USB-TTL | 板子焊盘 |
|---------|---------|
| TX | RX |
| RX | TX |
| GND | GND |

**进入刷写模式**：短接 IO0 焊盘和 GND 焊盘，然后上电。

```bash
# 擦除
python -m esptool --port COMx --baud 115200 --before no-reset erase-flash

# 刷写
python -m esptool --port COMx --baud 115200 --before no-reset write-flash -fm dout 0x0 firmware.bin
```

> 板子没有 FLASH 按键，必须短接 IO0-GND 才能进入刷写模式。Micro USB 仅供电，没有数据线。

### 3. 使用

1. 刷写完成后拔插电源重启
2. 手机搜索 WiFi `IR-AC`，密码 `12345678`
3. 连接后自动弹出控制页面（Captive Portal）
4. 选择空调品牌 → 设置温度/模式 → 一键控制
5. iOS 用户：Safari → 分享 → 添加到主屏幕（变成独立 App）

## 多设备协同

多台 GK01 可以自动组网协同工作，实现一台手机控制所有设备同步发射红外信号。

### 工作原理

设备上电后自动判断角色：

- **扫描到已有的 `IR-AC` WiFi** → 成为**从机**（STA 模式），连接主机，监听 UDP 广播
- **没有扫描到** → 成为**主机**（AP 模式），开热点、运行 WebUI，向从机广播指令

```
📱 手机
  │ 连接 IR-AC
  ▼
🖥 主机 (AP + WebUI + IR发射)
  │ UDP 广播（端口 8888）
  ├── 🖥 从机 A (STA + IR发射)
  └── 🖥 从机 B (STA + IR发射)
```

### 使用方法

1. 所有板子刷入相同固件
2. 先给一台设备上电 → 自动成为主机
3. 再给其他设备上电 → 自动发现主机并成为从机
4. 手机连主机的 WiFi，打开 WebUI 控制即可
5. 所有设备同步发射红外信号

### 从机状态指示

| LED 行为 | 含义 |
|----------|------|
| 常亮 1 秒后灭 | 从机已连接主机 |
| 短暂闪烁 | 正在发射红外信号 |
| 不亮 | 未连接（正在重连） |

## 网络配置

设备上电后自动创建 WiFi 热点（AP 模式），默认配置：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| WiFi 名称 | `IR-AC` | 可通过 WebUI 修改，从机自动适配 |
| WiFi 密码 | `12345678` | 可通过 WebUI 修改，至少 8 位 |
| 设备 IP | `10.1.1.1` | AP 模式固定 IP |
| 子网掩码 | `255.255.255.0` | — |
| 广播地址 | `10.1.1.255` | UDP 协同通信 |
| UDP 端口 | `8888` | 主从通信端口 |
| AP 信道 | 自动（0） | 自动选择干扰最小的信道 |

> WiFi 名称和密码可通过 WebUI「设置 → 热点配置」修改，保存后设备重启生效。所有从机需使用相同的热点名称和密码才能自动组网。从机启动时先尝试自定义名称，找不到则回退到默认 `IR-AC`。恢复出厂后回到默认配置。

## GPIO 引脚分配

| GPIO | NodeMCU | 功能 | 说明 |
|------|---------|------|------|
| 14 | D5 | IR 发射 | 8× 红外 LED，低电平有效发射 |
| 5 | D1 | IR 接收 | VS1838B 红外接收器 |
| 4 | D2 | 蓝色 LED | 系统状态指示（低电平点亮） |
| 12 | D6 | 红色 LED | IR 活动指示（低电平点亮） |
| 13 | D7 | 黄色 LED / 按键 | 状态指示 + 长按 5 秒恢复出厂（复用引脚） |
| 0 | IO0 | 刷写模式 | 短接 GND 进入刷写模式 |

## 工作模式

设备上电时自动判断角色，无需手动配置：

```
上电
 │
 ▼
扫描 WiFi，搜索 "IR-AC"
 │
 ├── 找到 → STA 模式（从机）
 │    连接主机 WiFi
 │    监听 UDP 8888 端口
 │    收到指令 → 发射红外信号
 │
 └── 未找到 → AP 模式（主机）
      创建 WiFi 热点 "IR-AC"
      启动 Web 服务器 (10.1.1.1)
      接收手机指令 → 发射红外 + UDP 广播给从机
```

### 主机模式（Master）

- 创建 WiFi AP，运行 WebUI
- 接收手机控制指令
- 通过 UDP 广播（`10.1.1.255:8888`）将指令转发给所有从机
- 自身同时发射红外信号

### 从机模式（Slave）

- 连接主机的 WiFi（STA 模式）
- 监听 UDP 广播（端口 8888）
- 收到指令后发射红外信号
- LED 状态反馈连接情况

## 页面说明

| 页面 | 功能 |
|------|------|
| 空调 | 选择品牌，设置温度/模式/风速，一键开关机 |
| 学习 | 捕获遥控器信号，命名保存 |
| 遥控 | 发送已保存的信号，支持编辑/删除 |
| 设置 | WiFi/MQTT/热点配置，导出/导入，恢复出厂，OTA 更新 |

## 空调品牌

内置支持 30+ 空调协议，WebUI 中可选的品牌：

### 自定义编码器（精准控制）

以下品牌使用自研编码器，直接构造红外信号，绕过 IRremoteESP8266 库的兼容问题：

| 品牌 | 协议名 | 说明 |
|------|--------|------|
| 华凌 | `WAHIN` | R05D 协议，Gray 码温度编码，MSB-first |
| 格力 | `GREE` | YBOFB 遥控器，LSB-first，双帧 A+B 模式 |

### IRremoteESP8266 库支持

| 品牌 | 协议名 | 说明 |
|------|--------|------|
| 美的 | `MIDEA` | 美的空调（48-bit 协议） |
| 美的 (24bit) | `MIDEA24` | 美的空调（24-bit 协议） |
| 海尔 | `HAIER` | 海尔空调 |
| 科龙 | `KELON` | 科龙空调 |
| TCL | `TCL112` | TCL 空调 |
| TECO | `TECO` | TECO 空调 |
| 大金 | `DAIKIN` | 大金空调 |
| 东芝 | `TOSHIBA` | 东芝空调 |
| 三菱 | `MITSUBISHI` | 三菱电机 |
| 三菱重工 | `MITSUBISHI_HEAVY` | 三菱重工 |
| 松下 | `PANASONIC` | 松下空调 |
| 富士通 | `FUJITSU` | 富士通空调 |
| 夏普 | `SHARP` | 夏普空调 |
| 三洋 | `SANYO` | 三洋空调 |
| 日立 | `HITACHI` | 日立空调 |
| 三星 | `SAMSUNG` | 三星空调 |
| LG | `LG` | LG 空调 |
| 惠而浦 | `WHIRLPOOL` | 惠而浦空调 |
| 博世 | `BOSCH` | 博世空调 |
| Carrier | `CARRIER` | Carrier 空调 |
| York | `YORK` | York 空调 |
| Coolix | `COOLIX` | 万能协议 |

> 如果内置品牌无法控制，请使用「学习」功能捕获遥控器原始信号。

## API

| 接口 | 方法 | 说明 |
|------|------|------|
| `/` | GET | WebUI 页面 |
| `/api/hvac` | POST | 空调控制 |
| `/api/send` | POST | 发送原始红外信号 |
| `/api/capture` | GET | 获取最近捕获信号 |
| `/api/wifi/scan` | GET | 扫描 WiFi 网络 |
| `/api/wifi/connect` | POST | 连接 WiFi 并重启 |
| `/api/wifi/status` | GET | 当前网络状态 |
| `/api/wifi/forget` | POST | 清除 WiFi 配置并重启 |
| `/api/ap/config` | GET/POST | 读取/保存 AP 热点配置 |
| `/api/mqtt/config` | GET/POST | 读取/保存 MQTT 配置 |
| `/api/factory/reset` | POST | 恢复出厂设置 |
| `/api/sensor` | GET | 温度和人体传感器状态 |
| `/api/capture` | GET | 获取最近捕获的信号 |

### `/api/hvac` 空调控制

POST 参数（JSON 或 form-data）：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `vendor` | string | ✅ | 空调品牌协议名（如 `GREE`、`MIDEA`、`TECO`） |
| `power` | string | ✅ | 开关：`On` / `Off` |
| `mode` | string | ✅ | 模式：`Cool`（制冷）/ `Heat`（制热）/ `Fan`（送风）/ `Dry`（除湿）/ `Auto`（自动） |
| `temp` | number | ✅ | 温度（16-30°C） |
| `fan` | string | ❌ | 风速：`Auto` / `Low` / `Medium` / `High` / `Max` |
| `swing` | string | ❌ | 摆风：`Auto` / `Off` |

### `/api/send` 发送原始信号

POST 参数：`raw=1000,500,1000,500,...`（逗号分隔的微秒时序）

### `/api/capture` 获取捕获信号

GET 返回最近一次红外学习捕获到的原始时序数据。

**错误返回**：所有 API 在参数错误时返回 `{"ok":false,"error":"reason"}`，成功返回 `{"ok":true}`。

## 项目结构

```
firmware/
├── platformio.ini          # PlatformIO 配置（rboot 双分区）
├── src/main.cpp            # 主程序（AP/STA/Home 三模式 + WebServer + IR + UDP + MQTT + 传感器）
└── include/webui.h         # iOS 风格 WebUI（PROGMEM 内嵌）
```

## 技术栈

- **框架**：Arduino (ESP8266)
- **IR 库**：[IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266) v2.8.6+
- **Web 服务器**：ESP8266WebServer
- **DNS**：DNSServer（Captive Portal 自动弹出）
- **设备通信**：WiFi UDP 广播（端口 8888）
- **前端**：原生 HTML/CSS/JS，无框架依赖

## 版本历史

| 版本 | 说明 |
|------|------|
| v1.3 | 初始版本，基础空调控制 + 学习 + 协同 |
| v1.4 | 网络配置宏化、AP 自动选频、按键触发 IR、灯光 bug 修复 |
| v1.5 | 华凌空调自定义编码器（R05D/Gray码）、格力 YBOFB 自定义编码器、Captive Portal 自动弹出 |
| v2.0 | WiFi STA + MQTT Home Assistant + DS18B20 温度 + AM312 人体传感器 + rboot 双分区 OTA |
| v2.2 | AP 热点名称/密码可配置、从机自适应组网、硬件长按恢复出厂、WebUI 恢复出厂设置 |

## 许可证

MIT
