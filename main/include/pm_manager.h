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
 * @note ESP32-C3只支持离散功率值: 8.5, 11, 15, 18.5, 20 dBm
 *       使用滞回算法+冷却机制避免频繁切换：
 *       - 15dB滞回窗口，相邻档位间产生5dB死区
 *       - RSSI < up_rssi: 升一档功率（信号变差）
 *       - RSSI >= down_rssi: 降一档功率（信号变好）
 *       - RSSI在死区内: 不切换（防止边界振荡）
 *       - 调整后5分钟冷却期，冷却期内不启动新的调整计数
 * @param rssi 当前RSSI值(dBm)
 * @return ESP_OK 成功
 */
esp_err_t pm_manager_adjust_wifi_tx_power(int8_t rssi);

/**
 * @brief 获取当前WiFi TX功率
 * @return 当前功率(0.25dBm单位)，如34=8.5dBm实际8dBm，80=20dBm
 *         返回0表示未初始化
 */
uint8_t pm_manager_get_wifi_tx_power(void);

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
