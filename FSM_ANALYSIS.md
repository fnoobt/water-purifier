# FSM 状态机全面分析报告（第六轮）

> 分析时间: 2026-04-18
> 代码版本: water_purifier_fsm.c (约 1340 行)

## 状态机当前状态（更新于 2026-04-18）

第五轮和第六轮分析中发现的关键问题均已修复：
- `transition_to_pure_flush()` 已调用 `transition_to(FSM_STATE_PURE_FLUSH)`
- 所有无条件 `transition_to()` 前均添加了状态守卫
- `standby_manual` 标志阻止 check_inputs 自动制水
- `fsm_force_standby()` 清理了 `flush_start_time`
- `execute_pure_flush()` 中 came_from_normal_flush 分支删除了冗余 GPIO 检查
- 换芯冲洗默认时长改为 20 分钟（1200秒）
- 纯水洗膜水锤过渡阶段增加 FLUSH_PHASE_RUNNING 阶段守卫
- standby_manual 清除条件改为桶缺水时清除（逻辑更清晰）

### 待机循环修复（第五轮）

**问题**: 完整周期（待机→制水→水满→常规冲洗→纯水洗膜→待机）后，系统立即重新开始制水，形成死循环。

**根因**: 纯水洗膜进入待机后 `standby_manual` 未被设置，下一次 `check_inputs()` 检测到压力桶缺水立即触发制水。

**修复**:
1. `execute_pure_flush()` 正常完成时设 `standby_manual = true`
2. `check_inputs()` 纯水洗膜中断时也设 `standby_manual = true`
3. 待机清除条件改为 `!low_pressure || tank_pressure` —— 缺水恢复或桶缺水时清除标志，清除后同 tick 满足条件即触发自动制水

### 纯水洗膜阶段守卫修复（第六轮）

**问题**: `check_inputs()` 中 PURE_FLUSH 的 `tank_pressure` 检测无条件执行，水锤过渡阶段（停泵1s+关阀0.5s）期间若桶压力变化，冲洗会被误中断。

**修复**: 增加 `fsm_ctx.current_phase == FLUSH_PHASE_RUNNING` 阶段守卫，仅在纯水冲洗正常运行中检测用户用水中断。

新增功能（第四轮）：
- `FSM_EVENT_FILTER_FLUSH` 换芯冲洗事件
- `fsm_ctx.filter_flush_mode` 和 `filter_flush_start` 字段
- `fsm_manual_filter_flush()` 公共函数
- `execute_normal_flush()` 中换芯冲洗模式：仅普通冲洗持续20分钟，不切换纯水洗膜，不计统计

---

## 第三轮新发现问题

### BUG-MEDIUM-05: execute_pure_flush() GPIO 检查与 transition_to_pure_flush() 冲突

**严重级别:** 🟡 中等（水锤过渡阶段可能被跳过，纯水冲洗计时提前开始）

**位置:** `execute_pure_flush()` 第 532-549 行（修改前）

**描述:** 当从常规冲洗进入纯水洗膜时，`transition_to_pure_flush()` 已经：
1. 停止了增压泵
2. 设置了 `current_phase = FLUSH_PHASE_STOP_PUMP`
3. 设置了 `phase_start_time`

但 `execute_pure_flush()` 在 `is_state_just_entered()` 中重新检查 GPIO 状态，
根据 `pump_on || inlet_on` 做决策。由于泵已被停，GPIO 状态发生变化，
可能导致判断错误，跳过水锤过渡阶段。

**执行轨迹:**
```
transition_to_pure_flush():
  → gpio_driver_set_boost_pump(false)    // output_state.boost_pump = false
  → transition_to(PURE_FLUSH)
  → current_phase = FLUSH_PHASE_STOP_PUMP
  → phase_start_time = now

execute_pure_flush() (同一 tick 内):
  → came_from_normal_flush = true
  → pump_on = false (已被停!)
  → inlet_on = true
  → pump_on || inlet_on = true
  → 走 "执行水锤过渡" 分支
  → gpio_driver_set_boost_pump(false)   // 冗余
  → current_phase = FLUSH_PHASE_STOP_PUMP  // 覆盖
  → phase_start_time = now  // 重新设置！

潜在风险: 如果 GPIO 状态不同步（pump_on=false, inlet_on=false），
会直接跳过水锤过渡，进入 RUNNING 阶段。
```

