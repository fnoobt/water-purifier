/**
 * @file mqtt_client.c
 * @brief MQTT客户端模块实现
 * @note 用于接入Home Assistant，支持MQTT协议
 * @note 本地类型使用app_mqtt_xxx前缀避免与ESP-IDF类型冲突
 */

// 系统头文件 - 必须先包含
#include "esp_log.h"
#include "esp_event.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"  // 任务看门狗
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdio.h>

// 包含ESP-IDF系统MQTT客户端头文件
#include "mqtt_client.h"

// 包含本地公共接口头文件
#include "app_mqtt_public.h"

// ESP-IDF组件
#include "cJSON.h"

#define MQTT_MAX_TOPIC_LEN  128
#define MQTT_MAX_DATA_LEN   1024

// 本地模块头文件
#include "water_purifier_fsm.h"
#include "tds_sensor.h"
#include "wifi_manager.h"

static const char *TAG = "APP_MQTT";

// ==================== 本地类型定义（使用app_前缀避免冲突）================

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

// 回调函数类型
typedef void (*app_mqtt_message_callback_t)(const char *topic, const char *data, uint32_t data_len);
typedef void (*app_mqtt_connection_callback_t)(bool connected);

// ==================== 状态名称字符串 ====================

static const char* const app_state_names[] = {
    [APP_MQTT_STATE_DISCONNECTED] = "未连接",
    [APP_MQTT_STATE_CONNECTING] = "连接中",
    [APP_MQTT_STATE_CONNECTED] = "已连接",
    [APP_MQTT_STATE_ERROR] = "错误",
};

// ==================== 私有变量 ====================

static struct {
    bool initialized;
    bool started;
    bool stop_requested;         // 停止请求标志（通知重连任务退出）
    app_mqtt_state_t state;
    app_mqtt_config_t config;

    // MQTT客户端
    esp_mqtt_client_handle_t mqtt_client;

    // 回调函数
    app_mqtt_message_callback_t message_callback;
    app_mqtt_connection_callback_t connection_callback;

    // 重连任务
    TaskHandle_t reconnect_task;
} mqtt_ctx = {
    .initialized = false,
    .started = false,
    .stop_requested = false,
    .state = APP_MQTT_STATE_DISCONNECTED,
    .mqtt_client = NULL,
    .message_callback = NULL,
    .connection_callback = NULL,
    .reconnect_task = NULL,
};

/* 临界保护 spinlock（用于重连任务创建） */
static portMUX_TYPE mqtt_spinlock = portMUX_INITIALIZER_UNLOCKED;

// ==================== 私有函数声明 ====================

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data);
static char* mqtt_get_topic(const char *subtopic, char *buffer, size_t buffer_size);
static void mqtt_reconnect_task(void *pvParameters);

// ==================== 状态名称字符串 ====================

// ==================== 初始化和控制接口 ====================

esp_err_t mqtt_client_init(void)
{
    if (mqtt_ctx.initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "初始化MQTT客户端...");

    // 尝试从NVS加载配置
    if (mqtt_client_load_config() != ESP_OK) {
        ESP_LOGI(TAG, "无保存的MQTT配置，使用默认值");
    }

    // 如果未加载client_id，生成随机ID
    if (strlen(mqtt_ctx.config.client_id) == 0) {
        snprintf(mqtt_ctx.config.client_id, sizeof(mqtt_ctx.config.client_id),
                 "water_purifier_%08lx", (unsigned long)esp_random());
    }

    mqtt_ctx.initialized = true;
    ESP_LOGI(TAG, "MQTT客户端初始化完成");
    return ESP_OK;
}

esp_err_t mqtt_client_deinit(void)
{
    if (!mqtt_ctx.initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "反初始化MQTT客户端...");
    mqtt_ctx.initialized = false;
    return ESP_OK;
}

esp_err_t mqtt_client_start(void)
{
    taskENTER_CRITICAL(&mqtt_spinlock);
    if (mqtt_ctx.started) {
        taskEXIT_CRITICAL(&mqtt_spinlock);
        return ESP_OK;
    }
    mqtt_ctx.started = true;
    mqtt_ctx.stop_requested = false;  // 清除停止请求标志
    mqtt_ctx.state = APP_MQTT_STATE_CONNECTING;
    taskEXIT_CRITICAL(&mqtt_spinlock);

    ESP_LOGI(TAG, "启动MQTT客户端...");

    // 实际发起连接（如果配置已设置）
    if (strlen(mqtt_ctx.config.broker_uri) > 0) {
        esp_err_t ret = mqtt_client_connect();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "MQTT连接发起失败: %s", esp_err_to_name(ret));
        }
    }

    return ESP_OK;
}

