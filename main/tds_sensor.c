/**
 * @file tds_sensor.c
 * @brief TDS传感器检测模块实现
 * @note 进水TDS: GPIO4 (ADC1_CH4), 出水TDS: GPIO5 (ADC1_CH5)
 */

#include "tds_sensor.h"
#include "gpio_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>
#include <time.h>

static const char *TAG = "TDS";

// ==================== 私有变量 ====================

// ADC多采样配置
#define ADC_SAMPLE_COUNT    16      // 每次测量采样次数
#define ADC_SAMPLE_DELAY_US 500     // 采样间隔(微秒)

// 滤芯默认时间寿命（小时）
static const uint32_t filter_default_time_hours[FILTER_COUNT] = {
    3000,   // PP棉：约3-6个月
    4000,   // 颗粒活性炭：约6个月
    4000,   // 压缩活性炭：约6个月
    8000,   // RO膜：约24个月
    4000,   // 后置活性炭：约12个月
};

static adc_oneshot_unit_handle_t adc1_handle = NULL;

static struct {
    bool initialized;
    bool running;
    TaskHandle_t task_handle;

    // 报警阈值
    float alarm_threshold[TDS_SENSOR_COUNT];

    // 报警状态标志（避免重复日志）
    bool alarm_active[TDS_SENSOR_COUNT];

    // 校准参数
    tds_calibration_t calibration[TDS_SENSOR_COUNT];

    // 温度补偿
    float temperature;

    // 最新测量值
    tds_measurement_t latest[TDS_SENSOR_COUNT];

    // 滤芯寿命（兼容旧接口）
    uint32_t filter_total_liters;
    uint32_t filter_used_liters;
    uint32_t filter_install_time;  // Unix秒，NTP墙钟

    // 五级滤芯管理
    filter_info_t filters[FILTER_COUNT];
    uint32_t total_water_used;

    // 用水量累积器（避免短时间制水被丢弃）
    float water_usage_accumulator;

    // RO制水速率（升/小时）
    float production_rate_lph;
} tds_ctx = {
    .initialized = false,
    .running = false,
    .alarm_threshold = {500.0f, 100.0f},  // 进水500ppm，出水100ppm
    .temperature = 25.0f,
    .filter_total_liters = 3000,  // 默认3000升
    .filter_used_liters = 0,
    .filter_install_time = 0,
    .total_water_used = 0
};

// ==================== ESP32-C3 ADC通道映射 ====================
// ESP32-C3 ADC通道映射:
// GPIO0=ADC1_CH0, GPIO1=ADC1_CH1, GPIO2=ADC1_CH2, GPIO3=ADC1_CH3, GPIO4=ADC1_CH4
// GPIO5=ADC2_CH0 (WiFi运行时不可用!)
//
// 当前配置: GPIO2(进水TDS), GPIO3(出水TDS) - 都使用ADC1，与WiFi兼容

// ADC通道映射 (ESP32-C3) - 使用ADC1可用通道
static const adc_channel_t tds_adc_channels[TDS_SENSOR_COUNT] = {
    ADC_CHANNEL_2,  // 进水TDS - GPIO2 (ADC1_CH2)
    ADC_CHANNEL_3   // 出水TDS - GPIO3 (ADC1_CH3)
};

// 传感器名称
static const char* const sensor_names[] = {
    "进水TDS",
    "出水TDS"
};

// 滤芯名称
static const char* const filter_names[FILTER_COUNT] = {
    "PP棉",
    "颗粒活性炭",
    "压缩活性炭",
    "RO膜",
    "后置活性炭"
};

// 滤芯默认容量（升）
static const uint32_t filter_default_capacities[FILTER_COUNT] = {
    FILTER_DEFAULT_LIFE_PP,
    FILTER_DEFAULT_LIFE_GRANULAR,
    FILTER_DEFAULT_LIFE_COMPRESSED,
    FILTER_DEFAULT_LIFE_RO,
    FILTER_DEFAULT_LIFE_POST
};

