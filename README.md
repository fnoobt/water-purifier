# ESP32 净水器主控

基于 ESP32-C3 的净水器智能控制器，支持 75G RO 膜 + 3G 压力桶。

## 特性

- **8 状态 FSM**: 智能制水、双阶段冲洗、水锤控制
- **双 TDS 监测**: 进水/出水实时监测 + 自动校准
- **5 级滤芯管理**: 水量 + 时间双维度寿命追踪
- **OTA 升级**: Web 界面一键升级，版本检测 + 回滚保护
- **Home Assistant**: MQTT 自动发现集成

## 快速开始

```bash
idf.py build
idf.py -p COM3 flash monitor
```

首次启动进入 AP 配网模式，连接 `WaterPurifier-XXXX`，访问 `http://192.168.4.1`。

## 硬件

| 项目 | 规格 |
|------|------|
| 芯片 | ESP32-C3 (RISC-V 160MHz) |
| Flash | 4MB (双 OTA 槽位) |

**GPIO**: TDS(GPIO2/3) · 开关(GPIO6/7/10) · 阀门(GPIO0/1/4) · 泵(GPIO5) · LED(GPIO12/13)

## 状态流转

```
STANDBY → PRODUCTION → TANK_FULL → NORMAL_FLUSH → PURE_FLUSH → STANDBY
    ↓          ↓              ↓           ↓           ↓
WATER_SHORTAGE  ←────────  (任意状态 → LEAK_ALARM/STOP → 手动复位)
```

## 模块

| 模块 | 职责 |
|------|------|
| FSM | 状态机核心 |
| TDS | ADC 采集 + 校准 |
| Filter | 滤芯寿命管理 |
| GPIO | 输入防抖 + 继电器 |
| WiFi | STA/AP + 重连 |
| Web | 双页 UI + 14 API |
| MQTT | HA 自动发现 |
| Config | NVS 持久化 |
| OTA | 固件升级 |
| PM | CPU/功率管理 |
| History | 事件日志 |

详细文档见 [DOCUMENTATION.md](DOCUMENTATION.md)。