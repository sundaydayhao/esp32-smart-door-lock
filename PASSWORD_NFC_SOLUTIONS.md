# Smart Door Lock 密码识别与 NFC 刷卡扩展方案

## 1. 目标

在现有 `smart-door-lock` 项目基础上补充两类认证能力：

- **密码识别 / PIN 解锁**
- **NFC 刷卡解锁**

并给出两种硬件落地路线：

1. **方案 A：基于 ESP32-CAM 开发板做扩展**
2. **方案 B：从头自研开发板**

---

## 2. 当前项目现状

结合当前工程代码，可以确认：

### 已经具备的能力
- 人脸识别：`main/face_recognition/`
- 指纹识别：`main/fingerprint/`
- 门锁控制：`main/door_lock/`
- 用户管理：`main/user_management/`
- Web 管理：`main/http_server/`
- **矩阵键盘密码输入基础能力：`main/keypad/`**
- `app_main.c` 中已经存在：
  - 密码输入缓存
  - `#` 提交、`*` 清空逻辑
  - 通过 `CONFIG_SDL_KEYPAD_PASSWORD` 校验 PIN
  - 密码成功/失败事件分发
  - 密码成功后开锁

### 当前仍缺少的能力
- README 中没有完整说明密码识别功能
- 当前 PIN 为**单一系统密码**，不是“每个用户独立 PIN”
- **没有 NFC 模块**
- 用户结构中**没有卡 UID / 卡号字段**
- Web API 还没有卡片录入、卡片删除、卡片查询接口
- Kconfig 还没有 NFC 配置项

---

## 3. 密码识别功能建议

## 3.1 最小可用实现（当前代码已经基本具备）

当前代码已经支持：

- 4x3 矩阵键盘输入数字
- `*` 清空
- `#` 提交
- 与 `CONFIG_SDL_KEYPAD_PASSWORD` 比较
- 成功后开锁、记录日志、显示屏提示
- 错误后进入失败计数与锁定流程

### 当前默认交互
- 输入数字
- `*`：清空
- `#`：提交验证

### 当前默认密码配置
在 `menuconfig` 中：

- `Smart Door Lock Configuration`
- `Hardware Configuration`
- `Enable Matrix Keypad Password Unlock`
- `Keypad Unlock Password`

---

## 3.2 建议升级为“用户级 PIN”

为了和人脸、指纹、NFC 保持一致，建议将密码从“系统通用密码”升级为“用户级 PIN”：

### 数据结构建议
在 `sdl_user_t` 中增加：

```c
bool     has_pin;
char     pin[16];
```

### 认证逻辑建议
- 键盘输入完成后，不再只比较 `CONFIG_SDL_KEYPAD_PASSWORD`
- 改为遍历用户列表，查找匹配 PIN 的用户
- 命中用户后：
  - 检查用户状态
  - 检查有效期
  - 调用统一 `handle_auth_success(user.id, SDL_AUTH_METHOD_PASSWORD, 100)`

### 优点
- 每个用户可独立 PIN
- 管理员可重置某个用户 PIN
- 访问日志可以绑定具体用户
- 与人脸/指纹/NFC 共用统一认证框架

---

## 4. NFC 刷卡功能设计

## 4.1 推荐模块

优先推荐两类：

### 方案 1：PN532
- 接口：I2C / SPI / UART
- 优点：
  - 资料多
  - 兼容 Mifare Classic / NTAG / ISO14443A
  - 社区项目多，易调试
- 缺点：
  - 成本略高
  - 模块体积通常较大

### 方案 2：MFRC522 / WS1850S
- 接口：SPI
- 优点：
  - 便宜
  - 常见
- 缺点：
  - 功能窄于 PN532
  - 更适合 UID 卡识别，不适合复杂 NFC 交互

### 推荐结论
- **原型验证 / 快速开发：PN532**
- **成本敏感量产：MFRC522 或兼容芯片**

---

## 4.2 NFC 在软件中的定位

建议新增模块：

```text
main/nfc/
├── nfc_reader.h
└── nfc_reader.c
```

职责：
- 初始化 NFC 模块
- 轮询卡片靠近事件
- 读取 UID
- 返回卡片 UID 和长度
- 提供注册/删除/比对所需接口

---

## 4.3 用户数据结构扩展建议

