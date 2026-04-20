/**
 * @file pm_manager.h
 * @brief 电源管理模块接口
 * @note 支持CPU动态频率切换、WiFi TX功率动态调整、堆内存监控
 */

#ifndef PM_MANAGER_H
#define PM_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化电源管理（启用DFS自动频率调节）
 * @return ESP_OK 成功
 */
esp_err_t pm_manager_init(void);

/**
 * @brief 设置CPU频率模式
 * @param low_power true=低功耗模式(80MHz), false=高性能模式(160MHz)
 * @return ESP_OK 成功
 */
esp_err_t pm_manager_set_cpu_mode(bool low_power);

/**
 * @brief 根据RSSI动态调整WiFi TX功率
 * @note RSSI > -60dBm 降低功率, RSSI < -75dBm 恢复最大功率
 * @param rssi 当前RSSI值(dBm)
 * @return ESP_OK 成功
 */
esp_err_t pm_manager_adjust_wifi_tx_power(int8_t rssi);

/**
 * @brief 检查堆内存使用情况
 * @note 低于30KB告警，低于15KB自动重启
 * @return ESP_OK=正常, ESP_ERR_NO_MEM=低于告警阈值
 */
esp_err_t pm_manager_check_heap(void);

#ifdef __cplusplus
}
#endif

#endif // PM_MANAGER_H
