/**
 * @file app_mqtt.h
 * @brief MQTT客户端模块公共类型定义
 * @note 使用app_前缀避免与ESP-IDF系统mqtt_client.h类型冲突
 *       实现函数使用mqtt_client_*命名（见app_mqtt_public.h / mqtt_client.c）
 */

#ifndef APP_MQTT_H
#define APP_MQTT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 公共类型定义 ====================

typedef struct {
    char broker_uri[128];        // Broker URI (mqtt://host:port)
    char client_id[64];          // 客户端ID
    char username[64];           // 用户名
    char password[64];           // 密码
    char topic_prefix[64];       // 主题前缀
    uint16_t keepalive;          // 保活时间（秒）
    bool retain;                 // 保留标志
    uint8_t qos;                 // QoS等级
} app_mqtt_config_t;

typedef enum {
    APP_MQTT_STATE_DISCONNECTED = 0,  // 未连接
    APP_MQTT_STATE_CONNECTING,        // 连接中
    APP_MQTT_STATE_CONNECTED,         // 已连接
    APP_MQTT_STATE_ERROR,             // 错误
} app_mqtt_state_t;

typedef struct {
    char topic[128];             // 主题
    char data[512];              // 数据
    uint32_t data_len;           // 数据长度
    uint8_t qos;                 // QoS等级
    bool retain;                 // 保留标志
} app_mqtt_message_t;

// 回调函数类型定义
typedef void (*app_mqtt_message_callback_t)(const char *topic, const char *data, uint32_t data_len);
typedef void (*app_mqtt_connection_callback_t)(bool connected);

#ifdef __cplusplus
}
#endif

#endif // APP_MQTT_H
