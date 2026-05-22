/**
 * @file tds_sensor.c
 * @brief TDS传感器检测模块实现
 * @note 进水TDS: GPIO2 (ADC1_CH2), 出水TDS: GPIO3 (ADC1_CH3)
 */

#include "tds_sensor.h"
#include "gpio_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <math.h>
#include <time.h>

static const char *TAG = "TDS";

// ==================== 私有变量 ====================

// ADC多采样配置
#define ADC_SAMPLE_COUNT    16      // 每次测量采样次数
#define ADC_SAMPLE_DELAY_US 500     // 采样间隔(微秒)

// 报警迟滞：解除报警需要低于阈值5%，防止阈值附近频繁切换
#define ALARM_HYSTERESIS_PERCENT 0.05f  // 5%迟滞

static adc_oneshot_unit_handle_t adc1_handle = NULL;

static struct {
    bool initialized;
    bool running;
    TaskHandle_t task_handle;
    SemaphoreHandle_t data_mutex;  // 保护latest数组的互斥锁

    // 报警阈值
    float alarm_threshold[TDS_SENSOR_COUNT];

    // 报警状态标志（避免重复日志）
    bool alarm_active[TDS_SENSOR_COUNT];

    // 跳过报警检测标志（纯水洗膜期间使用）
    bool skip_alarm_detection;

    // 校准参数
    tds_calibration_t calibration[TDS_SENSOR_COUNT];

    // 温度补偿
    float temperature;

    // 最新测量值
    tds_measurement_t latest[TDS_SENSOR_COUNT];
} tds_ctx = {
    .initialized = false,
    .running = false,
    .alarm_threshold = {500.0f, 100.0f},  // 进水500ppm，出水100ppm
    .skip_alarm_detection = false,
    .temperature = 25.0f,
};

// ==================== ESP32-C3 ADC通道映射 ====================
// ESP32-C3 ADC1通道: GPIO0=CH0, GPIO1=CH1, GPIO2=CH2, GPIO3=CH3, GPIO4=CH4
// GPIO5=ADC2 (WiFi运行时不可用!)
//
// 当前配置: GPIO2(进水TDS), GPIO3(出水TDS) - 都使用ADC1，与WiFi兼容

static const adc_channel_t tds_adc_channels[TDS_SENSOR_COUNT] = {
    ADC_CHANNEL_2,  // 进水TDS - GPIO2 (ADC1_CH2)
    ADC_CHANNEL_3   // 出水TDS - GPIO3 (ADC1_CH3)
};

// 传感器名称
static const char* const sensor_names[] = {
    "进水TDS",
    "出水TDS"
};

// ==================== 私有函数 ====================

static float calculate_tds(float voltage, float temperature);
static float voltage_to_ec(float voltage);
static int adc_read_averaged(adc_channel_t channel);

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

    // 创建数据互斥锁
    tds_ctx.data_mutex = xSemaphoreCreateMutex();
    if (!tds_ctx.data_mutex) {
        ESP_LOGE(TAG, "创建数据互斥锁失败");
        adc_oneshot_del_unit(adc1_handle);
        return ESP_ERR_NO_MEM;
    }

    tds_ctx.initialized = true;
    return ESP_OK;
}