在 `sdl_user_t` 中增加：

```c
#define SDL_NFC_UID_MAX_LEN  8

bool     has_nfc_card;
uint8_t  nfc_uid[SDL_NFC_UID_MAX_LEN];
uint8_t  nfc_uid_len;
```

并增加认证方式位：

```c
#define SDL_AUTH_METHOD_NFC   (1 << 3)
```

---

## 4.4 用户管理接口扩展建议

建议新增接口：

```c
esp_err_t user_mgmt_bind_nfc_card(uint8_t user_id,
                                  const uint8_t *uid,
                                  uint8_t uid_len);

esp_err_t user_mgmt_unbind_nfc_card(uint8_t user_id);

esp_err_t user_mgmt_find_by_nfc_uid(const uint8_t *uid,
                                    uint8_t uid_len,
                                    sdl_user_t *user);
```

---

## 4.5 认证流程建议

### NFC 认证流程

```text
检测到卡片
   ↓
读取 UID
   ↓
在用户数据库中查找匹配 UID
   ↓
找到用户？
  ├─ 否 → 记录失败、红灯/蜂鸣器告警
  └─ 是 → 检查状态/有效期
            ↓
         校验通过
            ↓
         开锁 + 记录日志 + 显示用户名
```

### 建议增加的事件
在 `event_manager.h` 中增加：

```c
SDL_AUTH_EVENT_NFC_SUCCESS
SDL_AUTH_EVENT_NFC_FAILED
SDL_AUTH_EVENT_NFC_ENROLL_START
SDL_AUTH_EVENT_NFC_ENROLL_DONE
```

---

## 4.6 NFC 任务模型建议

建议在 `app_main.c` 中增加 `nfc_task`：

- 运行在 Core 0
- 优先级建议 3
- 轮询周期 100~200ms
- 与指纹、密码、公用 `s_auth_mutex`

伪代码：

```c
while (1) {
    if (!nfc_module_ok) {
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
    }

    if (user_mgmt_is_locked_out()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
    }

    if (nfc_reader_poll(&card) == ESP_OK) {
        if (xSemaphoreTake(s_auth_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            sdl_user_t user;
            if (user_mgmt_find_by_nfc_uid(card.uid, card.uid_len, &user) == ESP_OK) {
                handle_auth_success(user.id, SDL_AUTH_METHOD_NFC, 100);
            } else {
                handle_auth_failure(SDL_AUTH_METHOD_NFC);
            }
            xSemaphoreGive(s_auth_mutex);
        }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
}
```

---

## 5. Web 管理接口建议

