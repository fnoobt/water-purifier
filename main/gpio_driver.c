/**
 * @file gpio_driver.c
 * @brief GPIO驱动层实现 - 净水器系统输入输出控制
 */

#include "gpio_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "GPIO_DRIVER";

// ==================== 内部状态 ====================

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

// ==================== 继电器触发电平配置 ====================

esp_err_t gpio_driver_set_relay_trigger_level(uint8_t level)
{
    output_state.relay_trigger_level = (level == 0) ? 0 : 1;
    ESP_LOGI(TAG, "继电器触发电平设置为: %s", level ? "高电平" : "低电平");
    return ESP_OK;
}

uint8_t gpio_driver_get_relay_trigger_level(void)
{
    return output_state.relay_trigger_level;
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

bool gpio_driver_read_low_pressure(void)
{
    // 低电平表示有水（开关闭合）
    return (gpio_get_level(GPIO_LOW_PRESSURE_SWITCH) == 0);
}

bool gpio_driver_read_tank_pressure(void)
{
    // 低电平表示需要制水（开关闭合，压力桶未满）
    return (gpio_get_level(GPIO_TANK_PRESSURE_SWITCH) == 0);
}

bool gpio_driver_read_water_leak(void)
{
    // 低电平表示检测到漏水
    return (gpio_get_level(GPIO_WATER_LEAK_SENSOR) == 0);
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
 * @brief 计算实际输出电平
 * @param state 设备状态 (true=开启)
 * @return GPIO输出电平
 */
static inline int calc_output_level(bool state)
{
    // 如果触发电平是高电平(1)：开启=1，关闭=0
    // 如果触发电平是低电平(0)：开启=0，关闭=1
    if (output_state.relay_trigger_level) {
        return state ? 1 : 0;  // 高电平触发
    } else {
        return state ? 0 : 1;  // 低电平触发
    }
}

esp_err_t gpio_driver_set_inlet_valve(bool state)
{
    int level = calc_output_level(state);
    esp_err_t ret = gpio_set_level(GPIO_INLET_VALVE, level);
    if (ret == ESP_OK) {
        output_state.inlet_valve = state;
        ESP_LOGD(TAG, "进水阀: %s (GPIO=%d)", state ? "开启" : "关闭", level);
    }
    return ret;
}

esp_err_t gpio_driver_set_waste_valve(bool state)
{
    int level = calc_output_level(state);
    esp_err_t ret = gpio_set_level(GPIO_WASTE_VALVE, level);
    if (ret == ESP_OK) {
        output_state.waste_valve = state;
        ESP_LOGD(TAG, "废水阀: %s (GPIO=%d)", state ? "开启" : "关闭", level);
    }
    return ret;
}

esp_err_t gpio_driver_set_return_valve(bool state)
{
    int level = calc_output_level(state);
    esp_err_t ret = gpio_set_level(GPIO_RETURN_VALVE, level);
    if (ret == ESP_OK) {
        output_state.return_valve = state;
        ESP_LOGD(TAG, "回水阀: %s (GPIO=%d)", state ? "开启" : "关闭", level);
    }
    return ret;
}

esp_err_t gpio_driver_set_boost_pump(bool state)
{
    int level = calc_output_level(state);
    esp_err_t ret = gpio_set_level(GPIO_BOOST_PUMP, level);
    if (ret == ESP_OK) {
        output_state.boost_pump = state;
        ESP_LOGD(TAG, "增压泵: %s (GPIO=%d)", state ? "开启" : "关闭", level);
    }
    return ret;
}

esp_err_t gpio_driver_emergency_stop(void)
{
    ESP_LOGW(TAG, "紧急停止！关闭所有输出设备");

    int inactive_level = output_state.relay_trigger_level ? 0 : 1;

    output_state.inlet_valve = false;
    output_state.waste_valve = false;
    output_state.return_valve = false;
    output_state.boost_pump = false;

    gpio_set_level(GPIO_INLET_VALVE, inactive_level);
    gpio_set_level(GPIO_WASTE_VALVE, inactive_level);
    gpio_set_level(GPIO_RETURN_VALVE, inactive_level);
    gpio_set_level(GPIO_BOOST_PUMP, inactive_level);

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
    if (inlet_valve) *inlet_valve = output_state.inlet_valve;
    if (waste_valve) *waste_valve = output_state.waste_valve;
    if (return_valve) *return_valve = output_state.return_valve;
    if (boost_pump) *boost_pump = output_state.boost_pump;
    return ESP_OK;
}

// ==================== 去抖动处理 ====================

bool gpio_driver_debounce_read(gpio_num_t gpio, uint32_t debounce_time)
{
    bool initial_state = (gpio_get_level(gpio) == 0);
    vTaskDelay(pdMS_TO_TICKS(debounce_time));

    bool final_state = (gpio_get_level(gpio) == 0);

    // 如果状态稳定，返回最终状态
    return (initial_state == final_state) ? final_state : initial_state;
}