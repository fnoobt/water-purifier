/**
 * @file config_manager.c
 * @brief 配置管理模块实现
 */

#include "config_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <math.h>

static const char *TAG = "CONFIG";

// ==================== NVS命名空间 ====================

#define NVS_NAMESPACE "water_purifier"

// ==================== NVS迁移 ====================

/**
 * @brief 迁移旧NVS命名空间到统一结构
 * @note 仅在首次启动时执行，检测到旧命名空间后迁移并删除
 */
static void config_manager_migrate_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err;
    bool migrated = false;

    // 检查是否已完成迁移（避免每次重启重复迁移）
    uint8_t migration_done = 0;
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        if (nvs_get_u8(handle, "nvs_migrated", &migration_done) == ESP_OK && migration_done == 1) {
            nvs_close(handle);
            ESP_LOGD(TAG, "NVS迁移已完成，跳过");
            return;  // 已迁移，直接返回
        }
        nvs_close(handle);
    }

    // 1. 迁移wifi命名空间 → water_purifier
    err = nvs_open("wifi", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        char ssid[33] = {0};
        char pass[65] = {0};
        size_t len = sizeof(ssid);
        bool has_wifi = (nvs_get_str(handle, "ssid", ssid, &len) == ESP_OK && strlen(ssid) > 0);
        len = sizeof(pass);
        nvs_get_str(handle, "pass", pass, &len);

        nvs_close(handle);

        if (has_wifi) {
            // 写入water_purifier命名空间
            nvs_handle_t wp_handle;
            if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &wp_handle) == ESP_OK) {
                esp_err_t e1 = nvs_set_str(wp_handle, "wifi_ssid", ssid);
                esp_err_t e2 = nvs_set_str(wp_handle, "wifi_pass", pass);
                esp_err_t ce = nvs_commit(wp_handle);
                nvs_close(wp_handle);
                if (e1 == ESP_OK && e2 == ESP_OK && ce == ESP_OK) {
                    ESP_LOGI(TAG, "迁移WiFi配置: %s", ssid);
                    migrated = true;
                } else {
                    ESP_LOGW(TAG, "迁移WiFi配置写入失败: %s/%s/%s",
                             esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(ce));
                }
            } else {
                ESP_LOGW(TAG, "迁移WiFi配置: 打开water_purifier命名空间失败");
            }
        }

        // 删除旧wifi命名空间（v6.0: 打开后擦除全部）
        nvs_handle_t erase_handle;
        if (nvs_open("wifi", NVS_READWRITE, &erase_handle) == ESP_OK) {
            nvs_erase_all(erase_handle);
            nvs_commit(erase_handle);
            nvs_close(erase_handle);
            ESP_LOGI(TAG, "已删除旧wifi命名空间");
        }
    }

    // 2. 迁移mqtt_config命名空间 → water_purifier
    err = nvs_open("mqtt_config", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        char broker[128] = {0};
        char user[64] = {0};
        char pass[64] = {0};
        char topic[64] = {0};
        size_t len;

        len = sizeof(broker);
        bool has_mqtt = (nvs_get_str(handle, "broker_uri", broker, &len) == ESP_OK && strlen(broker) > 0);
        len = sizeof(user);
        nvs_get_str(handle, "username", user, &len);
        len = sizeof(pass);
        nvs_get_str(handle, "password", pass, &len);
        len = sizeof(topic);
        nvs_get_str(handle, "topic_prefix", topic, &len);

        nvs_close(handle);

        if (has_mqtt) {
            nvs_handle_t wp_handle;
            if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &wp_handle) == ESP_OK) {
                esp_err_t e1 = nvs_set_str(wp_handle, "mqtt_broker", broker);
                esp_err_t e2 = nvs_set_str(wp_handle, "mqtt_user", user);
                esp_err_t e3 = nvs_set_str(wp_handle, "mqtt_pass", pass);
                esp_err_t e4 = nvs_set_str(wp_handle, "mqtt_topic", topic);
                esp_err_t e5 = nvs_set_u8(wp_handle, "mqtt_en", 1);
                esp_err_t ce = nvs_commit(wp_handle);
                nvs_close(wp_handle);
                if (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK && e4 == ESP_OK && e5 == ESP_OK && ce == ESP_OK) {
                    ESP_LOGI(TAG, "迁移MQTT配置: %s", broker);
                    migrated = true;
                } else {
                    ESP_LOGW(TAG, "迁移MQTT配置写入失败");
                }
            } else {
                ESP_LOGW(TAG, "迁移MQTT配置: 打开water_purifier命名空间失败");
            }
        }

        // 删除旧mqtt_config命名空间（v6.0: 打开后擦除全部）
        nvs_handle_t erase_handle;
        if (nvs_open("mqtt_config", NVS_READWRITE, &erase_handle) == ESP_OK) {
            nvs_erase_all(erase_handle);
            nvs_commit(erase_handle);
            nvs_close(erase_handle);
            ESP_LOGI(TAG, "已删除旧mqtt_config命名空间");
        }
    }

    // 3. 迁移wp_rt命名空间 → wp_filters（仅total_water）
    err = nvs_open("wp_rt", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        uint32_t total_water = 0;
        if (nvs_get_u32(handle, "total_water", &total_water) == ESP_OK && total_water > 0) {
            // 写入wp_filters命名空间
            nvs_handle_t filter_handle;
            if (nvs_open("wp_filters", NVS_READWRITE, &filter_handle) == ESP_OK) {
                // 检查wp_filters是否已有total_water，避免覆盖新数据
                uint32_t existing_water = 0;
                if (nvs_get_u32(filter_handle, "total_water", &existing_water) != ESP_OK || existing_water == 0) {
                    esp_err_t se = nvs_set_u32(filter_handle, "total_water", total_water);
                    esp_err_t ce = nvs_commit(filter_handle);
                    if (se == ESP_OK && ce == ESP_OK) {
                        ESP_LOGI(TAG, "迁移总用水量: %lu升", total_water);
                        migrated = true;
                    } else {
                        ESP_LOGW(TAG, "迁移总用水量写入失败");
                    }
                }
                nvs_close(filter_handle);
            }
        }
        nvs_close(handle);

        // 删除旧wp_rt命名空间（v6.0: 打开后擦除全部）
        nvs_handle_t erase_handle;
        if (nvs_open("wp_rt", NVS_READWRITE, &erase_handle) == ESP_OK) {
            nvs_erase_all(erase_handle);
            nvs_commit(erase_handle);
            nvs_close(erase_handle);
            ESP_LOGI(TAG, "已删除旧wp_rt命名空间");
        }
    }

    // 4. 清理water_purifier中的废弃字段
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        // 检查废弃键是否存在（读取后不使用，仅检测存在性）
        uint32_t deprecated_val;
        bool has_deprecated = false;

        if (nvs_get_u32(handle, "flush_dur", &deprecated_val) == ESP_OK) {
            has_deprecated = true;
        }
        if (nvs_get_u32(handle, "filter_cap", &deprecated_val) == ESP_OK) {
            has_deprecated = true;
        }
        nvs_close(handle);

        if (has_deprecated) {
            nvs_handle_t wp_handle;
            if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &wp_handle) == ESP_OK) {
                esp_err_t e1 = nvs_erase_key(wp_handle, "flush_dur");
                esp_err_t e2 = nvs_erase_key(wp_handle, "filter_cap");
                esp_err_t ce = nvs_commit(wp_handle);
                nvs_close(wp_handle);
                if (ce == ESP_OK) {
                    ESP_LOGI(TAG, "已清理废弃字段(flush_dur, filter_cap)");
                    migrated = true;
                } else {
                    ESP_LOGW(TAG, "清理废弃字段提交失败");
                }
                (void)e1; (void)e2;  // erase_key may return NOT_FOUND, that's OK
            }
        }
    }

    if (migrated) {
        ESP_LOGI(TAG, "NVS迁移完成，旧数据已统一到新结构");
    }

    // 设置迁移完成标志（无论是否有迁移，都标记为已完成）
    nvs_handle_t wp_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &wp_handle) == ESP_OK) {
        esp_err_t se = nvs_set_u8(wp_handle, "nvs_migrated", 1);
        esp_err_t ce = nvs_commit(wp_handle);
        nvs_close(wp_handle);
        if (se != ESP_OK || ce != ESP_OK) {
            ESP_LOGW(TAG, "设置迁移完成标志失败");
        }
    } else {
        ESP_LOGW(TAG, "设置迁移完成标志: 打开命名空间失败");
    }
}

