/**
 * @file water_purifier_fsm.c
 * @brief 净水器有限状态机实现
 *
 * 状态流程：
 * 1. 上电 → 检测低压开关 → 缺水 → 进入待机
 * 2. 待机 → 制水 → 压力桶满 → 水满 → 常规冲洗 → 纯水洗膜 → 待机
 * 3. 缺水 → 进水恢复 → 待机 → 自动判断是否制水
 * 4. 漏水 → 紧急停机 → 停止闪烁 → 手动复位
 * 5. 制水超时 → 停止状态 → 手动复位
 * 6. 纯水洗膜期间用户用水 → 压力下降 → 立即停止洗膜进待机
 *
 * 水锤控制：
 * - 进入制水/常规冲洗：先开进水阀，延时后开泵（制水时废水阀关，冲洗时开废水阀）
 * - 常规冲洗→纯水洗膜：先停泵→延时→关进水阀→延时→开回水阀→开始计时
 */

#include "water_purifier_fsm.h"
#include "gpio_driver.h"
#include "tds_sensor.h"
#include "filter_manager.h"
#include "config_manager.h"
#include "history_logger.h"
#include "board_params.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "soc/soc_caps.h"
#include "portmacro.h"
#include <string.h>

static const char *TAG = "FSM";

// 静态spinlock用于并发保护（状态查询、状态转换、LED闪烁）
static portMUX_TYPE fsm_spinlock = portMUX_INITIALIZER_UNLOCKED;

// ==================== 配置参数 ====================
// 注：以下默认值与 config_manager.c 的 default_config 保持一致，
// 启动时由 fsm_set_*() 用 config 值覆盖，此处仅作为 fallback。

#define PRODUCTION_TIMEOUT_DEFAULT_SEC   (3 * 3600)
#define NORMAL_FLUSH_DURATION_DEFAULT_SEC 20
#define PURE_FLUSH_DURATION_DEFAULT_SEC   15
#define FILTER_FLUSH_DURATION_DEFAULT_SEC 3600
#define LEAK_CONFIRM_TIME_DEFAULT_SEC     5
#define TANK_CONFIRM_TIME_DEFAULT_SEC     5
#define CHECK_INTERVAL_MS                 100
#define LED_BLINK_SLOW_MS                 1000
#define LED_BLINK_FAST_MS                 200

// 水锤默认延时
#define WATER_HAMMER_VALVE_OPEN_DELAY_MS  1000
#define WATER_HAMMER_PUMP_STOP_DELAY_MS   1000
#define WATER_HAMMER_VALVE_CLOSE_DELAY_MS 500

// 短制水判断阈值
#define SHORT_PRODUCTION_THRESHOLD_SEC    180

// 冲洗阶段（用于水锤延时控制）
typedef enum {
    FLUSH_PHASE_NONE = 0,     // 非冲洗阶段
    FLUSH_PHASE_VALVE_DELAY,  // 开阀延时中
    FLUSH_PHASE_PUMP_DELAY,   // 开泵延时中
    FLUSH_PHASE_RUNNING,      // 正常运行中
    FLUSH_PHASE_STOP_PUMP,    // 停泵延时中（冲洗→洗膜过渡）
    FLUSH_PHASE_CLOSE_VALVE,  // 关阀延时中（冲洗→洗膜过渡）
    FLUSH_PHASE_RETURN_DELAY, // 开回水阀延时中
} flush_phase_t;

// ==================== 私有变量 ====================

static struct {
    bool initialized;
    bool running;
    bool stop_requested;           // 停止请求标志（通知FSM任务安全退出）
    fsm_state_t current_state;
    fsm_state_t previous_state;

    // 配置参数
    uint32_t production_timeout_sec;
    uint32_t normal_flush_duration_sec;   // 常规冲洗时间
    uint32_t pure_flush_duration_sec;     // 纯水洗膜时间
    uint32_t leak_confirm_time_ms;
    uint32_t tank_confirm_time_ms;        // 压力桶水满/需水确认时间（毫秒）
    uint32_t short_prod_threshold_sec;    // 短制水判断阈值
    uint32_t water_hammer_valve_open_delay_ms;
    uint32_t water_hammer_pump_stop_delay_ms;
    uint32_t water_hammer_valve_close_delay_ms;

    // 运行数据
    fsm_runtime_data_t runtime_data;

    // 保存间隔（分钟）
    uint16_t runtime_save_interval_min;
    bool runtime_dirty;               // 运行数据脏标志

    // 停止历史
    stop_record_t stop_history[16];
    uint32_t stop_history_index;

    // 时间跟踪（state_enter_time已移除，节省内存）
    uint64_t production_start_time;   // 制水开始时间（微秒）
    uint64_t flush_start_time;        // 冲洗开始时间（微秒）
    uint64_t phase_start_time;        // 当前阶段开始时间（微秒）
    flush_phase_t current_phase;      // 当前冲洗阶段
    bool state_init_done;             // 当前状态是否已初始化（避免用FLUSH_PHASE_RUNNING当标记）
    bool short_production;            // 上一次制水时间是否很短

    // 漏水检测确认时间（防止凝露误报）
    uint64_t leak_detect_start_time;
    bool leak_detected;           // 漏水信号已触发，等待确认
    bool leak_confirmed_logged;   // 确认日志已输出（边沿触发，防止报警状态下每轮询刷屏）

    // 压力桶开关确认（防止泵脉动/水锤导致开关抖动误判）
    uint64_t tank_full_detect_start_time;
    bool tank_full_detected;      // 水满信号已触发，等待确认
    uint64_t need_water_detect_start_time;
    bool need_water_detected;     // 需水信号已触发，等待确认

    // 手动待机标志（阻止check_inputs自动进入制水）
    bool standby_manual;

    // 换芯冲洗模式
    bool filter_flush_mode;          // 换芯冲洗模式（仅普通冲洗，不切纯水）
    uint64_t filter_flush_start;     // 换芯冲洗总开始时间（微秒）
    uint32_t filter_flush_duration_sec; // 换芯冲洗总时长（秒）

    // 任务相关
    QueueHandle_t event_queue;
    TaskHandle_t task_handle;

    // runtime_data访问互斥锁（保护uint64_t字段的原子读取）
    SemaphoreHandle_t runtime_data_mutex;

    // 状态转换日志用的冲洗时间（解决flush_start_time被清零/重置的问题）
    uint32_t pending_flush_duration_sec;

    // 回调
    fsm_state_callback_t state_callback;} fsm_ctx = {0};

// 前向声明
static void transition_to(fsm_state_t new_state);
static uint64_t get_elapsed_sec(uint64_t start_time);
static uint64_t get_elapsed_ms(uint64_t start_time);
static uint64_t accumulate_production_water(bool clear_timer, bool update_daily, uint64_t *out_duration_sec);
static uint64_t accumulate_flush_water(bool clear_timer, bool update_daily);

// ==================== LED控制 ====================

// LED闪烁模式枚举（FSM_前缀避免与gpio_config.h冲突）
typedef enum {
    FSM_LED_MODE_OFF,        // 全灭
    FSM_LED_MODE_LED1_ON,    // LED1常亮
    FSM_LED_MODE_LED1_BLINK_SLOW,    // LED1慢闪，LED2灭
    FSM_LED_MODE_LED1_BLINK_FAST,    // LED1快闪，LED2灭
    FSM_LED_MODE_LED1_LED2_ALTERNATE, // LED1和LED2交替慢闪
    FSM_LED_MODE_LED1_LED2_SYNC_BLINK, // LED1和LED2同步快闪
} led_blink_mode_t;

// LED闪烁状态（移到fsm_ctx避免静态变量竞态）
static struct {
    uint64_t last_toggle_ms;
    bool led1_state;
    bool led2_state;
    led_blink_mode_t current_mode;
} led_blink_ctx = {0};

/**
 * @brief 通用LED控制函数（替代多个重复函数）
 * @param mode LED闪烁模式
 */
static void led_set_mode(led_blink_mode_t mode)
{
    uint64_t now_ms = esp_timer_get_time() / 1000;
    uint32_t period_ms = (mode == FSM_LED_MODE_LED1_BLINK_SLOW ||
                         mode == FSM_LED_MODE_LED1_LED2_ALTERNATE) ? LED_BLINK_SLOW_MS : LED_BLINK_FAST_MS;

    // 非闪烁模式直接设置
    if (mode == FSM_LED_MODE_OFF) {
        gpio_driver_set_led1(false);
        gpio_driver_set_led2(false);
        return;
    }
    if (mode == FSM_LED_MODE_LED1_ON) {
        gpio_driver_set_led1(true);
        gpio_driver_set_led2(false);
        return;
    }

    // 闪烁模式：检查是否需要切换（spinlock保护状态和GPIO写入的一致性）
    taskENTER_CRITICAL(&fsm_spinlock);
    if (led_blink_ctx.current_mode != mode) {
        // 模式切换，重置状态
        led_blink_ctx.current_mode = mode;
        led_blink_ctx.last_toggle_ms = now_ms;
        led_blink_ctx.led1_state = true;
        led_blink_ctx.led2_state = (mode == FSM_LED_MODE_LED1_LED2_ALTERNATE) ? false : true;
    }

    if (now_ms - led_blink_ctx.last_toggle_ms >= period_ms) {
        led_blink_ctx.led1_state = !led_blink_ctx.led1_state;
        led_blink_ctx.led2_state = (mode == FSM_LED_MODE_LED1_LED2_ALTERNATE) ?
                                    !led_blink_ctx.led2_state : led_blink_ctx.led1_state;
        led_blink_ctx.last_toggle_ms = now_ms;
    }

    // GPIO写入在spinlock内完成，确保硬件输出与软件状态严格一致
    // ESP32-C3单核：gpio_set_level执行极快（直接寄存器写入），不会显著增加临界区时间
    gpio_driver_set_led1(led_blink_ctx.led1_state);
    gpio_driver_set_led2(led_blink_ctx.led2_state);
    taskEXIT_CRITICAL(&fsm_spinlock);
}

