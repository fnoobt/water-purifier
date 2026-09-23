/**
 * @file water_purifier_fsm.h
 * @brief 净水器有限状态机接口
 * @note 管理净水器的制水、冲洗、保护流程
 *
 * 状态说明：
 * - 待机：系统静止，等待压力桶压力下降
 * - 制水：正常制水，填充压力桶
 * - 水满：压力桶已满，准备冲洗（瞬时过渡）
 * - 常规冲洗：开机/定时触发，原水大流量冲膜
 * - 纯水洗膜：利用桶压回流，置换膜内陈水
 * - 缺水：进水低压保护，等待恢复（自动）
 * - 漏水：漏水报警状态（手动复位）
 * - 停止：系统停止状态（手动复位）
 */

#ifndef WATER_PURIFIER_FSM_H
#define WATER_PURIFIER_FSM_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 状态枚举定义 ====================

/**
 * @brief 净水器系统状态枚举
 */
typedef enum {
    FSM_STATE_STANDBY = 0,          // 待机状态
    FSM_STATE_PRODUCTION,           // 制水中
    FSM_STATE_TANK_FULL,            // 水满状态（瞬时过渡）
    FSM_STATE_NORMAL_FLUSH,         // 常规冲洗
    FSM_STATE_PURE_FLUSH,           // 纯水洗膜（反冲洗）
    FSM_STATE_WATER_SHORTAGE,       // 缺水状态
    FSM_STATE_LEAK_ALARM,           // 漏水报警状态
    FSM_STATE_STOP,                 // 停止状态

    FSM_STATE_COUNT                 // 状态数量
} fsm_state_t;

/**
 * @brief 事件枚举
 */
typedef enum {
    FSM_EVENT_NONE = 0,             // 无事件
    FSM_EVENT_LOW_PRESSURE_ON,      // 有水（低压开关闭合）
    FSM_EVENT_LOW_PRESSURE_OFF,     // 缺水（低压开关断开）
    FSM_EVENT_TANK_NEED_WATER,      // 压力桶需要制水
    FSM_EVENT_TANK_FULL,            // 压力桶满
    FSM_EVENT_WATER_LEAK,           // 漏水检测
    FSM_EVENT_NORMAL_FLUSH_DONE,    // 常规冲洗完成
    FSM_EVENT_PURE_FLUSH_DONE,      // 纯水洗膜完成
    FSM_EVENT_PRODUCTION_TIMEOUT,   // 制水超时（3小时）
    FSM_EVENT_RESET,                // 复位
    FSM_EVENT_FORCE_FLUSH,          // 强制冲洗（完整流程）
    FSM_EVENT_FORCE_PRODUCTION,     // 强制制水
    FSM_EVENT_NORMAL_FLUSH,         // 切换到常规冲洗
    FSM_EVENT_PURE_FLUSH,           // 切换到纯水洗膜
    FSM_EVENT_GO_STANDBY,           // 切换到待机
    FSM_EVENT_SHUTDOWN,             // 停机（进入停止但不记录）
    FSM_EVENT_FILTER_FLUSH,         // 换芯冲洗（仅普通冲洗，持续1小时）
} fsm_event_t;

/**
 * @brief 停止类型枚举
 */
typedef enum {
    STOP_TYPE_NONE = 0,
    STOP_TYPE_PRODUCTION_TIMEOUT,  // 制水超时
    STOP_TYPE_WATER_LEAK,          // 漏水
    STOP_TYPE_TDS_ALARM,           // TDS报警
} stop_type_t;

/**
 * @brief 系统运行数据结构
 */
typedef struct {
    // 运行统计
    uint32_t total_production_cycles;    // 总制水周期数
    uint32_t total_flush_cycles;         // 总冲洗周期数
    uint64_t total_production_time_sec;  // 总制水时间（秒）
    uint64_t total_flush_time_sec;       // 总冲洗时间（秒）

    // 当前周期数据
    uint64_t current_production_start;   // 当前制水开始时间
    uint64_t current_flush_start;        // 当前冲洗开始时间

    // 停止记录
    stop_type_t last_stop_type;        // 最近停止类型
    uint64_t last_stop_time;            // 最近停止时间

    uint64_t last_flush_time;            // 上次冲洗时间

    // 低压恢复标志
    bool low_pressure_occurred;          // 曾经低压断开过
} fsm_runtime_data_t;

