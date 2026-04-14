/**
 * @file tds_sensor.h
 * @brief TDS传感器检测模块接口（支持双TDS）
 * @note 支持进水TDS（原水，GPIO2）和出水TDS（纯水，GPIO3）同时检测
 *        支持五级滤芯寿命管理
 */

#ifndef TDS_SENSOR_H
#define TDS_SENSOR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== TDS模块ID定义 ====================

/**
 * @brief TDS传感器ID枚举
 */
typedef enum {
    TDS_SENSOR_INLET = 0,    // 进水TDS传感器（原水，GPIO2）
    TDS_SENSOR_OUTLET = 1,   // 出水TDS传感器（纯水，GPIO3）
    TDS_SENSOR_COUNT = 2     // TDS传感器总数
} tds_sensor_id_t;

// ==================== 五级滤芯定义 ====================

/**
 * @brief 五级滤芯类型枚举
 */
typedef enum {
    FILTER_PP_COTTON = 0,        // PP棉滤芯（第1级）
    FILTER_GRANULAR_CARBON,      // 颗粒活性炭（第2级）
    FILTER_COMPRESSED_CARBON,    // 压缩活性炭（第3级）
    FILTER_RO_MEMBRANE,          // RO膜（第4级）
    FILTER_POST_CARBON,          // 后置活性炭（第5级）
    FILTER_COUNT                 // 滤芯总数
} filter_type_t;

/**
 * @brief 滤芯默认寿命配置（升）
 */
#define FILTER_DEFAULT_LIFE_PP           3000    // PP棉：3000升（约3-6个月）
#define FILTER_DEFAULT_LIFE_GRANULAR     4000    // 颗粒活性炭：4000升（约6个月）
#define FILTER_DEFAULT_LIFE_COMPRESSED   4000    // 压缩活性炭：4000升（约6个月）
#define FILTER_DEFAULT_LIFE_RO           8000    // RO膜：8000升（约24个月）
#define FILTER_DEFAULT_LIFE_POST         4000    // 后置活性炭：4000升（约12个月）

/**
 * @brief 单个滤芯寿命信息结构体
 */
typedef struct {
    uint8_t percentage;           // 剩余寿命百分比（0-100），基于水量
    uint8_t time_percentage;      // 剩余寿命百分比（0-100），基于日历时间
    uint8_t effective_percentage;  // 有效寿命百分比（水量和时间的最小值）
    uint32_t used_liters;         // 已处理水量（升）
    uint32_t total_liters;        // 滤芯总容量（升）
    uint32_t time_limit_hours;    // 时间寿命上限（小时）
    uint32_t install_time;        // 安装时间戳（Unix秒，NTP墙钟）
    uint32_t last_reset_time;     // 上次重置时间戳（Unix秒，NTP墙钟）
    bool replacement_needed;       // 是否需要更换
    char name[16];                // 滤芯名称
} filter_info_t;

/**
 * @brief 五级滤芯状态结构体
 */
typedef struct {
    filter_info_t filters[FILTER_COUNT];  // 五级滤芯信息
    uint32_t total_water_used;             // 总用水量（升）
    bool any_filter_needs_replacement;     // 是否有滤芯需要更换
} filters_status_t;

// ==================== TDS参数定义 ====================

/**
 * @brief TDS测量结果结构体
 */
typedef struct {
    float tds_value;          // TDS值（ppm）
    float ec_value;           // 电导率（μS/cm）
    float voltage;            // 原始电压值（mV）
    float temperature;        // 温度值（°C），默认25°C
    bool valid;               // 数据是否有效
    uint64_t timestamp;       // 时间戳（微秒）
} tds_measurement_t;

/**
 * @brief 双TDS数据结构
 */
typedef struct {
    tds_measurement_t inlet;  // 进水TDS
    tds_measurement_t outlet; // 出水TDS
    float reduction_rate;     // TDS去除率（%）
    bool both_valid;          // 两个TDS数据都有效
} tds_dual_measurement_t;

/**
 * @brief 滤芯寿命信息结构体（兼容旧接口）
 */
typedef struct {
    uint8_t percentage;       // 剩余寿命百分比（0-100）
    uint32_t used_liters;     // 已处理水量（升）
    uint32_t total_liters;    // 滤芯总容量（升）
    uint32_t install_time;    // 安装时间戳（Unix秒，NTP墙钟）
    bool replacement_needed;  // 是否需要更换
} filter_life_t;

// ==================== 初始化和控制接口 ====================

/**
 * @brief 初始化TDS传感器模块
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_init(void);

/**
 * @brief 反初始化TDS传感器模块
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_deinit(void);

/**
 * @brief 启动TDS连续测量任务
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_start(void);

/**
 * @brief 停止TDS连续测量任务
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_stop(void);

// ==================== 测量接口 ====================

/**
 * @brief 执行单次TDS测量
 * @param sensor_id TDS传感器ID
 * @param measurement 输出：测量结果
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_measure(tds_sensor_id_t sensor_id, tds_measurement_t *measurement);

/**
 * @brief 执行双TDS同时测量
 * @param dual_measurement 输出：双TDS测量结果
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_measure_dual(tds_dual_measurement_t *dual_measurement);

/**
 * @brief 获取指定TDS传感器的最新测量值
 * @param sensor_id TDS传感器ID
 * @param measurement 输出：最新测量结果
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_get_latest(tds_sensor_id_t sensor_id, tds_measurement_t *measurement);

/**
 * @brief 获取双TDS最新测量值
 * @param dual_measurement 输出：双TDS测量结果
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_get_latest_dual(tds_dual_measurement_t *dual_measurement);

// ==================== 校准接口 ====================

/**
 * @brief TDS校准参数结构体
 */