**修复:** 删除 `execute_pure_flush()` 中 `came_from_normal_flush` 分支的 GPIO 检查，
直接信任 `transition_to_pure_flush()` 设置的阶段。仅在 `current_phase == FLUSH_PHASE_NONE`
（异常情况）时执行安全检查。

---

## 第一轮已修复问题回顾

| 编号 | 问题 | 修复方案 |
|------|------|---------|
| FIX-01 | prodTimeout 双重乘法 (×60×60) | web_server.c 移除 `* 60` |
| FIX-02 | 制水时间跨冲洗周期累积 | execute_tank_full() 水满时刻记录并清除 |
| FIX-03 | 冲洗事件无状态守卫 | 事件循环添加 STANDBY/NORMAL_FLUSH/PURE_FLUSH 守卫 |
| FIX-04 | 手动冲洗函数状态守卫不一致 | fsm_manual_normal_flush/pure_flush 添加 STOP/LEAK_ALARM 拒绝 |
| FIX-05 | SHUTDOWN 不清理漏水检测状态 | 事件处理器添加 production_start_time/flush_start_time 清除 |

---

## 第二轮新发现问题

### BUG-CRITICAL-01: NORMAL_FLUSH 永远无法进入 PURE_FLUSH

**严重级别:** 🔴 致命（冲洗流程断裂，纯水反冲洗永不执行）

**位置:** `transition_to_pure_flush()` 第 277 行

**描述:** `transition_to_pure_flush()` 设置了 `current_phase = FLUSH_PHASE_STOP_PUMP` 但**没有调用 `transition_to(FSM_STATE_PURE_FLUSH)`**。

**执行轨迹:**
```
NORMAL_FLUSH RUNNING → 计时完成 → transition_to_pure_flush()
  → current_phase = FLUSH_PHASE_STOP_PUMP
  → current_state 仍为 NORMAL_FLUSH (未改变!)
下一个 tick → execute_normal_flush() 运行 (不是 execute_pure_flush!)
  → phase 是 STOP_PUMP，不匹配任何分支
  → 什么都不做
结果: 状态机卡在 NORMAL_FLUSH，纯水洗膜永不执行
```

**修复:** 在 `transition_to_pure_flush()` 末尾添加 `transition_to(FSM_STATE_PURE_FLUSH)`，
注意 `transition_to()` 会重置 `current_phase = FLUSH_PHASE_NONE`，所以需要在调用后再设置 phase。

---

### BUG-CRITICAL-02: 事件处理状态被 execute 函数覆盖

**严重级别:** 🔴 致命（用户网页控制命令被忽略）

**位置:** `execute_tank_full()` 第 466 行, `execute_pure_flush()` 第 595 行

**描述:** 主任务循环中事件处理先于 execute 函数执行。如果事件处理器改变了 `current_state`，
但对应的 execute 函数无条件调用 `transition_to()`，会覆盖事件处理器的状态变更。

**执行轨迹 (以 TANK_FULL + GO_STANDBY 为例):**
```
tick N: current_state = TANK_FULL
  1. 事件循环: GO_STANDBY → transition_to(STANDBY) → current_state = STANDBY
  2. check_inputs(): STANDBY 逻辑运行
  3. switch(current_state): 仍然是 TANK_FULL (switch 在事件处理前求值)
  4. execute_tank_full(): transition_to(NORMAL_FLUSH) → current_state = NORMAL_FLUSH
结果: 用户的"待机"命令被忽略，系统进入冲洗而非待机
```

**影响范围:**
- TANK_FULL + GO_STANDBY/SHUTDOWN → 被覆盖为 NORMAL_FLUSH
- PURE_FLUSH 完成 + SHUTDOWN → 被覆盖为 STANDBY
- PURE_FLUSH 完成 + GO_STANDBY → STANDBY (同状态，无影响)