// ==================== 辅助函数 ====================

/**
 * @brief 比较两个配置是否相同（逐字段比较，避免padding问题）
 * @param a 配置A
 * @param b 配置B
 * @return true 相同
 */
static bool config_equal(const system_config_t *a, const system_config_t *b)
{
    if (!a || !b) return false;

    // WiFi配置
    if (strcmp(a->wifi_ssid, b->wifi_ssid) != 0) return false;
    if (strcmp(a->wifi_password, b->wifi_password) != 0) return false;

    // MQTT配置
    if (a->mqtt_enabled != b->mqtt_enabled) return false;
    if (strcmp(a->mqtt_broker, b->mqtt_broker) != 0) return false;
    if (strcmp(a->mqtt_username, b->mqtt_username) != 0) return false;
    if (strcmp(a->mqtt_password, b->mqtt_password) != 0) return false;
    if (strcmp(a->mqtt_topic_prefix, b->mqtt_topic_prefix) != 0) return false;

    // 硬件配置
    if (a->ro_membrane_type != b->ro_membrane_type) return false;
    if (a->pump_type != b->pump_type) return false;
    if (a->tank_size != b->tank_size) return false;
    if (a->waste_valve_flow_cc != b->waste_valve_flow_cc) return false;

    // 系统参数
    if (a->production_timeout_sec != b->production_timeout_sec) return false;
    if (a->leak_confirm_time_sec != b->leak_confirm_time_sec) return false;
    if (a->runtime_save_interval_min != b->runtime_save_interval_min) return false;

    // 冲洗参数
    if (a->normal_flush_duration_sec != b->normal_flush_duration_sec) return false;
    if (a->pure_flush_duration_sec != b->pure_flush_duration_sec) return false;
    if (a->filter_flush_duration_sec != b->filter_flush_duration_sec) return false;
    if (a->short_prod_threshold_sec != b->short_prod_threshold_sec) return false;
    if (a->water_hammer_valve_open_delay_ms != b->water_hammer_valve_open_delay_ms) return false;
    if (a->water_hammer_pump_stop_delay_ms != b->water_hammer_pump_stop_delay_ms) return false;
    if (a->water_hammer_valve_close_delay_ms != b->water_hammer_valve_close_delay_ms) return false;

    // 继电器配置
    if (a->relay_trigger_level != b->relay_trigger_level) return false;

    // TDS配置（浮点数使用epsilon比较）
    if (fabs(a->tds_inlet_threshold - b->tds_inlet_threshold) > 0.01f) return false;
    if (fabs(a->tds_outlet_threshold - b->tds_outlet_threshold) > 0.01f) return false;
    if (fabs(a->tds_calibration_offset[0] - b->tds_calibration_offset[0]) > 0.001f) return false;
    if (fabs(a->tds_calibration_offset[1] - b->tds_calibration_offset[1]) > 0.001f) return false;
    if (fabs(a->tds_calibration_scale[0] - b->tds_calibration_scale[0]) > 0.0001f) return false;
    if (fabs(a->tds_calibration_scale[1] - b->tds_calibration_scale[1]) > 0.0001f) return false;

    // Web配置
    if (a->web_port != b->web_port) return false;
    if (a->web_auth_enabled != b->web_auth_enabled) return false;
    if (strcmp(a->web_username, b->web_username) != 0) return false;
    if (strcmp(a->web_password, b->web_password) != 0) return false;

    return true;
}

// ==================== 默认配置 ====================

static const system_config_t default_config = {
    .wifi_ssid = "",
    .wifi_password = "",
    .mqtt_enabled = false,
    .mqtt_broker = "mqtt://homeassistant.local:1883",
    .mqtt_username = "",
    .mqtt_password = "",
    .mqtt_topic_prefix = "homeassistant/water_purifier",
    .ro_membrane_type = 1,               // 75G
    .pump_type = 1,                      // 三角洲75G
    .tank_size = 0,                      // 3G
    .waste_valve_flow_cc = 300,          // 300CC (18L/h)
    .production_timeout_sec = 3 * 3600,    // 3小时
    .leak_confirm_time_sec = 5,            // 5秒（漏水确认时间）
    .runtime_save_interval_min = 120,      // 2小时（默认保存间隔）

    // 冲洗参数
    .normal_flush_duration_sec = 20,       // 常规冲洗20秒
    .pure_flush_duration_sec = 15,         // 纯水洗膜15秒
    .filter_flush_duration_sec = 3600,     // 换芯冲洗3600秒（1小时）
    .short_prod_threshold_sec = 180,       // 短制水判断阈值3分钟
    .water_hammer_valve_open_delay_ms = 1000,  // 开阀延时1秒
    .water_hammer_pump_stop_delay_ms = 1000,   // 停泵延时1秒
    .water_hammer_valve_close_delay_ms = 500,  // 关阀延时0.5秒
    .relay_trigger_level = 0,              // 低电平触发
    .tds_inlet_threshold = 500.0f,         // 进水500ppm
    .tds_outlet_threshold = 100.0f,        // 出水100ppm
    .tds_calibration_offset = {0.0f, 0.0f},
    .tds_calibration_scale = {1.0f, 1.0f},
    .web_port = 80,
    .web_auth_enabled = false,
    .web_username = "admin",
    .web_password = "admin",
};

