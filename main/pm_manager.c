/**
 * @file pm_manager.c
 * @brief 电源管理模块实现
 * @note CPU动态频率切换、WiFi TX功率动态调整、堆内存监控
 */

#include "pm_manager.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include "esp_system.h"

// 外部模块声明（用于重启前同步NVS）
extern esp_err_t config_manager_save(void);
extern bool config_manager_periodic_save_all(uint32_t min_interval_sec);

static const char *TAG = "PM";

// ==================== 状态 ====================

static struct {
    bool initialized;
    bool cpu_low_power;          // 当前CPU模式
    int8_t last_rssi;            // 上次RSSI
    uint8_t current_wifi_tx_power; // 当前WiFi TX功率(0.25dBm单位)
    bool wifi_tx_adjusted;       // 是否已调整过WiFi功率
    esp_pm_lock_handle_t cpu_freq_lock; // CPU频率锁
} s_ctx = {0};

// ==================== CPU频率 ====================

esp_err_t pm_manager_init(void)
{
    if (s_ctx.initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "初始化电源管理...");

    // 启用DFS（动态频率调节），最小80MHz，最大160MHz
    // ESP32-C3不支持light sleep，必须设置为false
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,  // ESP32-C3不支持light sleep
    };
    esp_err_t ret = esp_pm_configure(&pm_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "电源管理配置失败: %s", esp_err_to_name(ret));
        // 不返回错误，继续初始化其他部分（电源管理可选）
    }

    // 创建CPU频率锁（用于强制保持160MHz）
    // ESP32-C3使用ESP_PM_APB_FREQ_MAX锁保持高频
    ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "cpu_freq", &s_ctx.cpu_freq_lock);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CPU频率锁创建失败: %s", esp_err_to_name(ret));
        // 继续初始化，频率锁可选
    }

    // 初始不持有锁，让DFS自动管理（空闲时会降到80MHz）
    s_ctx.cpu_low_power = true;

    // WiFi TX功率初始为默认值（~19.5dBm = 78 * 0.25dBm）
    s_ctx.current_wifi_tx_power = 78;  // 19.5dBm
    s_ctx.wifi_tx_adjusted = false;

    ESP_LOGI(TAG, "电源管理已启用: DFS 80MHz~160MHz (C3无light sleep)");
    s_ctx.initialized = true;
    return ESP_OK;
}

esp_err_t pm_manager_set_cpu_mode(bool low_power)
{
    if (!s_ctx.initialized || s_ctx.cpu_freq_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ctx.cpu_low_power == low_power) {
        return ESP_OK;  // 模式未变，无需切换
    }

    esp_err_t ret;
    if (low_power) {
        // 释放锁，让DFS自动降到80MHz
        ret = esp_pm_lock_release(s_ctx.cpu_freq_lock);
        if (ret == ESP_OK) {
            s_ctx.cpu_low_power = true;
            ESP_LOGD(TAG, "CPU频率降至80MHz（低功耗模式）");
        }
    } else {
        // 持有锁，强制保持160MHz
        ret = esp_pm_lock_acquire(s_ctx.cpu_freq_lock);
        if (ret == ESP_OK) {
            s_ctx.cpu_low_power = false;
            ESP_LOGD(TAG, "CPU频率恢复至160MHz（高性能模式）");
        }
    }

    return ret;
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
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // 根据当前功率档位，用滞回判断是否切换
    int8_t current_dbm = s_ctx.current_wifi_tx_power / 4;
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
    ESP_LOGI(TAG, "WiFi TX功率调整: RSSI=%ddBm -> %d.%ddBm",
             rssi, tx_power_qdbm / 4, (tx_power_qdbm % 4) * 25);
    s_ctx.current_wifi_tx_power = tx_power_qdbm;
    s_ctx.wifi_tx_adjusted = true;
    s_ctx.last_rssi = rssi;
    return ESP_OK;
}

// ==================== 堆内存监控 ====================

esp_err_t pm_manager_check_heap(void)
{
    size_t free_heap = esp_get_free_heap_size();
    size_t min_ever_heap = esp_get_minimum_free_heap_size();

    // 低于15KB立即重启
    if (free_heap < 15360) {
        ESP_LOGE(TAG, "堆内存严重不足! 可用=%u bytes, 历史最低=%u bytes, 即将重启",
                 (unsigned)free_heap, (unsigned)min_ever_heap);
        // 重启前尝试保存待写入的NVS数据（统一保存）
        config_manager_save();  // 系统配置
        config_manager_periodic_save_all(0);  // 滤芯+历史记录
        esp_restart();
        // 不会执行到这里
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
