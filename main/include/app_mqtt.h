/**
 * @file app_mqtt.h
 * @brief 本地MQTT客户端模块接口（重命名版本，避免与ESP-IDF头文件冲突）
 * @note 这是本地mqtt_client.h的别名，用于避免与ESP-IDF的mqtt_client.h冲突
 */

#ifndef APP_MQTT_H
#define APP_MQTT_H

// 直接包含本地mqtt_client.h的内容，但使用不同的include guard
#define MQTT_CLIENT_H_SKIP_INCLUDE 1

// 包含ESP-IDF的MQTT客户端头文件
#include "mqtt_client.h"  // 系统ESP-IDF MQTT客户端

#undef MQTT_CLIENT_H_SKIP_INCLUDE

// 重新定义本地类型，使用app_前缀避免冲突
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

// 函数声明 - 使用app_前缀
esp_err_t app_mqtt_client_init(void);
esp_err_t app_mqtt_client_deinit(void);
esp_err_t app_mqtt_client_start(void);
esp_err_t app_mqtt_client_stop(void);
esp_err_t app_mqtt_client_set_config(const app_mqtt_config_t *config);
esp_err_t app_mqtt_client_get_config(app_mqtt_config_t *config);
esp_err_t app_mqtt_client_connect(void);
esp_err_t app_mqtt_client_disconnect(void);
esp_err_t app_mqtt_client_publish(const char *topic, const char *data, uint32_t len, uint8_t qos, bool retain);
esp_err_t app_mqtt_publish_purifier_status(void);
esp_err_t app_mqtt_publish_tds_value(void);
esp_err_t app_mqtt_publish_system_status(void);
esp_err_t app_mqtt_client_subscribe(const char *topic, uint8_t qos);
esp_err_t app_mqtt_client_unsubscribe(const char *topic);
esp_err_t app_mqtt_subscribe_control_topics(void);
app_mqtt_state_t app_mqtt_client_get_state(void);
bool app_mqtt_client_is_connected(void);
esp_err_t app_mqtt_client_register_message_callback(app_mqtt_message_callback_t callback);
esp_err_t app_mqtt_client_register_connection_callback(app_mqtt_connection_callback_t callback);
esp_err_t app_mqtt_client_save_config(void);
esp_err_t app_mqtt_client_load_config(void);
esp_err_t app_mqtt_client_clear_config(void);
bool app_mqtt_client_has_saved_config(void);
esp_err_t app_mqtt_send_ha_discovery(void);
esp_err_t app_mqtt_send_ha_sensor_config(const char *sensor_name, const char *sensor_type, const char *unit);
esp_err_t app_mqtt_send_ha_switch_config(const char *switch_name);
esp_err_t app_mqtt_client_get_status(char *buffer, size_t buffer_size);
void app_mqtt_client_print_info(void);

#endif // APP_MQTT_H
