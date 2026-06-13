# ESP32-S3 WiFi 固件集

ESP32-S3 开发板的 WiFi 固件集合，核心是一个可用的 **WiFi 配网（Captive Portal）** 方案：
板子通电后开放热点 → 手机连上自动弹出配网页 → 选附近 WiFi 输密码 → 板子连上并记住密码，
下次通电自动重连。

---

## 硬件信息

| 项 | 值 |
|---|---|
| 芯片 | ESP32-S3 (QFN56) rev v0.2 |
| USB | **原生 USB-Serial-JTAG**（VID `0x303A` / PID `0x1001`，即 HWCDC）|
| 串口（macOS）| `/dev/cu.usbmodem101` |
| 出厂 MAC | `84:FC:E6:66:40:4C` |

> 这块板用的是 ESP32-S3 内置的 USB-Serial-JTAG，不是 CH340/CP2102 串口桥。
> 这点决定了下面编译选项和看串口的方式。

---

## 固件一览

> ⚠️ **ESP32 的 flash 同一时刻只能装一个固件**，烧录新的会覆盖旧的。
> 三份源码都保留在仓库里，按需切换编译烧录即可。

### `esp32s3_wifi_provision/` — WiFi 配网（主固件 / 当前烧录）

通电后的行为：

1. 读 NVS 里有没有存过的 WiFi 密码；有就直接连，连上即正常工作
2. 没密码 / 连不上 → 开放热点 **`ESP32-Setup`**（无密码）
3. DNS 劫持，让手机连上后自动弹出配网页（Captive Portal）
4. 配网页（中文）列出**附近 WiFi** → 选网络 + 输密码
5. 异步连接，网页轮询显示「连接中 / 成功 / 失败」
6. 连接成功 → 密码存进 NVS（掉电不丢）→ 关热点切 STA 正常工作
7. 串口每 5 秒打印一次 `[STA] connected=1 ip=... rssi=...`

### `esp32s3_wifi_self_test/` — 纯 WiFi 扫描自检

开机打印芯片信息，之后每 8 秒扫描一次周边 WiFi 并打印（SSID / 信号 / 信道 / 加密），
每秒一个心跳。用来快速验证 WiFi 接收是否正常、以及串口通道是否通。

### `esp32s3_radio_self_test/` — BLE + WiFi 射频自检

设计上做 BLE 广播（设备名 `ESP32S3-Radio-Test`）+ WiFi 扫描。
> 注：BLE 部分尚未在本板实测确认（验证 WiFi 时未深入 BLE），仅保留源码。

---

## 开发环境

- `arduino-cli` + `esp32:esp32` core **3.3.10**
- arduino-cli 数据放在**项目本地目录**（已 `.gitignore`）：
  `.arduino-data/`、`.arduino-downloads/`、`.arduino-user/`
- 第三方库（圆屏等）在 `.arduino-user/libraries/`：`Adafruit_BusIO`、`Adafruit_GFX`、`GC9A01A_AVR`
- 配网固件只用 core 自带库（`WiFi` / `DNSServer` / `WebServer` / `Preferences`），无第三方依赖

---

## 编译 & 烧录

### 0. 环境变量 + 关键参数

```bash
cd /Users/jiangzhibin/Documents/ardiuno

# 让 arduino-cli 用项目本地目录
export ARDUINO_DIRECTORIES_DATA="$PWD/.arduino-data"
export ARDUINO_DIRECTORIES_DOWNLOADS="$PWD/.arduino-downloads"
export ARDUINO_DIRECTORIES_USER="$PWD/.arduino-user"

FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc"
PORT="/dev/cu.usbmodem101"
SKETCH="esp32s3_wifi_provision"   # 要烧的固件目录名
```

> 🔑 **`CDCOnBoot=cdc` 必须加**！这块板是原生 USB-Serial-JTAG，不加这个选项的话
> `Serial.print` 会走物理 UART0 引脚（GPIO43/44），从 USB 口**完全看不到输出**。