// ==================== 私有函数 ====================

static float calculate_tds(float voltage, float temperature);
static float voltage_to_ec(float voltage);
static float temperature_compensation(float ec, float temperature);

// ==================== 初始化 ====================

esp_err_t tds_sensor_init(void)
{
    if (tds_ctx.initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "初始化TDS传感器...");

    // 先配置GPIO引脚为模拟输入模式，禁用上拉/下拉
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << ADC_TDS_IN_GPIO) | (1ULL << ADC_TDS_OUT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "GPIO引脚配置: 禁用上拉/下拉");

    // 初始化ADC1
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_cfg, &adc1_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 配置ADC通道
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };

    // 配置两个TDS传感器的ADC通道
    for (int i = 0; i < TDS_SENSOR_COUNT; i++) {
        ret = adc_oneshot_config_channel(adc1_handle, tds_adc_channels[i], &chan_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ADC通道%d配置失败: %s", i, esp_err_to_name(ret));
            adc_oneshot_del_unit(adc1_handle);
            return ret;
        }
    }
    ESP_LOGI(TAG, "ADC配置成功: 进水TDS(GPIO2), 出水TDS(GPIO3)");

    // 初始化校准参数
    for (int i = 0; i < TDS_SENSOR_COUNT; i++) {
        tds_ctx.calibration[i].offset = 0.0f;
        tds_ctx.calibration[i].scale = 1.0f;
        tds_ctx.latest[i].valid = false;
    }

    // 初始化五级滤芯
    time_t now_sec = time(NULL);
    for (int i = 0; i < FILTER_COUNT; i++) {
        tds_ctx.filters[i].percentage = 100;
        tds_ctx.filters[i].time_percentage = 100;
        tds_ctx.filters[i].effective_percentage = 100;
        tds_ctx.filters[i].used_liters = 0;
        tds_ctx.filters[i].total_liters = filter_default_capacities[i];
        tds_ctx.filters[i].time_limit_hours = filter_default_time_hours[i];
        tds_ctx.filters[i].install_time = (uint32_t)now_sec;
        tds_ctx.filters[i].last_reset_time = (uint32_t)now_sec;
        tds_ctx.filters[i].replacement_needed = false;
        strncpy(tds_ctx.filters[i].name, filter_names[i], sizeof(tds_ctx.filters[i].name) - 1);
    }
    tds_ctx.total_water_used = 0;
    tds_ctx.production_rate_lph = 12.0f;  // 默认汇通75G

    // 记录滤芯安装时间（兼容旧接口）
    tds_ctx.filter_install_time = (uint32_t)now_sec;

    tds_ctx.initialized = true;
    ESP_LOGI(TAG, "TDS传感器初始化完成");
    return ESP_OK;
}

esp_err_t tds_sensor_deinit(void)
{
    if (!tds_ctx.initialized) {
        return ESP_OK;
    }

    tds_sensor_stop();

    if (adc1_handle) {
        adc_oneshot_del_unit(adc1_handle);
        adc1_handle = NULL;
    }

    tds_ctx.initialized = false;
    return ESP_OK;
}

// ==================== 任务 ====================

