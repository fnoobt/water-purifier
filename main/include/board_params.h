/**
 * @file board_params.h
 * @brief 净水器主控板硬件配置参数
 * @note 基于ESP32-C3开发板，75G RO膜 + 3G压力桶配置
 * @note 便于后续查询和修改硬件参数
 */

#ifndef BOARD_PARAMS_H
#define BOARD_PARAMS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 硬件配置信息 ==================== */

// 系统配置
#define BOARD_MODEL                "ESP32-C3"
#define BOARD_NAME                 "WaterPurifier-C3"
#define RO_MEMBRANE_SIZE           "75G"           // RO膜规格
#define PRESSURE_TANK_SIZE         "3G"            // 压力桶规格
#define FIRMWARE_VERSION           "1.0.0"

/* ==================== GPIO引脚分配 ==================== */
/**
 * @note GPIO引脚定义已移至 gpio_config.h
 * 请在代码中包含 gpio_config.h 获取引脚配置
 */

/* ==================== 压力开关参数 ==================== */

/**
 * 高压开关参数（RO膜保护）
 * - 闭合压力：0.15±0.05 MPa (1.5±0.5 bar)
 * - 断开压力：0.25±0.05 MPa (2.5±0.5 bar)
 * - 作用：当压力过高时断开，保护RO膜
 */
#define HIGH_PRESSURE_CLOSE_MIN     0.10f     // 1.0 bar
#define HIGH_PRESSURE_CLOSE_NOM     0.15f     // 1.5 bar
#define HIGH_PRESSURE_CLOSE_MAX     0.20f     // 2.0 bar
#define HIGH_PRESSURE_OPEN_MIN      0.20f     // 2.0 bar
#define HIGH_PRESSURE_OPEN_NOM      0.25f     // 2.5 bar
#define HIGH_PRESSURE_OPEN_MAX      0.30f     // 3.0 bar

/**
 * 低压开关参数（原水保护）
 * - 闭合压力：0.05±0.05 MPa (0.5±0.5 bar)
 * - 断开压力：0.03±0.03 MPa (0.3±0.3 bar)
 * - 作用：当压力过低时断开，保护增压泵
 */
#define LOW_PRESSURE_CLOSE_MIN      0.00f     // 0.0 bar
#define LOW_PRESSURE_CLOSE_NOM      0.05f     // 0.5 bar
#define LOW_PRESSURE_CLOSE_MAX      0.10f     // 1.0 bar
#define LOW_PRESSURE_OPEN_MIN       0.00f     // 0.0 bar
#define LOW_PRESSURE_OPEN_NOM       0.03f     // 0.3 bar
#define LOW_PRESSURE_OPEN_MAX       0.06f     // 0.6 bar

/**
 * 压力桶开关参数
 * - 闭合压力：0.1 MPa (1.0 bar) - 桶满，停止制水
 * - 断开压力：0.25 MPa (2.5 bar) - 桶空，开始制水
 * - 3G压力桶有效存水量：约 2.3-3.4 升
 */
#define TANK_PRESSURE_CLOSE         0.10f     // 1.0 bar (满水)
#define TANK_PRESSURE_OPEN          0.25f     // 2.5 bar (空水)
#define TANK_CAPACITY_LITERS        3.0f      // 3加仑 ≈ 11.4升
#define TANK_USABLE_WATER_MIN       2.3f      // 最小可用水量 (升)
#define TANK_USABLE_WATER_MAX       3.4f      // 最大可用水量 (升)

/* ==================== 工作时间参数 ==================== */
/**
 * @note 冲洗和时间参数已移至运行时配置（config_manager），网页可配置：
 * - 常规冲洗时间：默认30秒（范围5-300秒）
 * - 纯水洗膜时间：默认20秒（范围5-300秒）
 * - 短制水判断阈值：默认180秒（范围30-600秒）
 * - 水锤开阀延时：默认1000ms（范围100-5000ms）
 * - 水锤停泵延时：默认1000ms（范围100-5000ms）
 * - 水锤关阀延时：默认500ms（范围100-5000ms）
 */

// RO膜制水速率 (默认75G，运行时根据配置动态设置)
// 汇通Vontron实际参数: 50G=7.8, 75G=12.0, 100G=15.6, 200G=31.2, 400G=62.4 L/h

/* ==================== TDS传感器参数 ==================== */

#define TDS_ALARM_THRESHOLD        100.0f    // TDS报警阈值 (ppm)
#define TDS_SAMPLE_INTERVAL_MS      1000      // TDS采样间隔：1秒
#define TDS_FILTER_ALPHA            0.1f      // 滤波系数 (0-1)

/* ==================== 系统限制 ==================== */

#define MAX_PRODUCTION_CYCLES       999       // 最大制水循环次数
#define MAX_FLUSH_CYCLES_PER_DAY    12        // 每日最大冲洗次数
#define MAX_ERROR_COUNT             5         // 最大错误计数

#ifdef __cplusplus
}
#endif

#endif // BOARD_PARAMS_H