### 1. 编译（务必指定 `--output-dir`）

```bash
arduino-cli compile --fqbn "$FQBN" --output-dir "build/$SKETCH" "$SKETCH"
```

### 2. 烧录（务必用 `--input-dir` 指向上一步的产物）

```bash
arduino-cli upload -p "$PORT" --fqbn "$FQBN" --input-dir "build/$SKETCH" "$SKETCH"
```

> 🔑 **必须用 `--output-dir` / `--input-dir` 显式指定产物目录**。
> 否则 `upload` 会去系统默认缓存目录找编译产物，经常找不到，
> 报错 `Compiled sketch not found in ...` 且 exit code = 1（烧录其实失败了）。

### 3. 确认是否真的烧进去了

烧录命令的文字输出**不一定可信**，要用真实信号确认：

- `upload` 命令的 **exit code 是否为 0**（`echo $?`）
- esptool 是否打印了真实的 `Chip type: ESP32-S3 ...` + `Wrote N bytes` + `Hash of data verified`
- **读串口看 uptime 是否归零**：如果还在涨着旧的 `uptime=`，说明新固件没生效

---

## 看串口输出

```bash
arduino-cli monitor -p /dev/cu.usbmodem101 -c baudrate=115200
```

> ⚠️ **HWCDC 的坑**：ESP32-S3 的 USB-Serial-JTAG 在「主机未建立连接」时会把 `Serial`
> 输出直接丢弃。如果 monitor 一行都收不到，试：
> 1. 按一下板上 **RST** 键让设备重启；
> 2. 或拔插一次 USB 重新枚举；
> 3. 串口工具打开时确保 **拉高 DTR**（部分工具默认没拉）。

---

## 配网使用方法（最终效果）

1. 给板子通电（首次，或还没存过 WiFi 密码时）
2. 手机 → 设置 → WiFi，连接热点 **`ESP32-Setup`**（开放，无密码）
3. 连上后等几秒：
   - iPhone 通常**自动弹出**配网页
   - 安卓在通知栏点「登录到此网络」；若提示「无法上网是否保持」选**保持**
   - 没弹就用浏览器打开 **`192.168.4.1`**
4. 配网页下拉选你的 WiFi → 输入密码 → 点「连接」
5. 显示 **✅ 已连接 + 设备 IP** 即成功；密码已保存，热点随后关闭（手机会自动断开 `ESP32-Setup`，正常）

---

## 换 WiFi / 重新配网

当前固件一旦存了密码，下次通电就直接连旧 WiFi、**不再开配网热点**。
要重新配网（比如换了路由器 / 换地方），需要先清掉 NVS 里存的密码：

- **彻底方式**：用 esptool 全片擦除后重烧固件（会清掉所有数据，包括已存密码）
- **推荐方式**：给固件加一个「重置」入口（见下方 TODO），比如长按某个按键清除密码并重新进配网

---

## 已知限制 / TODO

- [ ] **无重置入口**：换 WiFi 需要清 NVS 或重烧（建议加「长按按键清除密码重新配网」）
- [ ] **配网页 SSID 未做 HTML 转义**：含 `"` `<` 等特殊字符的 SSID 可能显示异常（代码内已标 `TODO(escape)`）
- [ ] **连接失败原因未细分**：密码错 / 信号弱 / 超时都只显示「连接失败」
- [ ] **BLE 未实测**：`esp32s3_radio_self_test` 的 BLE 广播部分尚未在本板验证
- [ ] **可选增强**：配网状态显示到圆屏（仓库已有 `GC9A01A` 驱动），如「配网中 / 已连接 / IP」

---

## 当前设备状态（最近一次验证）

- 烧录固件：`esp32s3_wifi_provision`
- 已配网连上家里 WiFi，STA 模式正常运行（示例 IP `192.168.3.219`，信号 `rssi≈-28`）
- 已通过手机完整跑通配网流程