typedef struct {
    float offset;    // 偏移量（ppm）
    float scale;     // 比例系数
} tds_calibration_t;

/**
 * @brief 设置TDS校准参数
 * @param sensor_id TDS传感器ID
 * @param calibration 校准参数
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_set_calibration(tds_sensor_id_t sensor_id, const tds_calibration_t *calibration);

/**
 * @brief 获取TDS校准参数
 * @param sensor_id TDS传感器ID
 * @param calibration 输出：校准参数
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_get_calibration(tds_sensor_id_t sensor_id, tds_calibration_t *calibration);

/**
 * @brief TDS标准液校准（一点校准）
 * @param sensor_id TDS传感器ID
 * @param standard_value 标准液TDS值（ppm）
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_calibrate(tds_sensor_id_t sensor_id, float standard_value);

/**
 * @brief 重置TDS校准为出厂默认值
 * @param sensor_id TDS传感器ID
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_reset_calibration(tds_sensor_id_t sensor_id);

// ==================== 报警接口 ====================

/**
 * @brief 设置TDS报警阈值
 * @param sensor_id TDS传感器ID
 * @param threshold 报警阈值（ppm）
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_set_alarm_threshold(tds_sensor_id_t sensor_id, float threshold);

/**
 * @brief 获取TDS报警阈值
 * @param sensor_id TDS传感器ID
 * @param threshold 输出：报警阈值
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_get_alarm_threshold(tds_sensor_id_t sensor_id, float *threshold);

/**
 * @brief 检查TDS是否超过报警阈值
 * @param sensor_id TDS传感器ID
 * @return true 超过阈值
 */
bool tds_sensor_is_alarm(tds_sensor_id_t sensor_id);

// ==================== 滤芯寿命接口 ====================

/**
 * @brief 设置滤芯总容量（兼容旧接口）
 * @param total_liters 总容量（升）
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_set_filter_capacity(uint32_t total_liters);

/**
 * @brief 更新已处理水量（累计，支持小数，内部累积后提交）
 * @param liters 增加的水量（升，可为小数）
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_update_water_usage(float liters);

/**
 * @brief 获取滤芯寿命信息（兼容旧接口，返回RO膜寿命）
 * @param life 输出：滤芯寿命信息
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_get_filter_life(filter_life_t *life);

/**
 * @brief 重置滤芯寿命（兼容旧接口，重置RO膜）
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_reset_filter_life(void);

// ==================== 五级滤芯管理接口 ====================

/**
 * @brief 获取所有滤芯状态
 * @param status 输出：滤芯状态结构体
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_get_filters_status(filters_status_t *status);

/**
 * @brief 获取单个滤芯信息
 * @param filter_type 滤芯类型
 * @param info 输出：滤芯信息
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_get_filter_info(filter_type_t filter_type, filter_info_t *info);

/**
 * @brief 重置单个滤芯（更换滤芯后调用）
 * @param filter_type 滤芯类型
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_reset_filter(filter_type_t filter_type);

/**
 * @brief 重置所有滤芯
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_reset_all_filters(void);

/**
 * @brief 设置单个滤芯容量
 * @param filter_type 滤芯类型
 * @param total_liters 总容量（升）
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_set_filter_capacity_ex(filter_type_t filter_type, uint32_t total_liters);

/**
 * @brief 获取滤芯名称
 * @param filter_type 滤芯类型
 * @return 滤芯名称字符串
 */
const char* tds_sensor_get_filter_name(filter_type_t filter_type);

/**
 * @brief 检查是否有滤芯需要更换
 * @return true 有滤芯需要更换
 */
bool tds_sensor_any_filter_needs_replacement(void);

/**
 * @brief 获取总用水量（升）
 * @return 总用水量（升）
 */
uint32_t tds_sensor_get_total_water_usage(void);

/**
 * @brief 设置RO制水速率（升/小时）
 * @param lph 制水速率
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_set_production_rate(float lph);

/**
 * @brief 获取当前RO制水速率（升/小时）
 * @return 制水速率
 */
float tds_sensor_get_production_rate(void);

// ==================== 温度补偿 ====================

/**
 * @brief 设置水温（用于温度补偿）
 * @param temperature 水温（°C）
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_set_temperature(float temperature);

/**
 * @brief 获取当前水温设置
 * @return 水温（°C）
 */
float tds_sensor_get_temperature(void);

// ==================== 状态查询 ====================

/**
 * @brief 检查TDS传感器是否正在运行
 * @return true 运行中
 */
bool tds_sensor_is_running(void);

/**
 * @brief 获取TDS传感器状态信息字符串
 * @param buffer 输出缓冲区
 * @param buffer_size 缓冲区大小
 * @return ESP_OK 成功
 */
esp_err_t tds_sensor_get_status_string(char *buffer, size_t buffer_size);

/**
 * @brief 获取TDS传感器名称
 * @param sensor_id TDS传感器ID
 * @return 传感器名称字符串
 */
const char* tds_sensor_get_name(tds_sensor_id_t sensor_id);

#ifdef __cplusplus
}
#endif

#endif // TDS_SENSOR_H