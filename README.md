# 智能门禁系统 (Smart Door Lock System)

基于 **ESP32-S3** 的智能门禁系统，集成人脸识别、指纹识别、WiFi 远程管理，使用 **ESP-IDF** 框架开发。

---

## 功能特性

| 功能 | 说明 |
|------|------|
| 🎭 人脸识别 | OV2640 摄像头 + 特征向量匹配，支持最多 10 个用户 |
| 👆 指纹识别 | AS608/R307/R503 指纹传感器，UART 通信，支持 127 个模板 |
| 🔢 密码解锁 | 4x3 矩阵键盘输入 PIN，支持 `*` 清空、`#` 提交 |
| 📶 NFC 刷卡 | 预留 PN532 / MFRC522 扩展方案，适合门禁卡解锁 |
| 🔒 门锁控制 | GPIO 控制继电器/电磁锁，自动上锁计时器 |
| 📺 OLED 显示 | SSD1306 128×64 I2C 显示屏，实时状态显示 |
| 📶 WiFi 管理 | 支持 AP 模式（热点）和 STA 模式（连接路由器） |
| 🌐 Web 管理界面 | 内置 HTTP 服务器，RESTful API，响应式 Web UI |
| 💾 用户管理 | NVS 持久化存储，支持管理员/用户/访客角色 |
| 🔐 安全防护 | 连续失败锁定、访问日志记录 |
| 🔔 声光提示 | 蜂鸣器 + 绿/红 LED 指示灯 |

--- 

## 密码识别与 NFC 扩展方案

项目已具备**矩阵键盘密码解锁基础实现**，并已整理两种 NFC/密码扩展落地路线：

- **方案 A：使用 ESP32-CAM 开发板做扩展**
- **方案 B：从头自研 ESP32-S3 开发板**

详见独立方案文档：[`PASSWORD_NFC_SOLUTIONS.md`](./PASSWORD_NFC_SOLUTIONS.md)

> 说明：当前代码中密码解锁已在 `app_main.c + keypad/` 中接入；NFC 刷卡功能目前仍属于待扩展模块，建议按方案文档逐步接入 `main/nfc/`、用户卡号存储、事件和 Web API。

## 硬件需求

### 主控芯片
- **ESP32-S3** (推荐带 PSRAM，如 ESP32-S3-WROOM-1-N8R8)

### 外设清单

| 模块 | 型号 | 接口 |
|------|------|------|
| 摄像头 | OV2640 (QVGA) | DVP 并口 |
| 指纹传感器 | AS608 / R307 / R503 | UART |
| OLED 显示屏 | SSD1306 128×64 | I2C |
| 门锁 | 电磁锁 / 电控锁 + 继电器 | GPIO |
| 蜂鸣器 | 有源蜂鸣器 | GPIO (PWM) |
| 指示灯 | 绿色 LED + 红色 LED | GPIO |

### 默认引脚配置 (ESP32-S3)

```
摄像头 (OV2640):
  PWDN  → -1 (不使用)    RESET → -1 (不使用)
  XCLK  → GPIO15         SIOD  → GPIO4
  SIOC  → GPIO5          D7    → GPIO16
  D6    → GPIO17         D5    → GPIO18
  D4    → GPIO12         D3    → GPIO10
  D2    → GPIO8          D1    → GPIO9
  D0    → GPIO11         VSYNC → GPIO6
  HREF  → GPIO7          PCLK  → GPIO13

指纹传感器 (AS608):
  TX → GPIO43,  RX → GPIO44,  VCC → 3.3V

OLED 显示屏 (SSD1306):
  SDA → GPIO1,  SCL → GPIO2,  VCC → 3.3V

门锁控制:  继电器 → GPIO38 (HIGH=开锁)
蜂鸣器:    GPIO39
绿色LED:   GPIO40
红色LED:   GPIO41
```

> 所有引脚均可通过 `idf.py menuconfig` → **Smart Door Lock Configuration** 修改。

---

## 软件架构

```
smart-door-lock/
├── CMakeLists.txt                    # 项目 CMake 配置
├── sdkconfig.defaults                # 默认 SDK 配置 (ESP32-S3, PSRAM)
├── partitions.csv                    # 自定义分区表 (app 4MB + nvs 512KB)
├── main/
│   ├── CMakeLists.txt                # 主组件 CMake
│   ├── Kconfig.projbuild             # menuconfig 配置项
│   ├── idf_component.yml             # 组件依赖声明
│   ├── app_main.c                    # 主入口，FreeRTOS 任务管理
│   ├── app_main.h
│   ├── event_manager/
│   │   ├── event_manager.h           # 事件系统接口
│   │   └── event_manager.c           # ESP 事件循环封装
│   ├── face_recognition/
│   │   ├── face_recognition.h        # 人脸识别接口
│   │   └── face_recognition.c        # OV2640 + 特征提取 + 匹配
│   ├── fingerprint/
│   │   ├── fingerprint.h             # 指纹传感器接口
│   │   └── fingerprint.c             # AS608 UART 协议驱动
│   ├── door_lock/
│   │   ├── door_lock.h               # 门锁控制接口
│   │   └── door_lock.c               # GPIO + 继电器 + 蜂鸣器 + LED
│   ├── user_management/
│   │   ├── user_management.h         # 用户管理接口
│   │   └── user_management.c         # NVS 持久化存储
│   ├── display/
│   │   ├── display.h                 # 显示模块接口
│   │   └── display.c                 # SSD1306 I2C 驱动 + UI
│   ├── wifi_manager/
│   │   ├── wifi_manager.h            # WiFi 管理接口
│   │   └── wifi_manager.c            # AP/STA 模式管理
│   └── http_server/
│       ├── http_server.h             # HTTP 服务器接口
│       └── http_server.c             # RESTful API + Web UI
```