建议新增 API：

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/users/{id}/pin` | 设置用户 PIN |
| DELETE | `/api/users/{id}/pin` | 清除用户 PIN |
| POST | `/api/users/{id}/nfc/enroll` | 将下一张刷到的卡绑定给用户 |
| DELETE | `/api/users/{id}/nfc` | 删除用户卡片 |
| GET | `/api/users/{id}/credentials` | 查询用户已启用认证方式 |

### 建议返回示例

```json
{
  "id": 3,
  "name": "张三",
  "credentials": {
    "face": true,
    "fingerprint": true,
    "pin": true,
    "nfc": true
  }
}
```

---

## 6. 两种硬件方案

# 方案 A：基于 ESP32-CAM 开发板扩展

## 6.1 适用场景
- 快速原型
- 低成本验证
- 先做功能演示，再决定是否自研板

## 6.2 核心思路
使用 **ESP32-CAM（AI Thinker）** 做主控与摄像头采集核心，再外挂：
- 矩阵键盘
- NFC 模块（优先 PN532 / MFRC522）
- 指纹模块（可选）
- OLED / TFT 显示
- 继电器门锁驱动
- 蜂鸣器与指示灯
- 外部 5V/12V 供电与降压

## 6.3 推荐器件
- 主控：ESP32-CAM
- 摄像头：OV2640（板载）
- NFC：PN532（I2C/SPI）或 MFRC522（SPI）
- 键盘：4x3 薄膜矩阵键盘
- 门锁驱动：MOSFET 或继电器模块
- 电源：12V 输入 + 5V/3.3V 双路降压

## 6.4 优点
- 成本低
- 摄像头板现成
- 开发快
- 适合演示与 PoC

## 6.5 缺点
- GPIO 非常紧张
- 板载下载/调试体验一般
- 电源完整性较弱
- 工业可靠性一般
- 外接模块多，布线复杂

## 6.6 建议接口分配
由于 ESP32-CAM 可用 GPIO 有限，建议：

- 摄像头：使用板载 OV2640
- NFC：优先 SPI
- 键盘：如 GPIO 不足，可改为：
  - I2C 扩展 IO（如 PCF8574）
  - 或电阻分压按键
- 门锁：单独驱动输出
- 蜂鸣器/LED：占用少量 GPIO

## 6.7 ESP32-CAM 扩展方案详细连线

以下接线以 **AI Thinker ESP32-CAM** 为例，目标是实现：

- 板载摄像头做人脸识别
- 外接 **PN532（SPI 模式）** 做 NFC 刷卡
- 外接 **PCF8574 I2C IO 扩展** 扫描 4x3 矩阵键盘
- 外接 **1 路继电器 / MOS 驱动模块** 控制门锁
- 外接蜂鸣器与状态灯
- 使用 **5V 主电源** 供电

### 6.7.1 为什么要这样分配

ESP32-CAM 的问题不是性能不够，而是 **GPIO 很紧张**：

- 大量 GPIO 已被 OV2640 摄像头占用
- GPIO1 / GPIO3 通常还要留给串口下载调试
- 板载闪光灯常挂在 GPIO4
- GPIO12 在上电启动时属于敏感脚
- 部分 GPIO 既能用，但不适合直接挂很多外设

所以建议：
- **NFC 用 SPI**
- **键盘不要直接占 7 根 GPIO，改用 PCF8574 扩展**
- **门锁一定不要直接挂 ESP32-CAM GPIO，必须经过驱动级**
- **供电必须独立规划**

### 6.7.2 推荐连接器件

| 功能 | 推荐器件 | 原因 |
|------|----------|------|
| NFC | PN532 SPI 模块 | 兼容性更好，调试资料多 |
| 键盘扩展 | PCF8574 | 只占 2 根 I2C 线 |
| 门锁驱动 | 5V 继电器模块 / MOS 驱动板 | 隔离单片机与锁体电流 |
| 电源 | 12V 转 5V DC-DC + AMS1117/MPM3610 3.3V | 保证锁、NFC、ESP32 稳定 |
| 蜂鸣器 | 有源蜂鸣器 | 驱动简单 |
| LED | 红/绿 LED + 220Ω 电阻 | 状态提示 |

### 6.7.3 推荐 GPIO 分配表

> 说明：不同 ESP32-CAM 小板丝印可能略有差异，接线前请以你手上板子的引脚丝印和原理图为准。

#### 1）ESP32-CAM 板载摄像头占用
这些脚通常不要再接别的外设：

| 摄像头信号 | ESP32 GPIO |
|-----------|------------|
| PWDN | GPIO32 |
| XCLK | GPIO0 |
| SIOD | GPIO26 |
| SIOC | GPIO27 |
| Y9 | GPIO35 |
| Y8 | GPIO34 |
| Y7 | GPIO39 |
| Y6 | GPIO36 |
| Y5 | GPIO21 |
| Y4 | GPIO19 |
| Y3 | GPIO18 |
| Y2 | GPIO5 |
| VSYNC | GPIO25 |
| HREF | GPIO23 |
| PCLK | GPIO22 |

#### 2）推荐给扩展模块使用的引脚
在常见 AI Thinker 版本上，比较适合扩展使用的通常是：

| 用途 | ESP32-CAM GPIO | 备注 |
|------|----------------|------|
| UART 下载 TX | GPIO1 | 下载/日志 |
| UART 下载 RX | GPIO3 | 下载/日志 |
| SPI SCK | GPIO14 | 可用于 PN532 / MFRC522 |
| SPI MISO | GPIO2 | 可用，但注意启动状态 |
| SPI MOSI | GPIO15 | 可用 |
| NFC CS | GPIO13 | 推荐给 SPI 片选 |
| 门锁控制 | GPIO12 | 可用，但上电状态要注意 |
| I2C SDA | GPIO16 | 某些开发板会引出 |
| I2C SCL | GPIO14 / GPIO16 / GPIO15 需按实际板修正 | 建议最终以板卡引出为准 |
| 状态 LED / 蜂鸣器 | GPIO33 / GPIO4 视板卡而定 | GPIO4 常与闪光灯复用，不优先推荐 |

> 最稳妥的做法：  
> **NFC 用一组 SPI，键盘通过 IO 扩展挂 I2C，门锁单独一个 GPIO 控制。**

### 6.7.4 一套更稳妥的推荐连线

由于很多 ESP32-CAM 小板真正稳定可用的扩展脚并不多，推荐你按下面这套思路来：

#### A. PN532（SPI 模式）接线

| PN532 引脚 | 接到 ESP32-CAM | 说明 |
|-----------|----------------|------|
| VCC | 3.3V | 优先 3.3V 供电 |
| GND | GND | 共地 |
| SCK | GPIO14 | SPI 时钟 |
| MISO | GPIO2 | SPI MISO |
| MOSI | GPIO15 | SPI MOSI |
| SS / CS | GPIO13 | 片选 |
| RSTO / RSTPDN | 可悬空或接 GPIO12 | 可选复位脚 |
| IRQ | 可不接 | 先用轮询方式即可 |

#### B. PCF8574（I2C）+ 4x3 矩阵键盘
如果你的 ESP32-CAM 板没有稳定引出足够 I2C 脚，也可以换成 **PCF8574 + 软件 I2C**。

| PCF8574 引脚 | 接到 ESP32-CAM | 说明 |
|-------------|----------------|------|
| VCC | 3.3V | 建议 3.3V |
| GND | GND | 共地 |
| SDA | GPIO16 | 软件 I2C SDA |
| SCL | GPIO12 或其他空闲脚 | 软件 I2C SCL，需避开门锁控制脚时再调整 |
| A0/A1/A2 | GND | 地址设为 0x20 |
| INT | 可不接 | 先轮询 |
| P0~P6 | 接矩阵键盘行列 | 4 行 + 3 列刚好 7 个 IO |

##### 矩阵键盘到 PCF8574 的建议连接
| 键盘端 | PCF8574 |
|--------|---------|
| R1 | P0 |
| R2 | P1 |
| R3 | P2 |
| R4 | P3 |
| C1 | P4 |
| C2 | P5 |
| C3 | P6 |

P7 可留作按键中断或备用输入。

> 如果 GPIO12 已经拿去做门锁控制，就把 I2C SCL 改到另一根空闲脚。  
> 这一点必须结合你手上的板卡引出针脚实际确认。

#### C. 门锁控制接线
**不要把电锁、继电器线圈直接接 ESP32-CAM。**

推荐接法：

| 门锁驱动模块 | 接到 ESP32-CAM | 说明 |
|-------------|----------------|------|
| IN | GPIO12 | 门锁控制信号 |
| VCC | 5V | 继电器模块一般用 5V |
| GND | GND | 共地 |

门锁执行部分：

##### 如果是继电器模块
- 继电器 `COM` 接锁电源正极输入
- 继电器 `NO` 接电锁正极
- 电锁负极接锁电源负极

##### 如果是 MOS 驱动
- GPIO 控制 MOS 驱动输入
- 电锁由独立 12V 供电
- 电锁两端加反向续流二极管

#### D. 蜂鸣器接线

| 蜂鸣器模块 | 接到 ESP32-CAM | 说明 |
|-----------|----------------|------|
| SIG | GPIO33 或空闲 GPIO | 若板子引出可用 |
| VCC | 5V 或 3.3V | 看模块规格 |
| GND | GND | 共地 |

如果没有多余 GPIO，可以暂时不接蜂鸣器，先做核心门禁功能。

#### E. 状态 LED 接线

| LED | 接到 ESP32-CAM | 说明 |
|-----|----------------|------|
| 绿灯阳极 | GPIO33 串 220Ω | 可用时连接 |
| 红灯阳极 | 另一空闲 GPIO 串 220Ω | 视实际引脚资源而定 |
| 阴极 | GND | 共地 |

如果 GPIO 不够，建议：
- 只保留 1 个状态灯
- 或完全依赖继电器动作 + Web 页面状态提示

### 6.7.5 电源连接建议

#### 推荐电源结构
```text
12V 电源适配器
   ├── 直接给电锁
   └── DC-DC 降压到 5V
         ├── 给 ESP32-CAM 5V
         ├── 给继电器模块 5V
         └── 给 PN532/PCF8574（再视模块决定是否转 3.3V）
