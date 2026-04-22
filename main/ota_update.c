/**
 * @file ota_update.c
 * @brief OTA 固件升级模块实现
 * @note 使用 esp_ota_ops API 流式写入固件，支持同版本检测和 Rollback
 */

#include "ota_update.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include <string.h>

static const char *TAG = "OTA";

// ==================== 私有状态 ====================

static struct {
    ota_state_t state;
    esp_ota_handle_t ota_handle;
    const esp_partition_t *update_partition;
    uint32_t total_written;
    bool image_header_checked;
    char running_version[33];

    // 初始缓冲：积累足够数据以解析 ESP 镜像头
    uint8_t init_buf[512];
    size_t init_len;
} s_ctx = {
    .state = OTA_STATE_IDLE,
    .ota_handle = 0,
    .update_partition = NULL,
    .total_written = 0,
    .image_header_checked = false,
    .running_version = {0},
    .init_buf = {0},
    .init_len = 0,
};

// ==================== 初始化 ====================

esp_err_t ota_update_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGE(TAG, "无法获取当前运行分区");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "当前运行分区: %s (地址 0x%lx, 大小 %lu KB)",
             running->label, running->address, running->size / 1024);

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        ESP_LOGE(TAG, "未找到 OTA 分区，请检查分区表配置");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "下一个 OTA 分区: %s (地址 0x%lx, 大小 %lu KB)",
             next->label, next->address, next->size / 1024);

    // 读取当前固件版本
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc) {
        strncpy(s_ctx.running_version, app_desc->version, sizeof(s_ctx.running_version) - 1);
        ESP_LOGI(TAG, "当前固件版本: %s", s_ctx.running_version);
    } else {
        strncpy(s_ctx.running_version, "unknown", sizeof(s_ctx.running_version) - 1);
    }

    s_ctx.state = OTA_STATE_IDLE;
    ESP_LOGI(TAG, "OTA 模块初始化完成");
    return ESP_OK;
}

// ==================== OTA 会话 ====================

esp_err_t ota_update_begin(void)
{
    if (s_ctx.state != OTA_STATE_IDLE && s_ctx.state != OTA_STATE_FAILED) {
        ESP_LOGW(TAG, "OTA 会话正在进行中，状态: %d", s_ctx.state);
        return ESP_ERR_INVALID_STATE;
    }

    // 清理旧状态，允许从失败中重试
    if (s_ctx.ota_handle != 0) {
        esp_ota_abort(s_ctx.ota_handle);
        s_ctx.ota_handle = 0;
    }

    // 注意：不在 begin 时调用 esp_ota_mark_app_valid_cancel_rollback
    // 如果当前固件正在 rollback 倒计时中，提前取消会导致 OTA 失败后失去回滚保护
    // 该操作应推迟到 ota_update_end() 成功后执行

    s_ctx.update_partition = esp_ota_get_next_update_partition(NULL);
    if (!s_ctx.update_partition) {
        ESP_LOGE(TAG, "未找到可用的 OTA 分区");
        s_ctx.state = OTA_STATE_FAILED;
        return ESP_ERR_NOT_FOUND;
    }

    s_ctx.total_written = 0;
    s_ctx.init_len = 0;
    s_ctx.image_header_checked = false;
    s_ctx.ota_handle = 0;
    s_ctx.state = OTA_STATE_UPLOADING;

    ESP_LOGI(TAG, "OTA 会话开始，目标分区: %s (0x%lx, %lu KB)",
             s_ctx.update_partition->label,
             s_ctx.update_partition->address,
             s_ctx.update_partition->size / 1024);

    return ESP_OK;
}