static void tds_sensor_task(void *arg)
{
    ESP_LOGI(TAG, "TDS测量任务启动");

    while (tds_ctx.running) {
        tds_dual_measurement_t dual;
        if (tds_sensor_measure_dual(&dual) == ESP_OK) {
            // 更新最新测量值
            memcpy(&tds_ctx.latest[TDS_SENSOR_INLET], &dual.inlet, sizeof(tds_measurement_t));
            memcpy(&tds_ctx.latest[TDS_SENSOR_OUTLET], &dual.outlet, sizeof(tds_measurement_t));

            ESP_LOGD(TAG, "进水: %.1f ppm, 出水: %.1f ppm, 去除率: %.1f%%",
                     dual.inlet.tds_value, dual.outlet.tds_value, dual.reduction_rate);

            // 检查报警
            for (int i = 0; i < TDS_SENSOR_COUNT; i++) {
                if (tds_ctx.latest[i].tds_value > tds_ctx.alarm_threshold[i]) {
                    if (!tds_ctx.alarm_active[i]) {
                        ESP_LOGW(TAG, "%s报警: %.1f > %.1f ppm",
                                 sensor_names[i],
                                 tds_ctx.latest[i].tds_value,
                                 tds_ctx.alarm_threshold[i]);
                        tds_ctx.alarm_active[i] = true;
                    }
                } else {
                    if (tds_ctx.alarm_active[i]) {
                        ESP_LOGI(TAG, "%s报警已解除", sensor_names[i]);
                        tds_ctx.alarm_active[i] = false;
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));  // 1秒采样间隔
    }

    ESP_LOGI(TAG, "TDS测量任务结束");
    vTaskDelete(NULL);
}

esp_err_t tds_sensor_start(void)
{
    if (!tds_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (tds_ctx.running) {
        return ESP_OK;
    }

    // 先设置running标志，避免任务启动时的竞态条件
    tds_ctx.running = true;

    BaseType_t ret = xTaskCreate(
        tds_sensor_task,
        "tds_task",
        3072,
        NULL,
        4,
        &tds_ctx.task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建任务失败");
        tds_ctx.running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TDS传感器启动");
    return ESP_OK;
}

esp_err_t tds_sensor_stop(void)
{
    if (!tds_ctx.running) {
        return ESP_OK;
    }

    tds_ctx.running = false;

    if (tds_ctx.task_handle) {
        vTaskDelete(tds_ctx.task_handle);
        tds_ctx.task_handle = NULL;
    }

    return ESP_OK;
}

// ==================== 测量 ====================

/**
 * @brief 在NTP同步后回补滤芯安装时间戳
 * 如果系统初始化时NTP尚未同步，last_reset_time会是0或1，
 * 当NTP同步后首次调用此函数自动修正为当前时间。
 */
static void fixup_filter_time_on_ntp_sync(void)
{
    time_t now_sec = time(NULL);
    if (now_sec <= 1) return;  // NTP仍未同步

    bool fixed = false;
    for (int i = 0; i < FILTER_COUNT; i++) {
        if (tds_ctx.filters[i].last_reset_time <= 1) {
            tds_ctx.filters[i].last_reset_time = (uint32_t)now_sec;
            tds_ctx.filters[i].install_time = (uint32_t)now_sec;
            fixed = true;
        }
    }
    if (tds_ctx.filter_install_time <= 1) {
        tds_ctx.filter_install_time = (uint32_t)now_sec;
        fixed = true;
    }
    if (fixed) {
        ESP_LOGI(TAG, "NTP已同步，修正滤芯日历时间戳");
    }
}

/**
 * @brief 更新所有滤芯的时间维度寿命百分比
 * 此函数独立于水量更新调用，确保设备闲置时倒计时仍然工作
 * @note 应在查询滤芯状态或周期性保存时调用
 */
static void update_filter_time_percentage(void)
{
    time_t now_sec = time(NULL);
    if (now_sec <= 0) return;  // NTP未同步，无法计算日历时长

    for (int i = 0; i < FILTER_COUNT; i++) {
        // 时间维度（日历时长，NTP墙钟）
        if (tds_ctx.filters[i].time_limit_hours > 0 &&
            now_sec > 0 && tds_ctx.filters[i].last_reset_time > 0 &&
            now_sec >= tds_ctx.filters[i].last_reset_time) {
            uint32_t elapsed_hours = (uint32_t)((now_sec - tds_ctx.filters[i].last_reset_time) / 3600);
            float time_remaining = 1.0f - (float)elapsed_hours / tds_ctx.filters[i].time_limit_hours;
            if (time_remaining < 0) time_remaining = 0;
            tds_ctx.filters[i].time_percentage = (uint8_t)(time_remaining * 100);
        }

        // 有效寿命 = min(水量%, 时间%)
        tds_ctx.filters[i].effective_percentage =
            (tds_ctx.filters[i].percentage < tds_ctx.filters[i].time_percentage) ?
            tds_ctx.filters[i].percentage : tds_ctx.filters[i].time_percentage;

        // 检查是否需要更换（低于10%）
        bool needs_replacement = (tds_ctx.filters[i].effective_percentage < 10);

        // 边沿触发警告：只在首次进入低于10%时输出
        if (needs_replacement && !tds_ctx.filters[i].replacement_needed) {
            ESP_LOGW(TAG, "%s滤芯寿命不足10%%，请及时更换", filter_names[i]);
        }

        tds_ctx.filters[i].replacement_needed = needs_replacement;
    }
}

/**
 * @brief 多次采样ADC并计算平均值（去除极值）
 */
static int adc_read_averaged(adc_channel_t channel)
{
    int sum = 0;
    int min_val = 4095, max_val = 0;

    // 采集多个样本
    for (int i = 0; i < ADC_SAMPLE_COUNT; i++) {
        int raw = 0;
        if (adc_oneshot_read(adc1_handle, channel, &raw) == ESP_OK) {
            sum += raw;
            if (raw < min_val) min_val = raw;
            if (raw > max_val) max_val = raw;
        }
        esp_rom_delay_us(ADC_SAMPLE_DELAY_US);
    }

    // 去掉最大最小值后计算平均
    sum = sum - min_val - max_val;
    return sum / (ADC_SAMPLE_COUNT - 2);
}

esp_err_t tds_sensor_measure(tds_sensor_id_t sensor_id, tds_measurement_t *measurement)
{
    if (!tds_ctx.initialized || !measurement || sensor_id >= TDS_SENSOR_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    // 多次采样取平均 (GPIO2/GPIO3 - ADC1_CH2/CH3)
    int adc_raw = adc_read_averaged(tds_adc_channels[sensor_id]);

    // 检测传感器是否连接（ADC值接近0或满量程表示异常）
    bool sensor_connected = (adc_raw > 5 && adc_raw < 4090);

    // 转换为电压（mV），12位ADC，3.3V参考
    float voltage = (adc_raw / 4095.0f) * 3300.0f;

    // 计算TDS
    float tds = calculate_tds(voltage, tds_ctx.temperature);

    // 应用校准
    tds = tds * tds_ctx.calibration[sensor_id].scale + tds_ctx.calibration[sensor_id].offset;
    if (tds < 0) tds = 0;

    // 填充结果
    measurement->voltage = voltage;
    measurement->tds_value = sensor_connected ? tds : 0.0f;  // 传感器未连接时显示0
    measurement->ec_value = sensor_connected ? voltage_to_ec(voltage) : 0.0f;
    measurement->temperature = tds_ctx.temperature;
    measurement->valid = sensor_connected;  // 传感器未连接时标记为无效
    measurement->timestamp = esp_timer_get_time();

    // 调试输出原始ADC值 (改为INFO级别方便调试)
    ESP_LOGD(TAG, "%s: ADC=%d, V=%.1fmV, TDS=%.1fppm, connected=%d",
             sensor_names[sensor_id], adc_raw, voltage, measurement->tds_value, sensor_connected);

    return ESP_OK;
}

esp_err_t tds_sensor_measure_dual(tds_dual_measurement_t *dual_measurement)
{
    if (!dual_measurement) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret1 = tds_sensor_measure(TDS_SENSOR_INLET, &dual_measurement->inlet);
    esp_err_t ret2 = tds_sensor_measure(TDS_SENSOR_OUTLET, &dual_measurement->outlet);

    dual_measurement->both_valid = (ret1 == ESP_OK && ret2 == ESP_OK);

    // 计算去除率
    if (dual_measurement->both_valid &&
        dual_measurement->inlet.tds_value > 0) {
        dual_measurement->reduction_rate =
            (1.0f - dual_measurement->outlet.tds_value / dual_measurement->inlet.tds_value) * 100.0f;
    } else {
        dual_measurement->reduction_rate = 0;
    }

    return dual_measurement->both_valid ? ESP_OK : ESP_FAIL;
}

esp_err_t tds_sensor_get_latest(tds_sensor_id_t sensor_id, tds_measurement_t *measurement)
{
    if (!measurement || sensor_id >= TDS_SENSOR_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!tds_ctx.latest[sensor_id].valid) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(measurement, &tds_ctx.latest[sensor_id], sizeof(tds_measurement_t));
    return ESP_OK;
}

esp_err_t tds_sensor_get_latest_dual(tds_dual_measurement_t *dual_measurement)
{
    if (!dual_measurement) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&dual_measurement->inlet, &tds_ctx.latest[TDS_SENSOR_INLET], sizeof(tds_measurement_t));
    memcpy(&dual_measurement->outlet, &tds_ctx.latest[TDS_SENSOR_OUTLET], sizeof(tds_measurement_t));

    dual_measurement->both_valid = (tds_ctx.latest[TDS_SENSOR_INLET].valid &&
                                    tds_ctx.latest[TDS_SENSOR_OUTLET].valid);

    if (dual_measurement->both_valid && dual_measurement->inlet.tds_value > 0) {
        dual_measurement->reduction_rate =
            (1.0f - dual_measurement->outlet.tds_value / dual_measurement->inlet.tds_value) * 100.0f;
    } else {
        dual_measurement->reduction_rate = 0;
    }

    return ESP_OK;
}

// ==================== 校准 ====================

esp_err_t tds_sensor_set_calibration(tds_sensor_id_t sensor_id, const tds_calibration_t *calibration)
{
    if (sensor_id >= TDS_SENSOR_COUNT || !calibration) {
        return ESP_ERR_INVALID_ARG;
    }

    tds_ctx.calibration[sensor_id] = *calibration;
    ESP_LOGI(TAG, "%s校准: offset=%.2f, scale=%.4f",
             sensor_names[sensor_id], calibration->offset, calibration->scale);
    return ESP_OK;
}

esp_err_t tds_sensor_get_calibration(tds_sensor_id_t sensor_id, tds_calibration_t *calibration)
{
    if (sensor_id >= TDS_SENSOR_COUNT || !calibration) {
        return ESP_ERR_INVALID_ARG;
    }

    *calibration = tds_ctx.calibration[sensor_id];
    return ESP_OK;
}

esp_err_t tds_sensor_calibrate(tds_sensor_id_t sensor_id, float standard_value)
{
    if (sensor_id >= TDS_SENSOR_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    // 测量当前值
    tds_measurement_t meas;
    if (tds_sensor_measure(sensor_id, &meas) != ESP_OK) {
        return ESP_FAIL;
    }

    // 计算比例系数
    if (meas.tds_value > 1.0f) {
        tds_ctx.calibration[sensor_id].scale = standard_value / meas.tds_value;
        ESP_LOGI(TAG, "%s校准完成: 测量值=%.1f, 标准值=%.1f, scale=%.4f",
                 sensor_names[sensor_id], meas.tds_value, standard_value,
                 tds_ctx.calibration[sensor_id].scale);
        return ESP_OK;
    }

    return ESP_ERR_INVALID_STATE;
}

esp_err_t tds_sensor_reset_calibration(tds_sensor_id_t sensor_id)
{
    if (sensor_id >= TDS_SENSOR_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    tds_ctx.calibration[sensor_id].offset = 0.0f;
    tds_ctx.calibration[sensor_id].scale = 1.0f;
    ESP_LOGI(TAG, "%s校准已重置", sensor_names[sensor_id]);
    return ESP_OK;
}

// ==================== 报警 ====================

esp_err_t tds_sensor_set_alarm_threshold(tds_sensor_id_t sensor_id, float threshold)
{
    if (sensor_id >= TDS_SENSOR_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    tds_ctx.alarm_threshold[sensor_id] = threshold;
    ESP_LOGI(TAG, "%s报警阈值: %.1f ppm", sensor_names[sensor_id], threshold);
    return ESP_OK;
}

esp_err_t tds_sensor_get_alarm_threshold(tds_sensor_id_t sensor_id, float *threshold)
{
    if (sensor_id >= TDS_SENSOR_COUNT || !threshold) {
        return ESP_ERR_INVALID_ARG;
    }

    *threshold = tds_ctx.alarm_threshold[sensor_id];
    return ESP_OK;
}

bool tds_sensor_is_alarm(tds_sensor_id_t sensor_id)
{
    if (sensor_id >= TDS_SENSOR_COUNT) {
        return false;
    }

    return (tds_ctx.latest[sensor_id].valid &&
            tds_ctx.latest[sensor_id].tds_value > tds_ctx.alarm_threshold[sensor_id]);
}

// ==================== 滤芯寿命 ====================

esp_err_t tds_sensor_set_filter_capacity(uint32_t total_liters)
{
    tds_ctx.filter_total_liters = total_liters;
    ESP_LOGI(TAG, "滤芯容量设置: %lu 升", total_liters);
    return ESP_OK;
}

esp_err_t tds_sensor_update_water_usage(float liters)
{
    if (liters <= 0.0f) {
        return ESP_OK;
    }

    // 如果NTP刚同步，回补滤芯时间戳
    fixup_filter_time_on_ntp_sync();

    // 累积小数水量
    tds_ctx.water_usage_accumulator += liters;

    // 当累积达到1升时，更新总量
    if (tds_ctx.water_usage_accumulator >= 1.0f) {
        uint32_t committed_liters = (uint32_t)tds_ctx.water_usage_accumulator;
        tds_ctx.water_usage_accumulator -= committed_liters;

        tds_ctx.filter_used_liters += committed_liters;
        tds_ctx.total_water_used += committed_liters;

        // 更新每个滤芯的用水量
        for (int i = 0; i < FILTER_COUNT; i++) {
            tds_ctx.filters[i].used_liters += committed_liters;

            // 水量维度
            if (tds_ctx.filters[i].total_liters > 0) {
                float remaining = 1.0f - (float)tds_ctx.filters[i].used_liters / tds_ctx.filters[i].total_liters;
                if (remaining < 0) remaining = 0;
                tds_ctx.filters[i].percentage = (uint8_t)(remaining * 100);
            }
        }
    }

    // 统一更新所有滤芯的时间维度寿命、有效寿命、更换标志
    update_filter_time_percentage();

    return ESP_OK;
}

esp_err_t tds_sensor_get_filter_life(filter_life_t *life)
{
    if (!life) {
        return ESP_ERR_INVALID_ARG;
    }

    update_filter_time_percentage();

    life->used_liters = tds_ctx.filter_used_liters;
    life->total_liters = tds_ctx.filter_total_liters;
    life->install_time = tds_ctx.filter_install_time;

    // 使用RO膜的有效寿命（水量+时间最小值）
    if (tds_ctx.filter_total_liters > 0) {
        uint8_t pct = tds_ctx.filters[FILTER_RO_MEMBRANE].effective_percentage;
        life->percentage = pct;
    } else {
        life->percentage = 100;
    }

    life->replacement_needed = (life->percentage < 10);

    return ESP_OK;
}

esp_err_t tds_sensor_reset_filter_life(void)
{
    tds_ctx.filter_used_liters = 0;
    tds_ctx.filter_install_time = (uint32_t)time(NULL);
    ESP_LOGI(TAG, "滤芯寿命已重置");
    return ESP_OK;
}

// ==================== 五级滤芯管理 ====================

esp_err_t tds_sensor_get_filters_status(filters_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    // 如果NTP刚同步，回补滤芯时间戳
    fixup_filter_time_on_ntp_sync();

    // 更新所有滤芯的时间维度寿命（确保查询时倒计时实时）
    update_filter_time_percentage();

    memcpy(status->filters, tds_ctx.filters, sizeof(tds_ctx.filters));
    status->total_water_used = tds_ctx.total_water_used;
    status->any_filter_needs_replacement = tds_sensor_any_filter_needs_replacement();

    return ESP_OK;
}

esp_err_t tds_sensor_get_filter_info(filter_type_t filter_type, filter_info_t *info)
{
    if (filter_type >= FILTER_COUNT || !info) {
        return ESP_ERR_INVALID_ARG;
    }

    update_filter_time_percentage();
    memcpy(info, &tds_ctx.filters[filter_type], sizeof(filter_info_t));
    return ESP_OK;
}

esp_err_t tds_sensor_reset_filter(filter_type_t filter_type)
{
    if (filter_type >= FILTER_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t now_sec = time(NULL);
    tds_ctx.filters[filter_type].percentage = 100;
    tds_ctx.filters[filter_type].time_percentage = 100;
    tds_ctx.filters[filter_type].effective_percentage = 100;
    tds_ctx.filters[filter_type].used_liters = 0;
    tds_ctx.filters[filter_type].install_time = (uint32_t)now_sec;
    tds_ctx.filters[filter_type].last_reset_time = (uint32_t)now_sec;
    tds_ctx.filters[filter_type].replacement_needed = false;

    ESP_LOGI(TAG, "%s滤芯已重置", filter_names[filter_type]);
    return ESP_OK;
}

esp_err_t tds_sensor_reset_all_filters(void)
{
    time_t now_sec = time(NULL);

    for (int i = 0; i < FILTER_COUNT; i++) {
        tds_ctx.filters[i].percentage = 100;
        tds_ctx.filters[i].time_percentage = 100;
        tds_ctx.filters[i].effective_percentage = 100;
        tds_ctx.filters[i].used_liters = 0;
        tds_ctx.filters[i].install_time = (uint32_t)now_sec;
        tds_ctx.filters[i].last_reset_time = (uint32_t)now_sec;
        tds_ctx.filters[i].replacement_needed = false;
    }
    tds_ctx.total_water_used = 0;
    tds_ctx.filter_used_liters = 0;
    tds_ctx.water_usage_accumulator = 0.0f;

    ESP_LOGI(TAG, "所有滤芯已重置");
    return ESP_OK;
}

esp_err_t tds_sensor_set_filter_capacity_ex(filter_type_t filter_type, uint32_t total_liters)
{
    if (filter_type >= FILTER_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    tds_ctx.filters[filter_type].total_liters = total_liters;
    // 重新计算水量百分比
    if (total_liters > 0) {
        float remaining = 1.0f - (float)tds_ctx.filters[filter_type].used_liters / total_liters;
        if (remaining < 0) remaining = 0;
        tds_ctx.filters[filter_type].percentage = (uint8_t)(remaining * 100);
        if (tds_ctx.filters[filter_type].percentage > 100) {
            tds_ctx.filters[filter_type].percentage = 100;
        }
    }
    // 更新有效寿命
    tds_ctx.filters[filter_type].effective_percentage =
        (tds_ctx.filters[filter_type].percentage < tds_ctx.filters[filter_type].time_percentage) ?
        tds_ctx.filters[filter_type].percentage : tds_ctx.filters[filter_type].time_percentage;
    tds_ctx.filters[filter_type].replacement_needed = (tds_ctx.filters[filter_type].effective_percentage < 10);

    ESP_LOGI(TAG, "%s滤芯容量设置为 %lu 升", filter_names[filter_type], total_liters);
    return ESP_OK;
}

const char* tds_sensor_get_filter_name(filter_type_t filter_type)
{
    if (filter_type < FILTER_COUNT) {
        return filter_names[filter_type];
    }
    return "未知滤芯";
}

bool tds_sensor_any_filter_needs_replacement(void)
{
    for (int i = 0; i < FILTER_COUNT; i++) {
        if (tds_ctx.filters[i].replacement_needed ||
            tds_ctx.filters[i].percentage < 10) {
            return true;
        }
    }
    return false;
}

// ==================== 温度 ====================

uint32_t tds_sensor_get_total_water_usage(void)
{
    return tds_ctx.total_water_used;
}

esp_err_t tds_sensor_set_production_rate(float lph)
{
    if (lph <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    tds_ctx.production_rate_lph = lph;
    ESP_LOGI(TAG, "RO制水速率设置: %.1f L/h", lph);
    return ESP_OK;
}

float tds_sensor_get_production_rate(void)
{
    return tds_ctx.production_rate_lph;
}

esp_err_t tds_sensor_set_temperature(float temperature)
{
    tds_ctx.temperature = temperature;
    return ESP_OK;
}

float tds_sensor_get_temperature(void)
{
    return tds_ctx.temperature;
}

// ==================== 状态 ====================

bool tds_sensor_is_running(void)
{
    return tds_ctx.running;
}

esp_err_t tds_sensor_get_status_string(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    float inlet_tds = tds_ctx.latest[TDS_SENSOR_INLET].valid ?
                      tds_ctx.latest[TDS_SENSOR_INLET].tds_value : 0;
    float outlet_tds = tds_ctx.latest[TDS_SENSOR_OUTLET].valid ?
                       tds_ctx.latest[TDS_SENSOR_OUTLET].tds_value : 0;

    filter_life_t filter;
    tds_sensor_get_filter_life(&filter);

    snprintf(buffer, buffer_size,
             "进水TDS: %.1f ppm (阈值: %.1f)\n"
             "出水TDS: %.1f ppm (阈值: %.1f)\n"
             "去除率: %.1f%%\n"
             "滤芯寿命: %d%%\n"
             "温度: %.1f°C",
             inlet_tds, tds_ctx.alarm_threshold[TDS_SENSOR_INLET],
             outlet_tds, tds_ctx.alarm_threshold[TDS_SENSOR_OUTLET],
             (inlet_tds > 0) ? (1.0f - outlet_tds / inlet_tds) * 100 : 0,
             filter.percentage,
             tds_ctx.temperature);

    return ESP_OK;
}

const char* tds_sensor_get_name(tds_sensor_id_t sensor_id)
{
    if (sensor_id < TDS_SENSOR_COUNT) {
        return sensor_names[sensor_id];
    }
    return "未知";
}

// ==================== 私有函数实现 ====================

static float calculate_tds(float voltage, float temperature)
{
    if (voltage < 10.0f) {
        return 0.0f;
    }

    // TDS探针规格: 0~2.3V -> 0~1000ppm
    // 线性转换: TDS = (voltage_mV / 2300) * 1000
    float tds = voltage * (1000.0f / 2300.0f);

    // 温度补偿 (系数约2%/°C)
    tds = tds / (1.0f + 0.02f * (temperature - 25.0f));

    return tds;
}

static float voltage_to_ec(float voltage)
{
    if (voltage < 10.0f) {
        return 0.0f;
    }
    // TDS探针规格: 0~2.3V -> 0~1000ppm
    float tds = voltage * (1000.0f / 2300.0f);
    // EC ≈ TDS / 0.65 (典型转换系数)
    return tds / 0.65f;
}

// 温度补偿函数（保留供将来扩展使用）
__attribute__((unused))
static float temperature_compensation(float ec, float temperature)
{
    // 温度补偿系数约2%/°C
    const float temp_coeff = 0.02f;
    return ec / (1.0f + temp_coeff * (temperature - 25.0f));
}