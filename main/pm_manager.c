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

    ESP_LOGI(TAG, "初始化电源管理...");
    ESP_LOGI(TAG, "ESP32-C3不支持DFS，CPU固定160MHz");

    taskENTER_CRITICAL(&pm_spinlock);
    s_ctx.current_wifi_tx_power = 80;  // 20dBm (80 * 0.25 = 20)
    s_ctx.wifi_tx_adjusted = false;
    s_ctx.initialized = true;
    taskEXIT_CRITICAL(&pm_spinlock);

    ESP_LOGI(TAG, "电源管理已启用: WiFi TX初始20dBm");
    return ESP_OK;
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
 * up_rssi: 低于此值 → 升一档功率（信号变差确认）
 * down_rssi: 高于此值 → 降一档功率（信号变好确认）
 * tx_dbm:  该档位的目标功率(dBm)
 *
 * 滞回效果：RSSI在边界附近波动时保持当前功率，
 * 只有信号明显变化（跨越滞回窗口）才切换功率。
 */
static const struct {
    int8_t up_rssi;    // 升功率阈值（低于此值升一档）
    int8_t down_rssi;  // 降功率阈值（高于此值降一档）
    int8_t tx_dbm;     // 目标功率(dBm)
} s_pwr_table[] = {
    { -55, -48,  8 },   // 8dBm:  RSSI<-55升到12, >=-48保持
    { -60, -50, 12 },   // 12dBm: RSSI<-60升到15, >=-50降到8
    { -65, -53, 15 },   // 15dBm: RSSI<-65升到19, >=-53降到12
    { -75, -58, 19 },   // 19dBm: RSSI<-75升到20, >=-58降到15
    { -99, -68, 20 },   // 20dBm: >=-68降到19, 不自动升更高
};

#define PWR_TABLE_SIZE (sizeof(s_pwr_table) / sizeof(s_pwr_table[0]))

esp_err_t pm_manager_adjust_wifi_tx_power(int8_t rssi)
{
    taskENTER_CRITICAL(&pm_spinlock);
    if (!s_ctx.initialized) {
        taskEXIT_CRITICAL(&pm_spinlock);
        return ESP_ERR_INVALID_STATE;
    }

    // 根据当前功率档位，用滞回判断是否切换
    int8_t current_dbm = s_ctx.current_wifi_tx_power / 4;
    taskEXIT_CRITICAL(&pm_spinlock);

    int8_t new_dbm = current_dbm;

    for (int i = 0; i < PWR_TABLE_SIZE; i++) {
        if (s_pwr_table[i].tx_dbm == current_dbm) {
            if (rssi < s_pwr_table[i].up_rssi) {
                // 信号变差，升一档功率
                new_dbm = (i + 1 < PWR_TABLE_SIZE) ? s_pwr_table[i + 1].tx_dbm : current_dbm;
            } else if (rssi >= s_pwr_table[i].down_rssi) {
                // 信号变好，降一档功率
                new_dbm = (i > 0) ? s_pwr_table[i - 1].tx_dbm : current_dbm;
            }
            break;
        }
    }

    if (new_dbm == current_dbm) {
        return ESP_OK;
    }

    uint8_t tx_power_qdbm = (uint8_t)(new_dbm * 4);
    esp_err_t ret = esp_wifi_set_max_tx_power(tx_power_qdbm);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi TX功率设置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 更新状态
    taskENTER_CRITICAL(&pm_spinlock);
    s_ctx.current_wifi_tx_power = tx_power_qdbm;
    s_ctx.wifi_tx_adjusted = true;
    taskEXIT_CRITICAL(&pm_spinlock);

    ESP_LOGD(TAG, "WiFi TX功率调整: RSSI=%ddBm -> %ddBm", rssi, new_dbm);
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