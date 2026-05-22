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
#include "esp_task_wdt.h"  // 任务看门狗

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

    // 纯水洗膜期间跳过TDS报警检测（泵停止、进水阀关闭，TDS读数无效）
    if (new_state == FSM_STATE_PURE_FLUSH) {
        tds_sensor_set_skip_alarm_detection(true);
    } else if (old_state == FSM_STATE_PURE_FLUSH) {
        tds_sensor_set_skip_alarm_detection(false);
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
        // WiFi连接成功后启动MQTT（IP已在wifi_manager中打印）
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
            // Home Assistant发现配置在MQTT_EVENT_CONNECTED事件中自动发送
        }
    } else if (state == WIFI_STATE_DISCONNECTED || state == WIFI_STATE_RECONNECTING) {
        // WiFi断开/重连时停止MQTT（避免无WiFi时MQTT重连浪费资源）
        if (mqtt_client_is_connected()) {
            ESP_LOGI(TAG, "WiFi断开，停止MQTT客户端...");
            mqtt_client_stop();
        }
    }
}

// ==================== 监控任务 ====================

static void monitor_task(void *arg)
{
    ESP_LOGI(TAG, "监控任务启动");

    // 注册监控任务到看门狗（必须在循环reset前注册）
    esp_err_t wdt_ret = esp_task_wdt_add(NULL);
    if (wdt_ret == ESP_OK) {
        ESP_LOGI(TAG, "监控任务已注册到看门狗");
    } else {
        ESP_LOGW(TAG, "监控任务看门狗注册失败: %s", esp_err_to_name(wdt_ret));
    }

    uint32_t loop_count = 0;

    while (1) {
        // 重置看门狗（循环开始时）
        esp_task_wdt_reset();

        // 每30秒执行一次完整检查
        if (loop_count % 30 == 0) {
            // 检查漏水
            if (gpio_driver_read_water_leak() && !fsm_is_stop_state()) {
                ESP_LOGE(TAG, "检测到漏水！");
                fsm_send_event(FSM_EVENT_WATER_LEAK);
            }

            // 状态日志改为DEBUG级别，减少刷屏
            fsm_state_t state = fsm_get_state();
            tds_dual_measurement_t tds;
            tds_sensor_get_latest_dual(&tds);

            ESP_LOGD(TAG, "状态: %s, TDS: %.0f/%.0f ppm",
                     fsm_get_state_name(state),
                     tds.inlet.valid ? tds.inlet.tds_value : 0,
                     tds.outlet.valid ? tds.outlet.tds_value : 0);

            // 堆内存监控（仅异常时输出）
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
                // MQTT发布后重置看门狗
                esp_task_wdt_reset();
            }
        }

        loop_count++;

        // 关键：延迟时间必须小于看门狗超时（5秒）
        // 使用1秒延迟确保每秒都能reset看门狗
        vTaskDelay(pdMS_TO_TICKS(1000));  // 1秒
    }
}

// ==================== 主函数 ====================

