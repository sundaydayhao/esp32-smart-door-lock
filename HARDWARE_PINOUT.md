# ESP32-S3 自研门锁主板引脚分配

本项目已删除现成摄像头开发板扩展方案，默认按自研 ESP32-S3 主板设计。推荐主控为 ESP32-S3-WROOM-1-N8R8 或同等级带 PSRAM 模组，OV2640 通过 DVP 并口直连，OLED 与 PN532 共用 I2C0。

## 默认引脚表

### OV2640 摄像头

| OV2640 信号 | ESP32-S3 GPIO | 说明 |
|---|---:|---|
| PWDN | -1 | 默认不接，由硬件上拉/下拉固定 |
| RESET | -1 | 默认不接 |
| XCLK | GPIO15 | 20 MHz 输出 |
| SIOD | GPIO4 | SCCB SDA |
| SIOC | GPIO5 | SCCB SCL |
| D7 | GPIO16 | 数据线 |
| D6 | GPIO17 | 数据线 |
| D5 | GPIO18 | 数据线 |
| D4 | GPIO12 | 数据线 |
| D3 | GPIO10 | 数据线 |
| D2 | GPIO8 | 数据线 |
| D1 | GPIO9 | 数据线 |
| D0 | GPIO11 | 数据线 |
| VSYNC | GPIO6 | 场同步 |
| HREF | GPIO7 | 行同步 |
| PCLK | GPIO13 | 像素时钟 |

### I2C0 总线：OLED + PN532

| 设备/信号 | ESP32-S3 GPIO | 说明 |
|---|---:|---|
| I2C SDA | GPIO1 | OLED SSD1306 与 PN532 共用 |
| I2C SCL | GPIO2 | OLED SSD1306 与 PN532 共用 |
| OLED 地址 | 0x3C | SSD1306 默认地址 |
| PN532 地址 | 0x24 | `CONFIG_SDL_NFC_I2C_ADDR=36` |
| PN532 RSTPDN | GPIO21 | 可设为 -1 不接 |
| PN532 IRQ | GPIO14 | 可选，当前驱动以 I2C 状态轮询为主 |

### 3x4 矩阵键盘

| 键盘信号 | ESP32-S3 GPIO | 说明 |
|---|---:|---|
| R1 | GPIO33 | 行输出 |
| R2 | GPIO34 | 行输出 |
| R3 | GPIO35 | 行输出 |
| R4 | GPIO36 | 行输出 |
| C1 | GPIO37 | 列输入，内部上拉 |
| C2 | GPIO38 | 列输入，内部上拉 |
| C3 | GPIO39 | 列输入，内部上拉 |

### AS608 指纹模块

| AS608 信号 | ESP32-S3 GPIO | 说明 |
|---|---:|---|
| RX | GPIO43 | ESP32-S3 UART1 TX 接模块 RX |
| TX | GPIO44 | ESP32-S3 UART1 RX 接模块 TX |
| VCC | 3.3V/5V | 按模块规格供电，信号电平建议 3.3V |
| GND | GND | 共地 |

### 门锁、LED、蜂鸣器

| 功能 | ESP32-S3 GPIO | 默认电平/说明 |
|---|---:|---|
| 门锁继电器 IN | GPIO40 | 默认 `HIGH=开锁` |
| 绿色 LED | GPIO41 | 开锁/通过状态 |
| 红色 LED | GPIO42 | 拒绝/锁定状态 |
| 有源蜂鸣器 | GPIO48 | LEDC PWM，失败/成功提示 |

## 引脚规划原则

- GPIO19/GPIO20 预留给 USB D-/D+，方便使用 USB Serial/JTAG 下载和调试。
- GPIO0、GPIO3、GPIO45、GPIO46 是启动相关敏感脚，默认不接键盘、继电器或外设中断。
- GPIO26-GPIO32 默认避开，避免与模组内部 Flash/PSRAM 连接冲突。
- I2C0 由 OLED 与 PN532 共用，板上需要 4.7k 上拉到 3.3V。
- 门锁继电器、电控锁或电磁锁必须由独立驱动级控制，不要由 GPIO 直接驱动线圈。

## 供电建议

推荐 12V 输入：

```text
12V 电源
  ├── 门锁/电磁锁供电
  └── DC-DC 降压到 5V
        ├── 继电器模块
        └── 3.3V LDO/DC-DC
              ├── ESP32-S3
              ├── OV2640
              ├── PN532
              └── OLED
```

锁体属于感性负载，继电器线圈和锁体驱动侧要做续流/浪涌保护；所有模块需要共地。

## menuconfig 对应项

所有默认引脚都可以在以下菜单修改：

```text
idf.py menuconfig
  Smart Door Lock Configuration
    Hardware Configuration
```

NFC 相关配置：

- `SDL_NFC_ENABLE`
- `SDL_NFC_I2C_PORT`
- `SDL_NFC_SDA_PIN`
- `SDL_NFC_SCL_PIN`
- `SDL_NFC_I2C_ADDR`
- `SDL_NFC_RST_GPIO`
- `SDL_NFC_IRQ_GPIO`
- `SDL_NFC_POLL_INTERVAL_MS`
- `SDL_NFC_CARD_COOLDOWN_MS`