esp_err_t ota_update_write(const uint8_t *data, size_t len)
{
    if (s_ctx.state != OTA_STATE_UPLOADING && s_ctx.state != OTA_STATE_WRITING) {
        ESP_LOGW(TAG, "OTA 不在写入状态，当前: %d", s_ctx.state);
        return ESP_ERR_INVALID_STATE;
    }

    if (len == 0) {
        return ESP_OK;
    }

    // 首次调用：积累数据直到能解析 ESP 镜像头
    if (!s_ctx.image_header_checked) {
        if (s_ctx.init_len == 0) {
            // 调试：打印前 64 字节的十六进制
            char hex[200];
            size_t hl = len > 64 ? 64 : len;
            for (size_t i = 0; i < hl; i++) {
                snprintf(hex + i * 3, 4, "%02x ", data[i]);
            }
            hex[hl * 3] = '\0';
            ESP_LOGD(TAG, "首次接收: len=%zu, magic=0x%02x, 前64字节=%s", len, data[0], hex);
        }

        // ESP镜像头所需的最小数据量（约280字节）
        size_t need = sizeof(esp_image_header_t) +
                      sizeof(esp_image_segment_header_t) +
                      sizeof(esp_app_desc_t);

        /* 安全检查：need必须小于init_buf容量 */
        if (need > sizeof(s_ctx.init_buf)) {
            ESP_LOGE(TAG, "镜像头大小异常: need=%zu > buf=%zu", need, sizeof(s_ctx.init_buf));
            s_ctx.state = OTA_STATE_FAILED;
            return ESP_ERR_INVALID_ARG;
        }

        /* 策略改进：只提取need字节到init_buf，多余数据直接处理
         * 避免因一次接收大数据块导致缓冲区溢出 */
        size_t copy_to_buf;
        size_t remaining_after_buf;

        if (s_ctx.init_len + len < need) {
            // 当前数据量仍不足need，全部缓存
            copy_to_buf = len;
            remaining_after_buf = 0;
        } else {
            // 数据量已足够，只提取need - init_len字节到缓冲区
            copy_to_buf = need - s_ctx.init_len;
            remaining_after_buf = len - copy_to_buf;
        }

        // 边界检查：确保不溢出init_buf（copy_to_buf已按need计算，不会溢出）
        if (copy_to_buf > sizeof(s_ctx.init_buf) - s_ctx.init_len) {
            ESP_LOGE(TAG, "init_buf 计算错误: copy=%zu > available=%zu",
                     copy_to_buf, sizeof(s_ctx.init_buf) - s_ctx.init_len);
            s_ctx.state = OTA_STATE_FAILED;
            return ESP_ERR_NO_MEM;
        }

        // 缓存需要的数据到init_buf
        if (copy_to_buf > 0) {
            memcpy(s_ctx.init_buf + s_ctx.init_len, data, copy_to_buf);
            s_ctx.init_len += copy_to_buf;
            s_ctx.total_written += copy_to_buf;
            data += copy_to_buf;
            len -= copy_to_buf;
        }

        // 检查是否已积累足够数据解析头部
        if (s_ctx.init_len < need) {
            return ESP_OK;  // 继续等待更多数据
        }

        // 解析 app descriptor
        const esp_app_desc_t *app_desc =
            (const esp_app_desc_t *)(s_ctx.init_buf + sizeof(esp_image_header_t) +
                                     sizeof(esp_image_segment_header_t));

        ESP_LOGI(TAG, "新固件: 版本=%s, 编译=%s %s",
                 app_desc->version, app_desc->date, app_desc->time);

        // 同版本检测
        if (strncmp(app_desc->version, s_ctx.running_version, sizeof(s_ctx.running_version)) == 0) {
            ESP_LOGW(TAG, "上传的固件版本与当前运行版本相同，拒绝升级");
            s_ctx.state = OTA_STATE_FAILED;
            return ESP_ERR_INVALID_ARG;
        }

        // 启动 OTA 写入
        esp_err_t err = esp_ota_begin(s_ctx.update_partition, OTA_SIZE_UNKNOWN, &s_ctx.ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin 失败: %s", esp_err_to_name(err));
            s_ctx.state = OTA_STATE_FAILED;
            return err;
        }

        // 写入初始缓冲区（包含 ESP 头 + 镜像头）
        err = esp_ota_write(s_ctx.ota_handle, s_ctx.init_buf, s_ctx.init_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write (init) 失败: %s", esp_err_to_name(err));
            esp_ota_abort(s_ctx.ota_handle);
            s_ctx.ota_handle = 0;
            s_ctx.state = OTA_STATE_FAILED;
            return err;
        }

        s_ctx.image_header_checked = true;
        s_ctx.state = OTA_STATE_WRITING;
        ESP_LOGI(TAG, "OTA Flash 写入开始，头部解析完成");

        // 写入当前调用中剩余的数据（如果有）
        if (remaining_after_buf > 0) {
            err = esp_ota_write(s_ctx.ota_handle, data, remaining_after_buf);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write 失败: %s", esp_err_to_name(err));
                esp_ota_abort(s_ctx.ota_handle);
                s_ctx.ota_handle = 0;
                s_ctx.state = OTA_STATE_FAILED;
                return err;
            }
            s_ctx.total_written += remaining_after_buf;
        }

        return ESP_OK;
    }

    // 后续调用：直接写入
    esp_err_t err = esp_ota_write(s_ctx.ota_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write 失败: %s", esp_err_to_name(err));
        esp_ota_abort(s_ctx.ota_handle);
        s_ctx.ota_handle = 0;
        s_ctx.state = OTA_STATE_FAILED;
        return err;
    }

    s_ctx.total_written += len;
    return ESP_OK;
}