esp_err_t tds_sensor_deinit(void)
{
    if (!tds_ctx.initialized) {
        return ESP_OK;
    }

    tds_sensor_stop();

    if (tds_ctx.data_mutex) {
        vSemaphoreDelete(tds_ctx.data_mutex);
        tds_ctx.data_mutex = NULL;
    }

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
            // 更新最新测量值（加锁保护）
            if (xSemaphoreTake(tds_ctx.data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                memcpy(&tds_ctx.latest[TDS_SENSOR_INLET], &dual.inlet, sizeof(tds_measurement_t));
                memcpy(&tds_ctx.latest[TDS_SENSOR_OUTLET], &dual.outlet, sizeof(tds_measurement_t));
                xSemaphoreGive(tds_ctx.data_mutex);
            }

            ESP_LOGD(TAG, "进水: %.1f ppm, 出水: %.1f ppm, 去除率: %.1f%%",
                     dual.inlet.tds_value, dual.outlet.tds_value, dual.reduction_rate);

            // 检查报警（纯水洗膜期间跳过，因无水流导致测量无效）
            if (!tds_ctx.skip_alarm_detection) {
                float tds_values[TDS_SENSOR_COUNT] = {dual.inlet.tds_value, dual.outlet.tds_value};
                for (int i = 0; i < TDS_SENSOR_COUNT; i++) {
                    float tds_val = tds_values[i];
                    float threshold = tds_ctx.alarm_threshold[i];
                    float clear_threshold = threshold * (1.0f - ALARM_HYSTERESIS_PERCENT);  // 解除阈值=阈值*0.95

                    if (tds_val > threshold) {
                        // 超过报警阈值
                        if (!tds_ctx.alarm_active[i]) {
                            ESP_LOGW(TAG, "%s报警: %.1f > %.1f ppm",
                                     sensor_names[i],
                                     tds_val,
                                     threshold);
                            tds_ctx.alarm_active[i] = true;
                        }
                    } else if (tds_val < clear_threshold) {
                        // 低于解除阈值（比报警阈值低5%），带迟滞
                        if (tds_ctx.alarm_active[i]) {
                            ESP_LOGI(TAG, "%s报警已解除: %.1f < %.1f ppm (迟滞阈值)",
                                     sensor_names[i],
                                     tds_val,
                                     clear_threshold);
                            tds_ctx.alarm_active[i] = false;
                        }
                    }
                    // 介于阈值和解除阈值之间时，保持当前报警状态不变（迟滞区间）
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
        2048,  // 优化：ADC读取栈需求小，2048字节足够
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
    // 16个样本，每个最大4095，sum最大65520，远小于INT_MAX(2147483647)，不会溢出
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

    // 调试输出原始ADC值
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

    // 计算去除率（防止除零）
    if (dual_measurement->both_valid &&
        dual_measurement->inlet.tds_value > 1.0f) {  // 使用阈值避免极小值导致的精度问题
        dual_measurement->reduction_rate =
            (1.0f - dual_measurement->outlet.tds_value / dual_measurement->inlet.tds_value) * 100.0f;
        // 去除率范围限制：0~100%
        if (dual_measurement->reduction_rate < 0) dual_measurement->reduction_rate = 0;
        if (dual_measurement->reduction_rate > 100) dual_measurement->reduction_rate = 100;
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

    if (!tds_ctx.data_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(tds_ctx.data_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // 在mutex保护下复制整个结构体，包括valid标志
    memcpy(measurement, &tds_ctx.latest[sensor_id], sizeof(tds_measurement_t));
    bool valid = measurement->valid;
    xSemaphoreGive(tds_ctx.data_mutex);

    if (!valid) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t tds_sensor_get_latest_dual(tds_dual_measurement_t *dual_measurement)
{
    if (!dual_measurement) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!tds_ctx.data_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(tds_ctx.data_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memcpy(&dual_measurement->inlet, &tds_ctx.latest[TDS_SENSOR_INLET], sizeof(tds_measurement_t));
    memcpy(&dual_measurement->outlet, &tds_ctx.latest[TDS_SENSOR_OUTLET], sizeof(tds_measurement_t));
    dual_measurement->both_valid = (tds_ctx.latest[TDS_SENSOR_INLET].valid &&
                                    tds_ctx.latest[TDS_SENSOR_OUTLET].valid);
    xSemaphoreGive(tds_ctx.data_mutex);

    if (dual_measurement->both_valid && dual_measurement->inlet.tds_value > 1.0f) {
        dual_measurement->reduction_rate =
            (1.0f - dual_measurement->outlet.tds_value / dual_measurement->inlet.tds_value) * 100.0f;
        // 去除率范围限制：0~100%
        if (dual_measurement->reduction_rate < 0) dual_measurement->reduction_rate = 0;
        if (dual_measurement->reduction_rate > 100) dual_measurement->reduction_rate = 100;
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

    if (!tds_ctx.data_mutex) {
        return false;
    }

    bool result = false;
    if (xSemaphoreTake(tds_ctx.data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // 使用alarm_active状态（带迟滞机制），与任务内部状态一致
        result = tds_ctx.alarm_active[sensor_id];
        xSemaphoreGive(tds_ctx.data_mutex);
    }
    return result;
}

void tds_sensor_set_skip_alarm_detection(bool skip)
{
    tds_ctx.skip_alarm_detection = skip;
    if (skip) {
        ESP_LOGD(TAG, "跳过报警检测（纯水洗膜期间）");
    }
}

// ==================== 温度 ====================

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

    float inlet_tds = 0, outlet_tds = 0;
    if (tds_ctx.data_mutex && xSemaphoreTake(tds_ctx.data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        inlet_tds = tds_ctx.latest[TDS_SENSOR_INLET].valid ?
                    tds_ctx.latest[TDS_SENSOR_INLET].tds_value : 0;
        outlet_tds = tds_ctx.latest[TDS_SENSOR_OUTLET].valid ?
                     tds_ctx.latest[TDS_SENSOR_OUTLET].tds_value : 0;
        xSemaphoreGive(tds_ctx.data_mutex);
    }

    snprintf(buffer, buffer_size,
             "进水TDS: %.1f ppm (阈值: %.1f)\n"
             "出水TDS: %.1f ppm (阈值: %.1f)\n"
             "去除率: %.1f%%\n"
             "温度: %.1f°C",
             inlet_tds, tds_ctx.alarm_threshold[TDS_SENSOR_INLET],
             outlet_tds, tds_ctx.alarm_threshold[TDS_SENSOR_OUTLET],
             (inlet_tds > 0) ? (1.0f - outlet_tds / inlet_tds) * 100 : 0,
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

    // 温度补偿 (系数约2%/°C)：温度升高时电导率增加
    // 将实测值补偿到25°C参考值（除法而非乘法）
    // denom > 1 表示温度高于25°C，实测值偏大，需除以系数补偿到25°C等效值
    float temp_coeff = 1.0f + 0.02f * (temperature - 25.0f);
    if (temp_coeff < 0.5f) {
        temp_coeff = 0.5f;  // 防止极低温度导致系数过小
    }
    tds = tds / temp_coeff;  // 正确：除以系数，温度高于25°C时TDS值降低到25°C等效值

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
    float denom = 1.0f + temp_coeff * (temperature - 25.0f);
    if (denom < 0.5f) denom = 0.5f;
    return ec / denom;
}