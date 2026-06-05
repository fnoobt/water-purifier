/**
 * @file pm_manager.c
 * @brief 电源管理模块实现 (ESP32-C3专用)
 * @note WiFi TX功率动态调整、堆内存监控
 *       ESP32-C3不支持DFS动态频率调节，CPU固定160MHz
 */

#include "pm_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "PM";

// ==================== 状态 ====================

static portMUX_TYPE pm_spinlock = portMUX_INITIALIZER_UNLOCKED;

static struct {
    bool initialized;
    uint8_t current_wifi_tx_power; // 当前WiFi TX功率(0.25dBm单位)
    bool wifi_tx_adjusted;         // 是否已调整过WiFi功率
} s_ctx = {0};

// ==================== 初始化 ====================

esp_err_t pm_manager_init(void)
{
    if (s_ctx.initialized) {
        return ESP_OK;
    }

    // 注意：PM manager在WiFi之前初始化，无法读取实际功率值
    // 初始值设为80(20dBm)，后续由adjust_wifi_tx_power()读取实际值
    taskENTER_CRITICAL(&pm_spinlock);
    s_ctx.current_wifi_tx_power = 80;  // 20dBm默认值，WiFi启动后会被实际值替换
    s_ctx.wifi_tx_adjusted = false;
    s_ctx.initialized = true;
    taskEXIT_CRITICAL(&pm_spinlock);

    ESP_LOGI(TAG, "电源管理已启用: WiFi TX默认20dBm");
    return ESP_OK;
}

uint8_t pm_manager_get_wifi_tx_power(void)
{
    taskENTER_CRITICAL(&pm_spinlock);
    uint8_t power = s_ctx.current_wifi_tx_power;
    taskEXIT_CRITICAL(&pm_spinlock);
    return power;
}

esp_err_t pm_manager_set_cpu_mode(bool low_power)
{
    // ESP32-C3不支持DFS动态频率调节，CPU固定160MHz
    // 此函数保留接口兼容性，但不执行任何操作
    (void)low_power;
    return ESP_OK;
}

// ==================== WiFi TX功率 ====================

/**
 * @brief WiFi TX功率滞回表
 *
 * ESP32-C3只支持特定的离散功率值，API输入值与实际功率有映射关系：
 * - 输入范围[34,43] → 实际值34 → 8dBm
 * - 输入范围[44,51] → 实际值44 → 11dBm
 * - 输入范围[60,65] → 实际值60 → 15dBm
 * - 输入范围[72,79] → 实际值72 → 18dBm
 * - 输入范围[80,84] → 实际值80 → 20dBm
 *
 * up_rssi: 低于此值 → 升一档功率（信号变差确认）
 * down_rssi: 高于此值 → 降一档功率（信号变好确认）
 * tx_dbm:  该档位的目标功率(dBm)，使用ESP32-C3支持的离散值
 * tx_qdbm: API输入值(0.25dBm单位)，直接传给esp_wifi_set_max_tx_power()
 *
 * 滞回效果：RSSI在边界附近波动时保持当前功率，
 * 只有信号明显变化（跨越滞回窗口）才切换功率。
 */
static const struct {
    int8_t up_rssi;    // 升功率阈值（低于此值升一档）
    int8_t down_rssi;  // 降功率阈值（高于此值降一档）
    int8_t tx_dbm;     // 目标功率(dBm) - ESP32-C3支持的离散值
    uint8_t tx_qdbm;   // API输入值(0.25dBm) - 直接传给API
} s_pwr_table[] = {
    { -55, -48,  8,  34 },  // 8dBm:  RSSI<-55升到11dBm, >=-48保持
    { -60, -50, 11,  44 },  // 11dBm: RSSI<-60升到15dBm, >=-50降到8dBm
    { -65, -53, 15,  60 },  // 15dBm: RSSI<-65升到18dBm, >=-53降到11dBm
    { -75, -58, 18,  72 },  // 18dBm: RSSI<-75升到20dBm, >=-58降到15dBm
    { -99, -68, 20,  80 },  // 20dBm: >=-68降到18dBm, 不自动升更高
};

#define PWR_TABLE_SIZE (sizeof(s_pwr_table) / sizeof(s_pwr_table[0]))

esp_err_t pm_manager_adjust_wifi_tx_power(int8_t rssi)
{
    taskENTER_CRITICAL(&pm_spinlock);
    if (!s_ctx.initialized) {
        taskEXIT_CRITICAL(&pm_spinlock);
        return ESP_ERR_INVALID_STATE;
    }
    taskEXIT_CRITICAL(&pm_spinlock);

    // 获取当前实际功率值（直接从WiFi驱动读取）
    int8_t actual_power = 0;
    esp_err_t ret = esp_wifi_get_max_tx_power(&actual_power);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "无法获取当前TX功率: %s", esp_err_to_name(ret));
        return ret;
    }

    // 查找当前功率档位
    int current_idx = -1;
    for (int i = 0; i < PWR_TABLE_SIZE; i++) {
        if (s_pwr_table[i].tx_qdbm == actual_power) {
            current_idx = i;
            break;
        }
    }

    if (current_idx < 0) {
        // 当前功率不在表中，默认使用最大功率档
        current_idx = PWR_TABLE_SIZE - 1;
        ESP_LOGD(TAG, "当前功率%d(%.2fdBm)不在表中，默认最大档",
                 actual_power, actual_power / 4.0f);
    }

    // 滞回判断
    int new_idx = current_idx;
    if (rssi < s_pwr_table[current_idx].up_rssi) {
        // 信号变差，升一档功率
        new_idx = (current_idx + 1 < PWR_TABLE_SIZE) ? current_idx + 1 : current_idx;
    } else if (rssi >= s_pwr_table[current_idx].down_rssi) {
        // 信号变好，降一档功率
        new_idx = (current_idx > 0) ? current_idx - 1 : current_idx;
    }

    if (new_idx == current_idx) {
        return ESP_OK;  // 无需调整
    }

    // 设置新功率（使用正确的API输入值）
    uint8_t new_power = s_pwr_table[new_idx].tx_qdbm;
    ret = esp_wifi_set_max_tx_power(new_power);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TX功率设置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 更新状态
    taskENTER_CRITICAL(&pm_spinlock);
    s_ctx.current_wifi_tx_power = new_power;
    s_ctx.wifi_tx_adjusted = true;
    taskEXIT_CRITICAL(&pm_spinlock);

    ESP_LOGI(TAG, "WiFi TX功率调整: RSSI=%ddBm, %ddBm→%ddBm",
             rssi, s_pwr_table[current_idx].tx_dbm, s_pwr_table[new_idx].tx_dbm);
    return ESP_OK;
}

// ==================== 堆内存监控 ====================

esp_err_t pm_manager_check_heap(void)
{
    size_t free_heap = esp_get_free_heap_size();
    size_t min_ever_heap = esp_get_minimum_free_heap_size();

    // 低于15KB立即重启（跳过NVS保存，避免进一步消耗内存）
    if (free_heap < 15360) {
        ESP_LOGE(TAG, "堆内存严重不足! 可用=%u bytes, 历史最低=%u bytes, 即将重启",
                 (unsigned)free_heap, (unsigned)min_ever_heap);
        esp_restart();
        return ESP_ERR_NO_MEM;
    }

    // 低于30KB告警
    if (free_heap < 30720) {
        ESP_LOGW(TAG, "堆内存不足! 可用=%u bytes, 历史最低=%u bytes",
                 (unsigned)free_heap, (unsigned)min_ever_heap);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}