// ==================== 私有变量 ====================

static struct {
    bool initialized;
    bool config_dirty;
    system_config_t config;
    SemaphoreHandle_t mutex;  // 保护config和dirty标志的并发访问
} ctx = {
    .initialized = false,
    .config_dirty = false,
    .mutex = NULL,
};

// ==================== 初始化 ====================

esp_err_t config_manager_init(void)
{
    if (ctx.initialized) {
        return ESP_OK;
    }

    // 初始化NVS（遇到损坏时自动擦除恢复）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND ||
        ret == ESP_ERR_NVS_KEY_TOO_LONG) {
        ESP_LOGW(TAG, "NVS损坏，擦除恢复");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 执行NVS迁移（将旧命名空间数据统一到新结构）
    config_manager_migrate_nvs();

    // 创建互斥锁（保护config和dirty标志）
    ctx.mutex = xSemaphoreCreateMutex();
    if (!ctx.mutex) {
        ESP_LOGE(TAG, "创建互斥锁失败");
        return ESP_ERR_NO_MEM;
    }

    // 加载配置
    memcpy(&ctx.config, &default_config, sizeof(system_config_t));
    config_manager_load();

    ctx.initialized = true;
    return ESP_OK;
}

esp_err_t config_manager_deinit(void)
{
    if (!ctx.initialized) {
        return ESP_OK;
    }

    // 保存未写入的配置变更（避免数据丢失）
    if (ctx.config_dirty) {
        ESP_LOGI(TAG, "反初始化时保存未写入的配置");
        config_manager_save();
    }

    ctx.initialized = false;

    // 释放mutex资源
    if (ctx.mutex) {
        vSemaphoreDelete(ctx.mutex);
        ctx.mutex = NULL;
    }

    return ESP_OK;
}

// ==================== 加载和保存 ====================