```

#### 注意事项
1. **ESP32-CAM 建议从 5V 引脚供电**
   - 不建议直接拿 USB-TTL 模块的小电流 3.3V 长期带整套系统

2. **门锁必须独立供电**
   - 特别是电磁锁/电控锁启动电流很大
   - 不能直接从 ESP32-CAM 板载稳压器取电

3. **所有 GND 必须共地**
   - ESP32-CAM
   - PN532
   - PCF8574
   - 继电器模块
   - 锁电源地（若驱动级要求共地）

4. **锁体属于感性负载**
   - 继电器或 MOS 驱动处要加续流保护
   - 避免开锁瞬间把 ESP32-CAM 拉复位

### 6.7.6 下载与调试接线

ESP32-CAM 没有原生 USB，需要 USB-TTL 模块。

| USB-TTL | ESP32-CAM |
|---------|-----------|
| 5V | 5V |
| GND | GND |
| TX | U0R / GPIO3 |
| RX | U0T / GPIO1 |

进入下载模式通常需要：
- `GPIO0` 拉低
- 按一下 RST
- 烧录完成后再释放 GPIO0，重新复位启动

### 6.7.7 实际落地建议

如果你想**尽快做出来并稳定运行**，推荐优先级如下：

#### 最简可运行版
- ESP32-CAM 板载 OV2640
- PN532（SPI）
- 门锁驱动模块
- 不接矩阵键盘
- 先做人脸 + NFC 两种认证

#### 进阶版
- ESP32-CAM
- PN532（SPI）
- PCF8574 + 4x3 矩阵键盘
- 门锁驱动模块
- 可选 1 个蜂鸣器

#### 不太推荐的组合
- ESP32-CAM + 人脸 + 指纹 + 4x3 键盘直连 + NFC + OLED 全部同时上  
因为 GPIO、供电、结构空间都会非常吃紧，调试成本会明显升高。

## 6.8 对软件架构的影响
- 仍可复用当前工程主体逻辑
- 需要重新映射引脚
- 可能需要关闭部分可选模块：
  - OLED
  - 指纹
  - 全矩阵键盘
- 建议形成“裁剪版配置”

## 6.8 方案 A 总结
这是**最快最便宜的落地方案**，适合：
- Demo
- 课程项目
- 客户演示
- 小批量验证

---

# 方案 B：从头自研开发板

## 7.1 适用场景
- 正式产品
- 需要稳定量产
- 需要更高安全性与外设集成度
- 需要更好的结构与供电设计

## 7.2 主控建议

### 推荐 1：ESP32-S3-WROOM-1-N8R8
- 适合人脸识别
- 有 PSRAM
- USB 方便调试
- 资源比 ESP32-CAM 余量更大

### 推荐 2：ESP32-S3-WROOM-2
- 外接天线场景更合适
- 门口金属环境下无线更容易优化

## 7.3 自研板建议集成模块
- ESP32-S3 模组
- OV2640/GC2145 摄像头座
- PN532 或兼容前端
- 指纹模块接口
- 4x3/4x4 键盘接口
- OLED/TFT 接口
- 继电器/MOS 门锁驱动
- 蜂鸣器、红绿灯
- RS485/韦根（可选）
- 电源输入：
  - 12V
  - Type-C 5V 调试
- 后备电池/超级电容（可选）
- 防拆开关（可选）

## 7.4 自研板推荐系统框图

```text
          ┌────────────────────┐
          │   12V / 5V 输入     │
          └─────────┬──────────┘
                    │
             ┌──────▼──────┐
             │ 电源管理 PMU │
             │ 5V / 3.3V    │
             └──────┬──────┘
                    │
        ┌───────────▼────────────────┐
        │       ESP32-S3 主控         │
        │ Face / PIN / NFC / WiFi     │
        └──────┬──────┬──────┬───────┘
               │      │      │
        ┌──────▼┐ ┌──▼──┐ ┌─▼─────┐
        │摄像头 │ │NFC  │ │指纹模块│
        └───────┘ └─────┘ └───────┘
               │      │      │
        ┌──────▼──────▼──────▼──────┐
        │ 显示 / 键盘 / 蜂鸣器 / LED │
        └──────────────┬────────────┘
                       │
                 ┌─────▼─────┐
                 │ 门锁驱动级 │
                 └───────────┘
