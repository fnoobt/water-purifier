/**
 * @file gpio_driver.c
 * @brief GPIO驱动层实现 - 净水器系统输入输出控制
 */

#include "gpio_driver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "GPIO_DRIVER";

// ==================== 防抖配置 ====================
#define DEBOUNCE_MS 50  // 防抖稳定时间(毫秒)

// ==================== 内部状态 ====================

// 防抖状态跟踪 (索引0=低压开关GPIO6, 1=压力桶开关GPIO7, 2=漏水传感器GPIO10)
static struct {
    uint64_t last_change_time[3];  // 上次状态变化时间(毫秒)
    int last_raw_state[3];         // 上次原始GPIO状态
    bool debounced_state[3];       // 已防抖的稳定状态
    bool first_read_done[3];       // 首次读取完成标志
} debounce_ctx = {0};

static struct {
    bool inlet_valve;       // 进水电磁阀
    bool waste_valve;       // 废水电磁阀
    bool return_valve;      // 回水电磁阀
    bool boost_pump;        // 增压泵
    bool led1;              // LED D4
    bool led2;              // LED D5
    uint8_t relay_trigger_level;  // 继电器触发电平 (0=低电平, 1=高电平)
} output_state = {
    .inlet_valve = false,
    .waste_valve = false,
    .return_valve = false,
    .boost_pump = false,
    .led1 = false,
    .led2 = false,
    .relay_trigger_level = RELAY_TRIGGER_LEVEL_DEFAULT
};

// 保护 output_state 的互斥锁（跨任务访问安全）
static SemaphoreHandle_t output_mutex = NULL;

// ==================== 继电器触发电平配置 ====================

esp_err_t gpio_driver_set_relay_trigger_level(uint8_t level)
{
    uint8_t new_level = (level == 0) ? 0 : 1;
    /* 必须获取mutex才能修改，拒绝无保护访问以避免竞态 */
    if (output_mutex && xSemaphoreTake(output_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        output_state.relay_trigger_level = new_level;
        xSemaphoreGive(output_mutex);
        ESP_LOGI(TAG, "继电器触发电平设置为: %s", new_level ? "高电平" : "低电平");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "设置继电器触发电平失败：mutex获取超时");
    return ESP_ERR_TIMEOUT;
}

uint8_t gpio_driver_get_relay_trigger_level(void)
{
    uint8_t level = RELAY_TRIGGER_LEVEL_DEFAULT;  // 默认值作为fallback
    if (output_mutex && xSemaphoreTake(output_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        level = output_state.relay_trigger_level;
        xSemaphoreGive(output_mutex);
    } else {
        /* 读取失败时返回默认值，避免返回不一致数据 */
        ESP_LOGW(TAG, "获取继电器触发电平失败：使用默认值");
    }
    return level;
}

// ==================== 输入初始化 ====================

esp_err_t gpio_driver_init_inputs(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "初始化输入GPIO引脚...");

    // 配置低压开关 (GPIO6)
    ret = gpio_reset_pin(GPIO_LOW_PRESSURE_SWITCH);
    if (ret != ESP_OK) return ret;
    ret = gpio_set_direction(GPIO_LOW_PRESSURE_SWITCH, GPIO_MODE_INPUT);
    if (ret != ESP_OK) return ret;
    gpio_set_pull_mode(GPIO_LOW_PRESSURE_SWITCH, GPIO_PULLUP_ONLY);

    // 配置压力桶压力开关 (GPIO7)
    ret = gpio_reset_pin(GPIO_TANK_PRESSURE_SWITCH);
    if (ret != ESP_OK) return ret;
    ret = gpio_set_direction(GPIO_TANK_PRESSURE_SWITCH, GPIO_MODE_INPUT);
    if (ret != ESP_OK) return ret;
    gpio_set_pull_mode(GPIO_TANK_PRESSURE_SWITCH, GPIO_PULLUP_ONLY);

    // 配置漏水检测 (GPIO10)
    ret = gpio_reset_pin(GPIO_WATER_LEAK_SENSOR);
    if (ret != ESP_OK) return ret;
    ret = gpio_set_direction(GPIO_WATER_LEAK_SENSOR, GPIO_MODE_INPUT);
    if (ret != ESP_OK) return ret;
    gpio_set_pull_mode(GPIO_WATER_LEAK_SENSOR, GPIO_PULLUP_ONLY);

    ESP_LOGI(TAG, "输入GPIO初始化完成");
    return ESP_OK;
}

// ==================== 输出初始化 ====================

esp_err_t gpio_driver_init_outputs(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "初始化输出GPIO引脚...");

    // 获取当前触发电平
    uint8_t inactive_level = output_state.relay_trigger_level ? 0 : 1;

    // 配置进水电磁阀 (GPIO0)
    ret = gpio_reset_pin(GPIO_INLET_VALVE);
    if (ret != ESP_OK) return ret;
    ret = gpio_set_direction(GPIO_INLET_VALVE, GPIO_MODE_OUTPUT);
    if (ret != ESP_OK) return ret;
    gpio_set_level(GPIO_INLET_VALVE, inactive_level);

    // 配置废水电磁阀 (GPIO1)
    ret = gpio_reset_pin(GPIO_WASTE_VALVE);
    if (ret != ESP_OK) return ret;
    ret = gpio_set_direction(GPIO_WASTE_VALVE, GPIO_MODE_OUTPUT);
    if (ret != ESP_OK) return ret;
    gpio_set_level(GPIO_WASTE_VALVE, inactive_level);

    // 配置回水电磁阀 (GPIO4)
    ret = gpio_reset_pin(GPIO_RETURN_VALVE);
    if (ret != ESP_OK) return ret;
    ret = gpio_set_direction(GPIO_RETURN_VALVE, GPIO_MODE_OUTPUT);
    if (ret != ESP_OK) return ret;
    gpio_set_level(GPIO_RETURN_VALVE, inactive_level);

    // 配置增压泵 (GPIO5)
    ret = gpio_reset_pin(GPIO_BOOST_PUMP);
    if (ret != ESP_OK) return ret;
    ret = gpio_set_direction(GPIO_BOOST_PUMP, GPIO_MODE_OUTPUT);
    if (ret != ESP_OK) return ret;
    gpio_set_level(GPIO_BOOST_PUMP, inactive_level);

    ESP_LOGI(TAG, "输出GPIO初始化完成 (触发电平: %s)",
             output_state.relay_trigger_level ? "高电平" : "低电平");

    // 创建输出状态互斥锁
    if (!output_mutex) {
        output_mutex = xSemaphoreCreateMutex();
        if (!output_mutex) {
            ESP_LOGW(TAG, "创建输出互斥锁失败");
        }
    }

    return ESP_OK;
}

