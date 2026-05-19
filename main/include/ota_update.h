/**
 * @file ota_update.h
 * @brief OTA 固件升级模块接口
 * @note 使用 esp_ota_ops API，通过 HTTP 上传固件实现本地 OTA 升级
 */

#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief OTA 升级状态
 */
typedef enum {
    OTA_STATE_IDLE = 0,        // 空闲
    OTA_STATE_UPLOADING,       // 正在上传
    OTA_STATE_WRITING,         // 正在写入 Flash
    OTA_STATE_COMPLETE,        // 升级完成，等待重启
    OTA_STATE_FAILED,          // 升级失败
} ota_state_t;

/**
 * @brief 初始化 OTA 模块（验证 OTA 分区存在，读取当前固件版本）
 * @return ESP_OK 成功
 */
esp_err_t ota_update_init(void);

/**
 * @brief 开始 OTA 会话（找到下一个 OTA 分区，准备写入）
 * @return ESP_OK 成功
 */
esp_err_t ota_update_begin(void);

/**
 * @brief 写入固件数据块（流式写入，不缓冲整个固件）
 * @param data 固件数据
 * @param len 数据长度
 * @return ESP_OK 成功
 */
esp_err_t ota_update_write(const uint8_t *data, size_t len);

/**
 * @brief 完成 OTA（校验镜像完整性，设置启动分区）
 * @note 调用成功后应尽快调用 esp_restart()
 * @return ESP_OK 校验通过，已设置启动分区
 */
esp_err_t ota_update_end(void);

/**
 * @brief 中止 OTA（清理 OTA 句柄和状态）
 */
void ota_update_abort(void);

/**
 * @brief 获取当前 OTA 状态
 * @return OTA 状态
 */
ota_state_t ota_update_get_state(void);

/**
 * @brief 获取已写入的固件字节数
 * @return 已写字节数
 */
uint32_t ota_update_get_bytes_written(void);

/**
 * @brief 获取当前运行的固件版本字符串
 * @return 版本字符串（静态缓冲，调用者勿修改）
 */
const char *ota_update_get_running_version(void);

/**
 * @brief 恢复到factory出厂分区
 * @note 设置启动分区为factory并重启
 * @return ESP_OK 成功，ESP_ERR_NOT_FOUND factory分区不存在
 */
esp_err_t ota_update_revert_to_factory(void);

/**
 * @brief 回滚到上一个OTA固件
 * @note 从ota_0切换到ota_1或反之，检查目标分区固件有效性
 * @return ESP_OK 成功，ESP_ERR_NOT_FOUND 无可回滚分区，ESP_ERR_INVALID_STATE 目标分区无效
 */
esp_err_t ota_update_rollback(void);

#ifdef __cplusplus
}
#endif

#endif // OTA_UPDATE_H