esp_err_t config_manager_load(void)
{
    /* 注意：此函数主要在init中调用（单线程环境），此时mutex已创建但多任务未启动。
     * 如果从多任务环境调用，建议先调用save()确保数据一致性。 */

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);

    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS中无配置，使用默认值");
        return ESP_OK;  // 设计意图：返回OK表示"初始化成功（使用默认值）"，调用方无需区分有无旧配置
    }

    size_t len;

    // WiFi配置
    len = sizeof(ctx.config.wifi_ssid);
    err = nvs_get_str(handle, "wifi_ssid", ctx.config.wifi_ssid, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取wifi_ssid失败: %s", esp_err_to_name(err));
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ctx.config.wifi_ssid[0] = '\0';
    }
    ctx.config.wifi_ssid[sizeof(ctx.config.wifi_ssid) - 1] = '\0';

    len = sizeof(ctx.config.wifi_password);
    err = nvs_get_str(handle, "wifi_pass", ctx.config.wifi_password, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取wifi_password失败: %s", esp_err_to_name(err));
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ctx.config.wifi_password[0] = '\0';
    }
    ctx.config.wifi_password[sizeof(ctx.config.wifi_password) - 1] = '\0';

    // MQTT配置
    uint8_t u8_val;
    if (nvs_get_u8(handle, "mqtt_en", &u8_val) == ESP_OK) {
        ctx.config.mqtt_enabled = u8_val;
    }

    // RO膜和硬件配置
    if (nvs_get_u8(handle, "ro_mem", &u8_val) == ESP_OK) {
        ctx.config.ro_membrane_type = u8_val;
    }
    if (nvs_get_u8(handle, "pump", &u8_val) == ESP_OK) {
        ctx.config.pump_type = u8_val;
    }
    if (nvs_get_u8(handle, "tank", &u8_val) == ESP_OK) {
        ctx.config.tank_size = u8_val;
    }
    {
        uint16_t wv_val;
        if (nvs_get_u16(handle, "wv_flow", &wv_val) == ESP_OK) {
            ctx.config.waste_valve_flow_cc = wv_val;
        }
    }

    char str_buf[128];

    len = sizeof(str_buf);
    if (nvs_get_str(handle, "mqtt_broker", str_buf, &len) == ESP_OK) {
        strlcpy(ctx.config.mqtt_broker, str_buf, sizeof(ctx.config.mqtt_broker));
    }
    len = sizeof(str_buf);
    if (nvs_get_str(handle, "mqtt_user", str_buf, &len) == ESP_OK) {
        strlcpy(ctx.config.mqtt_username, str_buf, sizeof(ctx.config.mqtt_username));
    }
    len = sizeof(str_buf);
    if (nvs_get_str(handle, "mqtt_pass", str_buf, &len) == ESP_OK) {
        strlcpy(ctx.config.mqtt_password, str_buf, sizeof(ctx.config.mqtt_password));
    }
    len = sizeof(str_buf);
    if (nvs_get_str(handle, "mqtt_topic", str_buf, &len) == ESP_OK) {
        strlcpy(ctx.config.mqtt_topic_prefix, str_buf, sizeof(ctx.config.mqtt_topic_prefix));
    }

    // 系统参数
    err = nvs_get_u32(handle, "prod_timeout", &ctx.config.production_timeout_sec);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取prod_timeout失败: %s", esp_err_to_name(err));
    }
    err = nvs_get_u32(handle, "leak_confirm", &ctx.config.leak_confirm_time_sec);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取leak_confirm失败: %s", esp_err_to_name(err));
    }

    // 冲洗参数（添加错误检查）
    err = nvs_get_u32(handle, "nflush_dur", &ctx.config.normal_flush_duration_sec);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取nflush_dur失败: %s", esp_err_to_name(err));
    }
    err = nvs_get_u32(handle, "pflush_dur", &ctx.config.pure_flush_duration_sec);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取pflush_dur失败: %s", esp_err_to_name(err));
    }
    err = nvs_get_u32(handle, "fflush_dur", &ctx.config.filter_flush_duration_sec);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取fflush_dur失败: %s", esp_err_to_name(err));
    }
    err = nvs_get_u32(handle, "short_prod", &ctx.config.short_prod_threshold_sec);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取short_prod失败: %s", esp_err_to_name(err));
    }
    err = nvs_get_u32(handle, "wh_vopn", &ctx.config.water_hammer_valve_open_delay_ms);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取wh_vopn失败: %s", esp_err_to_name(err));
    }
    err = nvs_get_u32(handle, "wh_pstp", &ctx.config.water_hammer_pump_stop_delay_ms);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取wh_pstp失败: %s", esp_err_to_name(err));
    }
    err = nvs_get_u32(handle, "wh_vcls", &ctx.config.water_hammer_valve_close_delay_ms);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取wh_vcls失败: %s", esp_err_to_name(err));
    }

    // 继电器配置
    if (nvs_get_u8(handle, "relay_lvl", &u8_val) == ESP_OK) {
        ctx.config.relay_trigger_level = u8_val;
    }

    // 运行数据保存间隔
    uint16_t u16_val2;
    if (nvs_get_u16(handle, "save_intv", &u16_val2) == ESP_OK) {
        ctx.config.runtime_save_interval_min = u16_val2;
    }

    // TDS配置（优先读取定点数格式，向后兼容旧的整数格式）
    int32_t i32_val;
    if (nvs_get_i32(handle, "tds_in_th", &i32_val) == ESP_OK) {
        ctx.config.tds_inlet_threshold = i32_val / 100.0f;
    } else {
        // 兼容旧版本：u32格式（整数）
        uint32_t u32_val;
        if (nvs_get_u32(handle, "tds_in_th", &u32_val) == ESP_OK) {
            ctx.config.tds_inlet_threshold = (float)u32_val;
        }
    }
    if (nvs_get_i32(handle, "tds_out_th", &i32_val) == ESP_OK) {
        ctx.config.tds_outlet_threshold = i32_val / 100.0f;
    } else {
        // 兼容旧版本：u32格式（整数）
        uint32_t u32_val;
        if (nvs_get_u32(handle, "tds_out_th", &u32_val) == ESP_OK) {
            ctx.config.tds_outlet_threshold = (float)u32_val;
        }
    }

    // TDS校准（定点数存储：offset乘100，scale乘10000）
    if (nvs_get_i32(handle, "tds_in_off", &i32_val) == ESP_OK) {
        ctx.config.tds_calibration_offset[0] = i32_val / 100.0f;
    }
    if (nvs_get_i32(handle, "tds_out_off", &i32_val) == ESP_OK) {
        ctx.config.tds_calibration_offset[1] = i32_val / 100.0f;
    }
    if (nvs_get_i32(handle, "tds_in_scale", &i32_val) == ESP_OK) {
        ctx.config.tds_calibration_scale[0] = i32_val / 10000.0f;
    }
    if (nvs_get_i32(handle, "tds_out_scale", &i32_val) == ESP_OK) {
        ctx.config.tds_calibration_scale[1] = i32_val / 10000.0f;
    }

    // Web配置（添加错误检查）
    err = nvs_get_u16(handle, "web_port", &u16_val2);
    if (err == ESP_OK) {
        ctx.config.web_port = u16_val2;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取web_port失败: %s", esp_err_to_name(err));
    }
    err = nvs_get_u8(handle, "web_auth", &u8_val);
    if (err == ESP_OK) {
        ctx.config.web_auth_enabled = u8_val;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取web_auth失败: %s", esp_err_to_name(err));
    }
    len = sizeof(ctx.config.web_username);
    err = nvs_get_str(handle, "web_user", ctx.config.web_username, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取web_user失败: %s", esp_err_to_name(err));
    }
    ctx.config.web_username[sizeof(ctx.config.web_username) - 1] = '\0';
    len = sizeof(ctx.config.web_password);
    err = nvs_get_str(handle, "web_pass", ctx.config.web_password, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取web_pass失败: %s", esp_err_to_name(err));
    }
    ctx.config.web_password[sizeof(ctx.config.web_password) - 1] = '\0';

    // FSM运行统计
    err = nvs_get_u32(handle, "fsm_prod_cyc", &ctx.config.fsm_prod_cycles);
    if (err == ESP_ERR_NVS_NOT_FOUND) ctx.config.fsm_prod_cycles = 0;
    err = nvs_get_u32(handle, "fsm_flush_cyc", &ctx.config.fsm_flush_cycles);
    if (err == ESP_ERR_NVS_NOT_FOUND) ctx.config.fsm_flush_cycles = 0;
    err = nvs_get_u64(handle, "fsm_prod_time", &ctx.config.fsm_prod_time_sec);
    if (err == ESP_ERR_NVS_NOT_FOUND) ctx.config.fsm_prod_time_sec = 0;
    err = nvs_get_u64(handle, "fsm_flush_time", &ctx.config.fsm_flush_time_sec);
    if (err == ESP_ERR_NVS_NOT_FOUND) ctx.config.fsm_flush_time_sec = 0;

    nvs_close(handle);

    // 使用mutex保护dirty标志清除
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        ctx.config_dirty = false;
        xSemaphoreGive(ctx.mutex);
    } else {
        ctx.config_dirty = false;  // 降级处理
    }

    return ESP_OK;
}

