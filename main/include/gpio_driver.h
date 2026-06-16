/**
 * @file gpio_driver.h
 * @brief GPIO驱动层接口 - 净水器系统输入输出控制
 * @note 提供统一的GPIO操作接口，处理去抖动和状态管理
 */

#ifndef GPIO_DRIVER_H
#define GPIO_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "gpio_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 初始化接口 ====================

/**
 * @brief 初始化所有输入GPIO引脚
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t gpio_driver_init_inputs(void);

/**
 * @brief 初始化所有输出GPIO引脚
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t gpio_driver_init_outputs(void);

/**
 * @brief 初始化LED GPIO引脚
 * @return ESP_OK 成功
 */
esp_err_t gpio_driver_init_leds(void);

/**
 * @brief 反初始化GPIO驱动，释放资源
 * @return ESP_OK 成功
 */
esp_err_t gpio_driver_deinit(void);

// ==================== 继电器触发电平配置 ====================

/**
 * @brief 设置继电器触发电平
 * @param level 0=低电平触发，1=高电平触发
 * @return ESP_OK 成功
 */
esp_err_t gpio_driver_set_relay_trigger_level(uint8_t level);

/**
 * @brief 获取继电器触发电平
 * @return 0=低电平触发，1=高电平触发
 */
uint8_t gpio_driver_get_relay_trigger_level(void);

// ==================== 输入接口 ====================

/**
 * @brief 读取低压开关状态（进水压力）
 * @return true 有水（开关闭合/低电平），false 缺水（开关断开）
 */
bool gpio_driver_read_low_pressure(void);

/**
 * @brief 读取压力桶压力开关状态
 * @return true 需要制水（开关闭合/低电平），false 水满（开关断开）
 */
bool gpio_driver_read_tank_pressure(void);

/**
 * @brief 读取漏水检测状态
 * @return true 检测到漏水（低电平），false 无漏水
 */
bool gpio_driver_read_water_leak(void);

/**
 * @brief 获取所有开关输入状态（一次性读取）
 * @param low_pressure 输出：低压状态（true=有水）
 * @param tank_pressure 输出：压力桶状态（true=需要制水）
 * @param water_leak 输出：漏水状态（true=漏水）
 * @return ESP_OK 成功
 */
esp_err_t gpio_driver_get_all_inputs(bool *low_pressure, bool *tank_pressure, bool *water_leak);

// ==================== 输出接口 ====================

/**
 * @brief 设置进水电磁阀状态
 * @param state true=开启，false=关闭
 * @return ESP_OK 成功
 */
esp_err_t gpio_driver_set_inlet_valve(bool state);

/**
 * @brief 设置废水电磁阀状态
 * @param state true=开启，false=关闭
 * @return ESP_OK 成功
 */
esp_err_t gpio_driver_set_waste_valve(bool state);

/**
 * @brief 设置回水电磁阀状态
 * @param state true=开启，false=关闭
 * @return ESP_OK 成功
 */
esp_err_t gpio_driver_set_return_valve(bool state);

/**
 * @brief 设置增压泵状态
 * @param state true=开启，false=关闭
 * @return ESP_OK 成功
 */
esp_err_t gpio_driver_set_boost_pump(bool state);

/**
 * @brief 关闭所有输出（紧急停止）
 * @note 触发后所有set_*函数将拒绝开启输出，直到调用clear_emergency()
 * @return ESP_OK 成功
 */
esp_err_t gpio_driver_emergency_stop(void);

/**
 * @brief 清除紧急停止状态，允许正常操作
 * @note 仅在确认安全后由FSM或用户手动调用
 * @return ESP_OK 成功
 */
esp_err_t gpio_driver_clear_emergency(void);

// ==================== LED控制接口 ====================

/**
 * @brief 设置LED1状态
 * @param state true=亮，false=灭
 * @return ESP_OK 成功
 */
esp_err_t gpio_driver_set_led1(bool state);

/**
 * @brief 设置LED2状态
 * @param state true=亮，false=灭
 * @return ESP_OK 成功
 */
esp_err_t gpio_driver_set_led2(bool state);

// ==================== 状态查询接口 ====================

/**
 * @brief 获取当前输出设备状态
 * @param inlet_valve 输出：进水阀状态
 * @param waste_valve 输出：废水阀状态
 * @param return_valve 输出：回水阀状态
 * @param boost_pump 输出：增压泵状态
 * @return ESP_OK 成功
 */
esp_err_t gpio_driver_get_output_states(bool *inlet_valve, bool *waste_valve,
                                         bool *return_valve, bool *boost_pump);

#ifdef __cplusplus
}
#endif

#endif // GPIO_DRIVER_H