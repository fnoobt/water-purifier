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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "PM";

// 功率调整冷却期：调整后至少等待5分钟再允许新的调整
#define PM_ADJUST_COOLDOWN_US  (5 * 60 * 1000LL)

// ==================== 状态 ====================

static portMUX_TYPE pm_spinlock = portMUX_INITIALIZER_UNLOCKED;

static struct {
    bool initialized;
    uint8_t current_wifi_tx_power; // 当前WiFi TX功率(0.25dBm单位)
    bool wifi_tx_adjusted;         // 是否已调整过WiFi功率
    int8_t pending_direction;      // 待调整方向: 0=无, 1=升, -1=降
    uint8_t pending_count;         // 连续超出阈值的次数计数
    int64_t last_adjust_time_us;   // 上次功率调整的时间戳(微秒)
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
    // 检查是否已初始化，避免返回未定义值
    if (!s_ctx.initialized) {
        ESP_LOGW(TAG, "pm_manager未初始化，返回默认功率值");
        return 80;  // 返回默认值20dBm (80 * 0.25)
    }
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
 * ESP32-C3的WiFi TX功率仅支持以下离散值（wifi_power_t枚举）：
 * -1, 2, 5, 8.5, 11, 13, 15, 17, 18.5, 19.5 dBm
 * API输入值(qdbm) = dBm × 4，非枚举值的输入会被硬件舍入到邻近有效值
 *
 * 本表选取5个有效离散值：8.5/11/15/18.5/20 dBm
 *
 * up_rssi: 低于此值 → 升一档功率（信号变差确认）
 * down_rssi: 高于此值 → 降一档功率（信号变好确认）
 * tx_dbm:  该档位的目标功率(dBm)
 * tx_qdbm: API输入值(0.25dBm单位)，直接传给esp_wifi_set_max_tx_power()
 *
 * 滞回设计原则：
 *   1. 每档自滞回窗口 = 15dB（down - up = 15），产生相邻档位间的死区
 *   2. 死区 = [上一档down_rssi, 当前档up_rssi]，RSSI在死区内时不切换
 *   3. 死区覆盖典型RSSI波动范围（~10dB），防止边界振荡
 *   4. 冷却机制：调整成功后5分钟内不再启动新的调整计数
 * 连续确认机制：RSSI需连续3次超出阈值才触发调整
 *
 * 档位布局（15dB滞回 + 5dB死区）：
 *   20dBm   | up≤-95  down≥-75
 *   18.5dBm | up≤-90  down≥-65   死区: -90~-75
 *   15dBm   | up≤-80  down≥-55   死区: -80~-65
 *   11dBm   | up≤-70  down≥-45   死区: -70~-55
 *   8.5dBm  | up≤-60  down≥-45   死区: -60~-45
 */
static const struct {
    int8_t up_rssi;    // 升功率阈值（低于此值升一档）
    int8_t down_rssi;  // 降功率阈值（高于此值降一档）
    int8_t tx_dbm;     // 目标功率(dBm) - 对应wifi_power_t离散值
    uint8_t tx_qdbm;   // API输入值(0.25dBm) - 直接传给API
} s_pwr_table[] = {
    { -60, -45,  8,  34 },  // 8.5dBm:  RSSI<-60升, ≥-45降  (自滞回15dB)
    { -70, -45, 11,  44 },  // 11dBm:   RSSI<-70升, ≥-45降  (自滞回15dB, 死区-60~-45)
    { -80, -55, 15,  60 },  // 15dBm:   RSSI<-80升, ≥-55降  (自滞回15dB, 死区-70~-55)
    { -90, -65, 18,  74 },  // 18.5dBm: RSSI<-90升, ≥-65降  (自滞回15dB, 死区-80~-65)
    { -95, -75, 20,  80 },  // 20dBm:   RSSI<-95升, ≥-75降  (自滞回15dB, 死区-90~-75)
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

    // 冷却期检查：调整成功后5分钟内不再启动新的调整计数
    if (s_ctx.wifi_tx_adjusted) {
        int64_t now = esp_timer_get_time();
        if (now - s_ctx.last_adjust_time_us < PM_ADJUST_COOLDOWN_US) {
            return ESP_OK;  // 冷却期内，跳过调整
        }
    }

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
        // 当前功率不在表中（可能被外部修改），默认使用中间档位
        // 选择中间档位而非最大功率，避免功耗过高；后续滞回算法会自动调整到合适档位
        current_idx = PWR_TABLE_SIZE / 2;  // 动态取中间档位
        ESP_LOGD(TAG, "当前功率%d(%.2fdBm)不在表中，默认中间档位%ddBm",
                 actual_power, actual_power / 4.0f, s_pwr_table[current_idx].tx_dbm);
    }