esp_err_t config_manager_save(void)
{
    // 使用mutex保护dirty标志检查
    bool is_dirty = false;
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        is_dirty = ctx.config_dirty;
        xSemaphoreGive(ctx.mutex);
    } else {
        /* mutex获取失败，拒绝操作以避免数据不一致 */
        ESP_LOGE(TAG, "save: mutex获取超时(检查dirty)，拒绝操作");
        return ESP_ERR_TIMEOUT;
    }

    if (!is_dirty) {
        return ESP_OK;  // 无变化，跳过保存
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开NVS失败: %s", esp_err_to_name(err));
        return err;
    }

    // 使用mutex保护配置读取（复制一份用于写入）
    system_config_t config_copy;
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(&config_copy, &ctx.config, sizeof(system_config_t));
        xSemaphoreGive(ctx.mutex);
    } else {
        /* mutex获取失败，拒绝保存以避免写入不一致数据 */
        ESP_LOGE(TAG, "save: mutex获取超时，拒绝保存避免数据不一致");
        nvs_close(handle);
        return ESP_ERR_TIMEOUT;
    }

    // WiFi配置
    nvs_set_str(handle, "wifi_ssid", config_copy.wifi_ssid);
    nvs_set_str(handle, "wifi_pass", config_copy.wifi_password);

    // MQTT配置
    nvs_set_u8(handle, "mqtt_en", config_copy.mqtt_enabled);
    nvs_set_u8(handle, "ro_mem", config_copy.ro_membrane_type);
    nvs_set_u8(handle, "pump", config_copy.pump_type);
    nvs_set_u8(handle, "tank", config_copy.tank_size);
    nvs_set_u16(handle, "wv_flow", config_copy.waste_valve_flow_cc);
    nvs_set_str(handle, "mqtt_broker", config_copy.mqtt_broker);
    nvs_set_str(handle, "mqtt_user", config_copy.mqtt_username);
    nvs_set_str(handle, "mqtt_pass", config_copy.mqtt_password);
    nvs_set_str(handle, "mqtt_topic", config_copy.mqtt_topic_prefix);

    // 系统参数
    nvs_set_u32(handle, "prod_timeout", config_copy.production_timeout_sec);
    nvs_set_u32(handle, "leak_confirm", config_copy.leak_confirm_time_sec);

    // 冲洗参数
    nvs_set_u32(handle, "nflush_dur", config_copy.normal_flush_duration_sec);
    nvs_set_u32(handle, "pflush_dur", config_copy.pure_flush_duration_sec);
    nvs_set_u32(handle, "fflush_dur", config_copy.filter_flush_duration_sec);
    nvs_set_u32(handle, "short_prod", config_copy.short_prod_threshold_sec);
    nvs_set_u32(handle, "wh_vopn", config_copy.water_hammer_valve_open_delay_ms);
    nvs_set_u32(handle, "wh_pstp", config_copy.water_hammer_pump_stop_delay_ms);
    nvs_set_u32(handle, "wh_vcls", config_copy.water_hammer_valve_close_delay_ms);

    // 继电器配置
    nvs_set_u8(handle, "relay_lvl", config_copy.relay_trigger_level);

    // 运行数据保存间隔
    nvs_set_u16(handle, "save_intv", config_copy.runtime_save_interval_min);

    // TDS配置（定点数存储：乘100保留2位小数精度）
    // 钳位防止float溢出int32_t范围（阈值0~2000ppm * 100 = 0~200000，安全）
    nvs_set_i32(handle, "tds_in_th", (int32_t)(config_copy.tds_inlet_threshold * 100.0f));
    nvs_set_i32(handle, "tds_out_th", (int32_t)(config_copy.tds_outlet_threshold * 100.0f));

    // TDS校准（定点数存储：offset乘100，scale乘10000）
    // 钳位到安全范围：offset [-1000,1000] → [-100000,100000]，scale [0.01,100] → [100,1000000]
    float off0 = config_copy.tds_calibration_offset[0];
    float off1 = config_copy.tds_calibration_offset[1];
    float sc0 = config_copy.tds_calibration_scale[0];
    float sc1 = config_copy.tds_calibration_scale[1];
    if (off0 < -1000.0f) off0 = -1000.0f;
    if (off0 > 1000.0f) off0 = 1000.0f;
    if (off1 < -1000.0f) off1 = -1000.0f;
    if (off1 > 1000.0f) off1 = 1000.0f;
    if (sc0 < 0.01f) sc0 = 0.01f;
    if (sc0 > 100.0f) sc0 = 100.0f;
    if (sc1 < 0.01f) sc1 = 0.01f;
    if (sc1 > 100.0f) sc1 = 100.0f;
    nvs_set_i32(handle, "tds_in_off", (int32_t)(off0 * 100.0f));
    nvs_set_i32(handle, "tds_out_off", (int32_t)(off1 * 100.0f));
    nvs_set_i32(handle, "tds_in_scale", (int32_t)(sc0 * 10000.0f));
    nvs_set_i32(handle, "tds_out_scale", (int32_t)(sc1 * 10000.0f));

    // Web配置
    nvs_set_u16(handle, "web_port", config_copy.web_port);
    nvs_set_u8(handle, "web_auth", config_copy.web_auth_enabled);
    nvs_set_str(handle, "web_user", config_copy.web_username);
    nvs_set_str(handle, "web_pass", config_copy.web_password);

    // FSM运行统计
    nvs_set_u32(handle, "fsm_prod_cyc", config_copy.fsm_prod_cycles);
    nvs_set_u32(handle, "fsm_flush_cyc", config_copy.fsm_flush_cycles);
    nvs_set_u64(handle, "fsm_prod_time", config_copy.fsm_prod_time_sec);
    nvs_set_u64(handle, "fsm_flush_time", config_copy.fsm_flush_time_sec);

    err = nvs_commit(handle);
    nvs_close(handle);

    // 使用mutex保护脏标志清除（确保一致性）
    if (err == ESP_OK) {
        if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            ctx.config_dirty = false;
            xSemaphoreGive(ctx.mutex);
            ESP_LOGI(TAG, "配置已保存");
        } else {
            /* mutex获取失败，保持dirty标志为true，下次保存时会再次尝试
             * 这样避免了竞态：如果另一个任务正在设置dirty，强制清除会丢失更新 */
            ESP_LOGW(TAG, "保存成功但mutex获取失败(清除dirty)，保持脏标志以便下次保存");
            // 不清除dirty标志，下次save会重新保存
        }
    }
    return err;
}

esp_err_t config_manager_factory_reset(void)
{
    ESP_LOGW(TAG, "恢复出厂配置");

    // 使用mutex保护配置重置
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(&ctx.config, &default_config, sizeof(system_config_t));
        ctx.config_dirty = true;  // 标记为脏，以便下次保存
        xSemaphoreGive(ctx.mutex);
    } else {
        // 降级处理
        memcpy(&ctx.config, &default_config, sizeof(system_config_t));
        ctx.config_dirty = true;
        ESP_LOGW(TAG, "factory_reset: mutex获取失败");
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_all(handle);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    // 使用mutex保护脏标志更新
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (err == ESP_OK) {
            ctx.config_dirty = false;  // NVS已擦除，清除脏标志
            ESP_LOGI(TAG, "出厂配置恢复成功");
        } else {
            ESP_LOGE(TAG, "出厂重置失败: %s，内存配置已更新但NVS未擦除", esp_err_to_name(err));
            // 保持dirty标志为true，因为内存与NVS不一致
        }
        xSemaphoreGive(ctx.mutex);
    } else {
        if (err == ESP_OK) {
            ctx.config_dirty = false;
        }
    }
    return err;  // 返回实际错误状态
}

// ==================== 配置获取和设置 ====================

esp_err_t config_manager_get_config(system_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    // 使用mutex保护配置读取
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(config, &ctx.config, sizeof(system_config_t));
        xSemaphoreGive(ctx.mutex);
        return ESP_OK;
    }

    // 互斥锁获取失败时降级处理（仍返回数据但可能不一致）
    memcpy(config, &ctx.config, sizeof(system_config_t));
    ESP_LOGW(TAG, "get_config: mutex获取失败，返回潜在不一致数据");
    return ESP_OK;
}