esp_err_t ota_update_end(void)
{
    if (s_ctx.state != OTA_STATE_WRITING) {
        ESP_LOGW(TAG, "OTA 未处于写入完成状态，当前: %d", s_ctx.state);
        s_ctx.state = OTA_STATE_FAILED;
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ctx.total_written == 0) {
        ESP_LOGE(TAG, "未接收到任何固件数据");
        s_ctx.state = OTA_STATE_FAILED;
        return ESP_ERR_INVALID_SIZE;
    }

    // 完成 OTA，验证镜像
    esp_err_t err = esp_ota_end(s_ctx.ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end 验证失败: %s", esp_err_to_name(err));
        s_ctx.state = OTA_STATE_FAILED;
        return err;
    }

    // 设置启动分区
    err = esp_ota_set_boot_partition(s_ctx.update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition 失败: %s", esp_err_to_name(err));
        s_ctx.state = OTA_STATE_FAILED;
        return err;
    }

    s_ctx.state = OTA_STATE_COMPLETE;

    // OTA 成功后才取消回滚倒计时（如果当前固件处于 PENDING_VERIFY 状态）
    esp_err_t mark_err = esp_ota_mark_app_valid_cancel_rollback();
    if (mark_err == ESP_ERR_NOT_SUPPORTED) {
        // 当前固件不是待验证状态，正常情况
    } else if (mark_err != ESP_OK) {
        ESP_LOGW(TAG, "标记当前固件有效失败: %s", esp_err_to_name(mark_err));
    } else {
        ESP_LOGI(TAG, "取消 OTA Rollback 倒计时");
    }

    ESP_LOGI(TAG, "OTA 升级完成! 写入 %lu 字节, 下次启动将进入新固件", s_ctx.total_written);
    return ESP_OK;
}

void ota_update_abort(void)
{
    if (s_ctx.ota_handle != 0) {
        esp_ota_abort(s_ctx.ota_handle);
        s_ctx.ota_handle = 0;
    }
    s_ctx.state = OTA_STATE_FAILED;
    s_ctx.total_written = 0;
    s_ctx.init_len = 0;
    s_ctx.image_header_checked = false;
    s_ctx.update_partition = NULL;  // 清空分区指针，防止下次begin使用旧分区
    ESP_LOGW(TAG, "OTA 会话已中止");
}

// ==================== 状态查询 ====================

ota_state_t ota_update_get_state(void)
{
    return s_ctx.state;
}

uint32_t ota_update_get_bytes_written(void)
{
    return s_ctx.total_written;
}

const char *ota_update_get_running_version(void)
{
    return s_ctx.running_version;
}