    // 滞回判断
    int new_idx = current_idx;
    int8_t direction = 0;  // 0=无变化, 1=升功率, -1=降功率
    if (rssi < s_pwr_table[current_idx].up_rssi) {
        // 信号变差，需要升功率
        new_idx = (current_idx + 1 < PWR_TABLE_SIZE) ? current_idx + 1 : current_idx;
        direction = 1;
    } else if (rssi >= s_pwr_table[current_idx].down_rssi) {
        // 信号变好，需要降功率
        new_idx = (current_idx > 0) ? current_idx - 1 : current_idx;
        direction = -1;
    }

    if (new_idx == current_idx) {
        // RSSI在滞回窗口内，重置计数器
        taskENTER_CRITICAL(&pm_spinlock);
        s_ctx.pending_direction = 0;
        s_ctx.pending_count = 0;
        taskEXIT_CRITICAL(&pm_spinlock);
        return ESP_OK;  // 无需调整
    }

    // 连续确认机制：需要连续3次超出阈值才触发调整
    taskENTER_CRITICAL(&pm_spinlock);
    if (s_ctx.pending_direction != direction) {
        // 方向改变，重置计数器
        s_ctx.pending_direction = direction;
        s_ctx.pending_count = 1;
        taskEXIT_CRITICAL(&pm_spinlock);
        ESP_LOGD(TAG, "RSSI=%ddBm超出阈值，方向=%s，计数=1/3",
                 rssi, direction > 0 ? "升功率" : "降功率");
        return ESP_OK;  // 需要继续观察
    }

    s_ctx.pending_count++;
    if (s_ctx.pending_count < 3) {
        taskEXIT_CRITICAL(&pm_spinlock);
        ESP_LOGD(TAG, "RSSI=%ddBm超出阈值，方向=%s，计数=%d/3",
                 rssi, direction > 0 ? "升功率" : "降功率", s_ctx.pending_count);
        return ESP_OK;  // 需要继续观察
    }
    taskEXIT_CRITICAL(&pm_spinlock);

    // 连续3次确认，执行功率调整
    uint8_t new_power = s_pwr_table[new_idx].tx_qdbm;
    ret = esp_wifi_set_max_tx_power(new_power);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TX功率设置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 更新状态并重置计数器（在spinlock内重新验证当前功率，防止并发修改）
    taskENTER_CRITICAL(&pm_spinlock);
    s_ctx.current_wifi_tx_power = new_power;
    s_ctx.wifi_tx_adjusted = true;
    s_ctx.last_adjust_time_us = esp_timer_get_time();  // 记录调整时间，启动冷却期
    s_ctx.pending_direction = 0;
    s_ctx.pending_count = 0;
    // 更新日志所需的功率值（使用spinlock内的实际值，而非函数开头读取的值）
    int8_t old_dbm = s_pwr_table[current_idx].tx_dbm;
    taskEXIT_CRITICAL(&pm_spinlock);

    ESP_LOGI(TAG, "WiFi TX功率调整: RSSI=%ddBm, %d→%ddBm (连续3次确认)",
             rssi, old_dbm, s_pwr_table[new_idx].tx_dbm);
    return ESP_OK;
}

// ==================== 堆内存监控 ====================

esp_err_t pm_manager_check_heap(void)
{
    size_t free_heap = esp_get_free_heap_size();
    size_t min_ever_heap = esp_get_minimum_free_heap_size();

    // 低于15KB立即重启（直接重启，不尝试清理资源）
    // 原因：内存严重不足时 fsm_send_event/wifi_manager_stop 等函数自身需要分配内存，
    // 极可能失败；且100ms延时不足以让复杂任务退出，反而增加卡死风险。
    // esp_restart() 由 ROM bootloader 执行，不依赖堆内存，是最可靠的重启方式。
    if (free_heap < 15360) {
        ESP_LOGE(TAG, "堆内存严重不足! 可用=%u bytes, 历史最低=%u bytes, 立即重启",
                 (unsigned)free_heap, (unsigned)min_ever_heap);
        esp_restart();
        return ESP_ERR_NO_MEM;  // 不可达，保持函数完整性
    }

    // 低于30KB告警
    if (free_heap < 30720) {
        ESP_LOGW(TAG, "堆内存不足! 可用=%u bytes, 历史最低=%u bytes",
                 (unsigned)free_heap, (unsigned)min_ever_heap);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}