/**
 * @brief 停止历史记录结构
 */
typedef struct {
    stop_type_t type;
    uint64_t timestamp;
    char description[64];
} stop_record_t;

// ==================== 状态机控制接口 ====================

/**
 * @brief 初始化状态机
 * @return ESP_OK 成功
 */
esp_err_t fsm_init(void);

/**
 * @brief 反初始化状态机
 * @return ESP_OK 成功
 */
esp_err_t fsm_deinit(void);

/**
 * @brief 启动状态机任务
 * @return ESP_OK 成功
 */
esp_err_t fsm_start(void);

/**
 * @brief 停止状态机任务
 * @return ESP_OK 成功
 */
esp_err_t fsm_stop(void);

/**
 * @brief 发送事件到状态机
 * @param event 事件类型
 * @return ESP_OK 成功
 */
esp_err_t fsm_send_event(fsm_event_t event);

// ==================== 状态查询接口 ====================

/**
 * @brief 获取当前状态
 * @return 当前状态枚举
 */
fsm_state_t fsm_get_state(void);

/**
 * @brief 获取状态名称字符串
 * @param state 状态枚举
 * @return 状态名称字符串
 */
const char* fsm_get_state_name(fsm_state_t state);

/**
 * @brief 获取事件名称字符串
 * @param event 事件枚举
 * @return 事件名称字符串
 */
const char* fsm_get_event_name(fsm_event_t event);

/**
 * @brief 检查状态机是否运行中
 * @return true 运行中
 */
bool fsm_is_running(void);

/**
 * @brief 检查系统是否处于停止状态
 * @return true 停止状态
 */
bool fsm_is_stop_state(void);

/**
 * @brief 检查系统是否处于制水状态
 * @return true 制水中
 */
bool fsm_is_producing(void);

/**
 * @brief 检查系统是否处于冲洗状态（常规冲洗或纯水洗膜）
 * @return true 冲洗中
 */
bool fsm_is_flushing(void);

// ==================== 运行数据接口 ====================

/**
 * @brief 获取运行数据
 * @param data 输出：运行数据
 * @return ESP_OK 成功
 */
esp_err_t fsm_get_runtime_data(fsm_runtime_data_t *data);

/**
 * @brief 重置运行统计数据
 * @return ESP_OK 成功
 */
esp_err_t fsm_reset_runtime_data(void);

/**
 * @brief 强制将运行统计同步到config_manager（忽略脏标志，用于NVS整理）
 * @note 统计实际由config_manager持久化到fsm_*键，本函数仅完成RAM间同步
 * @return ESP_OK 成功，ESP_ERR_TIMEOUT mutex超时
 */
esp_err_t fsm_force_save_runtime(void);

/**
 * @brief 清除停止状态
 * @return ESP_OK 成功
 */
esp_err_t fsm_clear_stop(void);

/**
 * @brief 获取停止历史记录
 * @param records 输出：停止记录数组
 * @param max_count 数组最大长度
 * @return 实际记录数
 */
uint32_t fsm_get_stop_history(stop_record_t *records, uint32_t max_count);

// ==================== 配置接口 ====================

/**
 * @brief 设置常规冲洗持续时间
 * @param duration_sec 冲洗时间（秒）
 * @return ESP_OK 成功
 */
esp_err_t fsm_set_normal_flush_duration(uint32_t duration_sec);

/**
 * @brief 获取常规冲洗持续时间
 * @return 冲洗时间（秒）
 */
uint32_t fsm_get_normal_flush_duration(void);

/**
 * @brief 设置纯水洗膜持续时间
 * @param duration_sec 洗膜时间（秒）
 * @return ESP_OK 成功
 */
esp_err_t fsm_set_pure_flush_duration(uint32_t duration_sec);

/**
 * @brief 获取纯水洗膜持续时间
 * @return 洗膜时间（秒）
 */
uint32_t fsm_get_pure_flush_duration(void);

/**
 * @brief 设置换芯冲洗持续时间
 * @param duration_sec 冲洗时间（秒），默认3600
 * @return ESP_OK 成功
 */
esp_err_t fsm_set_filter_flush_duration(uint32_t duration_sec);