void app_main(void)
{
    // 生产环境日志级别设置（保留关键INFO日志，Web页面可按级别过滤）
    // 调试时可临时改为 ESP_LOG_DEBUG
    esp_log_level_set("CONFIG", ESP_LOG_INFO);
    esp_log_level_set("GPIO_DRIVER", ESP_LOG_INFO);
    esp_log_level_set("TDS", ESP_LOG_INFO);
    esp_log_level_set("FILTER_MGR", ESP_LOG_INFO);
    esp_log_level_set("FSM", ESP_LOG_INFO);
    esp_log_level_set("HISTORY", ESP_LOG_INFO);
    esp_log_level_set("WIFI", ESP_LOG_INFO);
    esp_log_level_set("WEB", ESP_LOG_INFO);
    esp_log_level_set("APP_MQTT", ESP_LOG_INFO);
    esp_log_level_set("OTA", ESP_LOG_INFO);
    esp_log_level_set("PM", ESP_LOG_INFO);
    esp_log_level_set("MAIN", ESP_LOG_INFO);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  净水器主控板程序启动");
    ESP_LOGI(TAG, "  硬件: ESP32-C3");
    ESP_LOGI(TAG, "========================================");

    // 0. 任务看门狗说明
    // ESP-IDF v6.0系统启动时已初始化看门狗（~5秒超时），无需重复初始化
    // 我们的monitor和fsm任务会注册到看门狗并定期reset
    ESP_LOGI(TAG, "[0/10] 任务看门狗由系统配置（~5秒超时）");

    // 1. 初始化配置管理器
    ESP_LOGI(TAG, "[1/10] 初始化配置管理器...");
    config_manager_init();
    config_manager_print_config();

    // 2. 初始化GPIO
    ESP_LOGI(TAG, "[2/10] 初始化GPIO驱动...");
    gpio_driver_init_inputs();
    gpio_driver_init_outputs();
    gpio_driver_init_leds();

    // 应用继电器触发电平配置
    system_config_t cfg;
    config_manager_get_config(&cfg);
    gpio_driver_set_relay_trigger_level(cfg.relay_trigger_level);

    // 3. 初始化电源管理（FSM回调会调用pm_manager_set_cpu_mode，需提前初始化）
    ESP_LOGI(TAG, "[3/10] 初始化电源管理...");
    if (pm_manager_init() != ESP_OK) {
        ESP_LOGW(TAG, "电源管理初始化失败");
    }

    // 4. 初始化TDS传感器
    ESP_LOGI(TAG, "[4/10] 初始化TDS传感器...");
    tds_sensor_init();
    tds_sensor_set_alarm_threshold(TDS_SENSOR_INLET, cfg.tds_inlet_threshold);
    tds_sensor_set_alarm_threshold(TDS_SENSOR_OUTLET, cfg.tds_outlet_threshold);
    // 加载TDS校准参数（从config_manager持久化数据恢复）
    tds_calibration_t cal;
    cal.offset = cfg.tds_calibration_offset[TDS_SENSOR_INLET];
    cal.scale = cfg.tds_calibration_scale[TDS_SENSOR_INLET];
    tds_sensor_set_calibration(TDS_SENSOR_INLET, &cal);
    cal.offset = cfg.tds_calibration_offset[TDS_SENSOR_OUTLET];
    cal.scale = cfg.tds_calibration_scale[TDS_SENSOR_OUTLET];
    tds_sensor_set_calibration(TDS_SENSOR_OUTLET, &cal);
    filter_mgr_init();
    tds_sensor_start();

    // 5. 初始化状态机
    ESP_LOGI(TAG, "[5/10] 初始化状态机...");
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

    // 6. 初始化历史记录模块
    ESP_LOGI(TAG, "[6/10] 初始化历史记录模块...");
    history_logger_init();

    // 7. 初始化WiFi
    ESP_LOGI(TAG, "[7/10] 初始化WiFi...");
    wifi_manager_init();
    wifi_manager_register_callback(wifi_state_callback);
    wifi_manager_start();

    // 8. 初始化MQTT客户端
    ESP_LOGI(TAG, "[8/10] 初始化MQTT客户端...");
    mqtt_client_init();

    // 9. 初始化OTA模块
    ESP_LOGI(TAG, "[9/10] 初始化OTA模块...");
    if (ota_update_init() != ESP_OK) {
        ESP_LOGW(TAG, "OTA模块初始化失败，固件升级功能不可用");
    }

    // 10. 初始化Web服务器
    ESP_LOGI(TAG, "[10/10] 初始化Web服务器...");
    web_server_init();
    web_server_init_log_interceptor();
    web_server_start();

    // 创建监控任务（高优先级，确保能及时reset看门狗）
    // 优先级必须高于fsm_task(5)，否则会被阻塞导致看门狗超时
    BaseType_t ret = xTaskCreate(monitor_task, "monitor", 2048, NULL, 6, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "监控任务创建失败");
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  系统初始化完成！");
    ESP_LOGI(TAG, "  可用内存: %lu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "========================================");

    // 主循环
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));  // 1分钟
    }
}