esp_err_t config_manager_set_config(const system_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    // 验证配置值的有效性
    if (!config_manager_validate(config)) {
        ESP_LOGW(TAG, "set_config: 配置值无效，拒绝更新");
        return ESP_ERR_INVALID_ARG;
    }

    // 使用mutex保护配置更新
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        // 使用逐字段比较，避免padding问题
        if (!config_equal(&ctx.config, config)) {
            memcpy(&ctx.config, config, sizeof(system_config_t));
            ctx.config_dirty = true;
        }
        xSemaphoreGive(ctx.mutex);
        return ESP_OK;
    }

    // 互斥锁获取失败时降级处理
    if (!config_equal(&ctx.config, config)) {
        memcpy(&ctx.config, config, sizeof(system_config_t));
        ctx.config_dirty = true;
    }
    ESP_LOGW(TAG, "set_config: mutex获取失败，配置已更新但可能有竞态");
    return ESP_OK;
}

esp_err_t config_manager_get_string(const char *key, char *value, size_t value_size)
{
    if (!key || !value || value_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = value_size;
    err = nvs_get_str(handle, key, value, &len);
    nvs_close(handle);
    return err;
}

/**
 * @brief 根据key更新内存中的配置字段
 */
static void update_config_from_key(const char *key, const char *value, bool *changed)
{
    *changed = false;
#define UPDATE_STR_FIELD(fld) do { \
    if (strcmp(key, #fld) == 0 && strcmp(ctx.config.fld, value) != 0) { \
        strlcpy(ctx.config.fld, value, sizeof(ctx.config.fld)); \
        *changed = true; \
    } \
} while (0)

    UPDATE_STR_FIELD(wifi_ssid);
    UPDATE_STR_FIELD(wifi_password);
    UPDATE_STR_FIELD(mqtt_broker);
    UPDATE_STR_FIELD(mqtt_username);
    UPDATE_STR_FIELD(mqtt_password);
    UPDATE_STR_FIELD(mqtt_topic_prefix);
    UPDATE_STR_FIELD(web_username);
    UPDATE_STR_FIELD(web_password);

#undef UPDATE_STR_FIELD
}

esp_err_t config_manager_set_string(const char *key, const char *value)
{
    if (!key || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    // 输入长度校验：NVS单条目限制约4000字节，配置字符串通常不超过128字节
    size_t value_len = strlen(value);
    if (value_len > 256) {
        ESP_LOGW(TAG, "set_string(%s): 值过长(%zu字节)，拒绝写入", key, value_len);
        return ESP_ERR_INVALID_ARG;
    }

    // 一次READWRITE打开完成读写
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    // 检查NVS中现有值，避免不必要的写入
    size_t cur_len = 0;
    err = nvs_get_str(handle, key, NULL, &cur_len);
    bool value_changed = true;
    if (err == ESP_OK && cur_len > 0 && cur_len <= 512) {
        // cur_len上限校验：防止NVS中损坏的长度值导致大量内存分配
        char *cur_val = malloc(cur_len);
        if (cur_val) {
            err = nvs_get_str(handle, key, cur_val, &cur_len);
            if (err == ESP_OK && strcmp(cur_val, value) == 0) {
                value_changed = false;
            }
            free(cur_val);
        }
    }

    if (!value_changed) {
        nvs_close(handle);
        return ESP_OK;
    }

    // 先写入NVS（确保持久化成功后再更新内存）
    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_string: NVS写入失败(%s)，内存未更新", esp_err_to_name(err));
        return err;
    }

    // NVS写入成功，更新内存配置
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        bool mem_changed = false;
        update_config_from_key(key, value, &mem_changed);
        if (mem_changed) {
            ctx.config_dirty = false;  // NVS已是最新，清除脏标志
        }
        xSemaphoreGive(ctx.mutex);
    } else {
        ESP_LOGW(TAG, "set_string: NVS已写入但mutex获取失败，内存可能不一致");
    }

    return ESP_OK;
}

esp_err_t config_manager_get_int(const char *key, int *value)
{
    if (!key || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    int32_t val;
    err = nvs_get_i32(handle, key, &val);
    if (err == ESP_OK) {
        *value = val;
    }
    nvs_close(handle);
    return err;
}

/**
 * @brief 根据key更新内存中的int配置字段
 */
static void update_config_int_from_key(const char *key, int value, bool *changed)
{
    *changed = false;
#define UPDATE_U8_FIELD(fld) do { \
    if (strcmp(key, #fld) == 0 && ctx.config.fld != (uint8_t)value) { \
        ctx.config.fld = (uint8_t)value; \
        *changed = true; \
    } \
} while (0)
#define UPDATE_U32_FIELD(fld) do { \
    if (strcmp(key, #fld) == 0 && ctx.config.fld != (uint32_t)value) { \
        ctx.config.fld = (uint32_t)value; \
        *changed = true; \
    } \
} while (0)
#define UPDATE_U16_FIELD(fld) do { \
    if (strcmp(key, #fld) == 0 && ctx.config.fld != (uint16_t)value) { \
        ctx.config.fld = (uint16_t)value; \
        *changed = true; \
    } \
} while (0)

    UPDATE_U8_FIELD(ro_membrane_type);
    UPDATE_U8_FIELD(pump_type);
    UPDATE_U8_FIELD(tank_size);
    UPDATE_U16_FIELD(waste_valve_flow_cc);
    UPDATE_U32_FIELD(production_timeout_sec);
    UPDATE_U32_FIELD(leak_confirm_time_sec);
    UPDATE_U32_FIELD(normal_flush_duration_sec);
    UPDATE_U32_FIELD(pure_flush_duration_sec);
    UPDATE_U32_FIELD(filter_flush_duration_sec);
    UPDATE_U32_FIELD(short_prod_threshold_sec);
    UPDATE_U32_FIELD(water_hammer_valve_open_delay_ms);
    UPDATE_U32_FIELD(water_hammer_pump_stop_delay_ms);
    UPDATE_U32_FIELD(water_hammer_valve_close_delay_ms);
    UPDATE_U8_FIELD(relay_trigger_level);
    UPDATE_U16_FIELD(runtime_save_interval_min);
    UPDATE_U16_FIELD(web_port);

#undef UPDATE_U8_FIELD
#undef UPDATE_U32_FIELD
#undef UPDATE_U16_FIELD
}

esp_err_t config_manager_set_int(const char *key, int value)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }

    // 单字段范围校验（与config_manager_validate保持一致）
    if (value < 0) {
        // 除web_port等字段外，大部分配置不应为负数
        // 允许负数但在UPDATE_U8/U16/U32_FIELD中会被截断，此处先放行由NVS存储
    }
    // 关键字段范围检查
    if ((strcmp(key, "production_timeout_sec") == 0 && (value < 600 || value > 86400)) ||
        (strcmp(key, "leak_confirm_time_sec") == 0 && (value < 1 || value > 60)) ||
        (strcmp(key, "normal_flush_duration_sec") == 0 && (value < 5 || value > 300)) ||
        (strcmp(key, "pure_flush_duration_sec") == 0 && (value < 5 || value > 300)) ||
        (strcmp(key, "filter_flush_duration_sec") == 0 && (value < 60 || value > 7200)) ||
        (strcmp(key, "short_prod_threshold_sec") == 0 && (value < 30 || value > 600)) ||
        (strcmp(key, "water_hammer_valve_open_delay_ms") == 0 && (value < 100 || value > 5000)) ||
        (strcmp(key, "water_hammer_pump_stop_delay_ms") == 0 && (value < 100 || value > 5000)) ||
        (strcmp(key, "water_hammer_valve_close_delay_ms") == 0 && (value < 100 || value > 5000)) ||
        (strcmp(key, "waste_valve_flow_cc") == 0 && (value < 100 || value > 1000)) ||
        (strcmp(key, "ro_membrane_type") == 0 && (value < 0 || value > 4)) ||
        (strcmp(key, "pump_type") == 0 && (value < 0 || value > 5)) ||
        (strcmp(key, "tank_size") == 0 && (value < 0 || value > 4)) ||
        (strcmp(key, "relay_trigger_level") == 0 && (value < 0 || value > 1))) {
        ESP_LOGW(TAG, "set_int(%s): 值%d超出有效范围，拒绝写入", key, value);
        return ESP_ERR_INVALID_ARG;
    }

    // 先写入NVS（确保持久化成功后再更新内存）
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    // 检查现有值，避免不必要的写入
    int32_t cur_val;
    if (nvs_get_i32(handle, key, &cur_val) == ESP_OK && cur_val == value) {
        nvs_close(handle);
        return ESP_OK;  // 值未变化，不写入
    }

    err = nvs_set_i32(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_int: NVS写入失败(%s)，内存未更新", esp_err_to_name(err));
        return err;
    }

    // NVS写入成功，更新内存配置
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        bool mem_changed = false;
        update_config_int_from_key(key, value, &mem_changed);
        if (mem_changed) {
            ctx.config_dirty = false;  // NVS已是最新
        }
        xSemaphoreGive(ctx.mutex);
    } else {
        ESP_LOGW(TAG, "set_int: NVS已写入但mutex获取失败，内存可能不一致");
    }

    return ESP_OK;
}

