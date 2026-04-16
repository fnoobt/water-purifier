/**
 * @file filter_manager.h
 * @brief 五级滤芯管理与用水量统计模块接口
 * @note 独立于TDS传感器，管理滤芯寿命、总用水量、制水速率
 */

#ifndef FILTER_MANAGER_H
#define FILTER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

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

// ==================== 初始化 ====================

/**
 * @brief 初始化滤芯管理模块（从NVS恢复滤芯状态）
 * @return ESP_OK 成功
 */
esp_err_t filter_mgr_init(void);

// ==================== 用水量统计 ====================

/**
 * @brief 设置滤芯总容量（兼容旧接口，设置所有滤芯）
 * @param total_liters 总容量（升）
 * @return ESP_OK 成功
 */
esp_err_t filter_mgr_set_filter_capacity(uint32_t total_liters);

/**
 * @brief 更新已处理水量（累计，支持小数，内部累积后提交）
 * @param liters 增加的水量（升，可为小数）
 * @return ESP_OK 成功
 */
esp_err_t filter_mgr_update_water_usage(float liters);

/**
 * @brief 获取总用水量（升）
 * @return 总用水量（升）
 */
uint32_t filter_mgr_get_total_water_usage(void);

/**
 * @brief 设置总用水量（用于从NVS恢复）
 * @param liters 总用水量（升）
 */
void filter_mgr_set_total_water_usage(uint32_t liters);

// ==================== 滤芯寿命管理 ====================

/**
 * @brief 获取滤芯寿命信息（兼容旧接口，返回RO膜寿命）
 * @param life 输出：滤芯寿命信息
 * @return ESP_OK 成功
 */
esp_err_t filter_mgr_get_filter_life(filter_life_t *life);

/**
 * @brief 重置滤芯寿命（兼容旧接口，重置RO膜）
 * @return ESP_OK 成功
 */
esp_err_t filter_mgr_reset_filter_life(void);

/**
 * @brief 获取所有滤芯状态
 * @param status 输出：滤芯状态结构体
 * @return ESP_OK 成功
 */
esp_err_t filter_mgr_get_filters_status(filters_status_t *status);

/**
 * @brief 获取单个滤芯信息
 * @param filter_type 滤芯类型
 * @param info 输出：滤芯信息
 * @return ESP_OK 成功
 */
esp_err_t filter_mgr_get_filter_info(filter_type_t filter_type, filter_info_t *info);

/**
 * @brief 重置单个滤芯（更换滤芯后调用）
 * @param filter_type 滤芯类型
 * @return ESP_OK 成功
 */
esp_err_t filter_mgr_reset_filter(filter_type_t filter_type);

/**
 * @brief 重置所有滤芯
 * @return ESP_OK 成功
 */
esp_err_t filter_mgr_reset_all_filters(void);

/**
 * @brief 设置单个滤芯容量
 * @param filter_type 滤芯类型
 * @param total_liters 总容量（升）
 * @return ESP_OK 成功
 */
esp_err_t filter_mgr_set_filter_capacity_ex(filter_type_t filter_type, uint32_t total_liters);

/**
 * @brief 批量设置所有滤芯容量（一次NVs写入）
 * @param capacities 容量数组，长度必须为FILTER_COUNT
 * @return ESP_OK 成功
 */
esp_err_t filter_mgr_set_all_filter_capacity(const uint32_t capacities[FILTER_COUNT]);

/**
 * @brief 获取滤芯名称
 * @param filter_type 滤芯类型
 * @return 滤芯名称字符串
 */
const char* filter_mgr_get_filter_name(filter_type_t filter_type);

/**
 * @brief 检查是否有滤芯需要更换
 * @return true 有滤芯需要更换
 */
bool filter_mgr_any_filter_needs_replacement(void);

// ==================== 制水速率 ====================

/**
 * @brief 设置RO制水速率（升/小时）
 * @param lph 制水速率
 * @return ESP_OK 成功
 */
esp_err_t filter_mgr_set_production_rate(float lph);

/**
 * @brief 获取当前RO制水速率（升/小时）
 * @return 制水速率
 */
float filter_mgr_get_production_rate(void);

/**
 * @brief 周期性保存滤芯数据到NVS（检查脏标志，有变化时才写入）
 * @return true 执行了保存
 */
bool filter_mgr_periodic_save(void);

#ifdef __cplusplus
}
#endif

#endif // FILTER_MANAGER_H
