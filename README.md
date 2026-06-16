# ESP32 净水器主控

基于 ESP32-C3 的净水器智能控制器，支持 75G RO 膜 + 3G 压力桶。

## 特性

- **8 状态 FSM**: 智能制水、双阶段冲洗、水锤控制
- **双 TDS 监测**: 进水/出水实时监测 + 温度补偿校准
- **5 级滤芯管理**: 每级独立水量+时间双维度寿命追踪
- **Web 认证**: Basic Auth + Session Cookie（24小时滑动过期），管理页面/OTA/日志需认证
- **Web 日志**: 8KB环形缓冲区 + 分块流式传输(1KB chunks)，最多200行，时间戳转换+级别过滤
- **OTA 升级**: Web 界面一键升级，版本检测 + 回滚保护 + 出厂恢复，bootloader自动状态转换
- **Home Assistant**: MQTT 自动发现集成

## 快速开始

```bash
# ESP-IDF 6.0 环境
idf.py build
idf.py -p COM3 flash monitor
```

首次启动进入 AP 配网模式，连接 `WaterPurifier-XXXX`（密码 `12345678`），访问 `http://192.168.4.1`。

## 硬件

| 项目 | 规格 |
|------|------|
| 芯片 | ESP32-C3 (RISC-V 160MHz) |
| Flash | 4MB（factory + 双OTA槽位） |
| RAM | ~400KB（可用堆 ~280KB，日志流式传输时动态分配1KB） |

**GPIO**: TDS(GPIO2/3) · 开关(GPIO6/7/10) · 阀门(GPIO0/1/4) · 泵(GPIO5) · LED(GPIO12/13)

## 状态流转

```
STANDBY → PRODUCTION → TANK_FULL → NORMAL_FLUSH → PURE_FLUSH → STANDBY
    ↓          ↓              ↓           ↓           ↓
WATER_SHORTAGE  ←────────  (任意状态 → LEAK_ALARM/STOP → 手动复位)
```

## Web 认证

| 页面 | 认证要求 |
|------|---------|
| 首页 `/` | 公开 |
| WiFi配网 `/api/wifi` | 公开 |
| 状态 `/api/status` | 公开 |
| 管理页面 `/admin` | 需认证 |
| OTA升级 `/ota` | 需认证 |
| 系统日志 `/logs` | 需认证 |
| 所有管理API | 需认证 |

**默认账户**: 用户名 `admin`，密码 `admin`（认证默认禁用）

在管理页面 → Web认证配置 中启用认证并设置用户名/密码。

## 模块

| 模块 | 职责 |
|------|------|
| FSM | 状态机核心（8状态 + 双阶段冲洗 + 水锤控制） |
| TDS | ADC采集 + 温度补偿 + 校准 + 报警 |
| Filter | 滤芯寿命管理（每级独立水量/时间双维度，独立累加器） |
| GPIO | 输入防抖 + 继电器控制 + 紧急停止锁定 |
| WiFi | STA/AP + 指数退避重连（60s上限） |
| MQTT | Home Assistant 自动发现 + 指数退避重连 |
| Web | 首页 + 管理页 + 日志页（流式传输） + OTA + Basic Auth |
| Config | NVS 持久化配置（周期性保存+脏标志） |
| OTA | 固件升级 + 回滚保护 + 恢复出厂（bootloader自动状态转换） |
| PM | WiFi TX功率动态调整(滞回算法) + 堆内存监控(告警+直接重启) |
| History | 事件日志 + 每日统计 |

详细文档见 [DOCUMENTATION.md](DOCUMENTATION.md)。