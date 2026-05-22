/**
 * @file tds_sensor.h
 * @brief TDS传感器检测模块接口（支持双TDS）
 * @note 支持进水TDS（原水，GPIO2）和出水TDS（纯水，GPIO3）同时检测
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

/**
 * @brief 设置是否跳过报警检测（纯水洗膜期间使用）
 * @param skip true=跳过报警检测，false=恢复报警检测
 * @note 纯水洗膜期间泵停止、进水阀关闭，TDS传感器测量停滞水/气泡导致读数异常，
 *       调用此函数跳过报警检测以避免假报警
 */
void tds_sensor_set_skip_alarm_detection(bool skip);

#ifdef __cplusplus
}
#endif

#endif // TDS_SENSOR_H