// ==================== LED初始化 ====================

esp_err_t gpio_driver_init_leds(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "初始化LED引脚...");

    // 配置LED D4 (GPIO12)
    ret = gpio_reset_pin(GPIO_LED_STATUS_1);
    if (ret != ESP_OK) return ret;
    ret = gpio_set_direction(GPIO_LED_STATUS_1, GPIO_MODE_OUTPUT);
    if (ret != ESP_OK) return ret;
    gpio_set_level(GPIO_LED_STATUS_1, 0);

    // 配置LED D5 (GPIO13)
    ret = gpio_reset_pin(GPIO_LED_STATUS_2);
    if (ret != ESP_OK) return ret;
    ret = gpio_set_direction(GPIO_LED_STATUS_2, GPIO_MODE_OUTPUT);
    if (ret != ESP_OK) return ret;
    gpio_set_level(GPIO_LED_STATUS_2, 0);

    ESP_LOGI(TAG, "LED初始化完成");
    return ESP_OK;
}

// ==================== 输入读取函数 ====================

/**
 * @brief 防抖读取GPIO输入
 * @param pin GPIO引脚号
 * @param idx 防抖状态索引 (0=低压开关, 1=压力桶开关, 2=漏水传感器)
 * @return 防抖后的稳定状态 (true=低电平/开关闭合)
 * @note 首次读取时进行多次采样确认，避免启动时的电气噪声误判
 */