esp_err_t mqtt_client_stop(void)
{
    if (!mqtt_ctx.started) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "停止MQTT客户端...");

    // 设置停止请求标志，通知重连任务退出
    taskENTER_CRITICAL(&mqtt_spinlock);
    mqtt_ctx.stop_requested = true;
    taskEXIT_CRITICAL(&mqtt_spinlock);

    // 等待重连任务自行退出（最多500ms）
    int wait_count = 0;
    while (mqtt_ctx.reconnect_task != NULL && wait_count < 10) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wait_count++;
    }

    // 如果任务未退出，强制删除
    if (mqtt_ctx.reconnect_task) {
        ESP_LOGW(TAG, "重连任务未响应停止请求，强制删除");
        taskENTER_CRITICAL(&mqtt_spinlock);
        vTaskDelete(mqtt_ctx.reconnect_task);
        mqtt_ctx.reconnect_task = NULL;
        taskEXIT_CRITICAL(&mqtt_spinlock);
    }

    // 清除停止标志
    taskENTER_CRITICAL(&mqtt_spinlock);
    mqtt_ctx.stop_requested = false;
    taskEXIT_CRITICAL(&mqtt_spinlock);

    if (mqtt_ctx.mqtt_client != NULL) {
        esp_mqtt_client_disconnect(mqtt_ctx.mqtt_client);
        vTaskDelay(pdMS_TO_TICKS(100));  // 等待断开完成
        esp_mqtt_client_stop(mqtt_ctx.mqtt_client);
        vTaskDelay(pdMS_TO_TICKS(100));  // 等待停止完成
        esp_mqtt_client_destroy(mqtt_ctx.mqtt_client);
        mqtt_ctx.mqtt_client = NULL;
    }

    taskENTER_CRITICAL(&mqtt_spinlock);
    mqtt_ctx.started = false;
    mqtt_ctx.state = APP_MQTT_STATE_DISCONNECTED;
    taskEXIT_CRITICAL(&mqtt_spinlock);

    ESP_LOGI(TAG, "MQTT客户端已完全停止");
    return ESP_OK;
}

esp_err_t mqtt_client_set_config(const mqtt_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // 将mqtt_config_t转换为app_mqtt_config_t（安全拷贝，强制null终止）
    size_t len = strlen(config->broker_uri);
    memcpy(mqtt_ctx.config.broker_uri, config->broker_uri,
           (len < sizeof(mqtt_ctx.config.broker_uri) - 1) ? len : sizeof(mqtt_ctx.config.broker_uri) - 1);
    mqtt_ctx.config.broker_uri[sizeof(mqtt_ctx.config.broker_uri) - 1] = '\0';

    len = strlen(config->client_id);
    memcpy(mqtt_ctx.config.client_id, config->client_id,
           (len < sizeof(mqtt_ctx.config.client_id) - 1) ? len : sizeof(mqtt_ctx.config.client_id) - 1);
    mqtt_ctx.config.client_id[sizeof(mqtt_ctx.config.client_id) - 1] = '\0';

    len = strlen(config->username);
    memcpy(mqtt_ctx.config.username, config->username,
           (len < sizeof(mqtt_ctx.config.username) - 1) ? len : sizeof(mqtt_ctx.config.username) - 1);
    mqtt_ctx.config.username[sizeof(mqtt_ctx.config.username) - 1] = '\0';

    len = strlen(config->password);
    memcpy(mqtt_ctx.config.password, config->password,
           (len < sizeof(mqtt_ctx.config.password) - 1) ? len : sizeof(mqtt_ctx.config.password) - 1);
    mqtt_ctx.config.password[sizeof(mqtt_ctx.config.password) - 1] = '\0';

    len = strlen(config->topic_prefix);
    memcpy(mqtt_ctx.config.topic_prefix, config->topic_prefix,
           (len < sizeof(mqtt_ctx.config.topic_prefix) - 1) ? len : sizeof(mqtt_ctx.config.topic_prefix) - 1);
    mqtt_ctx.config.topic_prefix[sizeof(mqtt_ctx.config.topic_prefix) - 1] = '\0';

    mqtt_ctx.config.keepalive = config->keepalive;
    mqtt_ctx.config.retain = config->retain;
    mqtt_ctx.config.qos = config->qos;

    ESP_LOGI(TAG, "MQTT配置已更新");
    return ESP_OK;
}

