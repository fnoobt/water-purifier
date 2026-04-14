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

// ==================== 引脚模式宏定义 ====================

#define GPIO_INPUT_PIN_MASK  ((1ULL << GPIO_LOW_PRESSURE_SWITCH) | \
                              (1ULL << GPIO_TANK_PRESSURE_SWITCH) | \
                              (1ULL << GPIO_WATER_LEAK_SENSOR))

#define GPIO_OUTPUT_PIN_MASK ((1ULL << GPIO_INLET_VALVE) | \
                              (1ULL << GPIO_WASTE_VALVE) | \
                              (1ULL << GPIO_RETURN_VALVE) | \
                              (1ULL << GPIO_BOOST_PUMP) | \
                              (1ULL << GPIO_LED_STATUS_1) | \
                              (1ULL << GPIO_LED_STATUS_2))

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

// ==================== 设备状态枚举 ====================

/**
 * @brief 开关状态枚举
 */
typedef enum {
    SWITCH_STATE_OPEN = 0,         // 断开状态
    SWITCH_STATE_CLOSED = 1,       // 闭合状态
    SWITCH_STATE_UNKNOWN = 0xFF    // 未知状态
} switch_state_t;

/**
 * @brief 电磁阀状态枚举
 */
typedef enum {
    VALVE_STATE_CLOSED = 0,        // 关闭状态
    VALVE_STATE_OPEN = 1,          // 开启状态
    VALVE_STATE_UNKNOWN = 0xFF     // 未知状态
} valve_state_t;

/**
 * @brief 增压泵状态枚举
 */
typedef enum {
    PUMP_STATE_OFF = 0,            // 关闭状态
    PUMP_STATE_ON = 1,             // 开启状态
    PUMP_STATE_UNKNOWN = 0xFF      // 未知状态
} pump_state_t;

/**
 * @brief LED指示模式枚举
 */
typedef enum {
    LED_MODE_OFF = 0,              // 常灭
    LED_MODE_ON = 1,               // 常亮
    LED_MODE_BLINK_SLOW = 2,       // 慢闪（1Hz）
    LED_MODE_BLINK_FAST = 3,       // 快闪（5Hz）
    LED_MODE_BLINK_DOUBLE = 4      // 双闪（闪两次停顿）
} led_mode_t;

// ==================== 系统参数配置 ====================

/**
 * @brief 制水系统时间参数（单位：毫秒）
 */
#define SYSTEM_STARTUP_DELAY_MS        3000          // 系统启动延时
#define PUMP_STARTUP_DELAY_MS          2000          // 增压泵启动延时
#define PRE_FILL_TIME_MS               5000          // 预充水时间（制水前）
#define FLUSH_DURATION_DEFAULT_MS      30000         // RO膜冲洗时间默认值（30秒）
#define MAX_PRODUCTION_TIME_MS         (3 * 3600000) // 单次制水最大时间（3小时）
#define COOLDOWN_TIME_MS               30000         // 冷却时间（30秒）

/**
 * @brief TDS检测参数
 */
#define TDS_SAMPLING_INTERVAL_MS       1000          // TDS采样间隔（1秒）
#define TDS_SAMPLE_COUNT               10            // TDS采样次数（取平均值）
#define TDS_ALARM_THRESHOLD_DEFAULT    100.0f        // TDS报警阈值默认值（ppm）

/**
 * @brief 系统维护参数
 */
#define FORCE_FLUSH_INTERVAL_MS        3600000       // 强制冲洗间隔（1小时）

#ifdef __cplusplus
}
#endif

#endif // GPIO_CONFIG_H