static bool debounced_read(gpio_num_t pin, int idx)
{
    int raw = gpio_get_level(pin);
    uint64_t now = esp_timer_get_time() / 1000;  // 毫秒时间戳

    // 首次读取：进行多次采样确认初始状态（避免启动噪声误判）
    if (!debounce_ctx.first_read_done[idx]) {
        // 快速采样3次确认状态一致
        int sample1 = gpio_get_level(pin);
        esp_rom_delay_us(1000);  // 等待1ms
        int sample2 = gpio_get_level(pin);
        esp_rom_delay_us(1000);
        int sample3 = gpio_get_level(pin);

        // 三次采样一致才认为初始状态稳定
        bool stable = (sample1 == sample2 && sample2 == sample3);
        int stable_value = stable ? sample1 : raw;

        debounce_ctx.last_raw_state[idx] = stable_value;
        debounce_ctx.debounced_state[idx] = (stable_value == 0);
        debounce_ctx.last_change_time[idx] = now;
        debounce_ctx.first_read_done[idx] = true;

        if (!stable) {
            // 样本不一致，标记为需要防抖确认（返回当前值但立即触发防抖逻辑）
            ESP_LOGW(TAG, "GPIO%d首次读取不稳定，样本=%d/%d/%d，等待防抖确认",
                     pin, sample1, sample2, sample3);
        }

        return (stable_value == 0);
    }

    // 检测到状态变化：记录变化时间和新状态
    if (raw != debounce_ctx.last_raw_state[idx]) {
        debounce_ctx.last_raw_state[idx] = raw;
        debounce_ctx.last_change_time[idx] = now;
    }
    // 状态稳定超过DEBOUNCE_MS：更新防抖状态
    else if (now - debounce_ctx.last_change_time[idx] >= DEBOUNCE_MS) {
        debounce_ctx.debounced_state[idx] = (raw == 0);
    }

    return debounce_ctx.debounced_state[idx];
}

bool gpio_driver_read_low_pressure(void)
{
    // 低电平表示有水（开关闭合），使用防抖逻辑
    return debounced_read(GPIO_LOW_PRESSURE_SWITCH, 0);
}

bool gpio_driver_read_tank_pressure(void)
{
    // 低电平表示需要制水（开关闭合，压力桶未满），使用防抖逻辑
    return debounced_read(GPIO_TANK_PRESSURE_SWITCH, 1);
}

bool gpio_driver_read_water_leak(void)
{
    // 低电平表示检测到漏水，使用防抖逻辑
    return debounced_read(GPIO_WATER_LEAK_SENSOR, 2);
}

esp_err_t gpio_driver_get_all_inputs(bool *low_pressure, bool *tank_pressure, bool *water_leak)
{
    if (low_pressure) *low_pressure = gpio_driver_read_low_pressure();
    if (tank_pressure) *tank_pressure = gpio_driver_read_tank_pressure();
    if (water_leak) *water_leak = gpio_driver_read_water_leak();
    return ESP_OK;
}

// ==================== 输出控制函数 ====================

/**
 * @brief 计算实际输出电平（使用缓存的触发电平避免竞态）
 * @param state 设备状态 (true=开启)
 * @param trigger_level 当前继电器触发电平
 * @return GPIO输出电平
 */
static inline int calc_output_level_with_trigger(bool state, uint8_t trigger_level)
{
    // 如果触发电平是高电平(1)：开启=1，关闭=0
    // 如果触发电平是低电平(0)：开启=0，关闭=1
    if (trigger_level) {
        return state ? 1 : 0;  // 高电平触发
    } else {
        return state ? 0 : 1;  // 低电平触发
    }
}

/**
 * @brief 安全获取继电器触发电平（用于输出计算）
 * @return 当前触发电平
 */
static inline uint8_t safe_get_trigger_level(void)
{
    /* 快速路径：无锁读取（relay_trigger_level只在初始化时设置，运行期间不变）
     * 如果严格需要一致性，可以在调用前获取mutex */
    return output_state.relay_trigger_level;
}

esp_err_t gpio_driver_set_inlet_valve(bool state)
{
    uint8_t trigger = safe_get_trigger_level();
    int level = calc_output_level_with_trigger(state, trigger);
    output_state.inlet_valve = state;  // 先更新状态，确保一致性
    esp_err_t ret = gpio_set_level(GPIO_INLET_VALVE, level);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "进水阀GPIO写入失败: %s (期望=%d)", esp_err_to_name(ret), level);
    } else {
        ESP_LOGD(TAG, "进水阀: %s (GPIO=%d)", state ? "开启" : "关闭", level);
    }
    return ret;
}

esp_err_t gpio_driver_set_waste_valve(bool state)
{
    uint8_t trigger = safe_get_trigger_level();
    int level = calc_output_level_with_trigger(state, trigger);
    output_state.waste_valve = state;  // 先更新状态，确保一致性
    esp_err_t ret = gpio_set_level(GPIO_WASTE_VALVE, level);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "废水阀GPIO写入失败: %s (期望=%d)", esp_err_to_name(ret), level);
    } else {
        ESP_LOGD(TAG, "废水阀: %s (GPIO=%d)", state ? "开启" : "关闭", level);
    }
    return ret;
}