esp_err_t mqtt_client_get_config(mqtt_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // 将app_mqtt_config_t转换为mqtt_config_t（安全拷贝，强制null终止）
    size_t len = strlen(mqtt_ctx.config.broker_uri);
    memcpy(config->broker_uri, mqtt_ctx.config.broker_uri,
           (len < sizeof(config->broker_uri) - 1) ? len : sizeof(config->broker_uri) - 1);
    config->broker_uri[sizeof(config->broker_uri) - 1] = '\0';

    len = strlen(mqtt_ctx.config.client_id);
    memcpy(config->client_id, mqtt_ctx.config.client_id,
           (len < sizeof(config->client_id) - 1) ? len : sizeof(config->client_id) - 1);
    config->client_id[sizeof(config->client_id) - 1] = '\0';

    len = strlen(mqtt_ctx.config.username);
    memcpy(config->username, mqtt_ctx.config.username,
           (len < sizeof(config->username) - 1) ? len : sizeof(config->username) - 1);
    config->username[sizeof(config->username) - 1] = '\0';

    len = strlen(mqtt_ctx.config.password);
    memcpy(config->password, mqtt_ctx.config.password,
           (len < sizeof(config->password) - 1) ? len : sizeof(config->password) - 1);
    config->password[sizeof(config->password) - 1] = '\0';

    len = strlen(mqtt_ctx.config.topic_prefix);
    memcpy(config->topic_prefix, mqtt_ctx.config.topic_prefix,
           (len < sizeof(config->topic_prefix) - 1) ? len : sizeof(config->topic_prefix) - 1);
    config->topic_prefix[sizeof(config->topic_prefix) - 1] = '\0';

    config->keepalive = mqtt_ctx.config.keepalive;
    config->retain = mqtt_ctx.config.retain;
    config->qos = mqtt_ctx.config.qos;

    return ESP_OK;
}

esp_err_t mqtt_client_connect(void)
{
    // 检查是否已连接（状态+客户端对象）
    if (mqtt_ctx.mqtt_client != NULL) {
        if (mqtt_client_is_connected()) {
            ESP_LOGW(TAG, "MQTT已经连接");
            return ESP_OK;
        }
        // 客户端存在但未连接，尝试重新启动
        ESP_LOGI(TAG, "MQTT客户端已存在但未连接，尝试重启");
        esp_err_t ret = esp_mqtt_client_start(mqtt_ctx.mqtt_client);
        if (ret == ESP_OK) {
            taskENTER_CRITICAL(&mqtt_spinlock);
            mqtt_ctx.state = APP_MQTT_STATE_CONNECTING;
            taskEXIT_CRITICAL(&mqtt_spinlock);
            return ESP_OK;
        }
        // 重启失败，销毁旧客户端重新创建
        ESP_LOGW(TAG, "MQTT客户端重启失败，重新创建");
        esp_mqtt_client_destroy(mqtt_ctx.mqtt_client);
        mqtt_ctx.mqtt_client = NULL;
    }

    ESP_LOGI(TAG, "连接到MQTT Broker: %s", mqtt_ctx.config.broker_uri);

    // 配置MQTT客户端
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = mqtt_ctx.config.broker_uri,
        .credentials.username = mqtt_ctx.config.username,
        .credentials.authentication.password = mqtt_ctx.config.password,
        .credentials.client_id = mqtt_ctx.config.client_id,
        .session.keepalive = mqtt_ctx.config.keepalive,
    };

    mqtt_ctx.mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_ctx.mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT客户端初始化失败");
        mqtt_ctx.state = APP_MQTT_STATE_ERROR;
        return ESP_FAIL;
    }

    // 注册事件处理程序
    esp_mqtt_client_register_event(mqtt_ctx.mqtt_client, ESP_EVENT_ANY_ID,
                                     mqtt_event_handler, NULL);

    // 启动MQTT客户端
    esp_err_t ret = esp_mqtt_client_start(mqtt_ctx.mqtt_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MQTT客户端启动失败: %s", esp_err_to_name(ret));
        esp_mqtt_client_destroy(mqtt_ctx.mqtt_client);
        mqtt_ctx.mqtt_client = NULL;
        mqtt_ctx.state = APP_MQTT_STATE_ERROR;
        return ret;
    }

    return ESP_OK;
}

