/**
 * @file mqtt_client.h
 * @brief MQTT客户端模块接口
 * @note 用于接入Home Assistant，支持MQTT协议
 */

// 如果定义了USE_ESP_MQTT_CLIENT_H，则跳过此头文件（用于包含ESP-IDF系统头文件）
#ifndef USE_ESP_MQTT_CLIENT_H

#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== MQTT配置参数 ====================

/**
 * @brief MQTT配置结构体
 */
typedef struct {
    char broker_uri[128];        // Broker URI (mqtt://host:port)
    char client_id[64];          // 客户端ID
    char username[64];           // 用户名
    char password[64];           // 密码
    char topic_prefix[64];       // 主题前缀
    uint16_t keepalive;          // 保活时间（秒）
    bool retain;                 // 保留标志
    uint8_t qos;                 // QoS等级
} mqtt_config_t;

/**
 * @brief MQTT连接状态枚举
 */
typedef enum {
    MQTT_STATE_DISCONNECTED = 0,  // 未连接
    MQTT_STATE_CONNECTING,        // 连接中
    MQTT_STATE_CONNECTED,         // 已连接
    MQTT_STATE_ERROR,             // 错误
} mqtt_state_t;

/**
 * @brief MQTT消息结构体
 */
typedef struct {
    char topic[128];             // 主题
    char data[512];              // 数据
    uint32_t data_len;           // 数据长度
    uint8_t qos;                 // QoS等级
    bool retain;                 // 保留标志
} mqtt_message_t;

// ==================== 初始化和控制接口 ====================

/**
 * @brief 初始化MQTT客户端模块
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t mqtt_client_init(void);

/**
 * @brief 反初始化MQTT客户端模块
 * @return ESP_OK 成功
 */
esp_err_t mqtt_client_deinit(void);

/**
 * @brief 启动MQTT客户端
 * @return ESP_OK 成功
 */
esp_err_t mqtt_client_start(void);

/**
 * @brief 停止MQTT客户端
 * @return ESP_OK 成功
 */
esp_err_t mqtt_client_stop(void);

/**
 * @brief 设置MQTT配置
 * @param config MQTT配置结构体指针
 * @return ESP_OK 成功
 */
esp_err_t mqtt_client_set_config(const mqtt_config_t *config);

/**
 * @brief 获取当前MQTT配置
 * @param config 输出：MQTT配置结构体指针
 * @return ESP_OK 成功
 */
esp_err_t mqtt_client_get_config(mqtt_config_t *config);

/**
 * @brief 连接到MQTT Broker
 * @return ESP_OK 成功
 */
esp_err_t mqtt_client_connect(void);

/**
 * @brief 断开MQTT连接
 * @return ESP_OK 成功
 */
esp_err_t mqtt_client_disconnect(void);

// ==================== 消息发布接口 ====================

/**
 * @brief 发布MQTT消息
 * @param topic 主题
 * @param data 数据
 * @param len 数据长度
 * @param qos QoS等级
 * @param retain 保留标志
 * @return ESP_OK 成功
 */
esp_err_t mqtt_client_publish(const char *topic, const char *data, uint32_t len, uint8_t qos, bool retain);

/**
 * @brief 发布净水器状态
 * @return ESP_OK 成功
 */
esp_err_t mqtt_publish_purifier_status(void);

/**
 * @brief 发布TDS值
 * @return ESP_OK 成功
 */
esp_err_t mqtt_publish_tds_value(void);

/**
 * @brief 发布系统状态
 * @return ESP_OK 成功
 */
esp_err_t mqtt_publish_system_status(void);

// ==================== 订阅接口 ====================

/**
 * @brief 订阅MQTT主题
 * @param topic 主题
 * @param qos QoS等级
 * @return ESP_OK 成功
 */
esp_err_t mqtt_client_subscribe(const char *topic, uint8_t qos);

/**
 * @brief 取消订阅MQTT主题
 * @param topic 主题
 * @return ESP_OK 成功
 */
esp_err_t mqtt_client_unsubscribe(const char *topic);

/**
 * @brief 订阅所有控制主题
 * @return ESP_OK 成功
 */
esp_err_t mqtt_subscribe_control_topics(void);

// ==================== 状态查询接口 ====================

/**
 * @brief 获取MQTT连接状态
 * @return MQTT状态枚举
 */
mqtt_state_t mqtt_client_get_state(void);

/**
 * @brief 获取MQTT状态名称字符串
 * @param state MQTT状态枚举
 * @return 状态名称字符串
 */
const char* mqtt_client_get_state_name(mqtt_state_t state);

/**
 * @brief 检查MQTT是否已连接
 * @return true 已连接，false 未连接
 */
bool mqtt_client_is_connected(void);

// ==================== 回调接口 ====================

/**
 * @brief MQTT消息到达回调函数类型
 * @param topic 主题
 * @param data 数据
 * @param data_len 数据长度
 */
typedef void (*mqtt_message_callback_t)(const char *topic, const char *data, uint32_t data_len);

/**
 * @brief 注册MQTT消息回调函数
 * @param callback 回调函数指针
 * @return ESP_OK 成功
 */
esp_err_t mqtt_client_register_message_callback(mqtt_message_callback_t callback);

/**
 * @brief MQTT连接状态变化回调函数类型
 * @param connected true=已连接，false=已断开
 */
typedef void (*mqtt_connection_callback_t)(bool connected);

/**
 * @brief 注册MQTT连接状态变化回调函数
 * @param callback 回调函数指针
 * @return ESP_OK 成功
 */
esp_err_t mqtt_client_register_connection_callback(mqtt_connection_callback_t callback);

// ==================== 配置存储接口 ====================

/**
 * @brief 保存MQTT配置到NVS
 * @return ESP_OK 成功
 */
esp_err_t mqtt_client_save_config(void);

/**
 * @brief 从NVS加载MQTT配置
 * @return ESP_OK 成功
 */
esp_err_t mqtt_client_load_config(void);

/**
 * @brief 清除保存的MQTT配置
 * @return ESP_OK 成功
 */
esp_err_t mqtt_client_clear_config(void);

/**
 * @brief 检查是否有保存的MQTT配置
 * @return true 有配置，false 无配置
 */
bool mqtt_client_has_saved_config(void);

// ==================== Home Assistant集成 ====================

/**
 * @brief 发送Home Assistant发现配置
 * @return ESP_OK 成功
 */
esp_err_t mqtt_send_ha_discovery(void);

/**
 * @brief 发送Home Assistant传感器配置
 * @param sensor_name 传感器名称
 * @param sensor_type 传感器类型
 * @param unit 单位
 * @return ESP_OK 成功
 */
esp_err_t mqtt_send_ha_sensor_config(const char *sensor_name, const char *sensor_type, const char *unit);

/**
 * @brief 发送Home Assistant开关配置
 * @param switch_name 开关名称
 * @return ESP_OK 成功
 */
esp_err_t mqtt_send_ha_switch_config(const char *switch_name);

// ==================== 调试接口 ====================

/**
 * @brief 获取MQTT状态信息
 * @param buffer 输出缓冲区
 * @param buffer_size 缓冲区大小
 * @return ESP_OK 成功
 */
esp_err_t mqtt_client_get_status(char *buffer, size_t buffer_size);

/**
 * @brief 打印MQTT详细信息
 */
void mqtt_client_print_info(void);

#ifdef __cplusplus
}
#endif

#endif // MQTT_CLIENT_H

#endif // USE_ESP_MQTT_CLIENT_H
