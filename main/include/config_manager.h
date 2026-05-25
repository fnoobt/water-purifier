/**
 * @file config_manager.h
 * @brief 配置管理模块接口
 * @note 统一管理所有NVS配置存储
 */

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 系统配置结构体 ====================

/**
 * @brief 净水器系统配置结构体
 */
typedef struct {
    // WiFi配置
    char wifi_ssid[33];
    char wifi_password[65];

    // MQTT配置
    bool mqtt_enabled;
    char mqtt_broker[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];

    // RO膜和硬件配置
    uint8_t ro_membrane_type;         // RO膜通量类型 (0=50G, 1=75G, 2=100G, 3=200G, 4=400G)
    uint8_t pump_type;                // 增压泵类型 (0=三角洲50G, 1=75G, 2=100G, 3=200G, 4=300G, 5=400G)
    uint8_t tank_size;                // 压力桶大小 (0=3G, 1=3.2G, 2=4G, 3=6G, 4=10G)
    uint16_t waste_valve_flow_cc;     // 废水阀流量 (CC=mL/min), 默认300

    // 系统参数
    uint32_t production_timeout_sec;    // 制水超时时间（秒），默认3小时
    uint32_t leak_confirm_time_sec;     // 漏水确认时间（秒），默认5秒
    uint16_t runtime_save_interval_min;  // 运行数据保存间隔（分钟），默认120

    // 冲洗参数
    uint32_t normal_flush_duration_sec;  // 常规冲洗持续时间（秒），默认20
    uint32_t pure_flush_duration_sec;    // 纯水洗膜持续时间（秒），默认15
    uint32_t filter_flush_duration_sec;   // 换芯冲洗持续时间（秒），默认3600
    uint32_t short_prod_threshold_sec;   // 短制水判断阈值（秒），默认180
    uint32_t water_hammer_valve_open_delay_ms;  // 水锤-开阀延时（毫秒），默认1000
    uint32_t water_hammer_pump_stop_delay_ms;   // 水锤-停泵延时（毫秒），默认1000
    uint32_t water_hammer_valve_close_delay_ms; // 水锤-关阀延时（毫秒），默认500

    // 继电器配置
    uint8_t relay_trigger_level;        // 继电器触发电平（0=低电平，1=高电平）

    // TDS配置
    float tds_inlet_threshold;          // 进水TDS报警阈值
    float tds_outlet_threshold;         // 出水TDS报警阈值
    float tds_calibration_offset[2];    // TDS校准偏移量（进水、出水）
    float tds_calibration_scale[2];     // TDS校准比例系数（进水、出水）

    // Web服务器配置
    uint16_t web_port;
    bool web_auth_enabled;
    char web_username[32];
    char web_password[32];

    // FSM运行统计（持久化）
    uint32_t fsm_prod_cycles;       // 总制水周期数
    uint32_t fsm_flush_cycles;      // 总冲洗周期数
    uint64_t fsm_prod_time_sec;     // 总制水时间（秒）
    uint64_t fsm_flush_time_sec;    // 总冲洗时间（秒）
} system_config_t;

// ==================== 初始化接口 ====================

/**
 * @brief 初始化配置管理模块
 * @return ESP_OK 成功
 */
esp_err_t config_manager_init(void);

/**
 * @brief 反初始化配置管理模块
 * @return ESP_OK 成功
 */
esp_err_t config_manager_deinit(void);

/**
 * @brief 加载系统配置
 * @return ESP_OK 成功
 */
esp_err_t config_manager_load(void);

/**
 * @brief 保存系统配置
 * @note 仅在配置有变化时才执行写入，无变化时返回ESP_OK
 * @return ESP_OK 成功或无需保存
 */
esp_err_t config_manager_save(void);

/**
 * @brief 检查配置是否有未保存的变化
 * @return true 有未保存的变化
 */
bool config_manager_is_dirty(void);

/**
 * @brief 恢复出厂配置
 * @return ESP_OK 成功
 */
esp_err_t config_manager_factory_reset(void);

// ==================== 配置获取和设置 ====================

/**
 * @brief 获取系统配置
 * @param config 输出：系统配置
 * @return ESP_OK 成功
 */
esp_err_t config_manager_get_config(system_config_t *config);

/**
 * @brief 设置系统配置
 * @param config 系统配置
 * @return ESP_OK 成功
 */
esp_err_t config_manager_set_config(const system_config_t *config);

/**
 * @brief 获取配置项（字符串）
 * @param key 配置键
 * @param value 输出：配置值
 * @param value_size 缓冲区大小
 * @return ESP_OK 成功
 */
esp_err_t config_manager_get_string(const char *key, char *value, size_t value_size);

/**
 * @brief 设置配置项（字符串）
 * @param key 配置键
 * @param value 配置值
 * @return ESP_OK 成功
 */
esp_err_t config_manager_set_string(const char *key, const char *value);

/**
 * @brief 获取配置项（整数）
 * @param key 配置键
 * @param value 输出：配置值
 * @return ESP_OK 成功
 */
esp_err_t config_manager_get_int(const char *key, int *value);

/**
 * @brief 设置配置项（整数）
 * @param key 配置键
 * @param value 配置值
 * @return ESP_OK 成功
 */
esp_err_t config_manager_set_int(const char *key, int value);

/**
 * @brief 获取配置项（布尔）
 * @param key 配置键
 * @param value 输出：配置值
 * @return ESP_OK 成功
 */
esp_err_t config_manager_get_bool(const char *key, bool *value);

/**
 * @brief 设置配置项（布尔）
 * @param key 配置键
 * @param value 配置值
 * @return ESP_OK 成功
 */
esp_err_t config_manager_set_bool(const char *key, bool value);

// ==================== 统一保存接口 ====================

/**
 * @brief 统一周期保存接口（批量写入优化）
 * @note 协调所有模块的脏数据保存，减少NVS commit次数
 * @param min_interval_sec 最小保存间隔（秒），0表示立即保存
 * @return true 执行了保存
 */
bool config_manager_periodic_save_all(uint32_t min_interval_sec);

// ==================== 配置验证 ====================

/**
 * @brief 验证配置是否有效
 * @param config 系统配置
 * @return true 有效
 */
bool config_manager_validate(const system_config_t *config);

/**
 * @brief 检查是否有有效WiFi配置
 * @return true 有配置
 */
bool config_manager_has_wifi_config(void);

/**
 * @brief 检查是否有有效MQTT配置
 * @return true 有配置
 */
bool config_manager_has_mqtt_config(void);

// ==================== FSM运行统计同步 ====================

/**
 * @brief 同步FSM运行统计到配置（带变化检测，避免无意义写入）
 * @param prod_cycles 总制水周期数
 * @param flush_cycles 总冲洗周期数
 * @param prod_time_sec 总制水时间（秒）
 * @param flush_time_sec 总冲洗时间（秒）
 */
void config_manager_sync_fsm_stats(uint32_t prod_cycles, uint32_t flush_cycles,
                                   uint64_t prod_time_sec, uint64_t flush_time_sec);

/**
 * @brief 获取FSM运行统计
 * @param prod_cycles 输出：总制水周期数
 * @param flush_cycles 输出：总冲洗周期数
 * @param prod_time_sec 输出：总制水时间（秒）
 * @param flush_time_sec 输出：总冲洗时间（秒）
 */
void config_manager_get_fsm_stats(uint32_t *prod_cycles, uint32_t *flush_cycles,
                                  uint64_t *prod_time_sec, uint64_t *flush_time_sec);

// ==================== 调试接口 ====================

/**
 * @brief 打印配置信息
 */

void config_manager_print_config(void);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_MANAGER_H