esp_err_t config_manager_get_bool(const char *key, bool *value)
{
    if (!key || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t val;
    err = nvs_get_u8(handle, key, &val);
    if (err == ESP_OK) {
        *value = val;
    }
    nvs_close(handle);
    return err;
}

/**
 * @brief 根据key更新内存中的bool配置字段
 */
static void update_config_bool_from_key(const char *key, bool value, bool *changed)
{
    *changed = false;
#define UPDATE_BOOL_FIELD(fld) do { \
    if (strcmp(key, #fld) == 0 && ctx.config.fld != value) { \
        ctx.config.fld = value; \
        *changed = true; \
    } \
} while (0)

    UPDATE_BOOL_FIELD(mqtt_enabled);
    UPDATE_BOOL_FIELD(web_auth_enabled);

#undef UPDATE_BOOL_FIELD
}

esp_err_t config_manager_set_bool(const char *key, bool value)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }

    // 先检查NVS中现有值，避免不必要的写入
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        uint8_t cur_val;
        if (nvs_get_u8(handle, key, &cur_val) == ESP_OK && cur_val == (uint8_t)value) {
            nvs_close(handle);
            return ESP_OK;  // 值未变化，不写入
        }
        nvs_close(handle);
    }

    // 先写入NVS（确保持久化成功后再更新内存）
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, key, (uint8_t)value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_bool: NVS写入失败(%s)，内存未更新", esp_err_to_name(err));
        return err;
    }

    // NVS写入成功，更新内存配置
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        bool mem_changed = false;
        update_config_bool_from_key(key, value, &mem_changed);
        if (mem_changed) {
            ctx.config_dirty = false;  // NVS已是最新
        }
        xSemaphoreGive(ctx.mutex);
    } else {
        ESP_LOGW(TAG, "set_bool: NVS已写入但mutex获取失败，内存可能不一致");
    }

    return ESP_OK;
}

// ==================== 验证 ====================

bool config_manager_validate(const system_config_t *config)
{
    if (!config) {
        return false;
    }

    // 验证RO膜类型 (0-4)
    if (config->ro_membrane_type > 4) {
        return false;
    }
    // 验证泵类型 (0-5)
    if (config->pump_type > 5) {
        return false;
    }
    // 验证压力桶大小 (0-4)
    if (config->tank_size > 4) {
        return false;
    }
    // 验证废水阀流量 (100-1000 CC)
    if (config->waste_valve_flow_cc < 100 || config->waste_valve_flow_cc > 1000) {
        return false;
    }

    // 验证时间参数
    if (config->production_timeout_sec < 600 || config->production_timeout_sec > 86400) {
        return false;
    }
    if (config->leak_confirm_time_sec < 1 || config->leak_confirm_time_sec > 60) {
        return false;
    }

    // 验证冲洗参数
    if (config->normal_flush_duration_sec < 5 || config->normal_flush_duration_sec > 300) {
        return false;
    }
    if (config->pure_flush_duration_sec < 5 || config->pure_flush_duration_sec > 300) {
        return false;
    }
    if (config->filter_flush_duration_sec < 60 || config->filter_flush_duration_sec > 7200) {
        return false;
    }
    if (config->short_prod_threshold_sec < 30 || config->short_prod_threshold_sec > 600) {
        return false;
    }

    // 验证水锤延时
    if (config->water_hammer_valve_open_delay_ms < 100 || config->water_hammer_valve_open_delay_ms > 5000) {
        return false;
    }
    if (config->water_hammer_pump_stop_delay_ms < 100 || config->water_hammer_pump_stop_delay_ms > 5000) {
        return false;
    }
    if (config->water_hammer_valve_close_delay_ms < 100 || config->water_hammer_valve_close_delay_ms > 5000) {
        return false;
    }

    // 验证继电器电平
    if (config->relay_trigger_level > 1) {
        return false;
    }

    // 验证TDS阈值
    if (config->tds_inlet_threshold < 0 || config->tds_inlet_threshold > 2000) {
        return false;
    }
    if (config->tds_outlet_threshold < 0 || config->tds_outlet_threshold > 500) {
        return false;
    }

    // 验证Web端口 (uint16_t范围0-65535，有效端口1-65535)
    if (config->web_port == 0) {
        return false;
    }

    // 验证运行数据保存间隔 (10分钟~24小时)
    if (config->runtime_save_interval_min < 10 || config->runtime_save_interval_min > 1440) {
        return false;
    }

    return true;
}

