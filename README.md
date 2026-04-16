# 净水器主控板程序

基于ESP32-C3的净水器智能控制系统，支持多种RO膜规格、增压泵和压力桶配置。

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5.2-blue)](https://github.com/espressif/esp-idf)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

## 目录

- [功能特性](#功能特性)
- [硬件规格](#硬件规格)
- [快速开始](#快速开始)
- [状态机](#状态机)
- [Web控制界面](#web控制界面)
- [Home Assistant集成](#home-assistant集成)
- [API接口文档](#api接口文档)
- [项目结构](#项目结构)
- [配置参数](#配置参数)
- [故障排除](#故障排除)

---
## 功能特性

### 核心功能
- **智能制水控制**: 基于压力桶压力开关自动启停制水，无需人工干预
- **RO膜保护**: 进水低压保护、制水超时保护，防止RO膜和增压泵损坏
- **双阶段冲洗**: 压力桶水满后自动执行常规冲洗(30s)+纯水洗膜(20s)，延长RO膜寿命
- **水锤效应控制**: 启停泵与阀门间配置延时，防止水锤冲击损伤管路
- **双TDS监测**: 进水/出水TDS实时监测，自动计算去除率
- **TDS校准**: 支持网页端TDS传感器一点校准
- **漏水检测**: 实时漏水监测，检测到漏水立即停机报警
- **五级滤芯管理**: 水量+时间双维度寿命监测，先到为准

### 智能功能
- **WiFi连接**: 支持STA/AP双模式，无配置时自动进入AP热点配网
- **mDNS域名**: 支持 `http://waterpurifier.local` 域名访问
- **MQTT集成**: 支持Home Assistant自动发现，实时状态推送
- **双级Web界面**: 首页（只读监控）+ 管理页（配置控制），30秒自动刷新
- **配置持久化**: NVS非易失存储，断电不丢失
- **运行数据持久化**: 制水/冲洗次数、停止记录、维护时间自动保存

### 可配置硬件
- **RO膜通量**: 50G / 75G / 100G / 200G / 400G（自动匹配制水速率）
- **增压泵**: 三角洲 50G~400G（70psi，不同流量规格）
- **压力桶**: 3G / 3.2G / 4G / 6G / 10G

---

## 硬件规格

### 开发板配置

| 参数 | 规格 |
|------|------|
| 芯片 | ESP32-C3（RISC-V单核，160MHz） |
| Flash | 4MB |
| RAM | 400KB SRAM |
| 无线 | 802.11 b/g/n WiFi + BLE 5.0 |
| 工作电压 | 24V（设备供电）/ 5V（降压模块给ESP32供电） |

### GPIO引脚分配

#### 输入设备

| 设备 | GPIO | 说明 |
|------|------|------|
| 进水TDS | GPIO2 (ADC1_CH2) | 原水TDS测量，0~2.3V -> 0~1000ppm |
| 出水TDS | GPIO3 (ADC1_CH3) | 纯水TDS测量，0~2.3V -> 0~1000ppm |
| 进水低压开关 | GPIO6 | 低电平=有水，高电平=缺水 |
| 压力桶压力开关 | GPIO7 | 闭合=需要制水，断开=水满 |
| 漏水检测 | GPIO10 | 低电平=漏水报警 |

> **注意**: ESP32-C3的ADC1只有通道0-4（GPIO0~GPIO4），GPIO5使用ADC2，WiFi运行时不可用。

#### 输出设备（24V继电器控制，默认低电平触发）

| 设备 | GPIO | 说明 |
|------|------|------|
| 进水电磁阀 | GPIO0 | 控制原水进入RO系统 |
| 废水电磁阀 | GPIO1 | 控制废水排放 |
| 回水电磁阀 | GPIO4 | 控制纯水回流冲洗RO膜 |
| 增压泵 | GPIO5 | 提供RO膜工作压力（70psi） |

#### 状态指示

| 设备 | GPIO | 说明 |
|------|------|------|
| LED D4 | GPIO12 | 主状态指示灯 |
| LED D5 | GPIO13 | 次状态指示灯 |

### LED指示说明

| 系统状态 | LED D4 | LED D5 | 说明 |
|----------|--------|--------|------|
| 待机 | 灭 | 灭 | 系统空闲，水满等待 |
| 制水中 | 慢闪（1Hz） | 灭 | 正在制水 |
| 水满 | 常亮 | 灭 | 压力桶已满，准备冲洗（瞬时过渡） |
| 冲洗中 | 交替闪烁 | 交替闪烁 | RO膜冲洗 |
| 缺水 | 快闪（5Hz） | 灭 | 进水压力不足 |
| 漏水报警 | 快闪（5Hz） | 快闪（5Hz） | 检测到漏水，已停机 |
| 停止 | 快闪（5Hz） | 灭 | 系统停止（如制水超时/网页停止） |

---

## 快速开始

### 环境要求

- ESP-IDF v5.5.2
- Python 3.8+
- CMake 3.16+

### 编译步骤

#### Windows

```batch
REM 打开ESP-IDF命令提示符
ESP-IDF 5.5.2 CMD

REM 进入项目目录
cd /d D:\Projects\esp32\WaterPurifier

REM 编译项目
idf.py build

REM 烧录（替换COM端口）
idf.py -p COM3 flash

REM 监控输出
idf.py -p COM3 monitor
```

也可以使用项目中的批处理脚本：
- `do_build.bat` - 编译项目
- `flash_monitor.bat` - 烧录并监控
- `rebuild.bat` - 清理后重新编译

### 首次配置

1. **设备启动后自动进入AP配置模式**（无保存配置时）
   - AP名称: `WaterPurifier-XXXX`（XXXX为MAC地址后4位）
   - 密码: `12345678`

2. **连接设备WiFi，打开浏览器访问**: `http://192.168.4.1`

3. **在"WiFi配置"区域**:
   - 点击"扫描网络"
   - 选择你的WiFi网络
   - 输入密码并保存

4. **设备自动连接到你的WiFi**

5. **访问方式**:
   - IP地址: `http://<设备IP>`
   - 域名访问: `http://waterpurifier.local`（需要mDNS支持）

---

## 状态机

系统采用有限状态机（FSM）架构，由独立FreeRTOS任务运行（优先级5，栈4096字节），每100ms检查一次输入状态并执行当前状态逻辑。

### 状态定义

```
┌─────────────────────────────────────────────────────────────┐
│                        状态机状态                              │
├──────────────────┬──────────────────────────────────────────┤
│ FSM_STATE_STANDBY       │ 待机状态 - 系统空闲，等待制水条件      │
│ FSM_STATE_PRODUCTION    │ 制水状态 - 增压泵运行，正在制水        │
│ FSM_STATE_TANK_FULL     │ 水满状态 - 压力桶已满，准备冲洗        │
│ FSM_STATE_NORMAL_FLUSH  │ 常规冲洗 - 废水回流冲洗RO膜(30s)       │
│ FSM_STATE_PURE_FLUSH    │ 纯水洗膜 - 纯水回流冲洗RO膜(20s)       │
│ FSM_STATE_WATER_SHORTAGE│ 缺水状态 - 进水压力不足，等待恢复     │
│ FSM_STATE_LEAK_ALARM    │ 漏水报警 - 检测到漏水，紧急停机        │
│ FSM_STATE_STOP          │ 停止状态 - 系统停止（如制水超时）      │
└──────────────────┴──────────────────────────────────────────┘
```

### 状态转换图

```
                    ┌───────────────────────────────────────────────────┐
                    │                                                   │
                    ▼                                                   │
    ┌─────────┐  需要制水   ┌────────────┐                              │
    │ STANDBY │───────────>│ PRODUCTION │                              │
    │  待机   │<───────────│   制水     │                              │
    └────┬────┘ 手动复位   └─────┬──────┘                              │
         ▲                       │                                     │
         │     ┌─────────────────┤                                     │
         │     │ 缺水            │ 缺水                                │
         │     ▼                 ▼                                     │
         │ ┌───────────┐  ┌────────────┐  水满  ┌────────────┐       │
         │ │ WATER_    │  │ TANK_FULL  │──────>│ NORMAL_FLUSH │       │
         │ │ SHORTAGE  │  │ 水满(瞬时) │       │ 常规冲洗   │       │
         │ │ 缺水等待  │  └────────────┘       └─────┬──────┘       │
         │ └─────┬─────┘                            │                │
         │       │ 有水                             │ 冲洗完成       │
         │       └──────────────────────────────────▼                │
         │                                                   │
         │              ┌────────────────┐                   │
         │              │ PURE_FLUSH     │<──────────────────┘
         │              │ 纯水洗膜       │
         │              └─────┬──────────┘
         │                    │ 洗膜完成 / 用户用水
         │                    ▼
         │              ┌─────────┐
         └──────────────│ STANDBY │
                        │ 待机    │
                        └─────────┘

    ┌──────────────┐         ┌──────────────┐
    │ LEAK_ALARM   │<────────│  任意状态    │
    │ 漏水报警     │  漏水   │              │
    └──────┬───────┘         └──────────────┘
           │
           │ 手动复位
           ▼
    ┌─────────┐
    │ STANDBY │
    │ 待机    │
    └─────────┘

    ┌──────────────┐
    │ STOP         │<── 制水超时 / 网页停止
    │ 停止         │
    └──────┬───────┘
           │ 手动复位
           ▼
    ┌─────────┐
    │ STANDBY │
    └─────────┘
```

### 各状态行为详解

#### 1. 待机（STANDBY）
- **触发条件**: 系统初始化、冲洗完成、手动复位、网页切换
- **输出**: 所有阀门和泵关闭
- **转换条件**:
  - 有水 + 压力桶需要制水 -> **制水**
  - 无水 -> **缺水**
  - 漏水 -> **漏水报警**
- **其他行为**: 统计上次制水用水量并更新滤芯寿命

#### 2. 制水（PRODUCTION）
- **触发条件**: 待机状态下检测到需要制水，或网页端启动制水
- **输出**: 进水阀 + 增压泵 开启，废水阀 + 回水阀 关闭
- **水锤控制**: 开进水阀 → 延时 → 开增压泵
- **转换条件**:
  - 压力桶满（压力开关断开）-> **水满**
  - 进水缺水（低压开关断开）-> **缺水**
  - 漏水 -> **漏水报警**
  - 制水超时（默认3小时）-> **停止**
- **保护**: 实时监测低压开关和漏水传感器

#### 3. 水满（TANK_FULL）
- **触发条件**: 制水过程中压力桶压力开关断开
- **行为**: 自动转入常规冲洗状态（瞬时过渡）
- **转换条件**: 无条件 -> **常规冲洗**

#### 4. 常规冲洗（NORMAL_FLUSH）
- **触发条件**: 压力桶水满后，或网页端切换
- **输出**: 进水阀 + 废水阀 + 增压泵 开启，回水阀关闭
- **水锤控制**: 先开进水阀 → 延时 → 开废水阀 + 增压泵
- **短制水处理**: 若制水时间<3分钟，冲洗时间缩短为10秒
- **转换条件**:
  - 冲洗完成 -> **纯水洗膜**
  - 缺水 -> **缺水**
  - 漏水 -> **漏水报警**

#### 5. 纯水洗膜（PURE_FLUSH）
- **触发条件**: 常规冲洗完成后，或网页端切换
- **输出**: 回水阀 + 废水阀 开启，进水阀 + 增压泵 关闭
- **目的**: 用压力桶内的纯水回流冲洗RO膜，进一步排除浓缩水
- **水锤控制**: 先停泵→延时→关进水阀→延时→开回水阀+废水阀
- **短制水处理**: 若制水时间<3分钟，洗膜时间缩短为5秒
- **转换条件**:
  - 洗膜完成 -> **待机**
  - 压力开关闭合(用户使用水) -> **待机**（立即中断）
  - 缺水 -> **缺水**
  - 漏水 -> **漏水报警**

#### 6. 缺水（WATER_SHORTAGE）
- **触发条件**: 制水或冲洗过程中进水低压开关断开
- **输出**: 所有输出关闭（保护增压泵）
- **转换条件**:
  - 有水 -> **待机**（由待机逻辑判断是否需要制水）
  - 漏水 -> **漏水报警**

#### 7. 漏水报警（LEAK_ALARM）
- **触发条件**: 任意状态检测到漏水
- **输出**: 所有输出立即关闭
- **转换条件**: 只能通过手动复位 -> **待机**

#### 8. 停止（STOP）
- **触发条件**: 制水超时（默认3小时，可配置），或网页端停止
- **输出**: 所有输出关闭
- **转换条件**: 只能通过手动复位 -> **待机**

### 事件系统

状态机通过FreeRTOS消息队列接收外部事件：

| 事件 | 说明 | 触发方式 |
|------|------|---------|
| FSM_EVENT_RESET | 系统复位 | 网页端"复位"按钮 |
| FSM_EVENT_FORCE_FLUSH | 强制冲洗 | 网页端"冲洗"按钮 |
| FSM_EVENT_FORCE_PRODUCTION | 强制制水 | 网页端"制水"按钮 |
| FSM_EVENT_NORMAL_FLUSH | 常规冲洗 | 网页端"冲洗"按钮（从待机状态） |
| FSM_EVENT_PURE_FLUSH | 纯水洗膜 | 网页端"反冲洗"按钮 |
| FSM_EVENT_FILTER_FLUSH | 换芯冲洗 | 网页端"换芯冲洗"按钮 |
| FSM_EVENT_GO_STANDBY | 待机 | 网页端"待机"按钮 |
| FSM_EVENT_SHUTDOWN | 停止（进入停止状态不记录停止） | 网页端"停止"按钮 |
| FSM_EVENT_NORMAL_FLUSH_DONE | 常规冲洗完成 | 状态机内部定时器 |
| FSM_EVENT_PURE_FLUSH_DONE | 纯水洗膜完成 | 状态机内部定时器 |

### 运行数据持久化

系统在以下时机自动保存运行数据到NVS：
- 制水周期结束时
- 冲洗周期结束时
- 停止发生时
- 维护确认时
- 每60秒周期性保存

保存的数据包括：制水/冲洗次数、总制水/冲洗时间、停止次数、最后维护时间、总用水量。

---

## Web控制界面

### 访问方式

| 方式 | 地址 | 说明 |
|------|------|------|
| IP访问 | `http://<设备IP>` | 直接IP访问 |
| 域名访问 | `http://waterpurifier.local` | mDNS域名 |

> **mDNS支持**: Windows需安装Bonjour，macOS/iOS原生支持

### 界面结构

#### 首页（只读监控）
- 系统状态（待机/制水中/冲洗中等）
- 进出水TDS值（1位小数）及去除率
- 五级滤芯寿命（水量+时间双维度进度条）
- 漏水状态
- WiFi信号强度
- 30秒自动刷新

#### 管理页（`/admin`）

**控制面板**: 制水、常规冲洗、纯水洗膜、换芯冲洗、复位、待机、停止

**硬件配置**:
- RO膜通量选择（50G/75G/100G/200G/400G）
- 增压泵选择（三角洲50G~400G）
- 压力桶选择（3G/3.2G/4G/6G/10G）

**系统配置**:
- 常规冲洗时间（秒）
- 纯水洗膜时间（秒）
- 制水超时（分钟）
- 漏水确认时间（秒）
- 运行数据保存间隔（分钟）
- 水锤效应控制（开阀延时/停泵延时/关阀延时，毫秒）
- 继电器触发电平
- TDS报警阈值（进水/出水）

**滤芯管理**:
- 五级滤芯独立配置
- 每级显示水量进度和时间进度两个维度
- 支持单独重置和容量设置

**TDS校准**:
- 进水/出水TDS传感器一点校准
- 校准参数显示（偏移量+比例系数）

**WiFi配置**:
- 扫描可用网络
- 保存WiFi配置

### 滤芯寿命管理

系统采用**水量+时间双维度**计算滤芯寿命，取两者最小值作为有效寿命：

| 滤芯 | 默认水量 | 默认时间 | 建议更换周期 |
|------|---------|---------|-------------|
| PP棉 | 3000升 | 3000小时 | 3-6个月 |
| 颗粒活性炭 | 4000升 | 4000小时 | 6-12个月 |
| 压缩活性炭 | 4000升 | 4000小时 | 6-12个月 |
| RO膜 | 8000升 | 8000小时 | 24-36个月 |
| 后置活性炭 | 4000升 | 4000小时 | 12个月 |

**用水量计算**: `制水量(升) = 制水时间(秒) x 制水速率(升/小时) / 3600`

制水速率根据RO膜通量自动匹配（汇通Vontron实际参数）：

| RO膜 | 制水速率 | 实际参数 |
|------|---------|---------|
| 50G  | 7.8 L/h  | 0.13 L/min × 60 |
| 75G  | 12.0 L/h | 0.20 L/min × 60 |
| 100G | 15.6 L/h | 0.26 L/min × 60 |
| 200G | 31.2 L/h | 0.52 L/min × 60 |
| 400G | 62.4 L/h | 1.04 L/min × 60 |

### TDS校准

1. 将两个TDS传感器放入同一杯标准液中
2. 在管理页找到"TDS校准"区域
3. 输入标准液TDS值
4. 点击对应传感器的校准按钮

---

## Home Assistant集成

### 自动发现

系统启动后通过MQTT发送Home Assistant发现配置，自动添加以下实体：

#### 传感器 (Sensor)
- `water_purifier_tds_in`: 进水TDS值 (ppm)
- `water_purifier_tds_out`: 出水TDS值 (ppm)
- `water_purifier_tds_reduction_rate`: TDS去除率 (%)
- `water_purifier_rssi`: WiFi信号强度 (dBm)
- `water_purifier_cycles`: 总制水次数

#### 开关 (Switch)
- `water_purifier_production`: 制水控制
- `water_purifier_flush`: 冲洗控制

#### 二进制传感器 (Binary Sensor)
- `water_purifier_high_pressure`: 高压开关状态
- `water_purifier_low_pressure`: 低压开关状态
- `water_purifier_tank_pressure`: 压力桶开关状态
- `water_purifier_leak`: 漏水检测状态

### MQTT主题结构

```
# 状态发布
water-purifier/status              # 系统状态
water-purifier/tds/in              # 进水TDS
water-purifier/tds/out             # 出水TDS
water-purifier/tds/reduction_rate  # 去除率
water-purifier/pressure/high       # 高压状态
water-purifier/pressure/low        # 低压状态
water-purifier/pressure/tank       # 压力桶状态
water-purifier/leak                # 漏水状态
water-purifier/production/cycles   # 制水次数

# 控制订阅
water-purifier/set/state           # 状态控制
water-purifier/set/flush           # 冲洗控制
```

---

## API接口文档

### 基础信息

| 项目 | 值 |
|------|-----|
| Content-Type | application/json |
| 字符编码 | UTF-8 |
| 超时时间 | 5000ms |

### API端点

#### 1. 获取系统状态

```http
GET /api/status
```

**响应示例**:
```json
{
  "state": "制水中",
  "tds_in": 150.5,
  "tds_out": 8.2,
  "rate": 94.5,
  "filters": [
    {"name": "PP棉", "waterPct": 85, "timePct": 90, "effPct": 85, "total": 3000},
    {"name": "颗粒活性炭", "waterPct": 72, "timePct": 80, "effPct": 72, "total": 4000},
    {"name": "压缩活性炭", "waterPct": 68, "timePct": 75, "effPct": 68, "total": 4000},
    {"name": "RO膜", "waterPct": 91, "timePct": 88, "effPct": 88, "total": 8000},
    {"name": "后置活性炭", "waterPct": 55, "timePct": 60, "effPct": 55, "total": 4000}
  ],
  "leak": false,
  "wifiState": "已连接",
  "ip": "192.168.1.100"
}
```

#### 2. 控制命令

```http
POST /api/control
Content-Type: application/json

{
  "action": "start_production"
}
```

**action参数**:
- `start_production`: 开始制水
- `normal_flush`: 常规冲洗
- `pure_flush`: 纯水洗膜
- `filter_flush`: 换芯冲洗（仅普通冲洗持续1小时，不计统计）
- `standby`: 切换到待机
- `shutdown`: 停止（进入停止状态）
- `reset`: 系统复位（清除停止/漏水报警状态）

#### 3. TDS校准

```http
POST /api/tds/calibrate
Content-Type: application/json

{
  "sensor": 0,
  "value": 200.0
}
```

**参数说明**:
- `sensor`: 0=进水TDS, 1=出水TDS
- `value`: 标准液TDS值（ppm）

#### 4. 滤芯管理

**重置滤芯**:
```http
POST /api/filter/reset
Content-Type: application/json

{
  "filter": 0
}
```

**设置滤芯容量**:
```http
POST /api/filter/capacity
Content-Type: application/json

{
  "caps": [3000, 4000, 4000, 8000, 4000]
}
```

#### 5. 获取系统配置

```http
GET /api/config
```

**响应示例**:
```json
{
  "normalFlushDur": 30,
  "pureFlushDur": 20,
  "prodTimeout": 180,
  "leakConfirm": 5,
  "saveInterval": 120,
  "relayLevel": 0,
  "tdsInTh": 500,
  "tdsOutTh": 100,
  "roMem": 1,
  "pumpType": 1,
  "tankSize": 0,
  "whValveOpen": 1000,
  "whPumpStop": 1000,
  "whValveClose": 500
}
```

> **注意**: `prodTimeout` 单位为分钟，内部自动转换为秒存储。

#### 6. 保存系统配置

```http
POST /api/config
Content-Type: application/json

{
  "normalFlushDur": 30,
  "pureFlushDur": 20,
  "prodTimeout": 180,
  "leakConfirm": 5,
  "saveInterval": 120,
  "relayLevel": 0,
  "tdsInTh": 500,
  "tdsOutTh": 100,
  "roMem": 1,
  "pumpType": 1,
  "tankSize": 0,
  "whValveOpen": 1000,
  "whPumpStop": 1000,
  "whValveClose": 500
}
```

#### 7. WiFi配置

**扫描网络**:
```http
GET /api/wifi/scan
```

**保存配置**:
```http
POST /api/wifi
Content-Type: application/json

{
  "ssid": "YourWiFi",
  "password": "YourPassword"
}
```

---

## 项目结构

```
WaterPurifier/
├── main/                           # 主程序目录
│   ├── include/                    # 头文件目录
│   │   ├── board_params.h         # 硬件参数（压力开关、TDS阈值等）
│   │   ├── gpio_config.h          # GPIO引脚定义
│   │   ├── gpio_driver.h          # GPIO驱动接口
│   │   ├── tds_sensor.h           # TDS传感器接口（TDS测量）
│   │   ├── filter_manager.h       # 滤芯管理接口（5级滤芯寿命+用水量）
│   │   ├── water_purifier_fsm.h   # 状态机接口
│   │   ├── wifi_manager.h         # WiFi管理器接口
│   │   ├── mqtt_client.h          # MQTT客户端接口
│   │   ├── web_server.h           # Web服务器接口
│   │   ├── config_manager.h       # 配置管理器接口
│   │   └── history_logger.h       # 历史记录接口
│   │
│   ├── main.c                     # 主程序入口
│   ├── gpio_driver.c              # GPIO驱动实现
│   ├── tds_sensor.c               # TDS传感器实现（双路ADC采集+校准）
│   ├── filter_manager.c           # 滤芯管理实现（5级滤芯+用水量+NVS持久化）
│   ├── water_purifier_fsm.c       # 状态机实现（8状态FSM）
│   ├── wifi_manager.c             # WiFi管理器（STA/AP+mDNS）
│   ├── mqtt_client.c              # MQTT客户端（Home Assistant发现）
│   ├── web_server.c               # Web服务器（HTML/CSS/JS嵌入）
│   ├── config_manager.c           # 配置管理器（NVS存储）
│   ├── history_logger.c           # 历史记录实现
│   │
│   ├── CMakeLists.txt             # 构建配置
│   └── idf_component.yml          # 组件依赖（mDNS）
│
├── build/                         # 编译输出目录
├── sdkconfig                      # ESP-IDF配置文件
├── CMakeLists.txt                 # 项目构建配置
├── README.md                      # 项目说明
├── do_build.bat                   # Windows编译脚本
├── flash_monitor.bat              # 烧录+监控脚本
└── rebuild.bat                    # 清理+重新编译脚本
```

### 模块职责

| 模块 | 文件 | 职责 |
|------|------|------|
| **主控** | `main.c` | 初始化顺序、状态回调、MQTT启动、监控任务 |
| **FSM** | `water_purifier_fsm.c` | 8状态有限状态机、双阶段冲洗、水锤控制、停止处理 |
| **TDS** | `tds_sensor.c` | 双路ADC采集、TDS计算、校准、报警 |
| **Filter** | `filter_manager.c` | 5级滤芯寿命管理、用水量统计、NVS持久化 |
| **GPIO** | `gpio_driver.c` | 输入开关读取、24V继电器控制、LED指示 |
| **WiFi** | `wifi_manager.c` | STA/AP模式、自动重连、mDNS域名服务 |
| **Web** | `web_server.c` | HTTP服务器、REST API、HTML/CSS/JS嵌入 |
| **MQTT** | `mqtt_client.c` | Home Assistant自动发现、状态推送 |
| **Config** | `config_manager.c` | NVS配置存储/加载、运行数据持久化 |
| **History** | `history_logger.c` | 运行日志记录 |

### 初始化流程

```
1. config_manager_init()   -> NVS初始化，加载系统配置
2. gpio_driver_init_*()    -> GPIO输入/输出/LED初始化
3. tds_sensor_init()       -> ADC初始化，启动TDS测量任务
   tds_sensor_start()      -> 启动TDS测量任务（1秒周期）
4. filter_mgr_init()       -> 滤芯管理初始化，从NVS恢复滤芯状态
5. fsm_init()              -> 状态机初始化，加载运行数据
   fsm_set_*()             -> 应用配置（冲洗时间、超时、制水速率）
   fsm_start()             -> 启动FSM任务（100ms周期）
6. history_logger_init()   -> 历史记录初始化
7. wifi_manager_init()     -> WiFi初始化
   wifi_manager_start()    -> 启动WiFi（有配置则STA，无配置则AP）
8. mqtt_client_init()      -> MQTT客户端初始化
9. web_server_init()       -> HTTP服务器初始化
   web_server_start()      -> 启动Web服务
10. xTaskCreate(monitor)   -> 启动监控任务（30秒周期）
```

---

## 配置参数

### 系统参数

| 参数 | 默认值 | 网页配置 | 说明 |
|------|--------|---------|------|
| 常规冲洗时间 | 30秒 | 是 | 水满后常规冲洗持续时间 |
| 纯水洗膜时间 | 20秒 | 是 | 常规冲洗后纯水回流冲洗时间 |
| 短制水判断阈值 | 180秒 | 否 | 制水<此时间则使用缩短冲洗(10s/5s) |
| 制水超时 | 3小时（180分钟） | 是 | 单次制水最长时长，超时进入停止 |
| 水锤开阀延时 | 1000ms | 是 | 开阀后延时再启泵 |
| 水锤停泵延时 | 1000ms | 是 | 停泵后延时再关阀 |
| 水锤关阀延时 | 500ms | 是 | 关阀后延时再开回水阀 |
| 漏水确认时间 | 5秒 | 是 | 漏水确认时间 |
| 运行数据保存间隔 | 120分钟 | 是 | 运行数据保存间隔（10/60/120/360/720/1440） |
| 继电器触发 | 低电平 | 是 | 继电器控制电平（可配置） |

### TDS参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| 进水报警阈值 | 500 ppm | 进水TDS过高报警 |
| 出水报警阈值 | 100 ppm | 出水TDS超标报警 |
| 采样间隔 | 1秒 | TDS测量周期 |
| TDS公式 | V(mV) / 2300 x 1000 | DFRobot Gravity规格 |

### 压力开关参数

| 参数 | 值 | 说明 |
|------|-----|------|
| 高压开关断开压力 | 2.5 bar | RO膜保护，压力过高断开 |
| 高压开关闭合压力 | 1.5 bar | 压力恢复后闭合 |
| 低压开关断开压力 | 0.3 bar | 原水保护，压力过低断开 |
| 低压开关闭合压力 | 0.5 bar | 压力恢复后闭合 |
| 压力桶满压力 | 2.5 bar | 桶满停止制水 |
| 压力桶空压力 | 1.0 bar | 桶空开始制水 |

### 运行数据存储

所有配置和运行数据存储在ESP32的**NVS（Non-Volatile Storage）**分区中，使用3个独立的命名空间：

| 命名空间 | 用途 | 存储内容 |
|---------|------|---------|
| `water_purifier` | 系统配置 | WiFi、MQTT、硬件配置、TDS参数等 |
| `wp_rt` | 运行数据 | 制水/冲洗次数、时间、停止/漏水记录 |
| `wp_filters` | 滤芯数据 | 5级滤芯用水量、重置时间、自定义容量、总用水量、制水速率 |
| `wifi` | WiFi配置 | SSID和密码（由ESP-IDF WiFi框架管理） |

NVS存储在Flash的专用分区中，**断电不丢失**。可通过网页端恢复出厂设置擦除所有配置。

---

## 故障排除

### 常见问题

#### 1. 无法连接WiFi

**排查步骤**:
1. 检查WiFi密码是否正确
2. 确认路由器支持2.4GHz（ESP32-C3不支持5GHz）
3. 检查路由器信号强度（建议 > -70dBm）

**解决方法**:
- 重新进入AP配置模式配置WiFi

#### 2. TDS读数为0或异常

**排查步骤**:
1. 检查TDS传感器连接（GPIO2/GPIO3）
2. 确认传感器供电正常
3. 检查ADC引脚是否正确连接

**解决方法**:
- 使用网页端TDS校准功能

#### 3. 无法通过域名访问

**原因**: mDNS需要客户端支持

**解决方案**:
- Windows: 安装Bonjour服务
- macOS/iOS: 原生支持
- Android: 使用支持mDNS的浏览器
- Linux: 安装avahi-daemon

#### 4. Flash大小警告

**症状**: `Detected size(4096k) larger than the size in the binary image header(2048k)`

**解决方法**:
- 已在sdkconfig中设置Flash大小为4MB
- 运行 `idf.py fullclean` 后重新编译

#### 5. COM端口拒绝访问

**症状**: `Could not open COM3, the port is busy or doesn't exist`

**解决方法**:
1. 关闭串口监视器或其他占用COM端口的程序
2. 按住开发板BOOT键，按RESET键进入下载模式
3. 重新执行 `idf.py -p COM3 flash`

---

## 许可证

MIT License

---

## 作者

Water Purifier Controller - ESP32-C3
