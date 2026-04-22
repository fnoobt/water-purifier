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

static const char *TAG = "CONFIG";

// ==================== NVS命名空间 ====================

#define NVS_NAMESPACE "water_purifier"
#define NVS_RUNTIME_NAMESPACE "wp_rt"

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
    .flush_duration_sec = 30,              // 30秒（默认冲洗时间）
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
    .filter_capacity_liters = 3000,        // 3000升
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

    ESP_LOGI(TAG, "初始化配置管理器...");

    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "擦除NVS并重新初始化");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 创建互斥锁（保护config和dirty标志）
    ctx.mutex = xSemaphoreCreateMutex();
    if (!ctx.mutex) {
        ESP_LOGE(TAG, "创建互斥锁失败");
        return ESP_ERR_NO_MEM;
    }

    // 检测NVS中是否存在超长key，仅在确认超长key时才擦除
    {
        nvs_handle_t rt_handle;
        esp_err_t rt_err = nvs_open(NVS_RUNTIME_NAMESPACE, NVS_READONLY, &rt_handle);
        if (rt_err == ESP_ERR_NVS_KEY_TOO_LONG) {
            ESP_LOGW(TAG, "NVS中存在超长key，擦除并恢复");
            nvs_flash_erase();
            ret = nvs_flash_init();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "NVS恢复失败: %s", esp_err_to_name(ret));
                return ret;
            }
        } else {
            // NOT_FOUND（namespace不存在）或其他错误是正常的，不处理
            if (rt_err == ESP_OK) {
                nvs_close(rt_handle);
            }
        }
    }

    // 加载配置
    memcpy(&ctx.config, &default_config, sizeof(system_config_t));
    config_manager_load();

    ctx.initialized = true;
    ESP_LOGI(TAG, "配置管理器初始化完成");
    return ESP_OK;
}

esp_err_t config_manager_deinit(void)
{
    ctx.initialized = false;
    return ESP_OK;
}

// ==================== 加载和保存 ====================