/**
 * @brief 获取换芯冲洗持续时间
 * @return 冲洗时间（秒）
 */
uint32_t fsm_get_filter_flush_duration(void);

/**
 * @brief 设置制水超时时间
 * @param timeout_sec 超时时间（秒），默认3小时
 * @return ESP_OK 成功
 */
esp_err_t fsm_set_production_timeout(uint32_t timeout_sec);

/**
 * @brief 设置漏水确认时间
 * @param time_sec 确认时间（秒）
 * @return ESP_OK 成功
 */
esp_err_t fsm_set_leak_confirm_time(uint32_t time_sec);

/**
 * @brief 设置压力桶水满/需水确认时间
 * @param time_sec 确认时间（秒），默认5，范围1-60
 * @note  水满与需水共用一个确认时长：防止增压泵脉动/水锤导致
 *        压力开关在阈值附近颤动，单次采样误判水满或需水
 * @return ESP_OK 成功
 */
esp_err_t fsm_set_tank_confirm_time(uint32_t time_sec);

/**
 * @brief 设置水锤效应控制延时
 * @param valve_open_delay_ms 开阀延时（毫秒），默认1000
 * @param pump_stop_delay_ms 停泵延时（毫秒），默认1000
 * @param valve_close_delay_ms 关阀延时（毫秒），默认500
 * @return ESP_OK 成功
 */
esp_err_t fsm_set_water_hammer_delays(uint32_t valve_open_delay_ms,
                                       uint32_t pump_stop_delay_ms,
                                       uint32_t valve_close_delay_ms);

/**
 * @brief 设置运行数据保存间隔
 * @param interval_min 保存间隔（分钟）
 * @return ESP_OK 成功
 */
esp_err_t fsm_set_runtime_save_interval(uint16_t interval_min);

// ==================== 手动控制接口 ====================

/**
 * @brief 根据RO膜类型设置制水速率
 * @param ro_type RO膜类型 (0=50G, 1=75G, 2=100G, 3=200G, 4=400G)
 * @return ESP_OK 成功
 */
esp_err_t fsm_set_production_rate_by_membrane(uint8_t ro_type);

/**
 * @brief 获取当前制水速率（升/小时）
 * @return 制水速率
 */
float fsm_get_production_rate_lph(void);

/**
 * @brief 手动启动制水
 * @return ESP_OK 成功
 */
esp_err_t fsm_manual_start_production(void);

/**
 * @brief 手动启动冲洗（完整流程：常规冲洗+纯水洗膜）
 * @return ESP_OK 成功
 */
esp_err_t fsm_manual_start_flush(void);

/**
 * @brief 手动启动常规冲洗（仅常规冲洗阶段）
 * @return ESP_OK 成功
 */
esp_err_t fsm_manual_normal_flush(void);

/**
 * @brief 手动启动纯水洗膜（仅纯水洗膜阶段）
 * @return ESP_OK 成功
 */
esp_err_t fsm_manual_pure_flush(void);

/**
 * @brief 手动启动换芯冲洗（仅普通冲洗，持续1小时）
 * @return ESP_OK 成功
 */
esp_err_t fsm_manual_filter_flush(void);

/**
 * @brief 手动切换到待机状态
 * @return ESP_OK 成功
 */
esp_err_t fsm_manual_go_standby(void);

/**
 * @brief 手动停机（进入停止状态，不记录停止）
 * @return ESP_OK 成功
 */
esp_err_t fsm_manual_shutdown(void);

/**
 * @brief 强制停止所有操作并返回待机
 * @return ESP_OK 成功
 */
esp_err_t fsm_force_standby(void);

// ==================== 状态变化回调 ====================

typedef void (*fsm_state_callback_t)(fsm_state_t old_state, fsm_state_t new_state);

/**
 * @brief 注册状态变化回调
 * @param callback 回调函数
 * @return ESP_OK 成功
 */
esp_err_t fsm_register_state_callback(fsm_state_callback_t callback);

// ==================== 状态信息接口 ====================

/**
 * @brief 获取状态机状态信息字符串
 * @param buffer 输出缓冲区
 * @param buffer_size 缓冲区大小
 * @return ESP_OK 成功
 */
esp_err_t fsm_get_status_string(char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif // WATER_PURIFIER_FSM_H