**修复:** 在无条件 `transition_to()` 前添加状态守卫：
```c
if (fsm_ctx.current_state != FSM_STATE_TANK_FULL) return;
transition_to(FSM_STATE_NORMAL_FLUSH);
```

---

### BUG-MEDIUM-03: GO_STANDBY 后被 check_inputs 立即重新制水

**严重级别:** 🟡 中等（用户"待机"命令被自动制水覆盖）

**位置:** `check_inputs()` 第 680 行

**描述:** 当用户点击"待机"按钮时，如果传感器条件满足 (`low_pressure && tank_pressure`)，
`check_inputs()` 会立即将状态从 STANDBY 重新切换到 PRODUCTION。

**执行轨迹:**
```
当前: STANDBY, 低压闭合, 桶压闭合 (需要制水)
用户点击"待机"
  1. 事件循环: GO_STANDBY → transition_to(STANDBY), 关闭所有输出
  2. check_inputs(): STANDBY → low_pressure && tank_pressure → transition_to(PRODUCTION)
  3. execute_standby(): 关闭所有输出
  4. switch: PRODUCTION → execute_production() → 打开进水阀
结果: 用户点击"待机"后系统立即重新启动制水
```

**修复:** 添加 `standby_manual` 标志，GO_STANDBY 时设置，阻止 check_inputs 自动制水。
待机清除条件为 `!low_pressure || tank_pressure` —— 缺水恢复或压力桶缺水时自动清除标志，恢复制水功能。

---

### BUG-LOW-04: fsm_force_standby() 不清理 flush_start_time

**严重级别:** 🟢 轻微（残留冲洗时间可能影响后续操作）

**位置:** `fsm_force_standby()` 第 1220 行

**描述:** GO_STANDBY 事件处理器清理了 `flush_start_time`，但 `fsm_force_standby()` 没有。
如果从冲洗状态调用 `fsm_force_standby()`，`flush_start_time` 会保持非零值。

**修复:** 添加 `fsm_ctx.flush_start_time = 0`。

---

## 已验证无问题的路径

### 制水时间统计 (production_start_time)

| 路径 | 记录位置 | 是否正确 |
|------|---------|---------|
| PRODUCTION → TANK_FULL → ... → STANDBY | TANK_FULL 入口 | ✅ |
| PRODUCTION → STANDBY (手动) | GO_STANDBY 事件 + execute_standby | ✅ |
| PRODUCTION → WATER_SHORTAGE → STANDBY | execute_standby | ✅ |
| PRODUCTION → LEAK_ALARM | 清除不记录 | ✅ (异常中断) |
| PRODUCTION → STOP (超时) | 清除不记录 | ✅ (异常中断) |
| PRODUCTION → STANDBY (GO_STANDBY) | GO_STANDBY 事件处理器 | ✅ |

### 冲洗时间统计 (flush_start_time)

| 路径 | 记录位置 | 是否正确 |
|------|---------|---------|
| NORMAL_FLUSH → transition_to_pure_flush | transition_to_pure_flush 记录常规冲洗 | ✅ |
| PURE_FLUSH 完成 → STANDBY | execute_pure_flush 记录纯水冲洗 | ✅ |
| NORMAL_FLUSH → STANDBY (GO_STANDBY) | 清除不记录 | ✅ (用户中断) |
| PURE_FLUSH → STANDBY (GO_STANDBY) | 清除不记录 | ✅ (用户中断) |
| 网页直接进入 PURE_FLUSH → 完成 | execute_pure_flush | ✅ |

### 冲洗次数统计 (total_flush_cycles)

| 路径 | 增量位置 | 是否正确 |
|------|---------|---------|
| NORMAL_FLUSH 入口 | execute_normal_flush | ✅ |
| 网页直接进入 PURE_FLUSH | execute_pure_flush | ✅ |
| NORMAL_FLUSH → GO_STANDBY | execute_normal_flush 入口已计数 | ✅ (按钮即触发) |

