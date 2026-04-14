# 净水器主控板代码结构文档

## 目录

- [项目概述](#项目概述)
- [目录结构](#目录结构)
- [模块详细说明](#模块详细说明)
- [数据流图](#数据流图)
- [状态机详解](#状态机详解)
- [API接口文档](#api接口文档)
- [配置存储](#配置存储)
- [内存使用](#内存使用)

---

## 项目概述

### 硬件平台
- **芯片**: ESP32-C3（RISC-V 单核，160MHz）
- **Flash**: 4MB
- **RAM**: 400KB SRAM
- **WiFi**: 802.11 b/g/n（2.4GHz）
- **蓝牙**: BLE 5.0

### 系统配置
- **RO膜**: 可配置（50G/75G/100G/200G/400G）
- **增压泵**: 可配置（三角洲 50G~400G）
- **压力桶**: 可配置（3G/3.2G/4G/6G/10G）
- **工作电压**: 24V（设备供电）/ 5V（降压模块给ESP32）
- **固件版本**: 1.0.0

### 技术栈
- **框架**: ESP-IDF v5.5.2
- **协议**: WiFi, MQTT (Home Assistant), HTTP, mDNS
- **数据格式**: JSON
- **存储**: NVS（非易失性存储）

---

## 目录结构

```
WaterPurifier/
├── main/                           # 主程序目录
│   ├── include/                    # 头文件目录
│   │   ├── board_params.h         # 硬件配置参数（压力开关阈值等）
│   │   ├── gpio_config.h          # GPIO引脚定义和电气参数
│   │   ├── gpio_driver.h          # GPIO驱动接口
│   │   ├── tds_sensor.h           # TDS传感器接口（双路+滤芯管理）
│   │   ├── water_purifier_fsm.h   # 状态机接口（8状态FSM）
│   │   ├── wifi_manager.h         # WiFi管理器接口
│   │   ├── app_mqtt_public.h      # MQTT客户端接口
│   │   ├── web_server.h           # Web服务器接口
│   │   ├── config_manager.h       # 配置管理器接口
│   │   └── history_logger.h       # 历史记录接口
│   │
│   ├── main.c                     # 主程序入口
│   ├── gpio_driver.c              # GPIO驱动实现
│   ├── tds_sensor.c               # TDS传感器实现（ADC采集+5级滤芯管理）
│   ├── water_purifier_fsm.c       # 状态机实现（8状态FSM，双阶段冲洗+水锤控制）
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
├── DOCUMENTATION.md               # 本文档
└── *.bat                          # Windows构建脚本
```

---

## 模块详细说明

### 1. 主程序 (main.c)

**职责**: 系统初始化、模块协调、监控任务

**初始化顺序**:
```
1. config_manager_init()   -> NVS初始化，加载系统配置
2. gpio_driver_init_*()    -> GPIO输入/输出/LED初始化
3. tds_sensor_init()       -> ADC初始化
   tds_sensor_start()      -> 启动TDS测量任务（1秒周期）
4. fsm_init()              -> 状态机初始化，加载运行数据
   fsm_set_*()             -> 应用配置
   fsm_start()             -> 启动FSM任务（100ms周期）
5. history_logger_init()   -> 历史记录初始化
6. wifi_manager_init()     -> WiFi初始化
   wifi_manager_start()    -> 启动WiFi（有配置则STA，无配置则AP）
7. mqtt_client_init()      -> MQTT客户端初始化
8. web_server_init()       -> HTTP服务器初始化
   web_server_start()      -> 启动Web服务
9. xTaskCreate(monitor)    -> 启动监控任务（30秒周期）
```

**回调函数**:
- `fsm_state_callback()`: 状态变化时发布MQTT状态更新
- `wifi_state_callback()`: WiFi连接后启动MQTT和发送HA发现配置

---

### 2. GPIO驱动 (gpio_driver.c/h)

**职责**: GPIO引脚控制、输入检测、输出控制

**引脚分配**:

| GPIO | 设备 | 方向 | 说明 |
|------|------|------|------|
| GPIO2 | 进水TDS | ADC | ADC1_CH2，0~2.3V -> 0~1000ppm |
| GPIO3 | 出水TDS | ADC | ADC1_CH3，0~2.3V -> 0~1000ppm |
| GPIO6 | 进水低压开关 | 输入 | 低电平=有水 |
| GPIO7 | 压力桶压力开关 | 输入 | 闭合=需要制水 |
| GPIO10 | 漏水检测 | 输入 | 低电平=漏水报警 |
| GPIO0 | 进水电磁阀 | 输出 | 24V控制，默认低电平触发 |
| GPIO1 | 废水电磁阀 | 输出 | 24V控制，默认低电平触发 |
| GPIO4 | 回水电磁阀 | 输出 | 24V控制，默认低电平触发 |
| GPIO5 | 增压泵 | 输出 | 24V控制，默认低电平触发 |
| GPIO12 | LED D4 | 输出 | 主状态指示 |
| GPIO13 | LED D5 | 输出 | 次状态指示 |

> **注意**: ESP32-C3的ADC1只有通道0-4（GPIO0~GPIO4），GPIO5使用ADC2，WiFi运行时不可用。

**主要函数**:
```c
esp_err_t gpio_driver_init_inputs(void);     // 初始化输入引脚
esp_err_t gpio_driver_init_outputs(void);    // 初始化输出引脚
esp_err_t gpio_driver_init_leds(void);       // 初始化LED引脚

// 读取状态
bool gpio_driver_read_low_pressure(void);     // 低压开关
bool gpio_driver_read_tank_pressure(void);    // 压力桶开关
bool gpio_driver_read_water_leak(void);       // 漏水检测

// 控制输出
void gpio_driver_set_inlet_valve(bool on);    // 进水阀
void gpio_driver_set_waste_valve(bool on);    // 废水阀
void gpio_driver_set_return_valve(bool on);   // 回水阀
void gpio_driver_set_boost_pump(bool on);     // 增压泵
void gpio_driver_set_led1(bool on);           // LED D4
void gpio_driver_set_led2(bool on);           // LED D5

// 配置
void gpio_driver_set_relay_trigger_level(uint8_t level);  // 继电器触发电平
```

---

### 3. TDS传感器 (tds_sensor.c/h)

**职责**: TDS值采集、滤波、报警检测、五级滤芯寿命管理

**工作原理**:
```
ADC原始值 → 电压转换(mV) → TDS计算(线性) → 温度补偿 → 校准 → 报警检测
```

**TDS计算公式**: `TDS = voltage_mV / 2300 * 1000`（DFRobot Gravity规格）

**ADC采样**: 16次采样，去除最大最小值后平均

**主要函数**:
```c
esp_err_t tds_sensor_init(void);
esp_err_t tds_sensor_start(void);
esp_err_t tds_sensor_stop(void);

// 测量
esp_err_t tds_sensor_measure(tds_sensor_id_t id, tds_measurement_t *measurement);
esp_err_t tds_sensor_measure_dual(tds_dual_measurement_t *dual);
esp_err_t tds_sensor_get_latest(tds_sensor_id_t id, tds_measurement_t *measurement);
esp_err_t tds_sensor_get_latest_dual(tds_dual_measurement_t *dual);

// 校准
esp_err_t tds_sensor_calibrate(tds_sensor_id_t id, float standard_value);
esp_err_t tds_sensor_set_calibration(tds_sensor_id_t id, const tds_calibration_t *cal);
esp_err_t tds_sensor_reset_calibration(tds_sensor_id_t id);

// 报警
esp_err_t tds_sensor_set_alarm_threshold(tds_sensor_id_t id, float threshold);
bool tds_sensor_is_alarm(tds_sensor_id_t id);

// 五级滤芯管理
esp_err_t tds_sensor_get_filters_status(filters_status_t *status);
esp_err_t tds_sensor_reset_filter(filter_type_t filter_type);
esp_err_t tds_sensor_set_filter_capacity_ex(filter_type_t filter_type, uint32_t liters);
bool tds_sensor_any_filter_needs_replacement(void);

// 制水速率
esp_err_t tds_sensor_set_production_rate(float lph);
float tds_sensor_get_production_rate(void);
```

**数据结构**:
```c
typedef struct {
    float tds_value;           // TDS值 (ppm)
    float ec_value;            // 电导率 (μS/cm)
    float voltage;             // 原始电压值 (mV)
    float temperature;         // 温度值 (°C)
    bool valid;                // 数据有效性
    uint64_t timestamp;        // 时间戳 (微秒)
} tds_measurement_t;

typedef struct {
    uint8_t percentage;           // 剩余寿命百分比，基于水量
    uint8_t time_percentage;      // 剩余寿命百分比，基于时间
    uint8_t effective_percentage;  // 有效寿命百分比（min(水量%, 时间%)）
    uint32_t used_liters;         // 已处理水量（升）
    uint32_t total_liters;        // 滤芯总容量（升）
    uint32_t time_limit_hours;    // 时间寿命上限（小时）
    uint32_t install_time;        // 安装时间戳（Unix秒，NTP墙钟）
    uint32_t last_reset_time;     // 上次重置时间戳（Unix秒，NTP墙钟）
    bool replacement_needed;       // 是否需要更换
    char name[16];                // 滤芯名称
} filter_info_t;
```

**滤芯日历寿命说明**: `install_time` 和 `last_reset_time` 使用 `time(NULL)` 获取的 Unix 秒（NTP 墙钟），而非 `esp_timer_get_time()`。设备首次 NTP 同步后会自动回推滤芯安装时间，确保日历寿命准确反映自然时间流逝，即使设备断电期间也正常计时。

**五级滤芯默认配置**:

| 滤芯 | 默认水量 | 默认时间 | 建议更换周期 |
|------|---------|---------|-------------|
| PP棉 | 3000升 | 3000小时 | 3-6个月 |
| 颗粒活性炭 | 4000升 | 4000小时 | 6-12个月 |
| 压缩活性炭 | 4000升 | 4000小时 | 6-12个月 |
| RO膜 | 8000升 | 8000小时 | 24-36个月 |
| 后置活性炭 | 4000升 | 4000小时 | 12个月 |

**滤芯寿命计算**: `effective_percentage = min(water_pct, time_pct)`，低于10%触发更换提醒。

---

### 4. 状态机 (water_purifier_fsm.c/h)

**职责**: 净水器工作流程控制

**状态定义**:

| 状态 | 说明 | 触发条件 |
|------|------|---------|
| FSM_STATE_STANDBY | 待机 | 初始状态/冲洗完成/手动复位/网页切换 |
| FSM_STATE_PRODUCTION | 制水中 | 需要制水/网页启动制水 |
| FSM_STATE_TANK_FULL | 水满 | 压力桶满 |
| FSM_STATE_NORMAL_FLUSH | 常规冲洗 | 水满后自动冲洗/网页切换 |
| FSM_STATE_PURE_FLUSH | 纯水洗膜 | 常规冲洗完成后/网页切换 |
| FSM_STATE_WATER_SHORTAGE | 缺水 | 进水压力不足 |
| FSM_STATE_LEAK_ALARM | 漏水报警 | 漏水检测 |
| FSM_STATE_STOP | 停止 | 制水超时/网页停止 |

**事件定义**:
```c
typedef enum {
    FSM_EVENT_NONE = 0,             // 无事件
    FSM_EVENT_LOW_PRESSURE_ON,      // 有水
    FSM_EVENT_LOW_PRESSURE_OFF,     // 缺水
    FSM_EVENT_TANK_NEED_WATER,      // 需要制水
    FSM_EVENT_TANK_FULL,            // 压力桶满
    FSM_EVENT_WATER_LEAK,           // 漏水检测
    FSM_EVENT_NORMAL_FLUSH_DONE,    // 常规冲洗完成
    FSM_EVENT_PURE_FLUSH_DONE,      // 纯水洗膜完成
    FSM_EVENT_PRODUCTION_TIMEOUT,   // 制水超时
    FSM_EVENT_RESET,                // 复位
    FSM_EVENT_FORCE_FLUSH,          // 强制冲洗
    FSM_EVENT_FORCE_PRODUCTION,     // 强制制水
    FSM_EVENT_NORMAL_FLUSH,         // 常规冲洗（网页控制）
    FSM_EVENT_PURE_FLUSH,           // 纯水洗膜（网页控制）
    FSM_EVENT_GO_STANDBY,           // 待机（网页控制）
    FSM_EVENT_SHUTDOWN,             // 停止（进入停止状态不记录停止，网页控制）
} fsm_event_t;
```

**状态转换图**:
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

**各状态行为**:

| 状态 | 输出 | 转换条件 |
|------|------|---------|
| STANDBY | 全部关闭 | 有水+需制水→PRODUCTION，无水→SHORTAGE，漏水→LEAK |
| PRODUCTION | 进水阀+增压泵（开阀→延时→泵，废水阀关） | 压力桶满→TANK_FULL，缺水→SHORTAGE，漏水→LEAK，超时→STOP |
| TANK_FULL | 无（瞬时过渡） | 无条件→NORMAL_FLUSH |
| NORMAL_FLUSH | 进水阀+废水阀+增压泵（检测进水阀状态决定水锤流程） | 完成→PURE_FLUSH，缺水→SHORTAGE，漏水→LEAK |
| PURE_FLUSH | 回水阀+废水阀（停泵→延时→关进水阀→延时→开回水阀+废水阀） | 完成→STANDBY，压力开关闭合→STANDBY，缺水→SHORTAGE，漏水→LEAK |
| WATER_SHORTAGE | 全部关闭 | 有水→STANDBY（待机判断是否制水），漏水→LEAK |
| LEAK_ALARM | 全部关闭 | 手动复位→STANDBY |
| STOP | 全部关闭 | 手动复位→STANDBY |

**运行时数据**:
```c
typedef struct {
    uint32_t total_production_cycles;      // 总制水次数
    uint32_t total_flush_cycles;           // 总冲洗次数
    uint64_t total_production_time_sec;    // 总制水时间（秒）
    uint64_t total_flush_time_sec;         // 总冲洗时间（秒）
    stop_type_t last_stop_type;          // 最近停止类型
    uint32_t stop_count;                  // 停止次数
    uint64_t last_stop_time;              // 最近停止时间
    bool low_pressure_occurred;            // 曾经低压断开过
} fsm_runtime_data_t;
```

**制水速率映射**:
```c
// RO膜通量 -> 制水速率 (L/h) - 汇通Vontron实际参数
50G  -> 7.8 L/h   (0.13 L/min × 60)
75G  -> 12.0 L/h  (0.20 L/min × 60)
100G -> 15.6 L/h  (0.26 L/min × 60)
200G -> 31.2 L/h  (0.52 L/min × 60)
400G -> 62.4 L/h  (1.04 L/min × 60)
```

---

### 5. WiFi管理器 (wifi_manager.c/h)

**职责**: WiFi连接管理、AP配置模式、mDNS域名服务

**工作模式**:
- **STA模式**: 连接到配置的WiFi网络
- **AP+STA模式**: 无配置时启动AP热点配网
- **mDNS**: 注册 `http://waterpurifier.local`

**主要函数**:
```c
esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_stop(void);

// AP模式
esp_err_t wifi_manager_start_ap_mode(void);
esp_err_t wifi_manager_stop_ap_mode(void);

// 状态查询
wifi_state_t wifi_manager_get_state(void);
const char* wifi_manager_get_state_name(wifi_state_t state);
bool wifi_manager_is_connected(void);
bool wifi_manager_is_ap_mode(void);
esp_err_t wifi_manager_get_ip(char *ip_str, size_t buffer_size);
int8_t wifi_manager_get_rssi(void);
const char* wifi_manager_get_ssid(void);

// 配置
esp_err_t wifi_manager_set_config(const char *ssid, const char *password);
esp_err_t wifi_manager_save_config(void);
esp_err_t wifi_manager_load_config(void);
esp_err_t wifi_manager_clear_config(void);
bool wifi_manager_has_saved_config(void);
```

**AP配置模式**:
- AP SSID: `WaterPurifier-XXXX`（基于MAC后4位）
- AP密码: `12345678`
- 配置URL: `http://192.168.4.1`

---

### 6. MQTT客户端 (mqtt_client.c/h)

**职责**: MQTT通信、Home Assistant集成

**主要函数**:
```c
esp_err_t mqtt_client_init(void);
esp_err_t mqtt_client_start(void);
bool mqtt_client_is_connected(void);

// 发布
esp_err_t mqtt_publish_purifier_status(void);
esp_err_t mqtt_publish_tds_value(void);
esp_err_t mqtt_send_ha_discovery(void);

// 配置
esp_err_t mqtt_client_set_config(const mqtt_config_t *config);
```

**MQTT主题结构**:
```
# 状态发布
water-purifier/status              # 系统状态
water-purifier/tds/in              # 进水TDS值
water-purifier/tds/out             # 出水TDS值
water-purifier/tds/reduction_rate  # 去除率
water-purifier/pressure/high       # 高压开关状态
water-purifier/pressure/low        # 低压开关状态
water-purifier/pressure/tank       # 压力桶状态
water-purifier/leak                # 漏水状态
water-purifier/production/cycles   # 制水次数

# 控制订阅
water-purifier/set/state           # 状态控制
water-purifier/set/flush           # 冲洗控制
```

**Home Assistant发现**: 自动发送发现配置，HA会自动添加传感器、开关、二进制传感器实体。

---

### 7. Web服务器 (web_server.c/h)

**职责**: HTTP服务器、Web控制界面、RESTful API

**主要功能**:
- 首页（只读监控，30秒自动刷新）
- 管理页（配置控制）
- RESTful API接口

**API接口**:

| 接口 | 方法 | 功能 |
|------|------|------|
| `/` | GET | 首页（只读监控） |
| `/admin` | GET | 管理页面 |
| `/api/status` | GET | 获取系统状态（JSON） |
| `/api/control` | POST | 发送控制命令 |
| `/api/config` | GET | 获取系统配置 |
| `/api/config` | POST | 保存系统配置 |
| `/api/config/hardware` | POST | 保存硬件配置（RO膜/泵/压力桶） |
| `/api/wifi/scan` | GET | 扫描WiFi网络 |
| `/api/wifi` | POST | 保存WiFi配置 |
| `/api/filter/reset` | POST | 重置滤芯 |
| `/api/filter/capacity` | POST | 设置滤芯容量 |
| `/api/tds/calibrate` | POST | TDS传感器校准 |
| `/api/mqtt/config` | GET | 获取MQTT配置 |
| `/api/mqtt/config` | POST | 保存MQTT配置 |

**控制命令**:
```json
{"action": "start_production"}  // 制水
{"action": "normal_flush"}      // 冲洗
{"action": "pure_flush"}        // 反冲洗
{"action": "standby"}           // 待机
{"action": "shutdown"}          // 停止
{"action": "reset"}             // 复位
```

**状态响应示例**:
```json
{
  "state": "制水中",
  "tds_in": 150.5,
  "tds_out": 8.2,
  "rate": 94.5,
  "filters": [
    {"name":"PP棉","waterPct":85,"timePct":90,"effPct":85,"total":3000},
    {"name":"颗粒活性炭","waterPct":72,"timePct":80,"effPct":72,"total":4000},
    {"name":"压缩活性炭","waterPct":68,"timePct":75,"effPct":68,"total":4000},
    {"name":"RO膜","waterPct":91,"timePct":88,"effPct":88,"total":8000},
    {"name":"后置活性炭","waterPct":55,"timePct":60,"effPct":55,"total":4000}
  ],
  "totalWater": 1523,
  "leak": false,
  "wifiState": "已连接",
  "ssid": "MyWiFi",
  "ip": "192.168.1.100",
  "cycles": 156,
  "flushes": 156,
  "prodTime": 46800,
  "uptime": 86400
}
```

**系统配置请求/响应**:
```json
{
  "flushDur": 30,
  "prodTimeout": 10800,
  "leakConfirm": 5,
  "saveInterval": 120,
  "relayLevel": 0,
  "tdsInTh": 500,
  "tdsOutTh": 100
}
```

**硬件配置请求**:
```json
{
  "roMem": 1,
  "pumpType": 1,
  "tankSize": 0
}
```

---

### 8. 配置管理器 (config_manager.c/h)

**职责**: 配置持久化存储（NVS）

**配置结构**:
```c
typedef struct {
    // WiFi配置
    char wifi_ssid[33];
    char wifi_password[65];

    // MQTT配置
    bool mqtt_enabled;
    char mqtt_broker[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];

    // RO膜和硬件配置
    uint8_t ro_membrane_type;         // RO膜通量类型 (0=50G, 1=75G, 2=100G, 3=200G, 4=400G)
    uint8_t pump_type;                // 增压泵类型 (0=三角洲50G, 1=75G, 2=100G, 3=200G, 4=300G, 5=400G)
    uint8_t tank_size;                // 压力桶大小 (0=3G, 1=3.2G, 2=4G, 3=6G, 4=10G)

    // 系统参数
    uint32_t flush_duration_sec;        // 冲洗持续时间（秒），默认30
    uint32_t production_timeout_sec;    // 制水超时时间（秒），默认3小时
    uint32_t leak_confirm_time_sec;     // 漏水确认时间（秒），默认5秒
    uint16_t runtime_save_interval_min;  // 运行数据保存间隔（分钟），10/60/120/360/720/1440，默认120

    // 冲洗参数
    uint32_t normal_flush_duration_sec;  // 常规冲洗持续时间（秒），默认30
    uint32_t pure_flush_duration_sec;    // 纯水洗膜持续时间（秒），默认20
    uint32_t short_prod_threshold_sec;   // 短制水判断阈值（秒），默认180
    uint32_t water_hammer_valve_open_delay_ms;  // 水锤-开阀延时（毫秒），默认1000
    uint32_t water_hammer_pump_stop_delay_ms;   // 水锤-停泵延时（毫秒），默认1000
    uint32_t water_hammer_valve_close_delay_ms; // 水锤-关阀延时（毫秒），默认500

    // 继电器配置
    uint8_t relay_trigger_level;        // 继电器触发电平（0=低电平，1=高电平）

    // TDS配置
    float tds_inlet_threshold;          // 进水TDS报警阈值，默认500ppm
    float tds_outlet_threshold;         // 出水TDS报警阈值，默认100ppm
    float tds_calibration_offset[2];    // TDS校准偏移量（进水、出水）
    float tds_calibration_scale[2];     // TDS校准比例系数（进水、出水）

    // 滤芯配置
    uint32_t filter_capacity_liters;    // 滤芯容量（升）

    // Web服务器配置
    uint16_t web_port;
    bool web_auth_enabled;
    char web_username[32];
    char web_password[32];
} system_config_t;
```

**运行数据结构**:
```c
typedef struct {
    uint32_t total_production_cycles;
    uint32_t total_flush_cycles;
    uint64_t total_production_time_sec;
    uint64_t total_flush_time_sec;
    uint32_t stop_count;
    uint32_t total_water_used;
} runtime_data_t;
```

**NVS命名空间**:

| 命名空间 | 用途 | 存储内容 |
|---------|------|---------|
| `water_purifier` | 系统配置 | WiFi/MQTT/硬件/TDS/滤芯/Web配置 |
| `wp_rt` | 运行数据 | 制水/冲洗次数、时间、停止、用水量 |
| `wifi` | WiFi配置 | SSID和密码 |
| `history` | 历史记录 | 事件记录（最多50条循环） |
| `daily_stats` | 每日统计 | 每日制水/冲洗/TDS统计 |

---

### 9. 历史记录 (history_logger.c/h)

**职责**: 运行事件日志记录、每日统计汇总、NVS 脏数据节流写入

**事件类型**: 状态变化、开始/结束制水、开始/结束冲洗、缺水/恢复、水满、漏水报警、停止/清除、维护提醒、手动控制、系统启动，共 14 种。

**主要函数**:
```c
esp_err_t history_logger_init(void);
esp_err_t history_logger_deinit(void);

// 记录事件
esp_err_t history_add_record(history_event_type_t type, ...);
esp_err_t history_log_state_change(uint8_t from, uint8_t to);
esp_err_t history_log_production_end(uint32_t duration_sec);
esp_err_t history_log_flush_end(uint32_t duration_sec);
esp_err_t history_log_stop(uint8_t stop_type, const char *description);
esp_err_t history_log_leak_alarm(void);

// 查询
uint32_t history_get_count(void);
uint32_t history_get_records(history_record_t *records, uint32_t max_count);
esp_err_t history_clear_all(void);

// 每日统计
esp_err_t history_update_daily_production(uint32_t seconds);
esp_err_t history_increment_daily_flush(void);
esp_err_t history_update_daily_tds(float tds_in, float tds_out);
esp_err_t history_get_today_stats(daily_stats_t *stats);
uint32_t history_get_recent_stats(daily_stats_t *stats, uint32_t max_days);

// 周期保存（脏数据 + 节流）
bool history_periodic_save(uint32_t min_interval_sec);
```

**NVS 脏数据节流机制**:

历史记录的写入采用**脏标志 + 可配置节流**策略，避免每次事件都写 Flash，延长 NVS 寿命：

1. **添加记录时**：`history_add_record()` 只标记 `ctx.history_dirty = true`，不立即写 NVS
2. **每日统计更新时**：`history_update_daily_production()`、`history_increment_daily_flush()`、`history_update_daily_tds()` 只标记 `ctx.daily_stats_dirty = true`，不立即写 NVS
3. **周期保存**：由 FSM 监控任务每 N 秒（可配置，默认 2 小时）调用 `history_periodic_save()` 统一写入
4. **跨天处理**：日期切换时检查 `has_data`（制水时间 > 0 或冲洗次数 > 0 或 TDS 采样 > 0），无数据则跳过保存并重置统计，避免写入空的每日记录

**优化效果**：默认 2 小时间隔下，每个 NVS 命名空间每日最多写入 12 次，NVS 寿命估算约 23 年。

---

## 数据流图

### 制水流程数据流

```
┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│  压力开关   │───►│    GPIO     │───►│   状态机    │
│  (输入)     │    │   驱动      │    │    (FSM)    │
└─────────────┘    └─────────────┘    └──────┬──────┘
                                            │
                    ┌─────────────┐         │
                    │  TDS传感器  │◄────────┤
                    │  (ADC输入)  │         │
                    └──────┬──────┘         │
                           │                │
                           ▼                ▼
                    ┌─────────────┐    ┌─────────────┐
                    │  滤芯管理   │    │   输出控制   │
                    └─────────────┘    └──────┬──────┘
                                             │
                    ┌─────────────┐         │
                    │ MQTT客户端  │◄────────┤
                    │ Web服务器   │         │
                    └─────────────┘         │
                                             │
                    ┌─────────────┐         │
                    │  电磁阀/泵  │◄────────┘
                    │  (GPIO输出) │
                    └─────────────┘
```

### 配置流程数据流

```
┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│   用户操作   │───►│   Web API   │───►│ 配置管理器   │
│  (网页/MQTT) │    │             │    │    (NVS)    │
└─────────────┘    └─────────────┘    └──────┬──────┘
                                              │
                              ┌───────────────┤
                              ▼               ▼
                       ┌─────────────┐ ┌─────────────┐
                       │  WiFi模块   │ │  MQTT模块   │
                       │             │ │             │
                       │ 加载配置    │ │ 加载配置    │
                       │ 连接网络    │ │ 连接Broker  │
                       └─────────────┘ └─────────────┘
```

---

## API接口文档

### 获取系统状态

```http
GET /api/status
```

**响应**:
```json
{
  "state": "制水中",
  "tds_in": 150.5,
  "tds_out": 8.2,
  "rate": 94.5,
  "filters": [...],
  "totalWater": 1523,
  "leak": false,
  "wifiState": "已连接",
  "ssid": "MyWiFi",
  "ip": "192.168.1.100",
  "cycles": 156,
  "flushes": 156,
  "prodTime": 46800,
  "uptime": 86400
}
```

### 控制命令

```http
POST /api/control
Content-Type: application/json

{"action": "start"}
```

**action参数**:
- `start_production`: 开始制水
- `normal_flush`: 常规冲洗
- `pure_flush`: 纯水洗膜
- `standby`: 切换到待机
- `shutdown`: 停止（进入停止状态）
- `reset`: 系统复位

### WiFi配置

**扫描网络**:
```http
GET /api/wifi/scan
```

**保存配置**:
```http
POST /api/wifi
Content-Type: application/json

{"ssid": "WiFi名称", "password": "密码"}
```

### MQTT配置

**获取配置**:
```http
GET /api/mqtt/config
```

**保存配置**:
```http
POST /api/mqtt/config
Content-Type: application/json

{
  "enabled": true,
  "broker": "mqtt://homeassistant.local:1883",
  "user": "用户名",
  "password": "密码",
  "prefix": "water-purifier"
}
```

---

## 配置存储

### NVS键值映射

| 键名 | 类型 | 命名空间 | 说明 |
|------|------|---------|------|
| `wifi_ssid` | string | water_purifier | WiFi SSID |
| `wifi_pass` | string | water_purifier | WiFi密码 |
| `mqtt_en` | u8 | water_purifier | MQTT启用 |
| `mqtt_broker` | string | water_purifier | MQTT Broker |
| `mqtt_user` | string | water_purifier | MQTT用户名 |
| `mqtt_pass` | string | water_purifier | MQTT密码 |
| `mqtt_topic` | string | water_purifier | MQTT主题 |
| `ro_mem` | u8 | water_purifier | RO膜类型 (0=50G,1=75G,2=100G,3=200G,4=400G) |
| `pump` | u8 | water_purifier | 泵类型 (0=△50G~5=△400G) |
| `tank` | u8 | water_purifier | 压力桶大小 (0=3G,1=3.2G,2=4G,3=6G,4=10G) |
| `flush_dur` | u32 | water_purifier | 冲洗时间（秒），默认30 |
| `prod_timeout` | u32 | water_purifier | 制水超时（秒），默认10800 |
| `leak_confirm` | u32 | water_purifier | 漏水确认时间（秒），默认5 |
| `nflush_dur` | u32 | water_purifier | 常规冲洗时间（秒），默认30 |
| `pflush_dur` | u32 | water_purifier | 纯水洗膜时间（秒），默认20 |
| `short_prod` | u32 | water_purifier | 短制水判断阈值（秒），默认180 |
| `wh_vopn` | u32 | water_purifier | 水锤开阀延时（毫秒），默认1000 |
| `wh_pstp` | u32 | water_purifier | 水锤停泵延时（毫秒），默认1000 |
| `wh_vcls` | u32 | water_purifier | 水锤关阀延时（毫秒），默认500 |
| `relay_lvl` | u8 | water_purifier | 继电器电平 (0=低,1=高) |
| `save_intv` | u16 | water_purifier | 运行数据保存间隔（分钟），10/60/120/360/720/1440，默认120 |
| `tds_in_th` | u32 | water_purifier | 进水TDS阈值 |
| `tds_out_th` | u32 | water_purifier | 出水TDS阈值 |
| `tds_in_off` | i32 | water_purifier | 进水TDS校准偏移（×100） |
| `tds_out_off` | i32 | water_purifier | 出水TDS校准偏移（×100） |
| `tds_in_scale` | i32 | water_purifier | 进水TDS校准比例（×10000） |
| `tds_out_scale` | i32 | water_purifier | 出水TDS校准比例（×10000） |
| `filter_cap` | u32 | water_purifier | 滤芯容量 |
| `web_port` | u16 | water_purifier | Web端口 |
| `web_auth` | u8 | water_purifier | Web认证启用 |
| `web_user` | string | water_purifier | Web用户名 |
| `web_pass` | string | water_purifier | Web密码 |
| `ssid` | string | wifi | WiFi SSID |
| `pass` | string | wifi | WiFi密码 |
| `prod_cycles` | u32 | wp_rt | 制水次数 |
| `flush_cycles` | u32 | wp_rt | 冲洗次数 |
| `prod_time` | u64 | wp_rt | 总制水时间（秒） |
| `flush_time` | u64 | wp_rt | 总冲洗时间（秒） |
| `total_water` | u32 | wp_rt | 总用水量（升） |

---

## 内存使用

### Flash使用

| 分区 | 大小 | 说明 |
|------|------|------|
| Bootloader | 28KB | 系统引导 |
| NVS | 24KB | 配置存储 |
| PHY Init | 4KB | PHY初始化数据 |
| App | ~1MB | 应用程序 |
| **固件大小** | ~910KB | 编译后的二进制 |
| **剩余空间** | ~2.9MB | 未使用 |

### RAM使用

| 模块 | 估算使用 | 说明 |
|------|---------|------|
| WiFi协议栈 | ~100KB | WiFi驱动 |
| HTTP服务器 | ~30KB | Web服务 |
| MQTT客户端 | ~20KB | MQTT协议 |
| 状态机任务 | ~16KB | FSM任务栈(4096) |
| TDS传感器任务 | ~12KB | TDS任务栈(3072) |
| 配置管理 | ~2KB | 配置数据 |
| **总使用** | ~180KB | - |
| **可用** | ~220KB | - |

### 优化建议

1. **减少日志级别**: 生产环境使用WARN级别
2. **优化HTML**: 压缩Web界面HTML字符串
3. **减少缓冲区**: 适当减小HTTP/TCP缓冲区大小

---

## 开发指南

### 添加新传感器

1. 在传感器模块头文件中定义传感器ID和数据结构
2. 在实现文件中添加采集函数
3. 在 `main.c` 中初始化传感器
4. 在 `web_server.c` 中添加API接口

### 添加新控制命令

1. 在 `water_purifier_fsm.h` 中定义新事件
2. 在状态机中添加事件处理
3. 在 `web_server.c` 的API中添加命令
4. 在前端JavaScript中添加按钮

### 修改配置参数

1. 在 `config_manager.h` 的 `system_config_t` 中添加字段
2. 在 `config_manager.c` 中添加NVS读写
3. 在 `web_server.c` 的API中添加字段
4. 在管理页面添加UI控件

---

## 故障排除

### 编译问题

**错误**: `collect2: error: ld returned 1 exit status`
- **原因**: 分区表太小
- **解决**: 使用 `partitions_singleapp_large.csv`

**错误**: `fatal error: xxx.h: No such file or directory`
- **原因**: 头文件路径错误
- **解决**: 检查 `CMakeLists.txt` 中的 `INCLUDE_DIRS`

### 运行时问题

**问题**: WiFi无法连接
- **检查**: SSID和密码是否正确
- **检查**: 路由器是否支持2.4GHz
- **解决**: 使用AP模式重新配置

**问题**: MQTT无法连接
- **检查**: Broker地址格式 (mqtt://host:port)
- **检查**: 网络连接状态
- **检查**: DNS解析

**问题**: 状态机卡死
- **检查**: 压力开关状态
- **检查**: 超时时间设置
- **解决**: 发送复位事件

**问题**: COM端口拒绝访问
- **原因**: 串口被其他程序占用
- **解决**: 关闭串口监视器，按住BOOT键按RESET进入下载模式

---

## 版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| 1.0.0 | 2025-02 | 初始版本，75G RO膜 + 3G压力桶 |
| 1.0.1 | 2026-04 | 支持可配置RO膜/泵/压力桶，双维度滤芯管理，移除排空功能 |
| 1.0.2 | 2026-04 | 双阶段冲洗（常规冲洗+纯水洗膜），水锤效应控制，短制水判断，缺水自动恢复至待机，NVS脏数据节流写入，滤芯日历寿命使用NTP墙钟，运行数据保存间隔可配置，mDNS改为.lan，TDS校准参数持久化，NVS运行时命名空间缩短为wp_rt，TDS报警边沿触发日志 |
| 1.0.3 | 2026-04 | FSM_STATE_FAULT→STOP（故障改停止），制水水锤改为开进水阀→延时→开泵（废水阀关），常规冲洗水锤自适应（检测进水阀GPIO），冲洗时间统计修复（常规冲洗+纯水洗膜合并统计），纯水洗膜入口区分正常过渡/网页直接进入，网页面板6按钮（制水/冲洗/反冲洗/复位/待机/停止），水锤UI按启动/过渡阶段分组显示，API action参数重命名（start→start_production等） |

---

## 附录

### 压力单位换算

```
1 MPa = 10 bar = 145 psi
0.1 MPa = 1 bar = 14.5 psi
```

### TDS值说明

| TDS值 | 水质 | 适用场景 |
|-------|------|---------|
| 0-50 | 纯净水 | 直饮 |
| 50-100 | 极软水 | 烹饪 |
| 100-300 | 软水 | 一般用途 |
| 300-600 | 中等硬度 |  |
| >600 | 硬水 | 不建议饮用 |

### RO膜通量与制水速率

| RO膜 | 制水速率 | 汇通实际参数 |
|------|---------|------------|
| 50G  | 7.8 L/h  | 0.13 L/min × 60 |
| 75G  | 12.0 L/h | 0.20 L/min × 60 |
| 100G | 15.6 L/h | 0.26 L/min × 60 |
| 200G | 31.2 L/h | 0.52 L/min × 60 |
| 400G | 62.4 L/h | 1.04 L/min × 60 |

### 参考资料

- [ESP32-C3 技术手册](https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_cn.pdf)
- [ESP-IDF 编程指南](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/)
- [MQTT 协议规范](https://mqtt.org/mqtt-specification/)
- [Home Assistant MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/)
