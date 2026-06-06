/**
 * @file history_logger.h
 * @brief 历史记录日志模块接口
 * @note 记录状态变化、停止、冲洗等事件，支持15条记录
 */

#ifndef HISTORY_LOGGER_H
#define HISTORY_LOGGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 配置参数 ====================

#define HISTORY_MAX_RECORDS     15      // 最大历史记录数（优化NVS空间）

// ==================== 事件类型枚举 ====================

/**
 * @brief 历史事件类型
 */
typedef enum {
    HISTORY_EVENT_STATE_CHANGE = 0,     // 状态变化
    HISTORY_EVENT_PRODUCTION_START,     // 开始制水
    HISTORY_EVENT_PRODUCTION_END,       // 结束制水
    HISTORY_EVENT_FLUSH_START,          // 开始冲洗
    HISTORY_EVENT_FLUSH_END,            // 结束冲洗
    HISTORY_EVENT_WATER_SHORTAGE,       // 缺水
    HISTORY_EVENT_WATER_RESTORE,        // 水恢复
    HISTORY_EVENT_TANK_FULL,            // 水满
    HISTORY_EVENT_LEAK_ALARM,           // 漏水报警
    HISTORY_EVENT_STOP,                 // 停止
    HISTORY_EVENT_STOP_CLEAR,           // 停止清除
    HISTORY_EVENT_MAINTENANCE,          // 维护提醒
    HISTORY_EVENT_MANUAL_CONTROL,       // 手动控制
    HISTORY_EVENT_SYSTEM_START,         // 系统启动
} history_event_type_t;

// ==================== 数据结构 ====================

/**
 * @brief 历史记录条目
 */
typedef struct {
    uint64_t timestamp;         // 时间戳（微秒）
    history_event_type_t type;  // 事件类型
    uint8_t state_from;         // 原状态（用于状态变化）
    uint8_t state_to;           // 新状态（用于状态变化）
    float value1;               // 附加数值1（如TDS值、制水时间等）
    float value2;               // 附加数值2
    char description[32];       // 事件描述
} history_record_t;

/**
 * @brief 每日统计记录
 */
typedef struct {
    uint32_t date;              // 日期（YYYYMMDD格式，如20260411）
    uint32_t production_sec;    // 当日制水时间（秒）
    uint32_t flush_count;       // 当日冲洗次数
    float tds_in_avg;           // 当日平均进水TDS
    float tds_out_avg;          // 当日平均出水TDS
    uint16_t tds_sample_count;  // TDS采样次数
} daily_stats_t;

// ==================== 初始化接口 ====================

/**
 * @brief 初始化历史记录模块
 * @return ESP_OK 成功
 */
esp_err_t history_logger_init(void);

/**
 * @brief 反初始化历史记录模块
 * @return ESP_OK 成功
 */
esp_err_t history_logger_deinit(void);

// ==================== 记录接口 ====================

/**
 * @brief 添加历史记录
 * @param type 事件类型
 * @param state_from 原状态
 * @param state_to 新状态
 * @param value1 附加数值1
 * @param value2 附加数值2
 * @param description 事件描述
 * @return ESP_OK 成功
 */
esp_err_t history_add_record(history_event_type_t type, uint8_t state_from,
                              uint8_t state_to, float value1, float value2,
                              const char *description);

/**
 * @brief 记录状态变化
 * @param from 原状态
 * @param to 新状态
 * @return ESP_OK 成功
 */
esp_err_t history_log_state_change(uint8_t from, uint8_t to);

/**
 * @brief 记录制水开始
 * @return ESP_OK 成功
 */
esp_err_t history_log_production_start(void);

/**
 * @brief 记录制水结束
 * @param duration_sec 制水持续时间（秒）
 * @return ESP_OK 成功
 */
esp_err_t history_log_production_end(uint32_t duration_sec);

/**
 * @brief 记录冲洗开始
 * @return ESP_OK 成功
 */
esp_err_t history_log_flush_start(void);

/**
 * @brief 记录冲洗结束
 * @param duration_sec 冲洗持续时间（秒）
 * @return ESP_OK 成功
 */
esp_err_t history_log_flush_end(uint32_t duration_sec);

/**
 * @brief 记录停止事件
 * @param stop_type 停止类型
 * @param description 停止描述
 * @return ESP_OK 成功
 */
esp_err_t history_log_stop(uint8_t stop_type, const char *description);

/**
 * @brief 记录漏水报警
 * @return ESP_OK 成功
 */
esp_err_t history_log_leak_alarm(void);

// ==================== 查询接口 ====================

/**
 * @brief 获取历史记录数量
 * @return 记录数量
 */
uint32_t history_get_count(void);

/**
 * @brief 获取历史记录
 * @param records 输出：记录数组
 * @param max_count 最大记录数
 * @return 实际记录数
 */
uint32_t history_get_records(history_record_t *records, uint32_t max_count);

/**
 * @brief 清除所有历史记录
 * @return ESP_OK 成功
 */
esp_err_t history_clear_all(void);

// ==================== 每日统计接口 ====================

/**
 * @brief 更新每日制水时间
 * @param seconds 增加的制水时间（秒）
 * @return ESP_OK 成功
 */
esp_err_t history_update_daily_production(uint32_t seconds);

/**
 * @brief 更新每日冲洗次数
 * @return ESP_OK 成功
 */
esp_err_t history_increment_daily_flush(void);

/**
 * @brief 更新每日TDS平均值
 * @param tds_in 进水TDS
 * @param tds_out 出水TDS
 * @return ESP_OK 成功
 */
esp_err_t history_update_daily_tds(float tds_in, float tds_out);

/**
 * @brief 获取今日统计
 * @param stats 输出：每日统计
 * @return ESP_OK 成功
 */
esp_err_t history_get_today_stats(daily_stats_t *stats);

/**
 * @brief 获取最近N天的统计
 * @param stats 输出：统计数组
 * @param max_days 最大天数
 * @return 实际天数
 */
uint32_t history_get_recent_stats(daily_stats_t *stats, uint32_t max_days);

/**
 * @brief 周期保存每日统计（脏数据 + 节流）
 * @param min_interval_sec 最小保存间隔（秒）
 * @return true 执行了保存
 */
bool history_periodic_save(uint32_t min_interval_sec);

#ifdef __cplusplus
}
#endif

#endif // HISTORY_LOGGER_H