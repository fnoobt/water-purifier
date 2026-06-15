/**
 * @file gpio_config.h
 * @brief 净水器系统GPIO引脚配置定义
 * @note 基于ESP32-C3开发板引脚定义
 *
 * 引脚分配：
 *   输入设备:
 *     GPIO2  - 进水TDS探针 (ADC1_CH2)
 *     GPIO3  - 出水TDS探针 (ADC1_CH3)
 *     GPIO6  - 进水低压开关 (低电平=有水)
 *     GPIO7  - 压力桶压力开关 (闭合=制水)
 *     GPIO10 - 漏水检测模块 (低电平=漏水)
 *
 *   输出设备 (默认低电平触发):
 *     GPIO0  - 进水电磁阀
 *     GPIO1  - 废水电磁阀
 *     GPIO4  - 回水电磁阀
 *     GPIO5  - 增压泵
 *
 *   状态指示:
 *     GPIO12 - LED D4 (主状态指示)
 *     GPIO13 - LED D5 (次状态指示)
 */

#ifndef GPIO_CONFIG_H
#define GPIO_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 输入引脚定义 ====================

/**
 * @brief 输入设备GPIO配置
 * @note 所有输入开关均为低电平有效（闭合时为低电平）
 */
#define GPIO_LOW_PRESSURE_SWITCH     GPIO_NUM_6     // 进水低压开关（低电平=有水）
#define GPIO_TANK_PRESSURE_SWITCH    GPIO_NUM_7     // 压力桶压力开关（闭合=制水，低电平有效）
#define GPIO_WATER_LEAK_SENSOR       GPIO_NUM_10    // 漏水检测模块（低电平=漏水报警）

/**
 * @brief ADC通道配置 - TDS传感器
 * @note 使用ADC1通道
 */
// 进水TDS传感器（原水）
#define ADC_TDS_IN_CHANNEL           ADC_CHANNEL_2   // ADC1通道2 (GPIO2)
#define ADC_TDS_IN_ATTEN             ADC_ATTEN_DB_12 // 衰减12dB，测量范围0-3100mV
#define ADC_TDS_IN_GPIO              GPIO_NUM_2      // 进水TDS GPIO引脚

// 出水TDS传感器（纯水）
#define ADC_TDS_OUT_CHANNEL          ADC_CHANNEL_3   // ADC1通道3 (GPIO3)
#define ADC_TDS_OUT_ATTEN            ADC_ATTEN_DB_12 // 衰减12dB，测量范围0-3100mV
#define ADC_TDS_OUT_GPIO             GPIO_NUM_3      // 出水TDS GPIO引脚

#define ADC_TDS_RESOLUTION           ADC_BITWIDTH_12 // 12位分辨率，0-4095

// ==================== 输出引脚定义 ====================

/**
 * @brief 输出设备GPIO配置
 * @note 通过光耦继电器控制24V设备，默认低电平触发
 */
#define GPIO_INLET_VALVE             GPIO_NUM_0      // 进水电磁阀 (24V)
#define GPIO_WASTE_VALVE             GPIO_NUM_1      // 废水电磁阀 (24V)
#define GPIO_RETURN_VALVE            GPIO_NUM_4      // 回水电磁阀 (24V)
#define GPIO_BOOST_PUMP              GPIO_NUM_5      // 增压泵 (24V)

// ==================== 状态指示引脚定义 ====================

/**
 * @brief LED状态指示灯配置
 * @note 高电平有效
 */
#define GPIO_LED_STATUS_1            GPIO_NUM_12     // LED D4 - 主状态指示
#define GPIO_LED_STATUS_2            GPIO_NUM_13     // LED D5 - 次状态指示

// ==================== 电气参数配置 ====================

/**
 * @brief 继电器触发电平配置
 * @note 默认低电平触发，可通过网页配置
 */
#define RELAY_TRIGGER_LEVEL_DEFAULT  0               // 默认低电平触发（0=低电平，1=高电平）

/**
 * @brief 输入去抖动时间（毫秒）
 */
#define GPIO_DEBOUNCE_TIME_MS        50              // 按键去抖动时间

/**
 * @brief 漏水检测报警持续时间（毫秒）
 */
#define LEAK_ALARM_MIN_DURATION_MS   1000            // 最小报警持续时间，防止误报

// ==================== 系统参数配置 ====================
// @note 以下参数已移至运行时配置（config_manager / water_purifier_fsm.c 内部）：
// - 制水系统时间参数：水锤延时、冲洗时间等由 FSM 内部管理
// - TDS检测参数：采样数/间隔由 tds_sensor.c 内部管理
// - 报警阈值：由 config_manager 的 tds_inlet/outlet_threshold 配置
// - 系统维护参数：已由 FSM 状态机逻辑替代

#ifdef __cplusplus
}
#endif

#endif // GPIO_CONFIG_H