bool config_manager_is_dirty(void)
{
    bool is_dirty = false;
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        is_dirty = ctx.config_dirty;
        xSemaphoreGive(ctx.mutex);
    } else {
        // 降级处理（返回值可能不准确但不会崩溃）
        is_dirty = ctx.config_dirty;
    }
    return is_dirty;
}

bool config_manager_has_wifi_config(void)
{
    bool has_config = false;
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        has_config = (strlen(ctx.config.wifi_ssid) > 0);
        xSemaphoreGive(ctx.mutex);
    } else {
        // mutex获取失败时返回false（保守估计，避免无锁访问共享数据）
        // 调用者应该在连接前再次确认配置状态
        ESP_LOGW(TAG, "has_wifi_config: mutex获取失败，返回false");
    }
    return has_config;
}

bool config_manager_has_mqtt_config(void)
{
    bool has_config = false;
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        has_config = (strlen(ctx.config.mqtt_broker) > 0);
        xSemaphoreGive(ctx.mutex);
    } else {
        // mutex获取失败时返回false（保守估计，避免无锁访问共享数据）
        ESP_LOGW(TAG, "has_mqtt_config: mutex获取失败，返回false");
    }
    return has_config;
}

// ==================== 统一周期保存 ====================

#include "filter_manager.h"
#include "history_logger.h"

// 上次统一保存时间
static uint64_t s_last_unified_save_time = 0;

/**
 * @brief 统一周期保存接口（批量写入优化）
 * @note 协调所有模块的脏数据保存，减少NVS commit次数
 * @param min_interval_sec 最小保存间隔（秒），0表示立即保存
 * @return true 执行了保存
 */
bool config_manager_periodic_save_all(uint32_t min_interval_sec)
{
    uint64_t now_us = esp_timer_get_time();
    uint64_t elapsed_sec = (now_us - s_last_unified_save_time) / 1000000ULL;

    // 检查是否需要保存（间隔或立即）
    bool should_save = (min_interval_sec == 0) || (elapsed_sec >= min_interval_sec);
    if (!should_save) {
        return false;
    }

    bool did_save = false;

    // 1. 保存系统配置（如有变化）
    if (ctx.config_dirty) {
        config_manager_save();
        did_save = true;
    }

    // 2. 保存滤芯数据（如有变化）
    if (filter_mgr_periodic_save()) {
        did_save = true;
    }

    // 3. 保存历史记录和每日统计（如有变化）
    if (history_periodic_save(min_interval_sec)) {
        did_save = true;
    }

    if (did_save) {
        s_last_unified_save_time = now_us;
        ESP_LOGD(TAG, "统一保存完成（间隔%lu秒）", min_interval_sec);
    }

    return did_save;
}

// ==================== 调试 ====================

void config_manager_print_config(void)
{
    const char *ro_names[] = {"50G", "75G", "100G", "200G", "400G"};
    const char *pump_names[] = {"△50G", "△75G", "△100G", "△200G", "△300G", "△400G"};
    const char *tank_names[] = {"3G", "3.2G", "4G", "6G", "10G"};

    ESP_LOGI(TAG, "===== 系统配置 =====");
    ESP_LOGI(TAG, "WiFi: %s", ctx.config.wifi_ssid);
    ESP_LOGI(TAG, "MQTT: %s (%s)", ctx.config.mqtt_broker,
             ctx.config.mqtt_enabled ? "启用" : "禁用");
    ESP_LOGI(TAG, "RO膜: %s, 泵: %s, 桶: %s",
             ro_names[ctx.config.ro_membrane_type],
             pump_names[ctx.config.pump_type],
             tank_names[ctx.config.tank_size]);
    ESP_LOGI(TAG, "冲洗: 常规%lu秒/纯水%lu秒/换芯%lu秒",
             ctx.config.normal_flush_duration_sec,
             ctx.config.pure_flush_duration_sec,
             ctx.config.filter_flush_duration_sec);
    ESP_LOGI(TAG, "制水超时: %lu秒", ctx.config.production_timeout_sec);
    ESP_LOGI(TAG, "漏水确认: %lu秒", ctx.config.leak_confirm_time_sec);
    ESP_LOGI(TAG, "继电平: %s", ctx.config.relay_trigger_level ? "高" : "低");
    ESP_LOGI(TAG, "TDS阈值: 进水%.0f/出水%.0f ppm",
             ctx.config.tds_inlet_threshold, ctx.config.tds_outlet_threshold);
}

// ==================== FSM运行统计同步 ====================

void config_manager_sync_fsm_stats(uint32_t prod_cycles, uint32_t flush_cycles,
                                   uint64_t prod_time_sec, uint64_t flush_time_sec)
{
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // 仅在数据变化时更新并设置脏标志（避免无意义写入）
        if (ctx.config.fsm_prod_cycles != prod_cycles ||
            ctx.config.fsm_flush_cycles != flush_cycles ||
            ctx.config.fsm_prod_time_sec != prod_time_sec ||
            ctx.config.fsm_flush_time_sec != flush_time_sec) {
            ctx.config.fsm_prod_cycles = prod_cycles;
            ctx.config.fsm_flush_cycles = flush_cycles;
            ctx.config.fsm_prod_time_sec = prod_time_sec;
            ctx.config.fsm_flush_time_sec = flush_time_sec;
            ctx.config_dirty = true;
            ESP_LOGD(TAG, "FSM统计已更新: 制水%lu周期, 冲洗%lu周期, 制水%llu秒",
                     prod_cycles, flush_cycles, prod_time_sec);
        }
        xSemaphoreGive(ctx.mutex);
    } else {
        ESP_LOGW(TAG, "sync_fsm_stats: mutex获取超时");
    }
}

void config_manager_get_fsm_stats(uint32_t *prod_cycles, uint32_t *flush_cycles,
                                  uint64_t *prod_time_sec, uint64_t *flush_time_sec)
{
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *prod_cycles = ctx.config.fsm_prod_cycles;
        *flush_cycles = ctx.config.fsm_flush_cycles;
        *prod_time_sec = ctx.config.fsm_prod_time_sec;
        *flush_time_sec = ctx.config.fsm_flush_time_sec;
        xSemaphoreGive(ctx.mutex);
    } else {
        ESP_LOGW(TAG, "get_fsm_stats: mutex获取超时，返回默认值");
        *prod_cycles = 0;
        *flush_cycles = 0;
        *prod_time_sec = 0;
        *flush_time_sec = 0;
    }
}