esp_err_t config_manager_load(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS中无配置，使用默认值");
        return ESP_OK;
    }

    size_t len;

    // WiFi配置
    len = sizeof(ctx.config.wifi_ssid);
    nvs_get_str(handle, "wifi_ssid", ctx.config.wifi_ssid, &len);
    ctx.config.wifi_ssid[sizeof(ctx.config.wifi_ssid) - 1] = '\0';
    len = sizeof(ctx.config.wifi_password);
    nvs_get_str(handle, "wifi_pass", ctx.config.wifi_password, &len);
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
    nvs_get_u32(handle, "flush_dur", &ctx.config.flush_duration_sec);
    nvs_get_u32(handle, "prod_timeout", &ctx.config.production_timeout_sec);
    nvs_get_u32(handle, "leak_confirm", &ctx.config.leak_confirm_time_sec);

    // 冲洗参数
    nvs_get_u32(handle, "nflush_dur", &ctx.config.normal_flush_duration_sec);
    nvs_get_u32(handle, "pflush_dur", &ctx.config.pure_flush_duration_sec);
    nvs_get_u32(handle, "fflush_dur", &ctx.config.filter_flush_duration_sec);
    nvs_get_u32(handle, "short_prod", &ctx.config.short_prod_threshold_sec);
    nvs_get_u32(handle, "wh_vopn", &ctx.config.water_hammer_valve_open_delay_ms);
    nvs_get_u32(handle, "wh_pstp", &ctx.config.water_hammer_pump_stop_delay_ms);
    nvs_get_u32(handle, "wh_vcls", &ctx.config.water_hammer_valve_close_delay_ms);

    // 继电器配置
    if (nvs_get_u8(handle, "relay_lvl", &u8_val) == ESP_OK) {
        ctx.config.relay_trigger_level = u8_val;
    }

    // 运行数据保存间隔
    uint16_t u16_val2;
    if (nvs_get_u16(handle, "save_intv", &u16_val2) == ESP_OK) {
        ctx.config.runtime_save_interval_min = u16_val2;
    }

    // TDS配置
    uint32_t u32_val;
    if (nvs_get_u32(handle, "tds_in_th", &u32_val) == ESP_OK) {
        ctx.config.tds_inlet_threshold = (float)u32_val;
    }
    if (nvs_get_u32(handle, "tds_out_th", &u32_val) == ESP_OK) {
        ctx.config.tds_outlet_threshold = (float)u32_val;
    }

    // TDS校准
    int32_t i32_val;
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

    // 滤芯容量
    nvs_get_u32(handle, "filter_cap", &ctx.config.filter_capacity_liters);

    // Web配置
    uint16_t u16_val;
    if (nvs_get_u16(handle, "web_port", &u16_val) == ESP_OK) {
        ctx.config.web_port = u16_val;
    }
    if (nvs_get_u8(handle, "web_auth", &u8_val) == ESP_OK) {
        ctx.config.web_auth_enabled = u8_val;
    }
    len = sizeof(ctx.config.web_username);
    nvs_get_str(handle, "web_user", ctx.config.web_username, &len);
    ctx.config.web_username[sizeof(ctx.config.web_username) - 1] = '\0';
    len = sizeof(ctx.config.web_password);
    nvs_get_str(handle, "web_pass", ctx.config.web_password, &len);
    ctx.config.web_password[sizeof(ctx.config.web_password) - 1] = '\0';

    nvs_close(handle);
    ctx.config_dirty = false;
    ESP_LOGI(TAG, "配置已加载");
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
        is_dirty = ctx.config_dirty;  // 降级处理
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
    nvs_set_str(handle, "mqtt_broker", config_copy.mqtt_broker);
    nvs_set_str(handle, "mqtt_user", config_copy.mqtt_username);
    nvs_set_str(handle, "mqtt_pass", config_copy.mqtt_password);
    nvs_set_str(handle, "mqtt_topic", config_copy.mqtt_topic_prefix);

    // 系统参数
    nvs_set_u32(handle, "flush_dur", config_copy.flush_duration_sec);
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

    // TDS配置
    nvs_set_u32(handle, "tds_in_th", (uint32_t)config_copy.tds_inlet_threshold);
    nvs_set_u32(handle, "tds_out_th", (uint32_t)config_copy.tds_outlet_threshold);

    // TDS校准
    nvs_set_i32(handle, "tds_in_off", (int32_t)(config_copy.tds_calibration_offset[0] * 100));
    nvs_set_i32(handle, "tds_out_off", (int32_t)(config_copy.tds_calibration_offset[1] * 100));
    nvs_set_i32(handle, "tds_in_scale", (int32_t)(config_copy.tds_calibration_scale[0] * 10000));
    nvs_set_i32(handle, "tds_out_scale", (int32_t)(config_copy.tds_calibration_scale[1] * 10000));

    // 滤芯容量
    nvs_set_u32(handle, "filter_cap", config_copy.filter_capacity_liters);

    // Web配置
    nvs_set_u16(handle, "web_port", config_copy.web_port);
    nvs_set_u8(handle, "web_auth", config_copy.web_auth_enabled);
    nvs_set_str(handle, "web_user", config_copy.web_username);
    nvs_set_str(handle, "web_pass", config_copy.web_password);

    err = nvs_commit(handle);
    nvs_close(handle);

    // 使用mutex保护脏标志清除
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (err == ESP_OK) {
            ctx.config_dirty = false;
            ESP_LOGI(TAG, "配置已保存");
        }
        xSemaphoreGive(ctx.mutex);
    } else {
        if (err == ESP_OK) {
            ctx.config_dirty = false;
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

// ==================== 运行数据持久化 ====================

// 上次保存的运行数据，用于脏检查
static runtime_data_t s_last_saved_runtime = {0};
static bool s_runtime_data_initialized = false;

esp_err_t config_manager_save_runtime_data(const runtime_data_t *data)
{
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }

    // 脏检查：数据未变化则跳过写入
    if (s_runtime_data_initialized &&
        memcmp(data, &s_last_saved_runtime, sizeof(runtime_data_t)) == 0) {
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_RUNTIME_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS运行时数据打开失败: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u32(handle, "prod_cycles", data->total_production_cycles);
    nvs_set_u32(handle, "flush_cycles", data->total_flush_cycles);
    nvs_set_u64(handle, "prod_time", data->total_production_time_sec);
    nvs_set_u64(handle, "flush_time", data->total_flush_time_sec);
    nvs_set_u32(handle, "total_water", data->total_water_used);

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        s_last_saved_runtime = *data;
        s_runtime_data_initialized = true;
        ESP_LOGD(TAG, "运行数据已保存");
    }
    return err;
}

esp_err_t config_manager_load_runtime_data(runtime_data_t *data)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_RUNTIME_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS中无运行数据，使用默认值");
        memset(data, 0, sizeof(runtime_data_t));
        return ESP_OK;
    }

    nvs_get_u32(handle, "prod_cycles", &data->total_production_cycles);
    nvs_get_u32(handle, "flush_cycles", &data->total_flush_cycles);
    nvs_get_u64(handle, "prod_time", &data->total_production_time_sec);
    nvs_get_u64(handle, "flush_time", &data->total_flush_time_sec);
    nvs_get_u32(handle, "total_water", &data->total_water_used);

    nvs_close(handle);

    ESP_LOGI(TAG, "运行数据已加载: 制水%lu次, 冲洗%lu次, 总水%luL",
             data->total_production_cycles, data->total_flush_cycles, data->total_water_used);
    return ESP_OK;
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
        // 仅在值发生变化时标记为脏
        if (memcmp(&ctx.config, config, sizeof(system_config_t)) != 0) {
            memcpy(&ctx.config, config, sizeof(system_config_t));
            ctx.config_dirty = true;
        }
        xSemaphoreGive(ctx.mutex);
        return ESP_OK;
    }

    // 互斥锁获取失败时降级处理
    if (memcmp(&ctx.config, config, sizeof(system_config_t)) != 0) {
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

    // 优化：一次READWRITE打开完成读写，避免两次打开
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    // 在同一个handle中读取现有值，比较是否变化
    size_t cur_len = 0;
    err = nvs_get_str(handle, key, NULL, &cur_len);
    bool value_changed = true;  // 默认假设值已变化
    if (err == ESP_OK && cur_len > 0) {
        char *cur_val = malloc(cur_len);
        if (cur_val) {
            err = nvs_get_str(handle, key, cur_val, &cur_len);
            if (err == ESP_OK && strcmp(cur_val, value) == 0) {
                value_changed = false;  // 值未变化
            }
            free(cur_val);
        }
    }

    // 值未变化时直接关闭handle返回，避免不必要的写入
    if (!value_changed) {
        nvs_close(handle);
        return ESP_OK;
    }

    // 先更新内存配置（使用mutex保护，失败时返回错误避免竞态）
    bool mem_changed = false;
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        update_config_from_key(key, value, &mem_changed);
        if (mem_changed) {
            ctx.config_dirty = true;
        }
        xSemaphoreGive(ctx.mutex);
    } else {
        // mutex获取失败：关闭NVS handle并返回错误，避免竞态条件
        nvs_close(handle);
        ESP_LOGE(TAG, "set_string: mutex获取失败，拒绝更新以避免竞态");
        return ESP_ERR_TIMEOUT;
    }

    // 写入NVS（handle已打开）
    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
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
    UPDATE_U32_FIELD(flush_duration_sec);
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
    UPDATE_U32_FIELD(filter_capacity_liters);
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

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        int32_t cur_val;
        if (nvs_get_i32(handle, key, &cur_val) == ESP_OK && cur_val == value) {
            nvs_close(handle);
            return ESP_OK;  // 值未变化，不写入
        }
        nvs_close(handle);
    }

    // 先更新内存配置（使用mutex保护）
    bool mem_changed = false;
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        update_config_int_from_key(key, value, &mem_changed);
        if (mem_changed) {
            ctx.config_dirty = true;
        }
        xSemaphoreGive(ctx.mutex);
    } else {
        // mutex获取失败：返回错误避免竞态条件
        ESP_LOGE(TAG, "set_int: mutex获取失败，拒绝更新以避免竞态");
        return ESP_ERR_TIMEOUT;
    }

    // 再写入NVS
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_i32(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
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

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        uint8_t cur_val;
        if (nvs_get_u8(handle, key, &cur_val) == ESP_OK && cur_val == value) {
            nvs_close(handle);
            return ESP_OK;  // 值未变化，不写入
        }
        nvs_close(handle);
    }

    // 先更新内存配置（使用mutex保护）
    bool mem_changed = false;
    if (ctx.mutex && xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        update_config_bool_from_key(key, value, &mem_changed);
        if (mem_changed) {
            ctx.config_dirty = true;
        }
        xSemaphoreGive(ctx.mutex);
    } else {
        // mutex获取失败：返回错误避免竞态条件
        ESP_LOGE(TAG, "set_bool: mutex获取失败，拒绝更新以避免竞态");
        return ESP_ERR_TIMEOUT;
    }

    // 再写入NVS
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
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

    // 验证时间参数
    if (config->flush_duration_sec < 10 || config->flush_duration_sec > 600) {
        return false;
    }
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

    // 验证运行数据保存间隔 (10分钟/1小时/2小时/6小时/12小时/24小时)
    if (config->runtime_save_interval_min != 10 &&
        config->runtime_save_interval_min != 60 &&
        config->runtime_save_interval_min != 120 &&
        config->runtime_save_interval_min != 360 &&
        config->runtime_save_interval_min != 720 &&
        config->runtime_save_interval_min != 1440) {
        return false;
    }

    return true;
}

