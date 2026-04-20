/**
 * @file main.c
 * @brief 净水器主控板程序 - 主入口
 * @note 基于ESP32-C3开发板，75G RO膜 + 3G压力桶
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"

// 模块头文件
#include "gpio_driver.h"
#include "tds_sensor.h"
#include "filter_manager.h"
#include "water_purifier_fsm.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "config_manager.h"
#include "history_logger.h"
#include "ota_update.h"
#include "pm_manager.h"
#include "app_mqtt_public.h"

static const char *TAG = "MAIN";

// ==================== 回调函数 ====================

static void fsm_state_callback(fsm_state_t old_state, fsm_state_t new_state)
{
    ESP_LOGI(TAG, "状态变化: %s -> %s",
             fsm_get_state_name(old_state),
             fsm_get_state_name(new_state));

    // 根据状态切换CPU频率
    if (new_state == FSM_STATE_PRODUCTION ||
        new_state == FSM_STATE_NORMAL_FLUSH ||
        new_state == FSM_STATE_PURE_FLUSH ||
        new_state == FSM_STATE_TANK_FULL) {
        // 制水和冲洗期间保持160MHz高性能
        pm_manager_set_cpu_mode(false);
    } else {
        // 待机/缺水/停止/漏水时降低到80MHz
        pm_manager_set_cpu_mode(true);
    }

    // 发布MQTT状态更新
    if (mqtt_client_is_connected()) {
        mqtt_publish_purifier_status();
    }
}

static void wifi_state_callback(wifi_state_t state)
{
    ESP_LOGI(TAG, "WiFi状态: %s", wifi_manager_get_state_name(state));

    if (state == WIFI_STATE_CONNECTED) {
        char ip[16] = "";
        wifi_manager_get_ip(ip, sizeof(ip));
        ESP_LOGI(TAG, "获取IP: %s", ip);

        // WiFi连接成功后启动MQTT
        system_config_t cfg;
        config_manager_get_config(&cfg);

        if (cfg.mqtt_enabled && strlen(cfg.mqtt_broker) > 0) {
            ESP_LOGI(TAG, "启动MQTT客户端...");

            mqtt_config_t mqtt_cfg = {0};
            strncpy(mqtt_cfg.broker_uri, cfg.mqtt_broker, sizeof(mqtt_cfg.broker_uri) - 1);

            uint8_t mac[6] = {0};
            esp_wifi_get_mac(WIFI_IF_STA, mac);
            snprintf(mqtt_cfg.client_id, sizeof(mqtt_cfg.client_id),
                     "water-purifier-%02X%02X%02X", mac[3], mac[4], mac[5]);

            strncpy(mqtt_cfg.username, cfg.mqtt_username, sizeof(mqtt_cfg.username) - 1);
            strncpy(mqtt_cfg.password, cfg.mqtt_password, sizeof(mqtt_cfg.password) - 1);
            strncpy(mqtt_cfg.topic_prefix, cfg.mqtt_topic_prefix, sizeof(mqtt_cfg.topic_prefix) - 1);
            mqtt_cfg.keepalive = 60;
            mqtt_cfg.qos = 0;
            mqtt_cfg.retain = false;

            mqtt_client_set_config(&mqtt_cfg);
            mqtt_client_start();

            // 发送Home Assistant发现配置
            if (mqtt_client_is_connected()) {
                mqtt_send_ha_discovery();
            }
        }
    }
}

// ==================== 监控任务 ====================

static void monitor_task(void *arg)
{
    ESP_LOGI(TAG, "监控任务启动");

    while (1) {
        // 检查漏水
        if (gpio_driver_read_water_leak() && !fsm_is_stop_state()) {
            ESP_LOGE(TAG, "检测到漏水！");
            fsm_send_event(FSM_EVENT_WATER_LEAK);
        }

        // 打印状态
        fsm_state_t state = fsm_get_state();
        tds_dual_measurement_t tds;
        tds_sensor_get_latest_dual(&tds);

        ESP_LOGI(TAG, "状态: %s, TDS: %.0f/%.0f ppm",
                 fsm_get_state_name(state),
                 tds.inlet.valid ? tds.inlet.tds_value : 0,
                 tds.outlet.valid ? tds.outlet.tds_value : 0);

        // 堆内存监控
        pm_manager_check_heap();

        // WiFi TX功率动态调整（基于RSSI）
        if (wifi_manager_is_connected()) {
            int8_t rssi = wifi_manager_get_rssi();
            if (rssi != 0) {
                pm_manager_adjust_wifi_tx_power(rssi);
            }
        }

        // 发布MQTT状态
        if (mqtt_client_is_connected()) {
            mqtt_publish_purifier_status();
            mqtt_publish_tds_value();
        }

        vTaskDelay(pdMS_TO_TICKS(30000));  // 30秒
    }
}

// ==================== 主函数 ====================

void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  净水器主控板程序启动");
    ESP_LOGI(TAG, "  硬件: ESP32-C3");
    ESP_LOGI(TAG, "========================================");

    // 1. 初始化配置管理器
    ESP_LOGI(TAG, "[1/7] 初始化配置管理器...");
    config_manager_init();
    config_manager_print_config();

    // 2. 初始化GPIO
    ESP_LOGI(TAG, "[2/7] 初始化GPIO驱动...");
    gpio_driver_init_inputs();
    gpio_driver_init_outputs();
    gpio_driver_init_leds();

    // 应用继电器触发电平配置
    system_config_t cfg;
    config_manager_get_config(&cfg);
    gpio_driver_set_relay_trigger_level(cfg.relay_trigger_level);

    // 3. 初始化TDS传感器
    ESP_LOGI(TAG, "[3/7] 初始化TDS传感器...");
    tds_sensor_init();
    tds_sensor_set_alarm_threshold(TDS_SENSOR_INLET, cfg.tds_inlet_threshold);
    tds_sensor_set_alarm_threshold(TDS_SENSOR_OUTLET, cfg.tds_outlet_threshold);
    filter_mgr_init();
    tds_sensor_start();

    // 4. 初始化状态机
    ESP_LOGI(TAG, "[4/7] 初始化状态机...");
    fsm_init();
    fsm_register_state_callback(fsm_state_callback);
    fsm_set_normal_flush_duration(cfg.normal_flush_duration_sec);
    fsm_set_pure_flush_duration(cfg.pure_flush_duration_sec);
    fsm_set_filter_flush_duration(cfg.filter_flush_duration_sec);
    fsm_set_production_timeout(cfg.production_timeout_sec);
    fsm_set_leak_confirm_time(cfg.leak_confirm_time_sec);
    fsm_set_water_hammer_delays(cfg.water_hammer_valve_open_delay_ms,
                                 cfg.water_hammer_pump_stop_delay_ms,
                                 cfg.water_hammer_valve_close_delay_ms);
    fsm_set_runtime_save_interval(cfg.runtime_save_interval_min);
    fsm_set_production_rate_by_membrane(cfg.ro_membrane_type);
    fsm_start();

    // 5. 初始化历史记录模块
    ESP_LOGI(TAG, "[5/8] 初始化历史记录模块...");
    history_logger_init();

    // 6. 初始化WiFi
    ESP_LOGI(TAG, "[6/8] 初始化WiFi...");
    wifi_manager_init();
    wifi_manager_register_callback(wifi_state_callback);
    wifi_manager_start();

    // 7. 初始化MQTT客户端
    ESP_LOGI(TAG, "[7/9] 初始化MQTT客户端...");
    mqtt_client_init();

    // 8. 初始化OTA模块
    ESP_LOGI(TAG, "[8/9] 初始化OTA模块...");
    if (ota_update_init() != ESP_OK) {
        ESP_LOGW(TAG, "OTA模块初始化失败，固件升级功能不可用");
    }

    // 9. 初始化Web服务器
    ESP_LOGI(TAG, "[9/9] 初始化Web服务器...");
    web_server_init();
    web_server_start();

    // 创建监控任务
    xTaskCreate(monitor_task, "monitor", 4096, NULL, 3, NULL);

    // 初始化电源管理
    ESP_LOGI(TAG, "[10/10] 初始化电源管理...");
    if (pm_manager_init() != ESP_OK) {
        ESP_LOGW(TAG, "电源管理初始化失败");
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  系统初始化完成！");
    ESP_LOGI(TAG, "  可用内存: %lu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "========================================");

    // 主循环
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));  // 1分钟
    }
}