### 事件守卫一致性

| 事件 | 事件循环守卫 | 函数守卫 | 一致? |
|------|-------------|---------|-------|
| FORCE_FLUSH | STANDBY | STANDBY | ✅ |
| FORCE_PRODUCTION | STANDBY | STANDBY | ✅ |
| NORMAL_FLUSH | STANDBY/NORMAL_FLUSH/PURE_FLUSH | 非STOP/LEAK_ALARM | ✅ (函数宽松) |
| PURE_FLUSH | STANDBY/NORMAL_FLUSH/PURE_FLUSH | 非STOP/LEAK_ALARM | ✅ (函数宽松) |
| FILTER_FLUSH | STANDBY/NORMAL_FLUSH/PURE_FLUSH | 非STOP/LEAK_ALARM | ✅ (函数宽松) |
| GO_STANDBY | 任意状态 | 无守卫 | ✅ |
| SHUTDOWN | 任意状态 | 无守卫 | ✅ |
| RESET | STOP/LEAK_ALARM | 无单独函数 | ✅ |

---

## 设计说明（非问题）

### 1. execute_standby() 制水统计不在 is_state_just_entered() 内

这是故意的安全网设计：
- 正常流程: TANK_FULL 已记录并清除 → execute_standby() 无事可做
- 异常流程: PRODUCTION → GO_STANDBY 已记录 → execute_standby() 无事可做
- 兜底流程: PRODUCTION → 意外切换 → execute_standby() 捕获未统计时间

`production_start_time > 0` 检查确保只执行一次，不会重复累积。

### 2. NORMAL_FLUSH 入口不关闭进水阀

从 PRODUCTION 水满进入时，进水阀已打开。冲洗需要进水阀提供原水，所以保持打开是正确的。
只有从 STANDBY 进入时才需要打开进水阀。

### 3. transition_to() 重置 current_phase

这是状态机的设计原则：进入新状态时清除旧的阶段信息。
各状态的 execute 函数在 `is_state_just_entered()` 中重新初始化阶段。

---

## 修改汇总

| 修改位置 | 修改内容 |
|---------|---------|
| `transition_to_pure_flush()` | 添加 `transition_to(FSM_STATE_PURE_FLUSH)`，调整 phase 设置顺序 |
| `execute_tank_full()` | 添加 `current_state != TANK_FULL` 守卫 |
| `execute_pure_flush()` | 添加 `current_state != PURE_FLUSH` 守卫 |
| `fsm_ctx` 结构体 | 添加 `standby_manual` 标志 |
| `check_inputs()` STANDBY | 检查 `standby_manual` 阻止自动制水，缺水恢复或桶缺水时清除 |
| GO_STANDBY 事件处理 | 设置 `standby_manual = true` |
| RESET 事件处理 | 清除 `standby_manual` |
| FORCE_FLUSH/FORCE_PRODUCTION | 清除 `standby_manual` |
| `fsm_init()` | 初始化 `standby_manual = false` |
| `fsm_force_standby()` | 添加 `flush_start_time = 0` |
| `execute_pure_flush()` came_from_normal_flush | 删除冗余GPIO检查，信任 `transition_to_pure_flush()` 设置的阶段 |
| `transition_to_pure_flush()` | 添加 "已进入纯水洗膜状态" 确认日志 |
| `execute_normal_flush()` RUNNING | 增强完成日志，显示实际时长、配置时长、短制水标志 |
| `execute_pure_flush()` 完成 | 设置 `standby_manual = true` 防止冲洗后立即重新制水 |
| `check_inputs()` PURE_FLUSH中断 | 压力开关触发转待机时设 `standby_manual = true`，增加 FLUSH_PHASE_RUNNING 阶段守卫 |
| `FILTER_FLUSH_DURATION_DEFAULT_SEC` | 换芯冲洗默认时长改为 1200秒（20分钟） |
| `check_inputs()` STANDBY清除条件 | 改为 `!low_pressure || tank_pressure` —— 桶缺水时清除而非桶满时提前清除 |