esp_err_t mqtt_client_disconnect(void)
{
    if (mqtt_ctx.mqtt_client == NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "断开MQTT连接");

    esp_mqtt_client_disconnect(mqtt_ctx.mqtt_client);
    esp_mqtt_client_stop(mqtt_ctx.mqtt_client);
    esp_mqtt_client_destroy(mqtt_ctx.mqtt_client);
    mqtt_ctx.mqtt_client = NULL;
    mqtt_ctx.state = APP_MQTT_STATE_DISCONNECTED;

    return ESP_OK;
}

// ==================== 消息发布接口 ====================

esp_err_t mqtt_client_publish(const char *topic, const char *data, uint32_t len, uint8_t qos, bool retain)
{
    if (mqtt_ctx.mqtt_client == NULL || !mqtt_client_is_connected()) {
        ESP_LOGW(TAG, "MQTT未连接，无法发布消息");
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_publish(mqtt_ctx.mqtt_client, topic, data, len, qos, retain);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "MQTT发布失败");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t mqtt_publish_purifier_status(void)
{
    // 获取状态机状态
    fsm_state_t state = fsm_get_state();
    const char *state_name = fsm_get_state_name(state);

    // 获取双TDS数据
    tds_dual_measurement_t dual_tds;
    bool has_tds = (tds_sensor_get_latest_dual(&dual_tds) == ESP_OK && dual_tds.both_valid);

    // 使用静态缓冲区避免 cJSON_PrintUnformatted 的堆分配
    static char json_buf[256];
    snprintf(json_buf, sizeof(json_buf),
             "{\"state\":\"%s\",\"tds_in\":%.1f,\"tds_out\":%.1f,\"tds_reduction_rate\":%.1f}",
             state_name,
             has_tds ? dual_tds.inlet.tds_value : 0,
             has_tds ? dual_tds.outlet.tds_value : 0,
             has_tds ? dual_tds.reduction_rate : 0);

    // 使用配置的topic_prefix
    char topic[128];
    mqtt_get_topic("status", topic, sizeof(topic));
    return mqtt_client_publish(topic, json_buf, strlen(json_buf), 0, true);
}

esp_err_t mqtt_publish_tds_value(void)
{
    tds_dual_measurement_t dual_tds;
    if (tds_sensor_get_latest_dual(&dual_tds) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    static char json_buf[128];
    snprintf(json_buf, sizeof(json_buf),
             "{\"tds_in\":%.1f,\"tds_out\":%.1f,\"tds_reduction_rate\":%.1f}",
             dual_tds.inlet.tds_value, dual_tds.outlet.tds_value, dual_tds.reduction_rate);

    char topic[128];
    mqtt_get_topic("tds", topic, sizeof(topic));
    return mqtt_client_publish(topic, json_buf, strlen(json_buf), 0, true);
}

esp_err_t mqtt_publish_system_status(void)
{
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t uptime = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;

    static char json_buf[128];
    snprintf(json_buf, sizeof(json_buf),
             "{\"free_heap\":%lu,\"uptime\":%lu,\"wifi_connected\":%s}",
             (unsigned long)free_heap, (unsigned long)uptime,
             wifi_manager_is_connected() ? "true" : "false");

    return mqtt_client_publish("system/status", json_buf, strlen(json_buf), 0, true);
}

// ==================== 订阅接口 ====================

esp_err_t mqtt_client_subscribe(const char *topic, uint8_t qos)
{
    if (mqtt_ctx.mqtt_client == NULL || !mqtt_client_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_subscribe(mqtt_ctx.mqtt_client, topic, qos);
    if (msg_id < 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t mqtt_client_unsubscribe(const char *topic)
{
    if (mqtt_ctx.mqtt_client == NULL || !mqtt_client_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_unsubscribe(mqtt_ctx.mqtt_client, topic);
    if (msg_id < 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t mqtt_subscribe_control_topics(void)
{
    char topic[128];

    mqtt_get_topic("set/state", topic, sizeof(topic));
    mqtt_client_subscribe(topic, 0);

    mqtt_get_topic("set/flush", topic, sizeof(topic));
    mqtt_client_subscribe(topic, 0);

    return ESP_OK;
}

// ==================== 状态查询接口 ====================

mqtt_state_t mqtt_client_get_state(void)
{
    // 转换app_mqtt_state_t到mqtt_state_t（值相同）
    return (mqtt_state_t)mqtt_ctx.state;
}

const char* mqtt_client_get_state_name(mqtt_state_t state)
{
    return app_state_names[state];
}

bool mqtt_client_is_connected(void)
{
    if (mqtt_ctx.mqtt_client == NULL) {
        return false;
    }
    // ESP-IDF v6.0中没有esp_mqtt_client_is_connected函数，使用内部状态判断
    bool state_connected = (mqtt_ctx.state == APP_MQTT_STATE_CONNECTED);
    return state_connected;
}

// ==================== 回调接口 ====================

esp_err_t mqtt_client_register_message_callback(mqtt_message_callback_t callback)
{
    mqtt_ctx.message_callback = (app_mqtt_message_callback_t)callback;
    return ESP_OK;
}

esp_err_t mqtt_client_register_connection_callback(mqtt_connection_callback_t callback)
{
    mqtt_ctx.connection_callback = (app_mqtt_connection_callback_t)callback;
    return ESP_OK;
}

// ==================== 事件处理 ====================

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT已连接");
            mqtt_ctx.state = APP_MQTT_STATE_CONNECTED;
            /* 临界保护清除重连任务句柄（避免与断开事件竞态） */
            taskENTER_CRITICAL(&mqtt_spinlock);
            mqtt_ctx.reconnect_task = NULL;
            taskEXIT_CRITICAL(&mqtt_spinlock);
            if (mqtt_ctx.connection_callback) {
                mqtt_ctx.connection_callback(true);
            }
            mqtt_subscribe_control_topics();
            // 发送Home Assistant发现配置（首次连接时，有retain标志会持久化）
            mqtt_send_ha_discovery();
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT已断开，启动自动重连");
            mqtt_ctx.state = APP_MQTT_STATE_DISCONNECTED;
            if (mqtt_ctx.connection_callback) {
                mqtt_ctx.connection_callback(false);
            }
            /* 启动重连任务（临界保护防止多任务重复创建） */
            taskENTER_CRITICAL(&mqtt_spinlock);
            if (mqtt_ctx.reconnect_task == NULL) {
                BaseType_t ret = xTaskCreate(mqtt_reconnect_task, "mqtt_recon", 3072, NULL, 4, &mqtt_ctx.reconnect_task);
                if (ret != pdPASS) {
                    ESP_LOGE(TAG, "创建MQTT重连任务失败");
                }
            }
            taskEXIT_CRITICAL(&mqtt_spinlock);
            break;

        case MQTT_EVENT_DATA:
            if (event->data_len > 0 && event->topic_len > 0) {
                if (event->topic_len > MQTT_MAX_TOPIC_LEN || event->data_len > MQTT_MAX_DATA_LEN) {
                    ESP_LOGW(TAG, "MQTT消息过大，丢弃 (topic=%d, data=%d)", event->topic_len, event->data_len);
                    break;
                }
                char topic[MQTT_MAX_TOPIC_LEN + 1];
                char data[MQTT_MAX_DATA_LEN + 1];
                memcpy(topic, event->topic, event->topic_len);
                topic[event->topic_len] = '\0';
                memcpy(data, event->data, event->data_len);
                data[event->data_len] = '\0';

                ESP_LOGI(TAG, "收到MQTT消息: %s = %s", topic, data);

                // 处理控制消息
                if (strstr(topic, "/set/state")) {
                    cJSON *root = cJSON_Parse(data);
                    if (root == NULL) {
                        ESP_LOGW(TAG, "MQTT消息JSON解析失败: %s", data);
                        break;
                    }
                    cJSON *state = cJSON_GetObjectItem(root, "state");
                    if (state && cJSON_IsString(state)) {
                        if (strcmp(state->valuestring, "producing") == 0) {
                            fsm_send_event(FSM_EVENT_FORCE_PRODUCTION);
                        } else if (strcmp(state->valuestring, "idle") == 0) {
                            fsm_force_standby();
                        } else if (strcmp(state->valuestring, "flushing") == 0) {
                            fsm_send_event(FSM_EVENT_FORCE_FLUSH);
                        } else if (strcmp(state->valuestring, "reset") == 0) {
                            fsm_clear_stop();
                        }
                    }
                    cJSON_Delete(root);
                } else if (strstr(topic, "/set/flush")) {
                    fsm_send_event(FSM_EVENT_FORCE_FLUSH);
                }

                if (mqtt_ctx.message_callback) {
                    mqtt_ctx.message_callback(topic, data, event->data_len);
                }
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT错误");
            mqtt_ctx.state = APP_MQTT_STATE_ERROR;
            if (mqtt_ctx.connection_callback) {
                mqtt_ctx.connection_callback(false);
            }
            /* 与断开事件一样启动重连任务 */
            taskENTER_CRITICAL(&mqtt_spinlock);
            if (mqtt_ctx.reconnect_task == NULL && mqtt_ctx.started) {
                BaseType_t ret = xTaskCreate(mqtt_reconnect_task, "mqtt_recon", 3072, NULL, 4, &mqtt_ctx.reconnect_task);
                if (ret != pdPASS) {
                    ESP_LOGE(TAG, "创建MQTT重连任务失败");
                }
            }
            taskEXIT_CRITICAL(&mqtt_spinlock);
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "MQTT消息已发布");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT订阅成功");
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT取消订阅");
            break;

        default:
            break;
    }
}

// ==================== 辅助函数 ====================

static char* mqtt_get_topic(const char *subtopic, char *buffer, size_t buffer_size)
{
    snprintf(buffer, buffer_size, "%s/%s", mqtt_ctx.config.topic_prefix, subtopic);
    return buffer;
}

// ==================== 配置存储接口 ====================

esp_err_t mqtt_client_save_config(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("mqtt_config", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    nvs_set_str(nvs_handle, "broker_uri", mqtt_ctx.config.broker_uri);
    nvs_set_str(nvs_handle, "client_id", mqtt_ctx.config.client_id);
    nvs_set_str(nvs_handle, "username", mqtt_ctx.config.username);
    nvs_set_str(nvs_handle, "password", mqtt_ctx.config.password);
    nvs_set_str(nvs_handle, "topic_prefix", mqtt_ctx.config.topic_prefix);
    nvs_set_u16(nvs_handle, "keepalive", mqtt_ctx.config.keepalive);
    nvs_set_u8(nvs_handle, "retain", mqtt_ctx.config.retain);
    nvs_set_u8(nvs_handle, "qos", mqtt_ctx.config.qos);

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "MQTT配置已保存");
    return err;
}

esp_err_t mqtt_client_load_config(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("mqtt_config", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "未找到保存的MQTT配置");
        }
        return err;
    }

    size_t len = sizeof(mqtt_ctx.config.broker_uri);
    err = nvs_get_str(nvs_handle, "broker_uri", mqtt_ctx.config.broker_uri, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) goto exit;

    len = sizeof(mqtt_ctx.config.client_id);
    err = nvs_get_str(nvs_handle, "client_id", mqtt_ctx.config.client_id, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) goto exit;

    len = sizeof(mqtt_ctx.config.username);
    err = nvs_get_str(nvs_handle, "username", mqtt_ctx.config.username, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) goto exit;

    len = sizeof(mqtt_ctx.config.password);
    err = nvs_get_str(nvs_handle, "password", mqtt_ctx.config.password, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) goto exit;

    len = sizeof(mqtt_ctx.config.topic_prefix);
    err = nvs_get_str(nvs_handle, "topic_prefix", mqtt_ctx.config.topic_prefix, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) goto exit;

    uint16_t keepalive;
    err = nvs_get_u16(nvs_handle, "keepalive", &keepalive);
    if (err == ESP_OK) mqtt_ctx.config.keepalive = keepalive;

    uint8_t retain;
    err = nvs_get_u8(nvs_handle, "retain", &retain);
    if (err == ESP_OK) mqtt_ctx.config.retain = retain;

    uint8_t qos;
    err = nvs_get_u8(nvs_handle, "qos", &qos);
    if (err == ESP_OK) mqtt_ctx.config.qos = qos;

exit:
    nvs_close(nvs_handle);

    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "MQTT配置已加载");
        return ESP_OK;
    }
    return err;
}

esp_err_t mqtt_client_clear_config(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("mqtt_config", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    nvs_erase_all(nvs_handle);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "MQTT配置已清除");
    return ESP_OK;
}

bool mqtt_client_has_saved_config(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("mqtt_config", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t len;
    err = nvs_get_str(nvs_handle, "broker_uri", NULL, &len);
    nvs_close(nvs_handle);

    return err == ESP_OK;
}

// ==================== Home Assistant集成 ====================

esp_err_t mqtt_send_ha_discovery(void)
{
    if (!mqtt_client_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    mqtt_send_ha_sensor_config("TDS In", "tds_in", "ppm");
    mqtt_send_ha_sensor_config("TDS Out", "tds_out", "ppm");
    mqtt_send_ha_sensor_config("TDS Reduction Rate", "tds_reduction_rate", "%");
    mqtt_send_ha_sensor_config("State", "purifier_state", NULL);
    mqtt_send_ha_switch_config("Water Purifier");

    return ESP_OK;
}

esp_err_t mqtt_send_ha_sensor_config(const char *sensor_name, const char *sensor_type, const char *unit)
{
    char topic[128], discovery_msg[512], state_topic[128];
    char value_template[64];

    snprintf(topic, sizeof(topic), "homeassistant/sensor/water_purifier/%s/config", sensor_name);
    snprintf(value_template, sizeof(value_template), "{{ value_json.%s }}", sensor_type);

    // 使用配置的topic前缀构建state_topic
    mqtt_get_topic("state", state_topic, sizeof(state_topic));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "name", cJSON_CreateString(sensor_name));
    cJSON_AddItemToObject(root, "unique_id", cJSON_CreateString(topic));
    cJSON_AddItemToObject(root, "state_topic", cJSON_CreateString(state_topic));
    cJSON_AddItemToObject(root, "value_template", cJSON_CreateString(value_template));
    if (unit) {
        cJSON_AddItemToObject(root, "unit_of_measurement", cJSON_CreateString(unit));
    }
    cJSON_AddItemToObject(root, "device_class", cJSON_CreateString("sensor"));

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        snprintf(discovery_msg, sizeof(discovery_msg), "%s", json_str);
        cJSON_Delete(root);  // 先删除cJSON对象
        free(json_str);      // 再释放Print分配的内存
    } else {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    return mqtt_client_publish(topic, discovery_msg, strlen(discovery_msg), 0, true);
}

esp_err_t mqtt_send_ha_switch_config(const char *switch_name)
{
    char topic[128], discovery_msg[512], state_topic[128], command_topic[128];

    snprintf(topic, sizeof(topic), "homeassistant/switch/water_purifier/%s/config", switch_name);

    // 使用配置的topic前缀构建state_topic和command_topic
    mqtt_get_topic("state", state_topic, sizeof(state_topic));
    mqtt_get_topic("set/state", command_topic, sizeof(command_topic));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "name", cJSON_CreateString(switch_name));
    cJSON_AddItemToObject(root, "unique_id", cJSON_CreateString(topic));
    cJSON_AddItemToObject(root, "command_topic", cJSON_CreateString(command_topic));
    cJSON_AddItemToObject(root, "state_topic", cJSON_CreateString(state_topic));
    cJSON_AddItemToObject(root, "payload_on", cJSON_CreateString("producing"));
    cJSON_AddItemToObject(root, "payload_off", cJSON_CreateString("idle"));

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        snprintf(discovery_msg, sizeof(discovery_msg), "%s", json_str);
        cJSON_Delete(root);  // 先删除cJSON对象
        free(json_str);      // 再释放Print分配的内存
    } else {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    return mqtt_client_publish(topic, discovery_msg, strlen(discovery_msg), 0, true);
}

// ==================== 调试接口 ====================

esp_err_t mqtt_client_get_status(char *buffer, size_t buffer_size)
{
    snprintf(buffer, buffer_size,
             "MQTT状态: %s\n"
             "Broker: %s\n"
             "Client ID: %s\n"
             "连接%s",
             app_state_names[mqtt_ctx.state],
             mqtt_ctx.config.broker_uri,
             mqtt_ctx.config.client_id,
             mqtt_client_is_connected() ? "已连接" : "未连接");
    return ESP_OK;
}

void mqtt_client_print_info(void)
{
    char status[256];
    mqtt_client_get_status(status, sizeof(status));
    ESP_LOGI(TAG, "%s", status);
}

/**
 * @brief MQTT自动重连任务
 *
 * 策略：指数退避重连（1s→2s→4s→8s→16s→30s），连接成功后自动退出
 * 收到stop_requested信号时安全退出
 */
static void mqtt_reconnect_task(void *pvParameters)
{
    int retry = 0;
    const int max_retries = 20;
    const TickType_t delays[] = {
        pdMS_TO_TICKS(1000),
        pdMS_TO_TICKS(2000),
        pdMS_TO_TICKS(4000),
        pdMS_TO_TICKS(8000),
        pdMS_TO_TICKS(16000),
        pdMS_TO_TICKS(30000),
    };
    const int delay_count = sizeof(delays) / sizeof(delays[0]);

    ESP_LOGI(TAG, "MQTT重连任务启动");

    // 注册看门狗
    esp_task_wdt_add(NULL);

    // 首先检查WiFi是否已连接，避免无WiFi时浪费资源
    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "WiFi未连接，MQTT重连任务提前退出");
        esp_task_wdt_delete(NULL);
        taskENTER_CRITICAL(&mqtt_spinlock);
        mqtt_ctx.reconnect_task = NULL;
        taskEXIT_CRITICAL(&mqtt_spinlock);
        vTaskDelete(NULL);
        return;
    }

    while (retry < max_retries) {
        // 检查停止请求（优先检查）
        taskENTER_CRITICAL(&mqtt_spinlock);
        bool should_stop = mqtt_ctx.stop_requested;
        taskEXIT_CRITICAL(&mqtt_spinlock);
        if (should_stop) {
            ESP_LOGI(TAG, "收到停止请求，MQTT重连任务退出");
            break;
        }

        TickType_t delay = delays[(retry < delay_count) ? retry : delay_count - 1];
        esp_task_wdt_reset();  // 长延迟前重置看门狗

        // 分段延迟，每100ms检查一次停止请求
        TickType_t remaining = delay;
        while (remaining > 0) {
            TickType_t chunk = (remaining > pdMS_TO_TICKS(100)) ? pdMS_TO_TICKS(100) : remaining;
            vTaskDelay(chunk);
            remaining -= chunk;
            esp_task_wdt_reset();

            taskENTER_CRITICAL(&mqtt_spinlock);
            should_stop = mqtt_ctx.stop_requested;
            taskEXIT_CRITICAL(&mqtt_spinlock);
            if (should_stop) {
                ESP_LOGI(TAG, "延迟期间收到停止请求，MQTT重连任务退出");
                break;
            }
        }
        if (should_stop) break;

        // 每次重试前检查WiFi状态
        if (!wifi_manager_is_connected()) {
            ESP_LOGW(TAG, "WiFi连接断开，暂停MQTT重连");
            esp_task_wdt_delete(NULL);
            taskENTER_CRITICAL(&mqtt_spinlock);
            mqtt_ctx.reconnect_task = NULL;
            taskEXIT_CRITICAL(&mqtt_spinlock);
            vTaskDelete(NULL);
            return;
        }

        if (mqtt_client_is_connected()) {
            ESP_LOGI(TAG, "重连期间已恢复连接，取消重连任务");
            break;
        }

        taskENTER_CRITICAL(&mqtt_spinlock);
        bool is_started = mqtt_ctx.started;
        taskEXIT_CRITICAL(&mqtt_spinlock);
        if (!is_started) {
            ESP_LOGI(TAG, "MQTT已停止，取消重连任务");
            break;
        }

        if (mqtt_ctx.mqtt_client == NULL) {
            ESP_LOGW(TAG, "MQTT客户端为空，重新初始化");
            esp_err_t conn_ret = mqtt_client_connect();
            if (conn_ret != ESP_OK) {
                ESP_LOGE(TAG, "MQTT连接初始化失败: %s", esp_err_to_name(conn_ret));
                retry++;
                continue;
            }
        } else {
            ESP_LOGD(TAG, "尝试MQTT重连 %d/%d", retry + 1, max_retries);
            esp_err_t ret = esp_mqtt_client_start(mqtt_ctx.mqtt_client);
            if (ret == ESP_OK) {
                /* 等待连接成功或超时，最多等待5秒 */
                for (int wait = 0; wait < 50; wait++) {
                    esp_task_wdt_reset();  // 每100ms重置看门狗
                    vTaskDelay(pdMS_TO_TICKS(100));

                    // 检查停止请求
                    taskENTER_CRITICAL(&mqtt_spinlock);
                    should_stop = mqtt_ctx.stop_requested;
                    taskEXIT_CRITICAL(&mqtt_spinlock);
                    if (should_stop) {
                        ESP_LOGI(TAG, "等待连接期间收到停止请求，退出");
                        break;
                    }

                    if (mqtt_client_is_connected()) {
                        ESP_LOGI(TAG, "MQTT重连成功");
                        break;
                    }
                }
                if (should_stop) break;
                if (!mqtt_client_is_connected()) {
                    esp_mqtt_client_disconnect(mqtt_ctx.mqtt_client);
                }
            }
        }

        retry++;
    }

    if (retry >= max_retries && !mqtt_client_is_connected()) {
        ESP_LOGE(TAG, "MQTT重连失败，已达最大重试次数");
    }

    esp_task_wdt_delete(NULL);  // 任务结束前注销看门狗
    taskENTER_CRITICAL(&mqtt_spinlock);
    mqtt_ctx.reconnect_task = NULL;
    taskEXIT_CRITICAL(&mqtt_spinlock);
    vTaskDelete(NULL);
}
