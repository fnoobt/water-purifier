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
- [开发指南](#开发指南)
- [故障排除](#故障排除)
- [版本历史](#版本历史)
- [附录](#附录)

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

### 技术栈
- **框架**: ESP-IDF v6.0.0
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
│   │   ├── board_pinout.h         # ESP32-C3引脚定义图
│   │   ├── gpio_config.h          # GPIO引脚定义和电气参数
│   │   ├── gpio_driver.h          # GPIO驱动接口
│   │   ├── filter_manager.h       # 滤芯管理接口（5级滤芯）
│   │   ├── tds_sensor.h           # TDS传感器接口
│   │   ├── water_purifier_fsm.h   # 状态机接口（8状态FSM）
│   │   ├── wifi_manager.h         # WiFi管理器接口
│   │   ├── mqtt_client.h          # MQTT客户端接口
│   │   ├── app_mqtt.h             # MQTT客户端别名（app_前缀，避免与ESP-IDF冲突）
│   │   ├── app_mqtt_public.h      # MQTT公开接口
│   │   ├── web_server.h           # Web服务器接口
│   │   ├── config_manager.h       # 配置管理器接口
│   │   ├── ota_update.h           # OTA升级接口
│   │   ├── pm_manager.h           # 电源管理接口
│   │   └── history_logger.h       # 历史记录接口
│   │
│   ├── main.c                     # 主程序入口
│   ├── water_purifier_fsm.c       # 状态机实现（8状态FSM）
│   ├── tds_sensor.c               # TDS传感器实现
│   ├── filter_manager.c           # 滤芯管理实现
│   ├── gpio_driver.c              # GPIO驱动实现
│   ├── wifi_manager.c             # WiFi管理器（STA/AP+mDNS）
│   ├── mqtt_client.c              # MQTT客户端（HA发现）
│   ├── web_server.c               # Web服务器（HTML/CSS/JS嵌入）
│   ├── config_manager.c           # 配置管理器（NVS存储）
│   ├── ota_update.c               # OTA固件升级
│   ├── pm_manager.c               # 电源管理（CPU频率/TX功率/堆监控）
│   ├── history_logger.c           # 历史记录+每日统计
│   │
│   ├── CMakeLists.txt             # 构建配置
│   └── idf_component.yml          # 组件依赖（mDNS）
│
├── partitions.csv                 # 自定义分区表
├── sdkconfig                      # ESP-IDF配置
├── CMakeLists.txt                 # 项目构建
├── README.md                      # 项目简介
├── DOCUMENTATION.md               # 本文档
└── CLAUDE.md                      # 开发指南（AI辅助）
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
   filter_mgr_init()       -> 滤芯管理初始化
   tds_sensor_start()      -> TDS测量任务（1秒周期）
4. fsm_init()              -> 状态机初始化
   fsm_start()             -> FSM任务（100ms周期）
5. history_logger_init()   -> 历史记录初始化
6. wifi_manager_init()     -> WiFi初始化
   wifi_manager_start()    -> STA/AP模式
7. mqtt_client_init()      -> MQTT客户端初始化
8. ota_update_init()       -> OTA模块初始化
9. web_server_init()       -> HTTP服务器初始化
   web_server_start()      -> 启动Web服务
10. monitor_task           -> 监控任务（30秒周期）
    pm_manager_check_heap()-> 堆内存检查
```

**回调函数**:
- `fsm_state_callback()`: 状态变化时发布MQTT状态更新
- `wifi_state_callback()`: WiFi连接后启动MQTT和发送HA发现配置

---

### 2. GPIO驱动 (gpio_driver.c/h)

**职责**: GPIO引脚控制、输入防抖、输出控制

**引脚分配**:

| GPIO | 设备 | 方向 | 说明 |
|------|------|------|------|
| GPIO2 | 进水TDS | ADC | ADC1_CH2，0~2.3V -> 0~1000ppm |
| GPIO3 | 出水TDS | ADC | ADC1_CH3 |
| GPIO6 | 进水低压开关 | 输入 | 低电平=有水 |
| GPIO7 | 压力桶压力开关 | 输入 | 低电平（闭合）=需要制水 |
| GPIO10 | 漏水检测 | 输入 | 低电平=漏水报警 |
| GPIO0 | 进水电磁阀 | 输出 | 24V继电器，低电平触发 |
| GPIO1 | 废水电磁阀 | 输出 | 24V继电器，低电平触发 |
| GPIO4 | 回水电磁阀 | 输出 | 24V继电器，低电平触发 |
| GPIO5 | 增压泵 | 输出 | 24V继电器，低电平触发 |
| GPIO12 | LED D4 | 输出 | 主状态指示 |
| GPIO13 | LED D5 | 输出 | 次状态指示 |

> **注意**: ESP32-C3的ADC1只有通道0-4（GPIO0~GPIO4）。当前TDS配置使用GPIO2(ADC1_CH2)和GPIO3(ADC1_CH3)，与WiFi兼容。GPIO5用于增压泵控制（继电器输出），不涉及ADC。

**输入防抖**: 50ms去抖时间，首次读取立即返回初始稳定值。

**输出互斥锁**: `output_state` 结构体受 `output_mutex` 保护，防止跨任务访问竞态。

**主要函数**:
```c
esp_err_t gpio_driver_init_inputs(void);
esp_err_t gpio_driver_init_outputs(void);
esp_err_t gpio_driver_init_leds(void);

// 读取状态
bool gpio_driver_read_low_pressure(void);
bool gpio_driver_read_tank_pressure(void);
bool gpio_driver_read_water_leak(void);

// 控制输出
esp_err_t gpio_driver_set_inlet_valve(bool state);
esp_err_t gpio_driver_set_waste_valve(bool state);
esp_err_t gpio_driver_set_return_valve(bool state);
esp_err_t gpio_driver_set_boost_pump(bool state);
esp_err_t gpio_driver_set_led1(bool state);
esp_err_t gpio_driver_set_led2(bool state);
esp_err_t gpio_driver_emergency_stop(void);

esp_err_t gpio_driver_set_relay_trigger_level(uint8_t level);
uint8_t gpio_driver_get_relay_trigger_level(void);
esp_err_t gpio_driver_get_output_states(bool *inlet, bool *waste, bool *return, bool *pump);
```

---

### 3. TDS传感器 (tds_sensor.c/h)

**职责**: TDS值采集、滤波、校准、报警检测

**工作原理**:
```
ADC原始值 → 电压转换(mV) → TDS计算 → 温度补偿 → 校准 → 报警检测
```

**TDS计算公式**: `TDS = voltage_mV / 2300 * 1000`（DFRobot Gravity规格）

**ADC采样**: 16次采样，去除最大最小值后平均，采样间隔500μs。

**温度补偿**: `TDS_compensated = TDS / (1 + 0.02 × (temp - 25))`，补偿系数2%/°C。

**报警迟滞**: 5%迟滞区间，解除阈值 = 报警阈值 × 0.95，防止边界频繁切换。

**除零保护**: 温度补偿分母强制 ≥ 0.5，EC转换分母 ≥ 0.01。

**主要函数**:
```c
esp_err_t tds_sensor_init(void);
esp_err_t tds_sensor_start(void);
esp_err_t tds_sensor_stop(void);

// 测量
esp_err_t tds_sensor_measure(tds_sensor_id_t id, tds_measurement_t *result);
esp_err_t tds_sensor_measure_dual(tds_dual_measurement_t *dual);
esp_err_t tds_sensor_get_latest(tds_sensor_id_t id, tds_measurement_t *result);
esp_err_t tds_sensor_get_latest_dual(tds_dual_measurement_t *dual);

// 校准
esp_err_t tds_sensor_calibrate(tds_sensor_id_t id, float standard_value);
esp_err_t tds_sensor_set_calibration(tds_sensor_id_t id, const tds_calibration_t *cal);
esp_err_t tds_sensor_reset_calibration(tds_sensor_id_t id);

// 报警
esp_err_t tds_sensor_set_alarm_threshold(tds_sensor_id_t id, float threshold);
bool tds_sensor_is_alarm(tds_sensor_id_t id);
```

**数据结构**:
```c
typedef struct {
    float tds_value;           // TDS值 (ppm)
    float ec_value;            // 电导率 (μS/cm)
    float voltage;             // 原始电压 (mV)
    float temperature;         // 温度 (°C)
    bool valid;                // 数据有效性
    uint64_t timestamp;        // 时间戳 (微秒)
} tds_measurement_t;
```

**TDS校准步骤**:
1. 将两个TDS传感器放入同一杯标准液中
2. 在管理页找到"TDS校准"区域
3. 输入标准液TDS值（ppm）
4. 点击对应传感器的校准按钮
5. 校准结果自动持久化到Flash

---

### 4. 滤芯管理 (filter_manager.c/h)

**职责**: 5级滤芯寿命管理、用水量统计、制水速率、NVS持久化

**NVS命名空间**: `wp_filters`

**五级滤芯**:

| 滤芯 | 默认水量 | 默认时间 | 建议更换周期 |
|------|---------|---------|-------------|
| PP棉 | 3000升 | 2190小时（3个月） | 3个月 |
| 颗粒活性炭 | 5000升 | 4380小时（6个月） | 6个月 |
| 压缩活性炭 | 5000升 | 4380小时（6个月） | 6个月 |
| RO膜 | 10000升 | 17520小时（24个月） | 24个月 |
| 后置活性炭 | 4000升 | 6570小时（9个月） | 9个月 |

**寿命计算**: `effective_percentage = min(water_pct, time_pct)`，低于10%触发更换提醒。

**水量计量（区分制水/冲洗状态，分两组累计）**:

| 滤芯组 | 成员 | 计量来源 | 状态依赖 |
|--------|------|---------|---------|
| 前三级（泵前） | PP棉、颗粒碳、压缩碳 | 系统总进水量 | 制水=RO通量+废水流量，冲洗=泵流量 |
| 后两级（泵后） | RO膜、后置炭 | 纯水产量 | 仅制水=RO通量，冲洗/纯水=0 |

**首页显示**：
- **总用水量**：前三级过水量（含制水+冲洗，系统从水源抽取的总量）
- **总制水量**：后两级纯水量（仅PRODUCTION状态产出，不含冲洗）

**Flash写入策略**:
- `filter_mgr_update_water_usage()` — 仅标记脏，不调用NVS
- `filter_mgr_periodic_save()` — FSM周期任务调用，检查脏标志后写入
- `filter_mgr_reset_filter()` / `filter_mgr_set_filter_capacity_ex()` — 用户操作，立即写入
- `filter_mgr_set_all_filter_capacity()` — 批量设置5级滤芯，一次NVS写入
- `filter_mgr_set_all_filter_times()` — 批量设置时间寿命，一次NVS写入

**日历寿命**: `install_time` 和 `last_reset_time` 使用 `time(NULL)` Unix秒。NTP同步后自动回推安装时间，确保日历寿命准确。NTP未同步时使用启动时间估算。

**时间计算修复**: NTP未同步时使用启动后经过的秒数（`esp_timer_get_time()/1e6`），而非错误地将微秒与秒级时间戳相减。

**主要函数**:
```c
esp_err_t filter_mgr_init(void);
esp_err_t filter_mgr_update_water_usage(float liters);
uint32_t filter_mgr_get_total_water_usage(void);
bool filter_mgr_periodic_save(void);

esp_err_t filter_mgr_get_filters_status(filters_status_t *status);
esp_err_t filter_mgr_get_filter_info(filter_type_t type, filter_info_t *info);
esp_err_t filter_mgr_reset_filter(filter_type_t type);
esp_err_t filter_mgr_reset_all_filters(void);
esp_err_t filter_mgr_set_filter_capacity_ex(filter_type_t type, uint32_t liters);
esp_err_t filter_mgr_set_all_filter_capacity(const uint32_t capacities[FILTER_COUNT]);
esp_err_t filter_mgr_set_all_filter_times(const uint32_t time_hours[FILTER_COUNT]);
bool filter_mgr_any_filter_needs_replacement(void);

esp_err_t filter_mgr_set_production_rate(float lph);
float filter_mgr_get_production_rate(void);
```

---

### 5. 状态机 (water_purifier_fsm.c/h)

**职责**: 净水器工作流程控制

**状态定义**:

| 状态 | 说明 | 触发条件 |
|------|------|---------|
| STANDBY | 待机 | 初始状态/冲洗完成/手动复位/网页切换 |
| PRODUCTION | 制水中 | 需要制水/网页启动 |
| TANK_FULL | 水满（瞬时） | 压力桶满 |
| NORMAL_FLUSH | 常规冲洗 | 水满后自动/网页切换 |
| PURE_FLUSH | 纯水洗膜 | 常规冲洗完成/网页切换 |
| WATER_SHORTAGE | 缺水 | 进水压力不足 |
| LEAK_ALARM | 漏水报警 | 漏水检测 |
| STOP | 停止 | 制水超时/网页停止 |

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
           │ 手动复位
           ▼
    ┌─────────┐
    │ STANDBY │
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
| STANDBY | 全部关闭 | 有水+需制水+!standby_manual→PRODUCTION，无水→SHORTAGE，漏水→LEAK |
| PRODUCTION | 进水阀+增压泵（开阀→延时→泵，废水阀关） | 压力桶满→TANK_FULL，缺水→SHORTAGE，漏水→LEAK，超时→STOP |
| TANK_FULL | 无（瞬时过渡） | 无条件→NORMAL_FLUSH |
| NORMAL_FLUSH | 进水阀+废水阀+增压泵（自适应水锤流程） | 完成→PURE_FLUSH，缺水→SHORTAGE，漏水→LEAK |
| PURE_FLUSH | 回水阀+废水阀（停泵→延时→关进水阀→延时→开回水阀+废水阀） | 完成→STANDBY（设standby_manual），用户用水→STANDBY，缺水→SHORTAGE，漏水→LEAK |
| WATER_SHORTAGE | 全部关闭 | 有水→STANDBY，漏水→LEAK |
| LEAK_ALARM | 全部关闭 | 手动复位→STANDBY |
| STOP | 全部关闭 | 手动复位→STANDBY |

**水锤控制**:
- **制水**: 开进水阀 → 延时 → 开增压泵（废水阀关闭）
- **常规冲洗**: 检测进水阀是否已打开，未打开则先开阀延时再启泵
- **常规冲洗→纯水洗膜**: 停泵 → 延时 → 关进水阀 → 延时 → 开回水阀+废水阀

**短制水处理**: 制水<3分钟时，冲洗自动缩短（10s常规+5s纯水）。

**纯水中断**: 纯水冲洗期间若用户用水（压力开关闭合），且处于FLUSH_PHASE_RUNNING阶段，立即转入待机。

**待机行为**: 冲洗完成后设 `standby_manual=true`，压力桶缺水时清除标志自动恢复制水。

**换芯冲洗**: 网页"换芯冲洗"触发仅普通冲洗持续20分钟（不切纯水，不计统计），可提前通过"待机"中断。

**停止历史**: 循环缓冲区16条记录，索引自动防止溢出（>256时回绕）。

**事件驱动**: FSM通过FreeRTOS事件队列接收外部命令，`fsm_force_standby()` 改用事件队列而非直接修改状态，避免跨线程竞态。

**运行时数据**:
```c
typedef struct {
    uint32_t total_production_cycles;
    uint32_t total_flush_cycles;
    uint64_t total_production_time_sec;
    uint64_t total_flush_time_sec;
    stop_type_t last_stop_type;
    uint64_t last_stop_time;
    uint64_t last_flush_time;
    bool low_pressure_occurred;
} fsm_runtime_data_t;
```

**制水速率映射**（汇通Vontron实际参数）:
```
50G  -> 7.8 L/h   (0.13 L/min)
75G  -> 12.0 L/h  (0.20 L/min)
100G -> 15.6 L/h  (0.26 L/min)
200G -> 31.2 L/h  (0.52 L/min)
400G -> 62.4 L/h  (1.04 L/min)
```

**周期保存**: FSM任务循环中按配置的保存间隔（默认120分钟，可选10/60/120/360/720/1440分钟）统一调用 `config_manager_periodic_save_all()`，协调所有模块脏数据保存。

---

### 6. WiFi管理器 (wifi_manager.c/h)

**职责**: WiFi连接管理、AP配置模式、mDNS域名服务、自动重连

**工作模式**:
- **STA模式**: 连接到配置的WiFi网络
- **AP+STA模式**: 无配置时启动AP热点配网
- **mDNS**: 注册 `http://waterpurifier.local`

**自动重连机制**:
- WiFi断开后启动独立FreeRTOS任务执行指数退避重连（1s→2s→4s→8s→16s→30s）
- 约5分钟（20次尝试）后仍失败则进入AP模式并退出任务
- 路由器恢复后下次WiFi断开时自动重建重连任务，避免无限循环导致设备不稳定
- 重连任务不阻塞事件处理系统，重连成功后自动停止AP模式

**主要函数**:
```c
esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_stop(void);

esp_err_t wifi_manager_start_ap_mode(void);
esp_err_t wifi_manager_stop_ap_mode(void);

wifi_state_t wifi_manager_get_state(void);
bool wifi_manager_is_connected(void);
bool wifi_manager_is_ap_mode(void);
esp_err_t wifi_manager_get_ip(char *ip_str, size_t buffer_size);
int8_t wifi_manager_get_rssi(void);

esp_err_t wifi_manager_set_config(const char *ssid, const char *password);
esp_err_t wifi_manager_save_config(void);
```

**AP配置模式**:
- AP SSID: `WaterPurifier-XXXX`（基于MAC后4位）
- AP密码: `12345678`
- 配置URL: `http://192.168.4.1`

---

### 7. MQTT客户端 (mqtt_client.c/h)

**职责**: MQTT通信、Home Assistant集成

**主要函数**:
```c
esp_err_t mqtt_client_init(void);
esp_err_t mqtt_client_start(void);
bool mqtt_client_is_connected(void);

esp_err_t mqtt_publish_purifier_status(void);
esp_err_t mqtt_publish_tds_value(void);
esp_err_t mqtt_send_ha_discovery(void);
esp_err_t mqtt_client_set_config(const mqtt_config_t *config);
```

**MQTT主题结构**:
```
# 状态发布
water-purifier/status              # 系统状态
water-purifier/tds/in              # 进水TDS值
water-purifier/tds/out             # 出水TDS值
water-purifier/tds/reduction_rate  # TDS去除率

# 控制订阅
water-purifier/set/state           # 状态控制
water-purifier/set/flush           # 冲洗控制
```

**Home Assistant发现实体**:

| 实体类型 | 实体ID | 说明 |
|---------|--------|------|
| Sensor | `water_purifier_tds_in` | 进水TDS值 (ppm) |
| Sensor | `water_purifier_tds_out` | 出水TDS值 (ppm) |
| Sensor | `water_purifier_tds_reduction_rate` | TDS去除率 (%) |
| Sensor | `water_purifier_rssi` | WiFi信号强度 (dBm) |
| Sensor | `water_purifier_cycles` | 总制水次数 |
| Switch | `water_purifier_production` | 制水控制 |
| Switch | `water_purifier_flush` | 冲洗控制 |
| Binary Sensor | `water_purifier_high_pressure` | 高压开关状态 |
| Binary Sensor | `water_purifier_low_pressure` | 低压开关状态 |
| Binary Sensor | `water_purifier_tank_pressure` | 压力桶开关状态 |
| Binary Sensor | `water_purifier_leak` | 漏水检测状态 |

---

### 8. Web服务器 (web_server.c/h)

**职责**: HTTP服务器、Web控制界面、RESTful API

**主要功能**:
- 首页（只读监控，30秒自动刷新）
- 管理页（配置控制）
- 日志页（实时串口日志，SSE推送，SNTP同步后显示真实时间戳）
- RESTful API接口

**API接口**:

| 接口 | 方法 | 功能 |
|------|------|------|
| `/` | GET | 首页（只读监控） |
| `/admin` | GET | 管理页面 |
| `/api/status` | GET | 获取系统状态（JSON） |
| `/api/control` | POST | 发送控制命令 |
| `/api/config` | GET/POST | 获取/保存系统配置 |
| `/api/config/hardware` | POST | 保存硬件配置 |
| `/api/wifi/scan` | GET | 扫描WiFi网络 |
| `/api/wifi` | POST | 保存WiFi配置 |
| `/api/filter/reset` | POST | 重置滤芯 |
| `/api/filter/capacity` | POST | 设置滤芯容量（批量） |
| `/api/tds/calibrate` | POST | TDS传感器校准 |
| `/api/mqtt/config` | GET/POST | MQTT配置 |
| `/ota` | GET | OTA固件升级页面（独立页面） |
| `/api/ota/status` | GET | OTA升级状态 |
| `/api/ota/update` | POST | 上传固件OTA升级 |
| `/api/ota/factory` | POST | 恢复出厂固件（切换到factory分区并重启） |
| `/api/ota/rollback` | POST | 回滚到上一OTA固件（ota_0↔ota_1切换） |
| `/logs` | GET | 实时串口日志页面 |
| `/api/logs/debug` | GET | 日志缓冲区内容（HTML格式） |
| `/api/logs` | GET | 日志缓冲区内容（JSON数组） |

**控制命令**:
```json
{"action": "start_production"}  // 制水
{"action": "normal_flush"}      // 常规冲洗
{"action": "pure_flush"}        // 纯水洗膜
{"action": "filter_flush"}      // 换芯冲洗
{"action": "standby"}           // 待机
{"action": "shutdown"}          // 停止
{"action": "reset"}             // 复位
```

**状态响应示例**:
```json
{
  "state": "制水中",
  "tds_in": 150.5, "tds_out": 8.2, "rate": 94.5,
  "filters": [
    {"name":"PP棉","waterPct":85,"timePct":90,"effPct":85,"total":3000},
    {"name":"RO膜","waterPct":91,"timePct":88,"effPct":88,"total":8000}
  ],
  "totalWater": 1523, "prodWater": 432, "leak": false,
  "wifiState": "已连接", "ssid": "MyWiFi", "ip": "192.168.1.100",
  "cycles": 156, "flushes": 156, "prodTime": 46800, "uptime": 86400
}
```

**Web控制界面结构**:

| 页面 | 路径 | 功能 |
|------|------|------|
| 首页 | `/` | 只读监控，30秒自动刷新 |
| 管理页 | `/admin` | 配置控制 |
| OTA页 | `/ota` | 固件升级、恢复出厂、回滚上一版本 |
| 日志页 | `/logs` | 实时串口日志（SSE流式推送） |

**首页功能**:
- 系统状态（待机/制水中/冲洗中等）
- 进出水TDS值（1位小数）及去除率
- 五级滤芯寿命
- 总用水量 + 总制水量（分两行显示）
- 漏水状态
- WiFi信号强度

**管理页功能**:

| 区域 | 功能 |
|------|------|
| 控制面板 | 制水、常规冲洗、纯水洗膜、换芯冲洗、复位、待机、停止 |
| 硬件配置 | RO膜通量、增压泵、压力桶选择 |
| 系统配置 | 冲洗时间、制水超时、漏水确认、水锤延时、继电器电平 |
| 滤芯管理 | 五级滤芯水量+时间寿命设置，单独重置 |
| TDS校准 | 进水/出水TDS传感器一点校准 |
| WiFi配置 | 扫描网络、保存配置 |
| MQTT配置 | Broker地址、用户名、密码 |
| OTA升级 | 固件上传、进度显示、恢复出厂固件、回滚上一版本 |
| 日志查看 | 实时串口日志，远程监控设备状态 |

**访问方式**: `http://<设备IP>` 或 `http://waterpurifier.local`（mDNS）

---

### 9. 配置管理器 (config_manager.c/h)

**职责**: 配置持久化存储（NVS）

**配置结构**:
```c
typedef struct {
    // WiFi/MQTT配置
    char wifi_ssid[33], wifi_password[65];
    bool mqtt_enabled;
    char mqtt_broker[128], mqtt_username[64], mqtt_password[64], mqtt_topic_prefix[64];

    // 硬件配置
    uint8_t ro_membrane_type;     // 0=50G, 1=75G, 2=100G, 3=200G, 4=400G
    uint8_t pump_type;            // 0=△50G ~ 5=△400G
    uint8_t tank_size;            // 0=3G, 1=3.2G, 2=4G, 3=6G, 4=10G

    // 系统参数
    uint32_t flush_duration_sec, production_timeout_sec, leak_confirm_time_sec;
    uint16_t runtime_save_interval_min;   // 10/60/120/360/720/1440

    // 冲洗参数
    uint32_t normal_flush_duration_sec, pure_flush_duration_sec;
    uint32_t filter_flush_duration_sec, short_prod_threshold_sec;
    uint32_t water_hammer_valve_open_delay_ms;
    uint32_t water_hammer_pump_stop_delay_ms;
    uint32_t water_hammer_valve_close_delay_ms;

    // TDS/继电器/滤芯/Web配置
    uint8_t relay_trigger_level;
    float tds_inlet_threshold, tds_outlet_threshold;
    float tds_calibration_offset[2], tds_calibration_scale[2];
    uint32_t filter_capacity_liters;
    uint16_t web_port; bool web_auth_enabled;
    char web_username[32], web_password[32];
} system_config_t;
```

**验证机制**: `config_manager_validate()` 检查所有配置参数的有效范围，`config_manager_set_config()` 先验证后更新。

**NVS字符串终止保护**: `nvs_get_str()` 读取后强制添加 `\0` 终止符，防止NVS数据损坏导致缓冲区溢出。

**统一周期保存**: `config_manager_periodic_save_all()` 协调系统配置、滤芯数据、历史记录三个模块的脏数据批量保存。

**NVS命名空间**:

| 命名空间 | 用途 |
|---------|------|
| `water_purifier` | 系统配置 + WiFi凭证 + MQTT配置 |
| `wp_rt` | 运行数据（制水次数、冲洗次数、时间统计） |
| `wp_filters` | 滤芯数据（水量/时间寿命） |
| `history` | 事件日志（20条循环） |
| `daily_stats` | 每日统计（制水时间、TDS均值） |

---

### 10. OTA升级 (ota_update.c/h)

**职责**: 固件升级、版本检测、回滚保护、分区切换

**工作流程**:
1. `ota_update_begin()`: 获取OTA分区，初始化写入句柄
2. `ota_update_write()`: 流式写入固件（4KB缓冲区），首次接收时解析镜像头做版本检测
3. `ota_update_end()`: 验证镜像，设置启动分区，取消回滚倒计时
4. Reboot: `esp_restart()` 重启进入新固件

**版本检测**: 同版本固件拒绝升级，防止重复刷写。

**边界保护**: `init_buf[512]` 积累固件头部数据，解析ESP镜像头前检查大小有效性，防止异常输入导致缓冲区溢出。

**回滚保护**: OTA成功后调用 `esp_ota_mark_app_valid_cancel_rollback()`，取消回滚倒计时。启动时自动确认待验证固件有效。

**分区切换功能**:
- `ota_update_revert_to_factory()`: 恢复到factory出厂分区
- `ota_update_rollback()`: 回滚到上一OTA固件（ota_0↔ota_1），自动检测目标分区有效性

**主要函数**:
```c
esp_err_t ota_update_init(void);
esp_err_t ota_update_begin(void);
esp_err_t ota_update_write(const uint8_t *data, size_t len);
esp_err_t ota_update_end(void);
void ota_update_abort(void);
ota_state_t ota_update_get_state(void);
uint32_t ota_update_get_bytes_written(void);
const char *ota_update_get_running_version(void);
esp_err_t ota_update_revert_to_factory(void);
esp_err_t ota_update_rollback(void);
```

---

### 11. 电源管理 (pm_manager.c/h)

**职责**: WiFi TX功率调整、堆内存监控（ESP32-C3不支持DFS动态频率）

**ESP32-C3限制**: 该芯片不支持DFS动态频率调节，代码自动检测并跳过相关配置。

**WiFi TX功率**: 滞回窗口机制，RSSI < -55dBm升功率至20dBm，RSSI > -48dBm降至8dBm。

**堆内存监控**:
- < 30KB 输出警告
- < 15KB 自动重启

---

### 12. 历史记录 (history_logger.c/h)

**职责**: 运行事件日志、每日统计、NVS脏数据节流写入

**事件类型**: 状态变化、开始/结束制水、开始/结束冲洗、缺水/恢复、水满、漏水报警、停止/清除、维护提醒、手动控制、系统启动，共14种。

**最大记录数**: 20条（循环覆盖）。

**每日统计**: 制水时间、冲洗次数、进/出水TDS平均值。

**NVS脏数据节流**:
1. `history_add_record()` 仅标记 `history_dirty`，不立即写NVS
2. `history_update_daily_*()` 仅标记 `daily_stats_dirty`
3. `history_periodic_save()` 由FSM周期任务调用，统一写入
4. 跨天处理：有数据才保存昨天的记录，无数据则清除旧key

**NVS提交检查**: 每次 `nvs_commit()` 检查返回值，失败时输出警告日志。

**主要函数**:
```c
esp_err_t history_logger_init(void);
esp_err_t history_logger_deinit(void);

esp_err_t history_add_record(history_event_type_t type, ...);
esp_err_t history_log_state_change(uint8_t from, uint8_t to);
esp_err_t history_log_production_end(uint32_t duration_sec);
esp_err_t history_log_flush_end(uint32_t duration_sec);
esp_err_t history_log_stop(uint8_t stop_type, const char *description);
esp_err_t history_log_leak_alarm(void);

uint32_t history_get_count(void);
uint32_t history_get_records(history_record_t *records, uint32_t max_count);
esp_err_t history_clear_all(void);

esp_err_t history_update_daily_production(uint32_t seconds);
esp_err_t history_increment_daily_flush(void);
esp_err_t history_update_daily_tds(float tds_in, float tds_out);
esp_err_t history_get_today_stats(daily_stats_t *stats);
uint32_t history_get_recent_stats(daily_stats_t *stats, uint32_t max_days);

bool history_periodic_save(uint32_t min_interval_sec);
```

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
                           │                ▼
                    ┌─────────────┐    ┌─────────────┐
                    │  滤芯管理   │    │   输出控制   │
                    │  (NVS存储)  │    └──────┬──────┘
                    └─────────────┘         │
                    ┌─────────────┐         │
                    │ MQTT客户端  │◄────────┤
                    │ Web服务器   │         │
                    └─────────────┘         │
                    ┌─────────────┐         │
                    │  电磁阀/泵  │◄────────┘
                    │  (GPIO输出) │
                    └─────────────┘
```

---

## API接口文档

### 获取系统状态

```http
GET /api/status
```

### 控制命令

```http
POST /api/control
Content-Type: application/json

{"action": "start_production"}
```

**action参数**: `start_production` | `normal_flush` | `pure_flush` | `filter_flush` | `standby` | `shutdown` | `reset`

### WiFi配置

```http
GET /api/wifi/scan
POST /api/wifi
{"ssid": "WiFi名称", "password": "密码"}
```

### MQTT配置

```http
GET /api/mqtt/config
POST /api/mqtt/config
{"enabled": true, "broker": "mqtt://homeassistant.local:1883", "user": "", "password": "", "prefix": "water-purifier"}
```

### OTA分区切换

```http
POST /api/ota/factory
# 恢复出厂固件（切换到factory分区）

POST /api/ota/rollback
# 回滚到上一OTA固件（ota_0↔ota_1）
```

**响应**:
```json
{"status": "success", "message": "已设置启动分区为factory，设备将重启"}
{"status": "error", "message": "目标分区无有效固件"}
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
| `ro_mem` | u8 | water_purifier | RO膜类型 |
| `pump` | u8 | water_purifier | 泵类型 |
| `tank` | u8 | water_purifier | 压力桶大小 |
| `nflush_dur` | u32 | water_purifier | 常规冲洗时间（秒） |
| `pflush_dur` | u32 | water_purifier | 纯水洗膜时间（秒） |
| `fflush_dur` | u32 | water_purifier | 换芯冲洗时间（秒） |
| `wh_vopn` | u32 | water_purifier | 水锤开阀延时（毫秒） |
| `wh_pstp` | u32 | water_purifier | 水锤停泵延时（毫秒） |
| `wh_vcls` | u32 | water_purifier | 水锤关阀延时（毫秒） |
| `tds_in_th` | u32 | water_purifier | 进水TDS阈值 |
| `tds_out_th` | u32 | water_purifier | 出水TDS阈值 |
| `web_port` | u16 | water_purifier | Web端口 |
| `prod_cycles` | u32 | wp_rt | 制水次数 |
| `flush_cycles` | u32 | wp_rt | 冲洗次数 |
| `prod_time` | u64 | wp_rt | 总制水时间（秒） |
| `total_water` | u32 | wp_rt | 总用水量（升，含冲洗） |
| `prod_water` | u32 | wp_rt | 总制水量（升，仅纯水） |
| `f%d_used` | u32 | wp_filters | 各级滤芯已用水量 |
| `f%d_reset` | u32 | wp_filters | 各级滤芯重置时间 |
| `f%d_cap` | u32 | wp_filters | 各级滤芯自定义容量 |
| `f%d_time` | u32 | wp_filters | 各级滤芯时间寿命（小时） |

---

---

## 开发指南

### 添加新传感器

1. 在传感器模块头文件中定义ID和数据结构
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
2. 在 `config_manager.c` 中添加NVS读写和验证
3. 在 `web_server.c` 的API中添加字段
4. 在管理页面添加UI控件

---

## 故障排除

### 编译问题

**错误**: `ld returned 1 exit status`
- **原因**: 分区表太小
- **解决**: 使用 `partitions.csv`（已配置双OTA槽位）

**错误**: `fatal error: xxx.h: No such file or directory`
- **原因**: 头文件路径错误
- **解决**: 检查 `CMakeLists.txt` 中的 `INCLUDE_DIRS`

### 运行时问题

**WiFi无法连接**: 检查SSID/密码，确认2.4GHz，使用AP模式重新配置。

**MQTT无法连接**: 检查Broker地址格式 `mqtt://host:port`，确认网络连通性。

**状态机卡死**: 检查压力开关状态和超时设置，发送复位事件。

**COM端口拒绝访问**: 关闭串口监视器，按住BOOT键按RESET进入下载模式。

**Web服务器启动失败**: `httpd: Config option max_open_sockets is too large` - ESP-IDF v6.0限制了httpd最大socket数（LWIP_MAX_SOCKETS - httpd内部socket = 可用连接数）。已在代码中设置 `max_open_sockets=3`。

---

## 版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| 1.0.0 | 2025-02 | 初始版本，75G RO膜 + 3G压力桶 |
| 1.0.1 | 2026-04 | 可配置RO膜/泵/压力桶，双维度滤芯管理，移除排空功能 |
| 1.0.2 | 2026-04 | 双阶段冲洗，水锤控制，NVS脏数据节流，滤芯日历寿命NTP墙钟 |
| 1.0.3 | 2026-04 | 故障改停止，水锤自适应，冲洗时间统计修复，API重命名 |
| 1.0.4 | 2026-04 | 换芯冲洗，WiFi重连优化，mDNS/SNTP防护 |
| 1.0.5 | 2026-04 | 滤芯管理独立为filter_manager，TDS校准持久化 |
| 1.1.0 | 2026-04 | OTA固件升级功能，待机循环修复 |
| 1.1.1 | 2026-04 | 换芯冲洗20分钟，standby_manual清除优化，FLUSH_PHASE_RUNNING守卫 |
| 1.1.2 | 2026-04 | 滤芯默认容量修正，网页双维度设置，WiFi TX滞回窗口，fsm_force_standby修复 |
| 1.2.0 | 2026-04 | ESP-IDF v6.0.0适配，ADC驱动重构，WiFi/MQTT API更新 |
| 1.2.1 | 2026-05 | 滤芯水量精确计量（制水/冲洗分离），总用水量+制水量双显示，Web日志页，WiFi重连稳定性 |
| 1.2.2 | 2026-05-17 | **代码审查修复**：TDS温度补偿方向修正、水锤控制竞态修复、线程安全增强（config/GPIO/MQTT）、NVS错误处理完善、OTA回滚时序修正、总制水量NVS持久化补全 |
| 1.2.3 | 2026-05-19 | Web日志页增强：SNTP时间戳、DEBUG日志启用、级别过滤、空行跳过、自动滚动修复；OTA状态页增强：回滚状态检测、是否可升级提示、禁用按钮、错误提示；Flash保存周期新增4小时选项；日志噪声优化 |
| 1.2.4 | 2026-05-19 | OTA分区切换功能：恢复出厂固件按钮、回滚到上一OTA固件按钮、API接口 `/api/ota/factory` 和 `/api/ota/rollback` |
| 1.2.5 | 2026-05-20 | **代码审查修复（Critical/High）**：TDS温度补偿公式修正（multiply→divide）、Web服务器栈溢出修复（12KB）、FSM水量计算防绕过、日志缓冲区截断处理、FSM mutex泄漏修复、OTA写入计数修正（写入成功后增加） |
| 1.2.6 | 2026-05-20 | **代码审查修复（Medium）**：WiFi/MQTT重连任务竞态修复（reconnect_active标志）、MQTT发布参数NULL验证、任务栈优化（WiFi/MQTT重连4KB）；**日志清理**：移除重复初始化日志、降级重连轮询日志为DEBUG级别、合并FSM水量重复日志；**内存优化**：移除config_manager冗余NVS检查 |
| 1.2.7 | 2026-05-21 | **ESP-IDF v6.0兼容性修复**：httpd `max_open_sockets=3`（LWIP_MAX_SOCKETS=6，httpd内部占用3） |
| 2.2.1 | 2026-05-21 | **固件优化**：编译器SIZE优化、禁用GDB stub、禁用mbedTLS证书捆绑包（节省~50KB）、日志缓冲区4KB、生产级日志级别INFO；**代码清理**：删除6个废弃函数、NVS错误处理简化、pm_manager ESP32-C3 DFS跳过；**Web日志页**：级别解析修复、浅色背景；**版本管理**：手动版本号（CMake VERSION） |

---

## 内存使用详细分析

### Flash分区布局（4MB总容量）

| 分区 | 大小 | 用途 |
|------|------|------|
| Bootloader | 32 KB | 系统引导程序 |
| NVS | 24 KB | 非易失性存储（配置/运行数据） |
| otadata | 8 KB | OTA启动状态记录 |
| phy_init | 4 KB | RF校准数据 |
| factory | 1152 KB | 出厂固件分区 |
| ota_0 | 1408 KB | OTA固件槽位A |
| ota_1 | 1408 KB | OTA固件槽位B |

### RAM使用估算（ESP32-C3 ~400KB内部RAM）

| 类别 | 大小 | 说明 |
|------|------|------|
| 静态缓冲区(BSS) | ~13.5 KB | 全局结构体、日志环形缓冲区 |
| 任务栈 | ~34.5 KB | fsm(3KB), httpd(12KB), wifi_reconnect(4KB), mqtt_reconnect(4KB), 其他(11.5KB) |
| WiFi/LWIP | ~25-30 KB | 协议栈缓冲区（动态分配） |
| IDF系统开销 | ~20 KB | Heap管理器、定时器、系统任务 |
| **已用总计** | ~93-98 KB | |
| **可用堆内存** | ~300 KB | cJSON、临时缓冲区、动态分配 |

### 任务栈配置

| 任务 | 栈大小 | 优先级 | 说明 |
|------|--------|--------|------|
| main_task | 3.5 KB | 1 | ESP-IDF默认 |
| event_task | 4 KB | 1 | WiFi/系统事件处理 |
| fsm_task | 3 KB | 5 | 状态机主循环 |
| httpd | 12 KB | 5 | Web服务器（handle_log_debug需要9KB） |
| wifi_reconnect | 4 KB | 5 | WiFi指数退避重连 |
| mqtt_reconnect | 4 KB | 4 | MQTT指数退避重连 |
| tds_task | 2 KB | 4 | TDS传感器测量 |
| monitor_task | 2 KB | 6 | 系统监控（看门狗） |

### LED指示

| 系统状态 | LED D4 | LED D5 |
|----------|--------|--------|
| 待机 | 灭 | 灭 |
| 制水中 | 慢闪（1Hz） | 灭 |
| 冲洗中 | 交替闪烁 | 交替闪烁 |
| 缺水 | 快闪（5Hz） | 灭 |
| 漏水报警 | 快闪（5Hz） | 快闪（5Hz） |
| 停止 | 快闪（5Hz） | 灭 |

### 压力单位

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