bool config_manager_is_dirty(void)
{
    return ctx.config_dirty;
}

bool config_manager_has_wifi_config(void)
{
    return (strlen(ctx.config.wifi_ssid) > 0);
}

bool config_manager_has_mqtt_config(void)
{
    return (strlen(ctx.config.mqtt_broker) > 0);
}

// ==================== 统一周期保存 ====================

// 外部模块的周期保存函数声明
extern bool filter_mgr_periodic_save(void);
extern bool history_periodic_save(uint32_t min_interval_sec);

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
    ESP_LOGI(TAG, "冲洗时间: %lu秒", ctx.config.flush_duration_sec);
    ESP_LOGI(TAG, "制水超时: %lu秒", ctx.config.production_timeout_sec);
    ESP_LOGI(TAG, "漏水确认: %lu秒", ctx.config.leak_confirm_time_sec);
    ESP_LOGI(TAG, "继电平: %s", ctx.config.relay_trigger_level ? "高" : "低");
    ESP_LOGI(TAG, "TDS阈值: 进水%.0f/出水%.0f ppm",
             ctx.config.tds_inlet_threshold, ctx.config.tds_outlet_threshold);
    ESP_LOGI(TAG, "滤芯容量: %lu升", ctx.config.filter_capacity_liters);
    ESP_LOGI(TAG, "===================");
}

esp_err_t config_manager_get_status_string(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(buffer, buffer_size,
             "WiFi: %s\n"
             "MQTT: %s (%s)\n"
             "冲洗: %lu秒\n"
             "超时: %lu秒\n"
             "漏确认: %lu秒\n"
             "继电平: %s\n"
             "TDS阈值: %.0f/%.0f ppm\n"
             "滤芯: %lu升",
             ctx.config.wifi_ssid,
             ctx.config.mqtt_broker,
             ctx.config.mqtt_enabled ? "开" : "关",
             ctx.config.flush_duration_sec,
             ctx.config.production_timeout_sec,
             ctx.config.leak_confirm_time_sec,
             ctx.config.relay_trigger_level ? "高" : "低",
             ctx.config.tds_inlet_threshold,
             ctx.config.tds_outlet_threshold,
             ctx.config.filter_capacity_liters);

    return ESP_OK;
}