// 保持原有接口兼容性（调用通用函数）
static void led_set_standby(void) { led_set_mode(FSM_LED_MODE_OFF); }
static void led_set_production(void) { led_set_mode(FSM_LED_MODE_LED1_BLINK_SLOW); }
static void led_set_tank_full(void) { led_set_mode(FSM_LED_MODE_LED1_ON); }
static void led_set_water_shortage(void) { led_set_mode(FSM_LED_MODE_LED1_BLINK_FAST); }
static void led_set_flushing(void) { led_set_mode(FSM_LED_MODE_LED1_LED2_ALTERNATE); }
static void led_set_leak_alarm(void) { led_set_mode(FSM_LED_MODE_LED1_LED2_SYNC_BLINK); }
static void led_set_stop(void) { led_set_mode(FSM_LED_MODE_LED1_BLINK_FAST); }

static void update_led_for_state(fsm_state_t state)
{
    switch (state) {
        case FSM_STATE_STANDBY:
            led_set_standby();
            break;
        case FSM_STATE_PRODUCTION:
            led_set_production();
            break;
        case FSM_STATE_TANK_FULL:
            led_set_tank_full();
            break;
        case FSM_STATE_NORMAL_FLUSH:
        case FSM_STATE_PURE_FLUSH:
            led_set_flushing();
            break;
        case FSM_STATE_WATER_SHORTAGE:
            led_set_water_shortage();
            break;
        case FSM_STATE_LEAK_ALARM:
            led_set_leak_alarm();
            break;
        case FSM_STATE_STOP:
            led_set_stop();
            break;
        default:
            break;
    }
}

static void led_reset_blink_state(void)
{
    gpio_driver_set_led1(false);
    gpio_driver_set_led2(false);
}

// ==================== 输出控制 ====================

static void stop_all_outputs(void)
{
    gpio_driver_set_inlet_valve(false);
    gpio_driver_set_waste_valve(false);
    gpio_driver_set_return_valve(false);
    gpio_driver_set_boost_pump(false);
    // 重置相位标志，防止紧急情况恢复后状态机异常执行
    fsm_ctx.current_phase = FLUSH_PHASE_NONE;
    ESP_LOGD(TAG, "所有输出已关闭，相位已重置");
}

/**
 * @brief 启动制水（带水锤控制：先开进水阀，延时后再开增压泵，废水阀关闭）
 */
static void start_production_with_delay(void)
{
    ESP_LOGD(TAG, "启动制水（开进水阀，延时%lu ms后开泵）",
             fsm_ctx.water_hammer_valve_open_delay_ms);

    gpio_driver_set_inlet_valve(true);
    gpio_driver_set_waste_valve(false);
    gpio_driver_set_return_valve(false);
    gpio_driver_set_boost_pump(false);  // 先不开泵

    fsm_ctx.current_phase = FLUSH_PHASE_VALVE_DELAY;
    fsm_ctx.phase_start_time = esp_timer_get_time();

    fsm_ctx.runtime_data.total_production_cycles++;
}

/**
 * @brief 常规冲洗→纯水洗膜过渡（水锤控制：停泵→关进水阀→开回水阀）
 */
static void transition_to_pure_flush(void)
{
    ESP_LOGD(TAG, "常规冲洗→纯水洗膜过渡（停泵，延时%lu ms→关进水阀，延时%lu ms→开回水阀）",
             fsm_ctx.water_hammer_pump_stop_delay_ms,
             fsm_ctx.water_hammer_valve_close_delay_ms);

    // 冲洗时长和 pending_flush_duration_sec 已在 execute_normal_flush 中正确设置
    // 此处不再重复计算（accumulate_flush_water(true) 已清零 flush_start_time）

    gpio_driver_set_boost_pump(false);   // 先停泵

    // 先切换状态（transition_to会输出日志使用pending_flush_duration_sec）
    transition_to(FSM_STATE_PURE_FLUSH);
    fsm_ctx.current_phase = FLUSH_PHASE_STOP_PUMP;
    fsm_ctx.phase_start_time = esp_timer_get_time();
    // 纯水洗膜阶段重新计时（在transition_to之后，不影响日志输出）
    fsm_ctx.flush_start_time = esp_timer_get_time();
    ESP_LOGD(TAG, "已进入纯水洗膜状态，水锤过渡阶段：停泵延时");
}

// ==================== 状态名称 ====================

static const char* const state_names[] = {
    [FSM_STATE_STANDBY] = "待机",
    [FSM_STATE_PRODUCTION] = "制水",
    [FSM_STATE_TANK_FULL] = "水满",
    [FSM_STATE_NORMAL_FLUSH] = "常规冲洗",
    [FSM_STATE_PURE_FLUSH] = "纯水洗膜",
    [FSM_STATE_WATER_SHORTAGE] = "缺水",
    [FSM_STATE_LEAK_ALARM] = "漏水报警",
    [FSM_STATE_STOP] = "停止",
};

static const char* const event_names[] = {
    [FSM_EVENT_NONE] = "无",
    [FSM_EVENT_LOW_PRESSURE_ON] = "有水",
    [FSM_EVENT_LOW_PRESSURE_OFF] = "缺水",
    [FSM_EVENT_TANK_NEED_WATER] = "需要制水",
    [FSM_EVENT_TANK_FULL] = "水满",
    [FSM_EVENT_WATER_LEAK] = "漏水",
    [FSM_EVENT_NORMAL_FLUSH_DONE] = "常规冲洗完成",
    [FSM_EVENT_PURE_FLUSH_DONE] = "纯水洗膜完成",
    [FSM_EVENT_PRODUCTION_TIMEOUT] = "制水超时",
    [FSM_EVENT_RESET] = "复位",
    [FSM_EVENT_FORCE_FLUSH] = "强制冲洗",
    [FSM_EVENT_FORCE_PRODUCTION] = "强制制水",
    [FSM_EVENT_NORMAL_FLUSH] = "常规冲洗",
    [FSM_EVENT_PURE_FLUSH] = "纯水洗膜",
    [FSM_EVENT_GO_STANDBY] = "待机",
    [FSM_EVENT_SHUTDOWN] = "停机",
    [FSM_EVENT_FILTER_FLUSH] = "换芯冲洗",
};

// ==================== 私有函数 ====================

