/**
 * @file wifi_manager.h
 * @brief WiFi连接管理模块接口
 * @note 支持Station模式、AP热点配网
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== WiFi连接状态枚举 ====================

typedef enum {
    WIFI_STATE_IDLE = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_RECONNECTING,
    WIFI_STATE_AP_MODE,         // AP配置模式
    WIFI_STATE_ERROR,
} wifi_state_t;

// ==================== 初始化接口 ====================

/**
 * @brief 初始化WiFi管理模块
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief 反初始化WiFi管理模块
 */
esp_err_t wifi_manager_deinit(void);

/**
 * @brief 启动WiFi
 */
esp_err_t wifi_manager_start(void);

/**
 * @brief 停止WiFi
 */
esp_err_t wifi_manager_stop(void);

// ==================== 配置接口 ====================

/**
 * @brief 设置WiFi配置
 */
esp_err_t wifi_manager_set_config(const char *ssid, const char *password);

/**
 * @brief 启动AP配置模式
 */
esp_err_t wifi_manager_start_ap_mode(void);

/**
 * @brief 停止AP配置模式
 */
esp_err_t wifi_manager_stop_ap_mode(void);

// ==================== 状态查询 ====================

/**
 * @brief 获取WiFi状态
 */
wifi_state_t wifi_manager_get_state(void);

/**
 * @brief 获取状态名称
 */
const char* wifi_manager_get_state_name(wifi_state_t state);

/**
 * @brief 检查是否已连接
 */
bool wifi_manager_is_connected(void);

/**
 * @brief 检查是否处于AP模式
 */
bool wifi_manager_is_ap_mode(void);

/**
 * @brief 获取IP地址
 */
esp_err_t wifi_manager_get_ip(char *ip_str, size_t buffer_size);

/**
 * @brief 获取RSSI信号强度
 */
int8_t wifi_manager_get_rssi(void);

/**
 * @brief 获取当前连接的SSID
 */
const char* wifi_manager_get_ssid(void);

// ==================== 存储接口 ====================

/**
 * @brief 保存WiFi配置
 */
esp_err_t wifi_manager_save_config(void);

/**
 * @brief 加载WiFi配置
 */
esp_err_t wifi_manager_load_config(void);

/**
 * @brief 清除保存的配置
 */
esp_err_t wifi_manager_clear_config(void);

/**
 * @brief 检查是否有保存的配置
 */
bool wifi_manager_has_saved_config(void);

// ==================== 回调接口 ====================

typedef void (*wifi_state_callback_t)(wifi_state_t state);

/**
 * @brief 注册状态变化回调
 */
esp_err_t wifi_manager_register_callback(wifi_state_callback_t callback);

// ==================== 调试接口 ====================

/**
 * @brief 获取状态字符串
 */
esp_err_t wifi_manager_get_status_string(char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H