### FreeRTOS 任务架构

```
Core 0:                          Core 1:
┌─────────────────┐              ┌─────────────────────┐
│  finger_task    │              │  face_task          │
│  (Priority 4)   │              │  (Priority 5)       │
│  AS608 轮询     │              │  OV2640 摄像头      │
│  指纹验证/录入  │              │  人脸检测+识别      │
├─────────────────┤              └─────────────────────┘
│  display_task   │
│  (Priority 2)   │
│  OLED 更新      │
├─────────────────┤
│  WiFi/HTTP      │
│  (IDF 内部)     │
├─────────────────┤
│  app_main       │
│  (Priority 1)   │
│  监控/看门狗    │
└─────────────────┘
```

---

## 快速开始

### 1. 环境准备

```bash
# 安装 ESP-IDF v5.x
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
source export.sh
```

### 2. 克隆项目

```bash
git clone https://gitee.com/zhanghaogit/smart-door-lock.git
cd smart-door-lock
```

### 3. 配置项目

```bash
idf.py menuconfig
```

在 **Smart Door Lock Configuration** 中配置：
- WiFi SSID 和密码
- 摄像头引脚
- 指纹传感器 UART 引脚
- 门锁 GPIO
- 人脸识别阈值

### 4. 编译烧录

```bash
# 设置目标芯片
idf.py set-target esp32s3

# 编译
idf.py build

# 烧录 (替换 /dev/ttyUSB0 为实际串口)
idf.py -p /dev/ttyUSB0 flash monitor
```

### 5. 首次使用

系统启动后：
1. **连接 WiFi**：系统以 AP 模式启动，SSID 为 `SmartDoorLock`，密码 `12345678`
2. **打开 Web 界面**：浏览器访问 `http://192.168.4.1`
3. **添加用户**：在 Web 界面点击"添加用户"，输入姓名
4. **录入人脸**：通过 Web API 触发人脸录入流程
5. **录入指纹**：将手指放在传感器上，按提示完成录入

---

## Web API 参考

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | Web 管理界面 |
| GET | `/api/status` | 系统状态 (JSON) |
| GET | `/api/users` | 用户列表 |
| POST | `/api/users` | 添加用户 `{"name":"张三","role":1}` |
| DELETE | `/api/users/{id}` | 删除用户 |
| POST | `/api/door/unlock` | 远程开锁 |
| POST | `/api/door/lock` | 远程上锁 |
| GET | `/api/logs` | 访问日志 |
| POST | `/api/factory-reset` | 恢复出厂设置 |

### 示例：添加用户

```bash
curl -X POST http://192.168.4.1/api/users \
  -H "Content-Type: application/json" \
  -d '{"name":"张三","role":1}'
```

### 示例：远程开锁

```bash
curl -X POST http://192.168.4.1/api/door/unlock
```

---

## 认证流程

```
用户靠近门禁
     │
     ├─── 摄像头检测到人脸 ──→ 特征提取 ──→ 数据库匹配
     │                                          │
     └─── 手指触碰传感器 ──→ 指纹采集 ──→ 模板匹配
                                               │
                              ┌────────────────┴────────────────┐
                              │ 匹配成功                         │ 匹配失败
                              ▼                                  ▼
                         检查用户状态                      记录失败次数
                              │                                  │
                         检查访问权限                    失败次数 >= 阈值?
                              │                                  │
                         解锁门 + 绿灯                    是 → 系统锁定
                         + 显示欢迎                       否 → 红灯提示
                         + 记录日志
```

---

## 安全特性

- **连续失败锁定**：默认 5 次失败后锁定 5 分钟（可配置）
- **用户角色**：管理员 / 普通用户 / 访客
- **访问日志**：记录最近 50 条访问记录（时间、用户、方式、结果）
- **访问有效期**：可为访客设置访问过期时间
- **人脸阈值**：可调节识别严格程度（默认 0.75）

---

## 人脸识别说明

当前实现使用**像素统计特征**（8×8 网格均值+方差）作为人脸特征向量，通过余弦相似度匹配。

**生产环境建议**：集成 [ESP-WHO](https://github.com/espressif/esp-who) 框架，使用：
- **MTMN** 模型进行人脸检测
- **MobileFaceNet** 模型进行人脸特征提取

替换 `face_recognition.c` 中的 `detect_face()` 和 `extract_features()` 函数即可。

---

## 指纹传感器协议

支持 **光学指纹传感器通用协议**（AS608/R307/R503 兼容）：
- 波特率：57600 bps
- 通信：UART，自定义帧格式
- 功能：录入、验证、搜索、删除、清空

---

## 常见问题

**Q: 摄像头初始化失败？**
A: 检查 PSRAM 是否启用（`CONFIG_SPIRAM=y`），确认引脚配置正确。

**Q: 指纹传感器无响应？**
A: 检查 UART 引脚和波特率，确认传感器供电（3.3V 或 5V）。

**Q: Web 界面无法访问？**
A: 确认 WiFi 连接成功，查看串口日志中的 IP 地址。

**Q: 人脸识别误识率高？**
A: 提高识别阈值（menuconfig 中调整），或集成 ESP-WHO 的 MobileFaceNet 模型。

---

## 许可证

MIT License - 详见 [LICENSE](LICENSE) 文件。