static void record_stop(stop_type_t type, const char *description)
{
    /* 使用mutex保护stop_history数组的访问（与fsm_get_stop_history共享） */
    if (fsm_ctx.runtime_data_mutex && xSemaphoreTake(fsm_ctx.runtime_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        fsm_ctx.runtime_data.last_stop_type = type;
        fsm_ctx.runtime_data.last_stop_time = esp_timer_get_time();

        /* 循环缓冲区存储：索引持续递增，存储位置用modulo计算 */
        uint32_t idx = fsm_ctx.stop_history_index % 16;
        fsm_ctx.stop_history[idx].type = type;
        fsm_ctx.stop_history[idx].timestamp = esp_timer_get_time();
        /* 安全拷贝：强制null终止，防止strncpy不终止 */
        size_t desc_len = strlen(description);
        size_t copy_len = (desc_len < 63) ? desc_len : 63;
        memcpy(fsm_ctx.stop_history[idx].description, description, copy_len);
        fsm_ctx.stop_history[idx].description[copy_len] = '\0';
        fsm_ctx.stop_history_index++;

        fsm_ctx.runtime_dirty = true;  // 在mutex内设置脏标志
        xSemaphoreGive(fsm_ctx.runtime_data_mutex);
    } else {
        /* mutex获取失败，丢弃本次记录以保证数据一致性
         * 在单核ESP32-C3上mutex失败概率极低，偶尔丢失一条停止记录可接受 */
        ESP_LOGW(TAG, "record_stop: mutex获取失败，丢弃记录: %s", description);
    }

    ESP_LOGE(TAG, "停止记录: %s", description);
}

static void transition_to(fsm_state_t new_state)
{
    /* 状态边界检查，防止数组越界 */
    if (new_state >= FSM_STATE_COUNT) {
        ESP_LOGE(TAG, "无效状态: %d", new_state);
        return;
    }

    taskENTER_CRITICAL(&fsm_spinlock);
    if (new_state == fsm_ctx.current_state) {
        taskEXIT_CRITICAL(&fsm_spinlock);
        return;
    }

    fsm_state_t old_state = fsm_ctx.current_state;
    fsm_ctx.previous_state = old_state;
    fsm_ctx.current_state = new_state;

    /* 水锤相位保护：仅当相位为NONE或RUNNING时才重置，
     * 保留水锤过渡阶段（VALVE_DELAY/PUMP_DELAY/STOP_PUMP/CLOSE_VALVE/RETURN_DELAY） */
    if (fsm_ctx.current_phase == FLUSH_PHASE_NONE ||
        fsm_ctx.current_phase == FLUSH_PHASE_RUNNING) {
        fsm_ctx.current_phase = FLUSH_PHASE_NONE;
    }
    fsm_ctx.state_init_done = false;

    /* 压力开关确认状态入口复位：进入制水/待机时清除对应确认计时，
     * 防止陈旧时间戳跨状态残留导致瞬时误确认。
     * 此处是所有状态转换（传感器/事件/恢复）的唯一咽喉点 */
    if (new_state == FSM_STATE_PRODUCTION) {
        fsm_ctx.tank_full_detected = false;
        fsm_ctx.tank_full_detect_start_time = 0;
    }
    if (new_state == FSM_STATE_STANDBY) {
        fsm_ctx.need_water_detected = false;
        fsm_ctx.need_water_detect_start_time = 0;
    }
    taskEXIT_CRITICAL(&fsm_spinlock);

    /* 冲洗状态退出时：如果冲洗计时器仍运行（中断退出），计算实际时长和水量
     * 正常完成时 flush_start_time 已被 execute_* 清零，不会进入此分支
     * 覆盖所有中断路径：用水中断→待机、漏水→报警、缺水→保护
     * 注意：必须先于 accumulate_flush_water 执行，否则计时器被清后无法计算 */
    /* 中断路径：计算冲洗时长和水统计 */
    if ((old_state == FSM_STATE_PURE_FLUSH || old_state == FSM_STATE_NORMAL_FLUSH)
        && (fsm_ctx.flush_start_time > 0 || (fsm_ctx.filter_flush_mode && fsm_ctx.filter_flush_start > 0))) {

        uint64_t flush_sec = 0;

        // 换芯冲洗使用总计时器
        if (fsm_ctx.filter_flush_mode && fsm_ctx.filter_flush_start > 0) {
            flush_sec = get_elapsed_sec(fsm_ctx.filter_flush_start);
            fsm_ctx.filter_flush_start = 0;
            fsm_ctx.filter_flush_mode = false;
        }
        // 常规冲洗/纯水洗膜使用单循环计时器
        else if (fsm_ctx.flush_start_time > 0) {
            flush_sec = get_elapsed_sec(fsm_ctx.flush_start_time);
            fsm_ctx.flush_start_time = 0;
        }

        fsm_ctx.pending_flush_duration_sec = (uint32_t)flush_sec;

        /* 中断路径的水量统计（仅常规冲洗有水量，纯水洗膜泵已关闭无水） */
        if (old_state == FSM_STATE_NORMAL_FLUSH && flush_sec > 0 &&
            (new_state == FSM_STATE_LEAK_ALARM || new_state == FSM_STATE_STOP ||
             new_state == FSM_STATE_WATER_SHORTAGE || new_state == FSM_STATE_STANDBY)) {
            float pump_rate = filter_mgr_get_pump_flow_lph();
            float pre_liters = (float)flush_sec * pump_rate / 3600.0f;
            if (pre_liters > 0.1f) {
                filter_mgr_update_water_usage_dual(pre_liters, 0.0f);
            }
            fsm_ctx.runtime_data.total_flush_time_sec += flush_sec;
            fsm_ctx.runtime_data.last_flush_time = esp_timer_get_time();
            fsm_ctx.runtime_dirty = true;
        }
    }

    /* 重置LED闪烁静态变量，避免跨状态残留 */
    led_reset_blink_state();

    /* 进入报警/停止/缺水状态时，先累计未统计的水量再清除计时器 */
    if (new_state == FSM_STATE_LEAK_ALARM || new_state == FSM_STATE_STOP ||
        new_state == FSM_STATE_WATER_SHORTAGE) {
        accumulate_production_water(true, false, NULL);
        accumulate_flush_water(true, false);
    }

    /* 常规冲洗 -> 纯水洗膜：包含冲洗时长 */
    if (new_state == FSM_STATE_PURE_FLUSH && old_state == FSM_STATE_NORMAL_FLUSH) {
        ESP_LOGI(TAG, "状态转换: 常规冲洗(%lu秒) -> 纯水洗膜",
                 fsm_ctx.pending_flush_duration_sec);
        fsm_ctx.pending_flush_duration_sec = 0;  // 清除临时变量
        if (fsm_ctx.state_callback) {
            fsm_ctx.state_callback(old_state, new_state);
        }
        return;
    }

    /* 纯水洗膜 -> 待机：包含洗膜时长 */
    if (new_state == FSM_STATE_STANDBY && old_state == FSM_STATE_PURE_FLUSH) {
        ESP_LOGI(TAG, "状态转换: 纯水洗膜(%lu秒) -> 待机",
                 fsm_ctx.pending_flush_duration_sec);
        fsm_ctx.pending_flush_duration_sec = 0;  // 清除临时变量
        if (fsm_ctx.state_callback) {
            fsm_ctx.state_callback(old_state, new_state);
        }
        return;
    }

    /* 常规冲洗 -> 待机：包含冲洗时长（网页中断或换芯冲洗完成） */
    if (new_state == FSM_STATE_STANDBY && old_state == FSM_STATE_NORMAL_FLUSH) {
        if (fsm_ctx.pending_flush_duration_sec > 0) {
            ESP_LOGI(TAG, "状态转换: 常规冲洗(%lu秒) -> 待机",
                     fsm_ctx.pending_flush_duration_sec);
        } else {
            ESP_LOGI(TAG, "状态转换: 常规冲洗 -> 待机");
        }
        fsm_ctx.pending_flush_duration_sec = 0;
        if (fsm_ctx.state_callback) {
            fsm_ctx.state_callback(old_state, new_state);
        }
        return;
    }

    /* 进入水满状态时，也要累计制水水量（防止execute_tank_full被事件中断） */
    if (new_state == FSM_STATE_TANK_FULL && old_state == FSM_STATE_PRODUCTION) {
        if (fsm_ctx.production_start_time > 0) {
            uint64_t prod_sec = get_elapsed_sec(fsm_ctx.production_start_time);
            fsm_ctx.runtime_data.total_production_time_sec += prod_sec;

            float ro_rate = fsm_get_production_rate_lph();
            float waste_rate = filter_mgr_get_waste_flow_lph();
            float pre_rate = ro_rate + waste_rate;
            float pre_liters = (float)prod_sec * pre_rate / 3600.0f;
            float post_liters = (float)prod_sec * ro_rate / 3600.0f;

            if (pre_liters > 0.1f || post_liters > 0.1f) {
                filter_mgr_update_water_usage_dual(pre_liters, post_liters);
                history_update_daily_production((uint32_t)prod_sec);
                // 统计信息合并到状态转换日志中，包含制水时长
                ESP_LOGI(TAG, "状态转换: 制水(%lu秒, RO %.1f升, 过水 %.1f升) -> 水满",
                         (uint32_t)prod_sec, post_liters, pre_liters);
            } else {
                ESP_LOGI(TAG, "状态转换: 制水(%lu秒) -> 水满", (uint32_t)prod_sec);
            }

            // 短制水判断（标记供execute_tank_full使用）
            fsm_ctx.short_production = (prod_sec < fsm_ctx.short_prod_threshold_sec);
            fsm_ctx.runtime_dirty = true;
            fsm_ctx.production_start_time = 0;  // 统计完成后清除
            return;  // 已输出日志，跳过通用状态转换日志
        }
    }

    ESP_LOGI(TAG, "状态转换: %s -> %s",
             state_names[old_state], state_names[new_state]);

    if (fsm_ctx.state_callback) {
        fsm_ctx.state_callback(old_state, new_state);
    }
}

static uint64_t get_elapsed_sec(uint64_t start_time)
{
    return (esp_timer_get_time() - start_time) / 1000000;
}

static uint64_t get_elapsed_ms(uint64_t start_time)
{
    return (esp_timer_get_time() - start_time) / 1000;
}

// ==================== 水量统计公共函数 ====================

/**
 * @brief 统计当前制水周期的水量并累加到滤芯寿命
 * @param clear_timer true=清除计时器和dirty标志, false=仅统计不清除
 * @param update_daily true=同时更新每日制水统计
 * @param out_duration_sec 输出制水时长（秒），可为NULL
 * @return 制水时长（秒），0表示未在计时
 */
static uint64_t accumulate_production_water(bool clear_timer, bool update_daily, uint64_t *out_duration_sec)
{
    if (fsm_ctx.production_start_time == 0) {
        if (out_duration_sec) *out_duration_sec = 0;
        return 0;
    }

    uint64_t duration = get_elapsed_sec(fsm_ctx.production_start_time);
    float ro_rate = fsm_get_production_rate_lph();
    float waste_rate = filter_mgr_get_waste_flow_lph();
    float pre_rate = ro_rate + waste_rate;
    float pre_liters = (float)duration * pre_rate / 3600.0f;
    float post_liters = (float)duration * ro_rate / 3600.0f;

    if (pre_liters > 0.1f || post_liters > 0.1f) {
        filter_mgr_update_water_usage_dual(pre_liters, post_liters);
        if (update_daily) {
            history_update_daily_production((uint32_t)duration);
        }
    }

    if (clear_timer) {
        fsm_ctx.production_start_time = 0;
        fsm_ctx.runtime_dirty = true;
    }

    if (out_duration_sec) *out_duration_sec = duration;
    return duration;
}

/**
 * @brief 统计当前冲洗周期的水量并累加到滤芯寿命
 * @param clear_timer true=清除计时器并更新冲洗时间统计
 * @param update_daily true=同时递增每日冲洗计数
 * @return 冲洗时长（秒），0表示未在计时
 */
static uint64_t accumulate_flush_water(bool clear_timer, bool update_daily)
{
    uint64_t flush_sec = 0;

    // 换芯冲洗和普通冲洗互斥
    if (fsm_ctx.filter_flush_start > 0 && fsm_ctx.filter_flush_mode) {
        flush_sec = get_elapsed_sec(fsm_ctx.filter_flush_start);
        float pump_rate = filter_mgr_get_pump_flow_lph();
        float pre_liters = (float)flush_sec * pump_rate / 3600.0f;
        if (pre_liters > 0.1f) {
            filter_mgr_update_water_usage_dual(pre_liters, 0.0f);
        }
        if (clear_timer) {
            fsm_ctx.runtime_data.total_flush_time_sec += flush_sec;
            fsm_ctx.runtime_data.last_flush_time = esp_timer_get_time();
            fsm_ctx.filter_flush_start = 0;
        }
    } else if (fsm_ctx.flush_start_time > 0) {
        flush_sec = get_elapsed_sec(fsm_ctx.flush_start_time);
        float pump_rate = filter_mgr_get_pump_flow_lph();
        float pre_liters = (float)flush_sec * pump_rate / 3600.0f;
        if (pre_liters > 0.1f) {
            filter_mgr_update_water_usage_dual(pre_liters, 0.0f);
        }
        if (clear_timer) {
            fsm_ctx.runtime_data.total_flush_time_sec += flush_sec;
            fsm_ctx.runtime_data.last_flush_time = esp_timer_get_time();
            fsm_ctx.flush_start_time = 0;
        }
    } else {
        return 0;
    }

    if (clear_timer) {
        if (update_daily) {
            history_increment_daily_flush();
        }
    }

    return flush_sec;
}

// ==================== 状态执行函数 ====================

static void execute_standby(void)
{
    if (!fsm_ctx.state_init_done) {
        stop_all_outputs();
        fsm_ctx.state_init_done = true;
    }

    // 更新制水时间统计（异常恢复场景：production_start_time未清零就进入待机）
    {
        uint64_t prod_dur = 0;
        accumulate_production_water(true, true, &prod_dur);
        if (prod_dur > 0) {
            fsm_ctx.runtime_data.total_production_time_sec += prod_dur;
            ESP_LOGD(TAG, "待机时补充统计制水: %llu秒", prod_dur);
        }
    }
}

static void execute_production(void)
{
    if (fsm_ctx.current_phase == FLUSH_PHASE_NONE) {
        fsm_ctx.short_production = false;  // 重置标志，等待制水完成后判断
        start_production_with_delay();
    }

    // 水锤延时：开进水阀后延时再开泵（废水阀关闭）
    if (fsm_ctx.current_phase == FLUSH_PHASE_VALVE_DELAY) {
        uint64_t elapsed_ms = get_elapsed_ms(fsm_ctx.phase_start_time);
        if (elapsed_ms >= fsm_ctx.water_hammer_valve_open_delay_ms) {
            ESP_LOGD(TAG, "开增压泵（水锤延时完成）");
            gpio_driver_set_boost_pump(true);
            fsm_ctx.production_start_time = esp_timer_get_time();  // 泵实际开启时开始计时
            fsm_ctx.current_phase = FLUSH_PHASE_RUNNING;
        }
        return;
    }

    // 检查制水超时
    if (fsm_ctx.production_start_time > 0) {
        uint64_t elapsed = get_elapsed_sec(fsm_ctx.production_start_time);
        if (elapsed >= fsm_ctx.production_timeout_sec) {
            ESP_LOGE(TAG, "制水超时（%llu秒），进入停止状态", elapsed);
            stop_all_outputs();
            record_stop(STOP_TYPE_PRODUCTION_TIMEOUT, "制水超时");
            transition_to(FSM_STATE_STOP);
        }
    }
}

static void execute_tank_full(void)
{
    if (fsm_ctx.current_phase == FLUSH_PHASE_NONE) {
        // 记录制水时间统计（水满时刻的制水时长）
        // 注意：transition_to已处理PRODUCTION→TANK_FULL的水量统计，
        // 这里检查production_start_time是否已清零，避免重复统计
        if (fsm_ctx.production_start_time > 0) {
            uint64_t prod_dur = 0;
            accumulate_production_water(true, false, &prod_dur);
            if (prod_dur > 0) {
                fsm_ctx.runtime_data.total_production_time_sec += prod_dur;
            }
        }

        // 短制水判断（transition_to已设置，这里补充prod_sec=0的情况）
        if (fsm_ctx.production_start_time == 0 && !fsm_ctx.short_production) {
            // transition_to未处理（可能是直接进入TANK_FULL），设置默认值
            fsm_ctx.short_production = false;
        }
        // 清除production_start_time，防止待机时execute_standby重复统计
        fsm_ctx.production_start_time = 0;
        fsm_ctx.state_init_done = true;
    }
    // 如果事件处理已改变状态，尊重事件处理的结果
    if (fsm_ctx.current_state != FSM_STATE_TANK_FULL) return;
    transition_to(FSM_STATE_NORMAL_FLUSH);
}

static void execute_normal_flush(void)
{
    if (fsm_ctx.current_phase == FLUSH_PHASE_NONE) {
        // 水锤控制：先开进水阀，延时后再开废水阀+增压泵
        ESP_LOGD(TAG, "常规冲洗：开进水阀，延时%lu ms后开废水阀+增压泵",
                 fsm_ctx.water_hammer_valve_open_delay_ms);
        gpio_driver_set_inlet_valve(true);
        gpio_driver_set_waste_valve(false);
        gpio_driver_set_boost_pump(false);
        fsm_ctx.current_phase = FLUSH_PHASE_VALVE_DELAY;
        fsm_ctx.phase_start_time = esp_timer_get_time();
        // 换芯冲洗模式不计入冲洗次数统计
        if (!fsm_ctx.filter_flush_mode) {
            fsm_ctx.runtime_data.total_flush_cycles++;
        }
    }

    // 阶段1：开阀延时
    if (fsm_ctx.current_phase == FLUSH_PHASE_VALVE_DELAY) {
        uint64_t elapsed_ms = get_elapsed_ms(fsm_ctx.phase_start_time);
        if (elapsed_ms >= fsm_ctx.water_hammer_valve_open_delay_ms) {
            ESP_LOGD(TAG, "开废水阀+增压泵（水锤延时完成）");
            gpio_driver_set_waste_valve(true);
            gpio_driver_set_boost_pump(true);
            fsm_ctx.flush_start_time = esp_timer_get_time();
            fsm_ctx.current_phase = FLUSH_PHASE_RUNNING;
        }
        return;
    }

    // 阶段2：正常运行，检查冲洗时间
    if (fsm_ctx.current_phase == FLUSH_PHASE_RUNNING) {
        uint64_t elapsed = get_elapsed_sec(fsm_ctx.flush_start_time);
        uint32_t flush_dur = fsm_ctx.short_production ? 10 : fsm_ctx.normal_flush_duration_sec;

        if (elapsed >= flush_dur) {
            // 换芯冲洗模式：不切换到纯水洗膜，持续普通冲洗直到满1小时
            if (fsm_ctx.filter_flush_mode) {
                uint64_t total_elapsed = get_elapsed_sec(fsm_ctx.filter_flush_start);
                if (total_elapsed >= fsm_ctx.filter_flush_duration_sec) {
                    ESP_LOGI(TAG, "换芯冲洗完成，总时长: %lu秒", (uint32_t)total_elapsed);

                    // 换芯冲洗水量统计
                    uint64_t flush_dur_sec = accumulate_flush_water(true, true);
                    if (flush_dur_sec > 0) {
                        float pump_rate = filter_mgr_get_pump_flow_lph();
                        float pre_liters = (float)flush_dur_sec * pump_rate / 3600.0f;
                        ESP_LOGI(TAG, "换芯冲洗总过水: %.1f 升（前三级）", pre_liters);
                    }
                    fsm_ctx.runtime_dirty = true;

                    fsm_ctx.filter_flush_mode = false;
                    stop_all_outputs();
                    transition_to(FSM_STATE_STANDBY);
                    return;
                }
                ESP_LOGI(TAG, "换芯冲洗: 已运行%lu秒，继续下一轮冲洗", (uint32_t)total_elapsed);
                // 重置阶段，继续下一轮普通冲洗
                stop_all_outputs();
                fsm_ctx.current_phase = FLUSH_PHASE_NONE;
                return;
            }

            ESP_LOGD(TAG, "常规冲洗完成，进入纯水洗膜过渡");

            // 常规冲洗水量统计
            uint64_t flush_dur_sec = accumulate_flush_water(true, false);
            if (flush_dur_sec > 0) {
                float pump_rate = filter_mgr_get_pump_flow_lph();
                float pre_liters = (float)flush_dur_sec * pump_rate / 3600.0f;
                ESP_LOGD(TAG, "本次冲洗: 前三级过水 %.1f 升", pre_liters);
            }
            fsm_ctx.runtime_dirty = true;
            // 注意：此处不调用 history_increment_daily_flush()
            // 完整冲洗周期（常规+纯水）的每日计数由 execute_pure_flush 完成时统一递增，
            // 避免一次完整冲洗被计为两次

            // accumulate_flush_water(true) 已将 flush_start_time 清零，
            // 必须在此处保存常规冲洗时长，供 transition_to 日志使用
            fsm_ctx.pending_flush_duration_sec = (uint32_t)flush_dur_sec;

            transition_to_pure_flush();
        }
    }
}

static void execute_pure_flush(void)
{
    if (fsm_ctx.current_phase == FLUSH_PHASE_NONE) {
        // 区分两种情况：从常规冲洗过渡过来 vs 网页直接进入
        bool came_from_normal_flush = (fsm_ctx.previous_state == FSM_STATE_NORMAL_FLUSH);

        if (came_from_normal_flush) {
            // transition_to_pure_flush() 应已设置phase，这里是异常兜底
            ESP_LOGW(TAG, "纯水洗膜：阶段未设置，安全检查");
            gpio_driver_set_boost_pump(false);
            fsm_ctx.current_phase = FLUSH_PHASE_STOP_PUMP;
        } else {
            // 网页直接进入纯水洗膜，需要独立计数
            ESP_LOGI(TAG, "纯水洗膜：网页直接进入，独立计数");
            fsm_ctx.runtime_data.total_flush_cycles++;
            fsm_ctx.short_production = false;  // 非短制水场景
            fsm_ctx.current_phase = FLUSH_PHASE_STOP_PUMP;
            gpio_driver_set_boost_pump(false);
        }
        fsm_ctx.phase_start_time = esp_timer_get_time();
    }

    // 阶段1：停泵延时
    if (fsm_ctx.current_phase == FLUSH_PHASE_STOP_PUMP) {
        uint64_t elapsed_ms = get_elapsed_ms(fsm_ctx.phase_start_time);
        if (elapsed_ms >= fsm_ctx.water_hammer_pump_stop_delay_ms) {
            gpio_driver_set_inlet_valve(false);
            fsm_ctx.current_phase = FLUSH_PHASE_CLOSE_VALVE;
            fsm_ctx.phase_start_time = esp_timer_get_time();
            ESP_LOGD(TAG, "关进水阀，延时%lu ms后开回水阀",
                     fsm_ctx.water_hammer_valve_close_delay_ms);
        }
        return;
    }

    // 阶段2：关阀延时
    if (fsm_ctx.current_phase == FLUSH_PHASE_CLOSE_VALVE) {
        uint64_t elapsed_ms = get_elapsed_ms(fsm_ctx.phase_start_time);
        if (elapsed_ms >= fsm_ctx.water_hammer_valve_close_delay_ms) {
            gpio_driver_set_return_valve(true);
            gpio_driver_set_waste_valve(true);
            fsm_ctx.flush_start_time = esp_timer_get_time();
            fsm_ctx.current_phase = FLUSH_PHASE_RUNNING;
            ESP_LOGD(TAG, "纯水洗膜开始（开回水阀+废水阀）");
        }
        return;
    }

    // 阶段3：正常纯水洗膜，检查时间
    if (fsm_ctx.current_phase == FLUSH_PHASE_RUNNING) {
        uint64_t elapsed = get_elapsed_sec(fsm_ctx.flush_start_time);
        uint32_t flush_dur = fsm_ctx.short_production ? 5 : fsm_ctx.pure_flush_duration_sec;

        if (elapsed >= flush_dur) {
            ESP_LOGD(TAG, "纯水洗膜完成");
            stop_all_outputs();

            // 更新冲洗时间统计（换芯冲洗不计入，保持数据一致性）
            if (!fsm_ctx.filter_flush_mode) {
                uint64_t pure_flush_sec = get_elapsed_sec(fsm_ctx.flush_start_time);
                fsm_ctx.runtime_data.total_flush_time_sec += pure_flush_sec;
                fsm_ctx.runtime_data.last_flush_time = esp_timer_get_time();
                history_increment_daily_flush();
                fsm_ctx.runtime_dirty = true;
                // 保存纯水洗膜时间供transition_to日志使用
                fsm_ctx.pending_flush_duration_sec = (uint32_t)pure_flush_sec;
            } else {
                fsm_ctx.pending_flush_duration_sec = 0;
            }
            fsm_ctx.flush_start_time = 0;

            // 如果事件处理已改变状态，尊重事件处理的结果
            if (fsm_ctx.current_state != FSM_STATE_PURE_FLUSH) return;
            transition_to(FSM_STATE_STANDBY);
        }
    }
}

static void execute_water_shortage(void)
{
    if (!fsm_ctx.state_init_done) {
        stop_all_outputs();
        fsm_ctx.runtime_data.low_pressure_occurred = true;
        fsm_ctx.state_init_done = true;
        ESP_LOGW(TAG, "缺水保护，等待自动恢复");
    }
}

static void execute_leak_alarm(void)
{
    if (!fsm_ctx.state_init_done) {
        gpio_driver_emergency_stop();  // 紧急停止并锁定，防止恢复前误操作
        fsm_ctx.state_init_done = true;
        record_stop(STOP_TYPE_WATER_LEAK, "检测到漏水");
    }
}

static void execute_stop(void)
{
    if (!fsm_ctx.state_init_done) {
        stop_all_outputs();
        fsm_ctx.state_init_done = true;
        ESP_LOGE(TAG, "系统处于停止状态，需要手动复位");
    }
}

// ==================== 输入检测 ====================

static bool check_leak_confirmed(bool water_leak)
{
    uint64_t now_ms = esp_timer_get_time() / 1000;

    if (water_leak) {
        if (!fsm_ctx.leak_detected) {
            fsm_ctx.leak_detected = true;
            fsm_ctx.leak_confirmed_logged = false;
            fsm_ctx.leak_detect_start_time = now_ms;
            ESP_LOGW(TAG, "检测到漏水信号，开始确认计时(%lu秒)", fsm_ctx.leak_confirm_time_ms / 1000);
        } else if (now_ms - fsm_ctx.leak_detect_start_time >= fsm_ctx.leak_confirm_time_ms) {
            // 确认日志边沿触发：漏水信号持续期间（已进入报警状态）每100ms轮询都会命中此分支，
            // 仅首次输出，防止刷屏（与TDS报警边沿触发原则一致）；返回值不受影响
            if (!fsm_ctx.leak_confirmed_logged) {
                fsm_ctx.leak_confirmed_logged = true;
                ESP_LOGE(TAG, "漏水确认，持续%lu秒", fsm_ctx.leak_confirm_time_ms / 1000);
            }
            return true;
        }
        return false;
    } else {
        if (fsm_ctx.leak_detected) {
            ESP_LOGI(TAG, "漏水信号消失，取消报警");
            fsm_ctx.leak_detected = false;
            fsm_ctx.leak_confirmed_logged = false;
        }
        return false;
    }
}

/**
 * @brief 压力桶水满确认（防泵脉动/水锤导致开关颤动误判）
 * @param tank_pressure false=断开(水满信号) true=闭合(需要水)
 * @return true=水满已确认（断开持续≥确认时长）
 * @note  仅在PRODUCTION状态轮询调用；确认后自清，下次断开重新计时
 */
static bool check_tank_full_confirmed(bool tank_pressure)
{
    uint64_t now_ms = esp_timer_get_time() / 1000;

    if (!tank_pressure) {  // 开关断开 = 水满信号
        if (!fsm_ctx.tank_full_detected) {
            fsm_ctx.tank_full_detected = true;
            fsm_ctx.tank_full_detect_start_time = now_ms;
            ESP_LOGI(TAG, "检测到水满信号，开始确认计时(%lu秒)", fsm_ctx.tank_confirm_time_ms / 1000);
        } else if (now_ms - fsm_ctx.tank_full_detect_start_time >= fsm_ctx.tank_confirm_time_ms) {
            ESP_LOGI(TAG, "水满确认，持续%lu秒", fsm_ctx.tank_confirm_time_ms / 1000);
            fsm_ctx.tank_full_detected = false;
            fsm_ctx.tank_full_detect_start_time = 0;
            return true;
        }
        return false;
    } else {
        if (fsm_ctx.tank_full_detected) {
            ESP_LOGD(TAG, "水满信号消失，取消确认");
            fsm_ctx.tank_full_detected = false;
            fsm_ctx.tank_full_detect_start_time = 0;
        }
        return false;
    }
}

/**
 * @brief 压力桶需水确认（与check_tank_full_confirmed镜像对称）
 * @param tank_pressure true=闭合(需水信号) false=断开(水满)
 * @return true=需水已确认（闭合持续≥确认时长）
 * @note  仅在STANDBY状态轮询调用；确认后自清，下次闭合重新计时
 */
static bool check_need_water_confirmed(bool tank_pressure)
{
    uint64_t now_ms = esp_timer_get_time() / 1000;

    if (tank_pressure) {  // 开关闭合 = 需水信号
        if (!fsm_ctx.need_water_detected) {
            fsm_ctx.need_water_detected = true;
            fsm_ctx.need_water_detect_start_time = now_ms;
            ESP_LOGD(TAG, "检测到需水信号，开始确认计时(%lu秒)", fsm_ctx.tank_confirm_time_ms / 1000);
        } else if (now_ms - fsm_ctx.need_water_detect_start_time >= fsm_ctx.tank_confirm_time_ms) {
            ESP_LOGI(TAG, "需水确认，持续%lu秒", fsm_ctx.tank_confirm_time_ms / 1000);
            fsm_ctx.need_water_detected = false;
            fsm_ctx.need_water_detect_start_time = 0;
            return true;
        }
        return false;
    } else {
        if (fsm_ctx.need_water_detected) {
            ESP_LOGD(TAG, "需水信号消失，取消确认");
            fsm_ctx.need_water_detected = false;
            fsm_ctx.need_water_detect_start_time = 0;
        }
        return false;
    }
}

static void check_inputs(void)
{
    bool low_pressure = gpio_driver_read_low_pressure();
    bool tank_pressure = gpio_driver_read_tank_pressure();
    bool water_leak = gpio_driver_read_water_leak();
    bool leak_confirmed = check_leak_confirmed(water_leak);

    /* 压力开关确认值按当前状态门控计算（辅助函数仅在所属状态内调用，
     * 配合 transition_to() 的入口复位，保证确认计时不会跨状态残留） */
    bool tank_full_confirmed = false;
    bool need_water_confirmed = false;
    if (fsm_ctx.current_state == FSM_STATE_PRODUCTION) {
        tank_full_confirmed = check_tank_full_confirmed(tank_pressure);
    } else if (fsm_ctx.current_state == FSM_STATE_STANDBY) {
        need_water_confirmed = check_need_water_confirmed(tank_pressure);
    }

    switch (fsm_ctx.current_state) {
        case FSM_STATE_STANDBY:
            if (leak_confirmed) {
                transition_to(FSM_STATE_LEAK_ALARM);
                return;
            }
            // 传感器条件变化时清除手动待机标志（缺水恢复或桶缺水确认后清除，允许自动恢复制水）
            if (fsm_ctx.standby_manual && (!low_pressure || need_water_confirmed)) {
                fsm_ctx.standby_manual = false;
            }
            if (!low_pressure) {
                transition_to(FSM_STATE_WATER_SHORTAGE);
                return;
            }
            if (low_pressure && need_water_confirmed && !fsm_ctx.standby_manual) {
                transition_to(FSM_STATE_PRODUCTION);
                return;
            }
            break;

        case FSM_STATE_PRODUCTION:
            if (leak_confirmed) {
                stop_all_outputs();
                transition_to(FSM_STATE_LEAK_ALARM);
                return;
            }
            if (!low_pressure) {
                stop_all_outputs();
                transition_to(FSM_STATE_WATER_SHORTAGE);
                return;
            }
            if (tank_full_confirmed) {
                transition_to(FSM_STATE_TANK_FULL);
                return;
            }
            break;

        case FSM_STATE_TANK_FULL:
            /* 瞬时过渡状态也需要处理紧急事件（漏水由 check_leak_confirmed 统一处理） */
            if (leak_confirmed) {
                stop_all_outputs();
                transition_to(FSM_STATE_LEAK_ALARM);
            }
            /* 正常情况下由 execute_tank_full 自动转入冲洗；用户可通过事件队列发送待机/停止命令 */
            break;

        case FSM_STATE_NORMAL_FLUSH:
            if (leak_confirmed) {
                stop_all_outputs();
                transition_to(FSM_STATE_LEAK_ALARM);
                return;
            }
            if (!low_pressure) {
                ESP_LOGW(TAG, "常规冲洗中断：检测到缺水");
                stop_all_outputs();
                transition_to(FSM_STATE_WATER_SHORTAGE);
                return;
            }
            break;

        case FSM_STATE_PURE_FLUSH:
            // 纯水洗膜期间的特殊处理：用户用水导致压力下降
            // 仅在 FLUSH_PHASE_RUNNING 阶段检查 tank_pressure，避免水锤过渡阶段（停泵/关阀）被误中断
            if (fsm_ctx.current_phase == FLUSH_PHASE_RUNNING && tank_pressure) {
                // 压力桶压力开关闭合 = 用户正在用水/桶空
                ESP_LOGI(TAG, "纯水洗膜中断：检测到用水需求，停止洗膜进入待机");
                stop_all_outputs();
                transition_to(FSM_STATE_STANDBY);
                return;
            }
            if (leak_confirmed) {
                stop_all_outputs();
                transition_to(FSM_STATE_LEAK_ALARM);
                return;
            }
            if (!low_pressure) {
                ESP_LOGW(TAG, "纯水洗膜中断：检测到缺水");
                stop_all_outputs();
                transition_to(FSM_STATE_WATER_SHORTAGE);
                return;
            }
            break;

        case FSM_STATE_WATER_SHORTAGE:
            if (leak_confirmed) {
                transition_to(FSM_STATE_LEAK_ALARM);
                return;
            }

            // 缺水恢复，进入待机（由待机判断是否制水）
            if (low_pressure) {
                // 缺水恢复后清除手动待机标志，允许系统自动判断是否制水
                fsm_ctx.standby_manual = false;
                if (tank_pressure) {
                    ESP_LOGI(TAG, "缺水恢复，进入待机→制水");
                } else {
                    ESP_LOGI(TAG, "缺水恢复，压力桶满，进入待机");
                }
                transition_to(FSM_STATE_STANDBY);
                return;
            }
            break;

        case FSM_STATE_LEAK_ALARM:
        case FSM_STATE_STOP:
            // 只能通过手动复位
            break;

        default:
            break;
    }
}

// ==================== 主任务 ====================

static void fsm_task(void *arg)
{
    ESP_LOGI(TAG, "状态机任务启动");

    // 注册状态机任务到看门狗（协助monitor任务，防止长时间阻塞导致看门狗超时）
    esp_err_t wdt_ret = esp_task_wdt_add(NULL);
    if (wdt_ret == ESP_OK) {
        ESP_LOGI(TAG, "状态机任务已注册到看门狗");
    } else {
        ESP_LOGW(TAG, "状态机任务看门狗注册失败: %s", esp_err_to_name(wdt_ret));
    }

    fsm_event_t event;
    uint32_t periodic_save_counter = 0;

    while (true) {
        // 检查停止请求（优先检查）
        taskENTER_CRITICAL(&fsm_spinlock);
        bool should_stop = fsm_ctx.stop_requested;
        bool is_running = fsm_ctx.running;
        taskEXIT_CRITICAL(&fsm_spinlock);

        if (should_stop || !is_running) {
            ESP_LOGI(TAG, "FSM任务收到停止请求，准备退出");
            break;
        }

        // 重置看门狗（每次循环开始时）
        esp_task_wdt_reset();

        if (xQueueReceive(fsm_ctx.event_queue, &event, pdMS_TO_TICKS(CHECK_INTERVAL_MS)) == pdTRUE) {
            if (event == FSM_EVENT_RESET) {
                if (fsm_ctx.current_state == FSM_STATE_STOP ||
                    fsm_ctx.current_state == FSM_STATE_LEAK_ALARM) {
                    ESP_LOGI(TAG, "停止复位");
                    fsm_ctx.runtime_data.low_pressure_occurred = false;
                    fsm_ctx.leak_detected = false;
                    fsm_ctx.leak_confirmed_logged = false;
                    fsm_ctx.leak_detect_start_time = 0;
                    fsm_ctx.standby_manual = false;
                    gpio_driver_clear_emergency();  // 解除GPIO紧急锁定，允许输出操作
                    transition_to(FSM_STATE_STANDBY);
                }
            } else if (event == FSM_EVENT_FORCE_FLUSH) {
                if (fsm_ctx.current_state == FSM_STATE_STANDBY) {
                    fsm_ctx.standby_manual = false;
                    transition_to(FSM_STATE_NORMAL_FLUSH);
                }
            } else if (event == FSM_EVENT_FORCE_PRODUCTION) {
                if (fsm_ctx.current_state == FSM_STATE_STANDBY) {
                    fsm_ctx.standby_manual = false;
                    transition_to(FSM_STATE_PRODUCTION);
                }
            } else if (event == FSM_EVENT_NORMAL_FLUSH) {
                // 网页面板：切换到常规冲洗（禁止从制水等状态切入，防止污染计时）
                if (fsm_ctx.current_state == FSM_STATE_STANDBY ||
                    fsm_ctx.current_state == FSM_STATE_NORMAL_FLUSH ||
                    fsm_ctx.current_state == FSM_STATE_PURE_FLUSH) {
                    ESP_LOGI(TAG, "网页控制：切换到常规冲洗");
                    fsm_ctx.short_production = false;  // 手动冲洗按正常时间
                    transition_to(FSM_STATE_NORMAL_FLUSH);
                }
            } else if (event == FSM_EVENT_PURE_FLUSH) {
                // 网页面板：切换到纯水洗膜（禁止从制水等状态切入，防止污染计时）
                if (fsm_ctx.current_state == FSM_STATE_STANDBY ||
                    fsm_ctx.current_state == FSM_STATE_NORMAL_FLUSH ||
                    fsm_ctx.current_state == FSM_STATE_PURE_FLUSH) {
                    ESP_LOGI(TAG, "网页控制：切换到纯水洗膜");
                    fsm_ctx.short_production = false;
                    transition_to(FSM_STATE_PURE_FLUSH);
                }
            } else if (event == FSM_EVENT_GO_STANDBY) {
                // 网页面板：切换到待机（任意状态可切入）
                ESP_LOGI(TAG, "网页控制：切换到待机");
                // 清理未统计的制水/冲洗时间
                {
                    uint64_t prod_dur = 0;
                    accumulate_production_water(true, true, &prod_dur);
                    if (prod_dur > 0) {
                        fsm_ctx.runtime_data.total_production_time_sec += prod_dur;
                    }
                }
                {
                    uint64_t flush_dur = accumulate_flush_water(true, true);
                    if (flush_dur > 0) {
                        // flush time already accumulated inside helper
                    }
                }
                stop_all_outputs();
                fsm_ctx.leak_detected = false;
                fsm_ctx.leak_confirmed_logged = false;
                fsm_ctx.leak_detect_start_time = 0;
                fsm_ctx.standby_manual = true;  // 阻止自动进入制水
                fsm_ctx.filter_flush_mode = false;  // 取消换芯冲洗
                transition_to(FSM_STATE_STANDBY);
            } else if (event == FSM_EVENT_SHUTDOWN) {
                // 网页面板：停机（进入停止前先累计未统计的水量）
                ESP_LOGI(TAG, "网页控制：停机");
                {
                    uint64_t prod_dur = 0;
                    accumulate_production_water(true, true, &prod_dur);
                    if (prod_dur > 0) {
                        fsm_ctx.runtime_data.total_production_time_sec += prod_dur;
                    }
                }
                accumulate_flush_water(true, true);
                stop_all_outputs();
                transition_to(FSM_STATE_STOP);
            } else if (event == FSM_EVENT_FILTER_FLUSH) {
                // 网页面板：启动换芯冲洗（仅普通冲洗，持续1小时）
                if (fsm_ctx.current_state == FSM_STATE_STANDBY ||
                    fsm_ctx.current_state == FSM_STATE_NORMAL_FLUSH ||
                    fsm_ctx.current_state == FSM_STATE_PURE_FLUSH) {
                    ESP_LOGI(TAG, "网页控制：启动换芯冲洗（1小时）");
                    fsm_ctx.filter_flush_mode = true;
                    fsm_ctx.filter_flush_start = esp_timer_get_time();
                    fsm_ctx.short_production = false;
                    fsm_ctx.current_phase = FLUSH_PHASE_NONE;
                    transition_to(FSM_STATE_NORMAL_FLUSH);
                }
            }
        }

        check_inputs();

        // 周期性统一保存（运行数据、滤芯、历史记录）
        // 间隔内各模块仅标记脏标志不写NVS；到点后按脏标志统一写入，
        // 无脏标志则不产生任何写入，等待下一间隔点（最小化Flash磨损）。
        // 注意：NVS写入可能阻塞50-200ms，期间状态检查暂停。
        // 这在保存间隔（默认120分钟）下是可接受的——状态事件在队列中排队，
        // 保存完成后立即处理。看门狗在循环开始/结束均重置，不会因NVS阻塞超时。
        periodic_save_counter++;
        uint32_t save_interval_sec = fsm_ctx.runtime_save_interval_min * 60;
        uint32_t save_ticks = save_interval_sec * 1000 / CHECK_INTERVAL_MS;
        // 最小保存间隔为1分钟(600 ticks)，防止配置为0时频繁写入磨损Flash
        if (save_ticks < 600) save_ticks = 600;  // 600 * 100ms = 60秒
        if (periodic_save_counter >= save_ticks) {
            // FSM运行统计脏时才同步（避免无意义调用）
            if (fsm_ctx.runtime_dirty) {
                config_manager_sync_fsm_stats(
                    fsm_ctx.runtime_data.total_production_cycles,
                    fsm_ctx.runtime_data.total_flush_cycles,
                    fsm_ctx.runtime_data.total_production_time_sec,
                    fsm_ctx.runtime_data.total_flush_time_sec
                );
                fsm_ctx.runtime_dirty = false;  // 清除脏标志
            }
            // 调用统一保存接口（滤芯+历史记录）
            config_manager_periodic_save_all(save_interval_sec);
            periodic_save_counter = 0;
        }

        switch (fsm_ctx.current_state) {
            case FSM_STATE_STANDBY:
                execute_standby();
                break;
            case FSM_STATE_PRODUCTION:
                execute_production();
                break;
            case FSM_STATE_TANK_FULL:
                execute_tank_full();
                break;
            case FSM_STATE_NORMAL_FLUSH:
                execute_normal_flush();
                break;
            case FSM_STATE_PURE_FLUSH:
                execute_pure_flush();
                break;
            case FSM_STATE_WATER_SHORTAGE:
                execute_water_shortage();
                break;
            case FSM_STATE_LEAK_ALARM:
                execute_leak_alarm();
                break;
            case FSM_STATE_STOP:
                execute_stop();
                break;
            default:
                break;
        }

        update_led_for_state(fsm_ctx.current_state);

        // 重置看门狗（每次循环结束时，确保长时间操作后仍能reset）
        esp_task_wdt_reset();
    }

    // 任务结束前注销看门狗
    esp_task_wdt_delete(NULL);

    // 清除任务句柄（通知等待者任务已退出）
    taskENTER_CRITICAL(&fsm_spinlock);
    fsm_ctx.task_handle = NULL;
    taskEXIT_CRITICAL(&fsm_spinlock);

    ESP_LOGI(TAG, "状态机任务结束");
    vTaskDelete(NULL);
}

// ==================== 公共接口实现 ====================

esp_err_t fsm_init(void)
{
    if (fsm_ctx.initialized) {
        return ESP_OK;
    }

    fsm_ctx.event_queue = xQueueCreate(16, sizeof(fsm_event_t));
    if (!fsm_ctx.event_queue) {
        ESP_LOGE(TAG, "创建事件队列失败");
        return ESP_ERR_NO_MEM;
    }

    // 创建runtime_data互斥锁（避免双检锁模式竞态）
    fsm_ctx.runtime_data_mutex = xSemaphoreCreateMutex();
    if (!fsm_ctx.runtime_data_mutex) {
        ESP_LOGE(TAG, "创建运行数据mutex失败");
        vQueueDelete(fsm_ctx.event_queue);
        fsm_ctx.event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    // 初始化默认配置
    fsm_ctx.production_timeout_sec = PRODUCTION_TIMEOUT_DEFAULT_SEC;
    fsm_ctx.normal_flush_duration_sec = NORMAL_FLUSH_DURATION_DEFAULT_SEC;
    fsm_ctx.pure_flush_duration_sec = PURE_FLUSH_DURATION_DEFAULT_SEC;
    fsm_ctx.filter_flush_duration_sec = FILTER_FLUSH_DURATION_DEFAULT_SEC;
    fsm_ctx.leak_confirm_time_ms = LEAK_CONFIRM_TIME_DEFAULT_SEC * 1000;
    fsm_ctx.tank_confirm_time_ms = TANK_CONFIRM_TIME_DEFAULT_SEC * 1000;
    fsm_ctx.short_prod_threshold_sec = SHORT_PRODUCTION_THRESHOLD_SEC;
    fsm_ctx.water_hammer_valve_open_delay_ms = WATER_HAMMER_VALVE_OPEN_DELAY_MS;
    fsm_ctx.water_hammer_pump_stop_delay_ms = WATER_HAMMER_PUMP_STOP_DELAY_MS;
    fsm_ctx.water_hammer_valve_close_delay_ms = WATER_HAMMER_VALVE_CLOSE_DELAY_MS;
    fsm_ctx.runtime_save_interval_min = 120;

    // 运行数据现为临时状态（重启后清零），由filter_manager持久化水量数据

    // 初始化废水流量（用于制水状态前三级滤芯水量计算）
    system_config_t cfg;
    config_manager_get_config(&cfg);
    float waste_lph = cfg.waste_valve_flow_cc * 60.0f / 1000.0f;
    filter_mgr_set_waste_flow_lph(waste_lph);
    ESP_LOGI(TAG, "废水阀流量: %uCC (%.1f L/h)", cfg.waste_valve_flow_cc, waste_lph);

    // 加载FSM运行统计（持久化）
    uint32_t prod_cycles, flush_cycles;
    uint64_t prod_time_sec, flush_time_sec;
    config_manager_get_fsm_stats(&prod_cycles, &flush_cycles, &prod_time_sec, &flush_time_sec);
    fsm_ctx.runtime_data.total_production_cycles = prod_cycles;
    fsm_ctx.runtime_data.total_flush_cycles = flush_cycles;
    fsm_ctx.runtime_data.total_production_time_sec = prod_time_sec;
    fsm_ctx.runtime_data.total_flush_time_sec = flush_time_sec;
    ESP_LOGI(TAG, "运行统计: 制水%lu周期, 冲洗%lu周期, 制水%llu秒",
             prod_cycles, flush_cycles, prod_time_sec);

    // 增压泵流量根据泵类型设置（冲洗状态前三级滤芯过水量）
    // 泵流量数组(L/h)：三角洲50G=33(0.55), 75G=51(0.85), 100G=66(1.1), 200G=96(1.6), 300G=120(2.0), 400G=150(2.5)
    static const float pump_rates[] = {33.0f, 51.0f, 66.0f, 96.0f, 120.0f, 150.0f};
    float pump_lph = (cfg.pump_type < sizeof(pump_rates) / sizeof(pump_rates[0])) ?
                     pump_rates[cfg.pump_type] : 33.0f;  // 默认三角洲50G
    filter_mgr_set_pump_flow_lph(pump_lph);
    ESP_LOGI(TAG, "增压泵冲洗流量: %.1f L/h (类型%d)", pump_lph, cfg.pump_type);

    fsm_ctx.current_state = FSM_STATE_STANDBY;
    fsm_ctx.leak_detected = false;
    fsm_ctx.leak_confirmed_logged = false;
    fsm_ctx.leak_detect_start_time = 0;
    fsm_ctx.tank_full_detected = false;
    fsm_ctx.tank_full_detect_start_time = 0;
    fsm_ctx.need_water_detected = false;
    fsm_ctx.need_water_detect_start_time = 0;
    fsm_ctx.current_phase = FLUSH_PHASE_NONE;
    fsm_ctx.standby_manual = false;

    fsm_ctx.initialized = true;
    return ESP_OK;
}

esp_err_t fsm_deinit(void)
{
    if (!fsm_ctx.initialized) {
        return ESP_OK;
    }

    fsm_stop();

    if (fsm_ctx.event_queue) {
        vQueueDelete(fsm_ctx.event_queue);
        fsm_ctx.event_queue = NULL;
    }

    // 清理runtime_data_mutex
    if (fsm_ctx.runtime_data_mutex) {
        vSemaphoreDelete(fsm_ctx.runtime_data_mutex);
        fsm_ctx.runtime_data_mutex = NULL;
    }

    fsm_ctx.initialized = false;
    return ESP_OK;
}

esp_err_t fsm_start(void)
{
    if (!fsm_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&fsm_spinlock);
    if (fsm_ctx.running) {
        taskEXIT_CRITICAL(&fsm_spinlock);
        return ESP_OK;
    }
    fsm_ctx.running = true;
    fsm_ctx.stop_requested = false;  // 清除停止请求标志
    taskEXIT_CRITICAL(&fsm_spinlock);

    BaseType_t ret = xTaskCreate(
        fsm_task,
        "fsm_task",
        3072,  // 优化：FSM逻辑简单，状态管理+周期保存，3072字节足够
        NULL,
        5,
        &fsm_ctx.task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建状态机任务失败");
        fsm_ctx.running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "状态机启动，初始状态: %s", state_names[fsm_ctx.current_state]);
    return ESP_OK;
}

esp_err_t fsm_stop(void)
{
    if (!fsm_ctx.running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "停止状态机...");

    // 设置停止请求标志，通知FSM任务安全退出
    taskENTER_CRITICAL(&fsm_spinlock);
    fsm_ctx.stop_requested = true;
    fsm_ctx.running = false;
    taskEXIT_CRITICAL(&fsm_spinlock);

    // 等待任务自行退出（最多500ms）
    int wait_count = 0;
    while (fsm_ctx.task_handle != NULL && wait_count < 10) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wait_count++;
    }

    // 如果任务未退出，强制删除
    if (fsm_ctx.task_handle) {
        ESP_LOGW(TAG, "FSM任务未响应停止请求，强制删除");
        taskENTER_CRITICAL(&fsm_spinlock);
        vTaskDelete(fsm_ctx.task_handle);
        fsm_ctx.task_handle = NULL;
        taskEXIT_CRITICAL(&fsm_spinlock);
    }

    // 清除停止标志
    taskENTER_CRITICAL(&fsm_spinlock);
    fsm_ctx.stop_requested = false;
    taskEXIT_CRITICAL(&fsm_spinlock);

    stop_all_outputs();
    ESP_LOGI(TAG, "状态机已停止");
    return ESP_OK;
}

esp_err_t fsm_send_event(fsm_event_t event)
{
    if (!fsm_ctx.initialized || !fsm_ctx.event_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(fsm_ctx.event_queue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "事件队列已满");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

fsm_state_t fsm_get_state(void)
{
    taskENTER_CRITICAL(&fsm_spinlock);
    fsm_state_t state = fsm_ctx.current_state;
    taskEXIT_CRITICAL(&fsm_spinlock);
    return state;
}

const char* fsm_get_state_name(fsm_state_t state)
{
    if (state < FSM_STATE_COUNT) {
        return state_names[state];
    }
    return "未知";
}

const char* fsm_get_event_name(fsm_event_t event)
{
    if (event < sizeof(event_names) / sizeof(event_names[0])) {
        return event_names[event];
    }
    return "未知";
}

bool fsm_is_running(void)
{
    taskENTER_CRITICAL(&fsm_spinlock);
    bool running = fsm_ctx.running;
    taskEXIT_CRITICAL(&fsm_spinlock);
    return running;
}

bool fsm_is_stop_state(void)
{
    taskENTER_CRITICAL(&fsm_spinlock);
    fsm_state_t state = fsm_ctx.current_state;
    taskEXIT_CRITICAL(&fsm_spinlock);
    return (state == FSM_STATE_STOP || state == FSM_STATE_LEAK_ALARM);
}

bool fsm_is_producing(void)
{
    taskENTER_CRITICAL(&fsm_spinlock);
    fsm_state_t state = fsm_ctx.current_state;
    taskEXIT_CRITICAL(&fsm_spinlock);
    return state == FSM_STATE_PRODUCTION;
}

bool fsm_is_flushing(void)
{
    taskENTER_CRITICAL(&fsm_spinlock);
    fsm_state_t state = fsm_ctx.current_state;
    taskEXIT_CRITICAL(&fsm_spinlock);
    return (state == FSM_STATE_NORMAL_FLUSH || state == FSM_STATE_PURE_FLUSH);
}

esp_err_t fsm_get_runtime_data(fsm_runtime_data_t *data)
{
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 使用结构体内mutex保护原子读取（uint64_t字段在32位架构上需要两次读取）
     * mutex在fsm_init中创建，避免了双检锁模式的竞态问题 */
    if (!fsm_ctx.runtime_data_mutex) {
        /* 未初始化或mutex创建失败 */
        ESP_LOGE(TAG, "运行数据mutex未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(fsm_ctx.runtime_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(data, &fsm_ctx.runtime_data, sizeof(fsm_runtime_data_t));
        xSemaphoreGive(fsm_ctx.runtime_data_mutex);
        return ESP_OK;
    }
    // mutex超时：清零输出，防止调用方使用栈上的垃圾数据
    memset(data, 0, sizeof(fsm_runtime_data_t));
    ESP_LOGW(TAG, "fsm_get_runtime_data: mutex获取超时，返回零值");
    return ESP_ERR_TIMEOUT;
}

esp_err_t fsm_reset_runtime_data(void)
{
    /* 使用mutex保护，防止与fsm_get_runtime_data/fsm_get_stop_history并发访问竞态 */
    if (!fsm_ctx.runtime_data_mutex) {
        ESP_LOGE(TAG, "运行数据mutex未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(fsm_ctx.runtime_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(&fsm_ctx.runtime_data, 0, sizeof(fsm_runtime_data_t));
        fsm_ctx.stop_history_index = 0;
        xSemaphoreGive(fsm_ctx.runtime_data_mutex);
        ESP_LOGI(TAG, "运行数据已重置");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "fsm_reset_runtime_data: mutex获取超时");
    return ESP_ERR_TIMEOUT;
}

esp_err_t fsm_force_save_runtime(void)
{
    if (!fsm_ctx.runtime_data_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    /* mutex内拷贝统计值并清除脏标志，与周期保存路径(:1270)行为一致 */
    uint32_t prod_cycles, flush_cycles;
    uint64_t prod_time_sec, flush_time_sec;
    if (xSemaphoreTake(fsm_ctx.runtime_data_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "fsm_force_save_runtime: mutex获取超时");
        return ESP_ERR_TIMEOUT;
    }
    prod_cycles = fsm_ctx.runtime_data.total_production_cycles;
    flush_cycles = fsm_ctx.runtime_data.total_flush_cycles;
    prod_time_sec = fsm_ctx.runtime_data.total_production_time_sec;
    flush_time_sec = fsm_ctx.runtime_data.total_flush_time_sec;
    fsm_ctx.runtime_dirty = false;
    xSemaphoreGive(fsm_ctx.runtime_data_mutex);

    /* 同步到config_manager内存（由其统一持久化到water_purifier命名空间fsm_*键） */
    config_manager_sync_fsm_stats(prod_cycles, flush_cycles, prod_time_sec, flush_time_sec);
    return ESP_OK;
}

esp_err_t fsm_clear_stop(void)
{
    /* 通过事件队列复位，避免与 fsm_task 竞态 */
    if (fsm_ctx.current_state == FSM_STATE_STOP ||
        fsm_ctx.current_state == FSM_STATE_LEAK_ALARM) {
        return fsm_send_event(FSM_EVENT_RESET);
    }
    return ESP_ERR_INVALID_STATE;
}

uint32_t fsm_get_stop_history(stop_record_t *records, uint32_t max_count)
{
    if (!records || max_count == 0) {
        return 0;
    }

    if (!fsm_ctx.runtime_data_mutex) {
        ESP_LOGE(TAG, "运行数据mutex未初始化");
        return 0;
    }

    if (xSemaphoreTake(fsm_ctx.runtime_data_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "获取mutex超时，返回0条记录");
        return 0;
    }

    uint32_t total_written = fsm_ctx.stop_history_index;
    /* 已写入的总记录数决定了有多少有效记录 */
    uint32_t valid_count = (total_written < 16) ? total_written : 16;
    uint32_t count = (valid_count < max_count) ? valid_count : max_count;

    /* 从最旧的记录开始复制：
     * - 若总记录数<16，最旧在索引0
     * - 若总记录数>=16，最旧在当前写入位置（下一个将被覆盖的位置）
     */
    uint32_t oldest_idx = (total_written < 16) ? 0 : (total_written % 16);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (oldest_idx + i) % 16;
        memcpy(&records[i], &fsm_ctx.stop_history[idx], sizeof(stop_record_t));
    }

    xSemaphoreGive(fsm_ctx.runtime_data_mutex);
    return count;
}

// ==================== 配置接口实现 ====================

esp_err_t fsm_set_normal_flush_duration(uint32_t duration_sec)
{
    fsm_ctx.normal_flush_duration_sec = duration_sec;
    ESP_LOGI(TAG, "常规冲洗时间设置: %lu秒", duration_sec);
    return ESP_OK;
}

uint32_t fsm_get_normal_flush_duration(void)
{
    return fsm_ctx.normal_flush_duration_sec;
}

esp_err_t fsm_set_pure_flush_duration(uint32_t duration_sec)
{
    fsm_ctx.pure_flush_duration_sec = duration_sec;
    ESP_LOGI(TAG, "纯水洗膜时间设置: %lu秒", duration_sec);
    return ESP_OK;
}

uint32_t fsm_get_pure_flush_duration(void)
{
    return fsm_ctx.pure_flush_duration_sec;
}

esp_err_t fsm_set_filter_flush_duration(uint32_t duration_sec)
{
    fsm_ctx.filter_flush_duration_sec = duration_sec;
    ESP_LOGI(TAG, "换芯冲洗时间设置: %lu秒", duration_sec);
    return ESP_OK;
}

uint32_t fsm_get_filter_flush_duration(void)
{
    return fsm_ctx.filter_flush_duration_sec;
}

esp_err_t fsm_set_production_timeout(uint32_t timeout_sec)
{
    fsm_ctx.production_timeout_sec = timeout_sec;
    ESP_LOGI(TAG, "制水超时设置: %lu秒", timeout_sec);
    return ESP_OK;
}

esp_err_t fsm_set_leak_confirm_time(uint32_t time_sec)
{
    fsm_ctx.leak_confirm_time_ms = time_sec * 1000;
    ESP_LOGI(TAG, "漏水确认时间设置: %lu秒", time_sec);
    return ESP_OK;
}

esp_err_t fsm_set_tank_confirm_time(uint32_t time_sec)
{
    fsm_ctx.tank_confirm_time_ms = time_sec * 1000;
    ESP_LOGI(TAG, "压力桶确认时间设置: %lu秒", time_sec);
    return ESP_OK;
}

esp_err_t fsm_set_water_hammer_delays(uint32_t valve_open_delay_ms,
                                       uint32_t pump_stop_delay_ms,
                                       uint32_t valve_close_delay_ms)
{
    fsm_ctx.water_hammer_valve_open_delay_ms = valve_open_delay_ms;
    fsm_ctx.water_hammer_pump_stop_delay_ms = pump_stop_delay_ms;
    fsm_ctx.water_hammer_valve_close_delay_ms = valve_close_delay_ms;
    ESP_LOGI(TAG, "水锤延时设置: 开阀%lu ms, 停泵%lu ms, 关阀%lu ms",
             valve_open_delay_ms, pump_stop_delay_ms, valve_close_delay_ms);
    return ESP_OK;
}

esp_err_t fsm_set_runtime_save_interval(uint16_t interval_min)
{
    fsm_ctx.runtime_save_interval_min = interval_min;
    ESP_LOGI(TAG, "运行数据保存间隔设置: %u分钟", interval_min);
    return ESP_OK;
}

// ==================== 手动控制接口 ====================

esp_err_t fsm_manual_start_production(void)
{
    if (fsm_ctx.current_state != FSM_STATE_STANDBY) {
        return ESP_ERR_INVALID_STATE;
    }
    return fsm_send_event(FSM_EVENT_FORCE_PRODUCTION);
}

esp_err_t fsm_manual_start_flush(void)
{
    if (fsm_ctx.current_state != FSM_STATE_STANDBY) {
        return ESP_ERR_INVALID_STATE;
    }
    return fsm_send_event(FSM_EVENT_FORCE_FLUSH);
}

esp_err_t fsm_manual_normal_flush(void)
{
    if (fsm_ctx.current_state == FSM_STATE_STOP ||
        fsm_ctx.current_state == FSM_STATE_LEAK_ALARM) {
        return ESP_ERR_INVALID_STATE;
    }
    return fsm_send_event(FSM_EVENT_NORMAL_FLUSH);
}

esp_err_t fsm_manual_pure_flush(void)
{
    if (fsm_ctx.current_state == FSM_STATE_STOP ||
        fsm_ctx.current_state == FSM_STATE_LEAK_ALARM) {
        return ESP_ERR_INVALID_STATE;
    }
    return fsm_send_event(FSM_EVENT_PURE_FLUSH);
}

esp_err_t fsm_manual_filter_flush(void)
{
    if (fsm_ctx.current_state == FSM_STATE_STOP ||
        fsm_ctx.current_state == FSM_STATE_LEAK_ALARM) {
        return ESP_ERR_INVALID_STATE;
    }
    return fsm_send_event(FSM_EVENT_FILTER_FLUSH);
}

esp_err_t fsm_manual_go_standby(void)
{
    return fsm_send_event(FSM_EVENT_GO_STANDBY);
}

esp_err_t fsm_manual_shutdown(void)
{
    return fsm_send_event(FSM_EVENT_SHUTDOWN);
}

esp_err_t fsm_force_standby(void)
{
    // 与fsm_manual_go_standby功能相同，保留为向后兼容
    return fsm_manual_go_standby();
}

esp_err_t fsm_set_production_rate_by_membrane(uint8_t ro_type)
{
    static const float rates[] = {
        7.8f,   // 汇通50G:  0.13 L/min × 60
        12.0f,  // 汇通75G:  0.20 L/min × 60
        15.6f,  // 汇通100G: 0.26 L/min × 60
        31.2f,  // 汇通200G: 0.52 L/min × 60
        62.4f,  // 汇通400G: 1.04 L/min × 60
    };

    if (ro_type >= sizeof(rates) / sizeof(rates[0])) {
        return ESP_ERR_INVALID_ARG;
    }

    filter_mgr_set_production_rate(rates[ro_type]);

    // 泵冲洗流量由fsm_init根据pump_type配置，此处不再重复设置
    float pump_lph = filter_mgr_get_pump_flow_lph();

    ESP_LOGI(TAG, "RO膜制水速率: %.1f L/h, 增压泵冲洗流量: %.1f L/h",
             rates[ro_type], pump_lph);
    return ESP_OK;
}

float fsm_get_production_rate_lph(void)
{
    return filter_mgr_get_production_rate();
}

esp_err_t fsm_register_state_callback(fsm_state_callback_t callback)
{
    fsm_ctx.state_callback = callback;
    return ESP_OK;
}

esp_err_t fsm_get_status_string(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(buffer, buffer_size,
             "状态: %s\n"
             "制水周期: %lu\n"
             "冲洗周期: %lu\n"
             "总制水: %llu秒\n"
             "总冲洗: %llu秒",
             state_names[fsm_ctx.current_state],
             fsm_ctx.runtime_data.total_production_cycles,
             fsm_ctx.runtime_data.total_flush_cycles,
             fsm_ctx.runtime_data.total_production_time_sec,
             fsm_ctx.runtime_data.total_flush_time_sec);

    return ESP_OK;
}