```

## 7.5 自研板优点
- GPIO 资源充足
- 接口定义合理
- 抗干扰与供电更可靠
- 可以加入 ESD / TVS / 反接保护
- 更适合门禁产品化

## 7.6 自研板缺点
- 硬件设计成本高
- 打样周期长
- 需要调试射频、摄像头、供电和 EMC
- 软件适配工作量更大

## 7.7 量产建议
正式产品建议采用：
- **ESP32-S3 + OV2640 + PN532 + 独立门锁驱动**
- 预留：
  - 调试串口
  - USB
  - 指纹扩展接口
  - 防拆输入
  - 门磁输入
  - 出门按钮输入

## 7.8 方案 B 总结
这是**长期可维护的产品级路线**，适合：
- 商业门禁
- 社区门口机
- 办公室智能门锁
- 需要持续迭代的硬件平台

---

## 8. 两种方案对比

| 项目 | 方案 A：ESP32-CAM 扩展 | 方案 B：自研主板 |
|------|-------------------------|------------------|
| 初始成本 | 低 | 高 |
| 开发速度 | 快 | 中 |
| 硬件复杂度 | 低 | 高 |
| GPIO 余量 | 少 | 多 |
| 可靠性 | 一般 | 高 |
| 外观与结构 | 一般 | 可定制 |
| 量产适合度 | 低~中 | 高 |
| 适合阶段 | 原型/演示 | 正式产品 |

---

## 9. 实施优先级建议

建议按以下阶段推进：

### 阶段 1：先完成软件统一认证框架
- 保留现有人脸、指纹、密码
- 将密码升级为用户级 PIN
- 补全访问日志中的认证方式

### 阶段 2：新增 NFC 模块
- 增加 `main/nfc/`
- 增加 Kconfig 配置项
- 扩展 `sdl_user_t`
- 增加 NFC 查找接口
- 增加 `nfc_task`

### 阶段 3：补全 Web 管理
- 用户 PIN 设置
- NFC 绑定/解绑
- 认证方式显示

### 阶段 4：根据硬件方向选路
- 先做 Demo：走方案 A
- 需要量产：走方案 B

---

## 10. 推荐结论

### 如果你要“尽快做出能演示的门禁”
建议：
- **ESP32-CAM 扩展方案**
- 保留人脸
- 增加矩阵键盘 PIN
- 增加 PN532/MFRC522 刷卡
- 快速完成联调演示

### 如果你要“做成正式产品”
建议：
- **ESP32-S3 自研主板方案**
- 软件上统一支持：
  - 人脸
  - 指纹
  - PIN
  - NFC
- 硬件上统一考虑：
  - 电源
  - ESD
  - 锁驱动
  - 防拆
  - 扩展接口

---

## 11. 建议的下一步代码改造清单

建议后续在工程中落实以下改动：

1. 在 `user_management.h` 中增加：
   - `SDL_AUTH_METHOD_NFC`
   - 用户 PIN / NFC UID 字段
   - NFC 查找接口

2. 在 `event_manager.h` 中增加：
   - NFC 成功/失败/录卡事件

3. 在 `Kconfig.projbuild` 中增加：
   - `SDL_NFC_ENABLE`
   - NFC 总线接口与引脚
   - PN532 / MFRC522 选择项

4. 新增模块：
   - `main/nfc/nfc_reader.h`
   - `main/nfc/nfc_reader.c`

5. 在 `app_main.c` 中增加：
   - `nfc_module_ok`
   - `nfc_task`
   - NFC 认证处理逻辑

6. 在 Web API 中增加：
   - PIN 设置/删除
   - NFC 绑卡/解绑

---

## 12. 结论

当前工程已经有**密码识别的基础实现**，因此密码功能不是从零开始，主要需要：
- 升级为用户级 PIN
- 在 README / Web 管理中正式纳入

而 **NFC 刷卡功能目前仍需新增模块与数据结构支持**。  
如果追求快速验证，优先采用 **ESP32-CAM 扩展方案**；如果目标是稳定量产，则应采用 **ESP32-S3 自研主板方案**。