esp_err_t gpio_driver_set_return_valve(bool state)
{
    uint8_t trigger = safe_get_trigger_level();
    int level = calc_output_level_with_trigger(state, trigger);
    output_state.return_valve = state;  // 先更新状态，确保一致性
    esp_err_t ret = gpio_set_level(GPIO_RETURN_VALVE, level);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "回水阀GPIO写入失败: %s (期望=%d)", esp_err_to_name(ret), level);
    } else {
        ESP_LOGD(TAG, "回水阀: %s (GPIO=%d)", state ? "开启" : "关闭", level);
    }
    return ret;
}

esp_err_t gpio_driver_set_boost_pump(bool state)
{
    uint8_t trigger = safe_get_trigger_level();
    int level = calc_output_level_with_trigger(state, trigger);
    output_state.boost_pump = state;  // 先更新状态，确保一致性
    esp_err_t ret = gpio_set_level(GPIO_BOOST_PUMP, level);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "增压泵GPIO写入失败: %s (期望=%d)", esp_err_to_name(ret), level);
    } else {
        ESP_LOGD(TAG, "增压泵: %s (GPIO=%d)", state ? "开启" : "关闭", level);
    }
    return ret;
}

esp_err_t gpio_driver_emergency_stop(void)
{
    ESP_LOGW(TAG, "紧急停止！关闭所有输出设备");

    /* 紧急停止必须快速可靠执行：
     * 尝试获取mutex，如果失败仍继续执行（紧急操作优先保证响应速度）
     * 无论mutex是否获取成功，GPIO输出立即关闭 */
    bool mutex_acquired = (output_mutex && xSemaphoreTake(output_mutex, pdMS_TO_TICKS(5)) == pdTRUE);

    uint8_t trigger = safe_get_trigger_level();
    int inactive_level = calc_output_level_with_trigger(false, trigger);

    /* 先设置GPIO（硬件优先），再更新内部状态 */
    gpio_set_level(GPIO_INLET_VALVE, inactive_level);
    gpio_set_level(GPIO_WASTE_VALVE, inactive_level);
    gpio_set_level(GPIO_RETURN_VALVE, inactive_level);
    gpio_set_level(GPIO_BOOST_PUMP, inactive_level);

    output_state.inlet_valve = false;
    output_state.waste_valve = false;
    output_state.return_valve = false;
    output_state.boost_pump = false;

    if (mutex_acquired) {
        xSemaphoreGive(output_mutex);
    }

    return ESP_OK;
}

// ==================== LED控制函数 ====================

esp_err_t gpio_driver_set_led1(bool state)
{
    esp_err_t ret = gpio_set_level(GPIO_LED_STATUS_1, state ? 1 : 0);
    if (ret == ESP_OK) {
        output_state.led1 = state;
    }
    return ret;
}

esp_err_t gpio_driver_set_led2(bool state)
{
    esp_err_t ret = gpio_set_level(GPIO_LED_STATUS_2, state ? 1 : 0);
    if (ret == ESP_OK) {
        output_state.led2 = state;
    }
    return ret;
}

esp_err_t gpio_driver_set_led_mode(uint8_t led_num, led_mode_t mode)
{
    // LED模式由LED控制任务处理，这里仅设置基本状态
    // 实际闪烁逻辑在water_purifier_fsm.c中实现
    (void)led_num;
    (void)mode;
    return ESP_OK;
}

// ==================== 状态查询函数 ====================

esp_err_t gpio_driver_get_output_states(bool *inlet_valve, bool *waste_valve,
                                         bool *return_valve, bool *boost_pump)
{
    if (output_mutex && xSemaphoreTake(output_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (inlet_valve) *inlet_valve = output_state.inlet_valve;
        if (waste_valve) *waste_valve = output_state.waste_valve;
        if (return_valve) *return_valve = output_state.return_valve;
        if (boost_pump) *boost_pump = output_state.boost_pump;
        xSemaphoreGive(output_mutex);
        return ESP_OK;
    }
    // 降级：无锁读取（可能不一致）
    if (inlet_valve) *inlet_valve = output_state.inlet_valve;
    if (waste_valve) *waste_valve = output_state.waste_valve;
    if (return_valve) *return_valve = output_state.return_valve;
    if (boost_pump) *boost_pump = output_state.boost_pump;
    return ESP_ERR_TIMEOUT;
}

