/**
 * @file filter_manager.c
 * @brief 五级滤芯管理与用水量统计模块实现
 * @note 管理滤芯寿命、总用水量、制水速率
 */

#include "filter_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <math.h>
#include <time.h>

static const char *TAG = "FILTER_MGR";

// ==================== NVS命名空间 ====================

#define NVS_FILTERS_NAMESPACE "wp_filters"

// ==================== 累加器精度单位 ====================
// 使用整数累加器，单位为毫升(mL)，避免浮点精度误差累积
#define ACCUMULATOR_UNIT_ML    1      // 1mL精度
#define ACCUMULATOR_COMMIT_THRESHOLD 1000  // >=1000mL(1L)时提交

// ==================== 私有变量 ====================

// 滤芯默认时间寿命（小时）
static const uint32_t filter_default_time_hours[FILTER_COUNT] = {
    2190,   // PP棉：3个月
    4380,   // 颗粒活性炭：6个月
    4380,   // 压缩活性炭：6个月
    17520,  // RO膜：24个月
    6570,   // 后置活性炭：9个月
};

// 滤芯名称
static const char* const filter_names[FILTER_COUNT] = {
    "PP棉",
    "颗粒活性炭",
    "压缩活性炭",
    "RO膜",
    "后置活性炭"
};

// 滤芯默认容量（升）
static const uint32_t filter_default_capacities[FILTER_COUNT] = {
    FILTER_DEFAULT_LIFE_PP,
    FILTER_DEFAULT_LIFE_GRANULAR,
    FILTER_DEFAULT_LIFE_COMPRESSED,
    FILTER_DEFAULT_LIFE_RO,
    FILTER_DEFAULT_LIFE_POST
};

static struct {
    bool initialized;

    // 并发保护互斥锁
    SemaphoreHandle_t mutex;

    // 五级滤芯管理
    filter_info_t filters[FILTER_COUNT];
    uint32_t total_water_used;

    // 用水量累积器（整数，单位mL，避免浮点精度误差）
    // 每级滤芯独立累加器，重置时仅清除对应滤芯的累加器
    uint32_t filter_accumulator_ml[FILTER_COUNT];  // 每级滤芯独立累加器(mL)

    // RO制水速率（升/小时）
    float production_rate_lph;

    // 废水流量（升/小时），用于计算制水状态前三级实际过水量
    float waste_flow_lph;

    // 增压泵流量（升/小时），用于计算冲洗状态前三级实际过水量
    float pump_flow_lph;

    // 总制水量（仅PRODUCTION状态产出的纯水，用于RO膜/后置炭寿命计算和显示）
    uint32_t total_production_water;

    // 兼容旧接口的单滤芯状态
    uint32_t filter_total_liters;
    uint32_t filter_used_liters;
    uint32_t filter_install_time;

    // NVS写入控制（按周期检查，不每次写入）
    bool save_needed;

    // NVS保存失败计数（用于告警）
    uint8_t save_fail_count;
} fctx = {
    .initialized = false,
    .mutex = NULL,
    .total_water_used = 0,
    .total_production_water = 0,
    .filter_accumulator_ml = {0, 0, 0, 0, 0},  // 五级滤芯独立累加器初始化为0
    .production_rate_lph = 12.0f,  // 默认汇通75G
    .waste_flow_lph = 18.0f,       // 默认300CC (300*60/1000)
    .pump_flow_lph = 33.0f,        // 默认三角洲50G增压泵 (0.55L/min × 60)
    .filter_total_liters = 3000,
    .filter_used_liters = 0,
    .filter_install_time = 0,
    .save_needed = false,
    .save_fail_count = 0,
};

// ==================== 私有函数 ====================

static void load_from_nvs(void);
static esp_err_t save_to_nvs_locked(void);  // 假定已持有锁
static void fixup_filter_time_on_ntp_sync_locked(void);  // 假定已持有锁
static void update_filter_time_percentage_locked(void);  // 假定已持有锁
static bool commit_accumulators_locked(void);  // 提交累加器水量，返回是否实际提交

// ==================== 辅助宏 ====================

// 获取锁，超时返回ESP_ERR_TIMEOUT（用于返回esp_err_t的函数）
#define LOCK_GET(timeout_ms) \
    do { \
        if (!fctx.mutex) return ESP_ERR_INVALID_STATE; \
        if (xSemaphoreTake(fctx.mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) { \
            ESP_LOGW(TAG, "mutex获取超时"); \
            return ESP_ERR_TIMEOUT; \
        } \
    } while(0)

// 获取锁，超时直接返回（用于void函数）
#define LOCK_GET_VOID(timeout_ms) \
    do { \
        if (!fctx.mutex) { ESP_LOGW(TAG, "mutex未初始化"); return; } \
        if (xSemaphoreTake(fctx.mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) { \
            ESP_LOGW(TAG, "mutex获取超时"); return; \
        } \
    } while(0)

// 释放锁
#define LOCK_GIVE() \
    do { \
        if (fctx.mutex) xSemaphoreGive(fctx.mutex); \
    } while(0)

// ==================== NVS加载 ====================

static void load_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_FILTERS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS中无滤芯数据，使用默认值");
        return;
    }

    // 加载总用水量
    nvs_get_u32(handle, "total_water", &fctx.total_water_used);

    // 加载总制水量（新增字段，不存在时使用默认值0）
    nvs_get_u32(handle, "total_prod", &fctx.total_production_water);

    // 加载累加器（新格式：每级滤芯独立）
    bool has_new_acc = false;
    for (int i = 0; i < FILTER_COUNT; i++) {
        char key[16];
        snprintf(key, sizeof(key), "acc_%d", i);
        if (nvs_get_u32(handle, key, &fctx.filter_accumulator_ml[i]) == ESP_OK) {
            has_new_acc = true;
        } else {
            fctx.filter_accumulator_ml[i] = 0;
        }
    }

    // 兼容旧格式：如果新字段不存在，加载旧格式并迁移
    if (!has_new_acc) {
        uint32_t old_pre_acc = 0, old_post_acc = 0;
        bool has_old_pre = (nvs_get_u32(handle, "pre_acc", &old_pre_acc) == ESP_OK);
        bool has_old_post = (nvs_get_u32(handle, "post_acc", &old_post_acc) == ESP_OK);

        if (has_old_pre || has_old_post) {
            ESP_LOGI(TAG, "迁移旧累加器格式: pre=%lu, post=%lu", old_pre_acc, old_post_acc);
            // 前三级共享旧pre_acc（串联关系，处理相同水量）
            for (int i = 0; i <= 2; i++) {
                fctx.filter_accumulator_ml[i] = old_pre_acc;
            }
            // 后两级共享旧post_acc
            for (int i = 3; i <= 4; i++) {
                fctx.filter_accumulator_ml[i] = old_post_acc;
            }
            // 清除旧字段（延迟删除，等待save时提交）
            nvs_erase_key(handle, "pre_acc");
            nvs_erase_key(handle, "post_acc");
            nvs_commit(handle);
        }
    }

    // 加载制水速率
    int32_t rate_i32;
    if (nvs_get_i32(handle, "prod_rate", &rate_i32) == ESP_OK) {
        fctx.production_rate_lph = rate_i32 / 100.0f;
    }

    // 加载各级滤芯数据
    for (int i = 0; i < FILTER_COUNT; i++) {
        char key[16];

        // 已用水量
        snprintf(key, sizeof(key), "f%d_used", i);
        nvs_get_u32(handle, key, &fctx.filters[i].used_liters);

        // 重置时间（NTP未同步时使用0，等待fixup修正）
        snprintf(key, sizeof(key), "f%d_reset", i);
        uint32_t reset_time = 0;  // 默认0，表示未同步
        if (nvs_get_u32(handle, key, &reset_time) == ESP_OK) {
            // 验证时间戳有效性（> 2020-01-01）
            if (reset_time > 1577836800UL) {
                fctx.filters[i].last_reset_time = reset_time;
                fctx.filters[i].install_time = reset_time;
            } else {
                // 无效时间戳，标记为0等待fixup
                fctx.filters[i].last_reset_time = 0;
                fctx.filters[i].install_time = 0;
            }
        }

        // 自定义容量
        snprintf(key, sizeof(key), "f%d_cap", i);
        uint32_t cap;
        if (nvs_get_u32(handle, key, &cap) == ESP_OK) {
            fctx.filters[i].total_liters = cap;
        }

        // 时间寿命
        snprintf(key, sizeof(key), "f%d_time", i);
        uint32_t t;
        if (nvs_get_u32(handle, key, &t) == ESP_OK) {
            fctx.filters[i].time_limit_hours = t;
        }

        // 重新计算水量百分比
        if (fctx.filters[i].total_liters > 0) {
            float remaining = 1.0f - (float)fctx.filters[i].used_liters / fctx.filters[i].total_liters;
            if (remaining < 0) remaining = 0;
            if (remaining > 1) remaining = 1;
            fctx.filters[i].percentage = (uint8_t)(remaining * 100);
        }
    }

    // 兼容旧接口数据
    fctx.filter_used_liters = fctx.filters[FILTER_RO_MEMBRANE].used_liters;
    fctx.filter_install_time = fctx.filters[FILTER_RO_MEMBRANE].last_reset_time;

    nvs_close(handle);
    ESP_LOGI(TAG, "滤芯数据已加载: 总用水%luL, 总制水%luL, 累加器[%lu,%lu,%lu,%lu,%lu]mL",
             fctx.total_water_used, fctx.total_production_water,
             fctx.filter_accumulator_ml[0], fctx.filter_accumulator_ml[1], fctx.filter_accumulator_ml[2],
             fctx.filter_accumulator_ml[3], fctx.filter_accumulator_ml[4]);
}

// 内部保存函数（假定已持有锁）
static esp_err_t save_to_nvs_locked(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_FILTERS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS打开失败: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u32(handle, "total_water", fctx.total_water_used);
    nvs_set_u32(handle, "total_prod", fctx.total_production_water);  // 保存总制水量

    // 保存独立累加器（新格式）
    for (int i = 0; i < FILTER_COUNT; i++) {
        char key[16];
        snprintf(key, sizeof(key), "acc_%d", i);
        nvs_set_u32(handle, key, fctx.filter_accumulator_ml[i]);
    }

    nvs_set_i32(handle, "prod_rate", (int32_t)(fctx.production_rate_lph * 100));

    for (int i = 0; i < FILTER_COUNT; i++) {
        char key[16];

        snprintf(key, sizeof(key), "f%d_used", i);
        nvs_set_u32(handle, key, fctx.filters[i].used_liters);

        snprintf(key, sizeof(key), "f%d_reset", i);
        nvs_set_u32(handle, key, fctx.filters[i].last_reset_time);

        snprintf(key, sizeof(key), "f%d_cap", i);
        nvs_set_u32(handle, key, fctx.filters[i].total_liters);

        snprintf(key, sizeof(key), "f%d_time", i);
        nvs_set_u32(handle, key, fctx.filters[i].time_limit_hours);
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        fctx.save_needed = false;
        fctx.save_fail_count = 0;
        ESP_LOGD(TAG, "滤芯数据已保存");
    } else {
        ESP_LOGW(TAG, "NVS提交失败: %s", esp_err_to_name(err));
        fctx.save_fail_count++;
        if (fctx.save_fail_count >= 3) {
            ESP_LOGE(TAG, "NVS连续保存失败%d次，滤芯数据可能丢失！", fctx.save_fail_count);
        }
    }
    return err;
}

// ==================== 辅助函数（假定已持有锁） ====================

/**
 * @brief 在NTP同步后回补滤芯安装时间戳
 * @note 假定已持有锁，初始化时install_time=0表示未同步
 */
static void fixup_filter_time_on_ntp_sync_locked(void)
{
    time_t now_sec = time(NULL);
    if (now_sec <= 1) return;  // NTP仍未同步

    bool fixed = false;
    for (int i = 0; i < FILTER_COUNT; i++) {
        // 时间戳<=1表示未同步，需要修正（0或1都是无效值）
        if (fctx.filters[i].last_reset_time <= 1) {
            fctx.filters[i].last_reset_time = (uint32_t)now_sec;
            fctx.filters[i].install_time = (uint32_t)now_sec;
            fixed = true;
        }
    }
    // 兼容字段检查
    if (fctx.filter_install_time <= 1) {
        fctx.filter_install_time = (uint32_t)now_sec;
        fixed = true;
    }
    if (fixed) {
        ESP_LOGI(TAG, "NTP已同步，修正滤芯日历时间戳为%lu", (uint32_t)now_sec);
        // 立即保存修正后的时间戳
        fctx.save_needed = true;
        save_to_nvs_locked();
    }
}

/**
 * @brief 更新所有滤芯的时间维度寿命百分比
 * @note 假定已持有锁
 */
static void update_filter_time_percentage_locked(void)
{
    time_t now_sec = time(NULL);
    uint64_t boot_elapsed_sec = 0;

    // 与fixup函数保持一致，使用<=1判断NTP是否同步
    if (now_sec <= 1) {
        boot_elapsed_sec = (uint64_t)(esp_timer_get_time() / 1000000ULL);
        ESP_LOGD(TAG, "NTP未同步，使用启动时间估算滤芯日历寿命");
    }

    for (int i = 0; i < FILTER_COUNT; i++) {
        uint32_t elapsed_hours = 0;

        // 时间戳>1表示已同步，才能计算日历寿命
        if (fctx.filters[i].time_limit_hours > 0 && fctx.filters[i].last_reset_time > 1) {
            if (now_sec > 1 && now_sec >= (time_t)fctx.filters[i].last_reset_time) {
                elapsed_hours = (uint32_t)((now_sec - fctx.filters[i].last_reset_time) / 3600);
            } else if (boot_elapsed_sec > 0) {
                // NTP未同步时，使用启动时间估算（上限为启动时间）
                elapsed_hours = (uint32_t)(boot_elapsed_sec / 3600);
            }

            if (elapsed_hours > 0 && elapsed_hours < fctx.filters[i].time_limit_hours) {
                float time_remaining = 1.0f - (float)elapsed_hours / fctx.filters[i].time_limit_hours;
                if (time_remaining < 0) time_remaining = 0;
                if (time_remaining > 1) time_remaining = 1;
                fctx.filters[i].time_percentage = (uint8_t)(time_remaining * 100);
            } else if (elapsed_hours >= fctx.filters[i].time_limit_hours) {
                fctx.filters[i].time_percentage = 0;
            }
        } else if (fctx.filters[i].last_reset_time <= 1) {
            // 时间戳无效，保持time_percentage=100（初始化时已设为100）
            // 等待fixup修正后才能计算日历寿命
        }

        // 有效寿命 = min(水量%, 时间%)
        fctx.filters[i].effective_percentage =
            (fctx.filters[i].percentage < fctx.filters[i].time_percentage) ?
            fctx.filters[i].percentage : fctx.filters[i].time_percentage;

        // 检查是否需要更换（低于10%）
        bool needs_replacement = (fctx.filters[i].effective_percentage < 10);

        if (needs_replacement && !fctx.filters[i].replacement_needed) {
            ESP_LOGW(TAG, "%s滤芯寿命不足10%%，请及时更换", filter_names[i]);
        }

        fctx.filters[i].replacement_needed = needs_replacement;
    }
}

/**
 * @brief 提交累加器中的水量到各滤芯
 * @note 假定已持有锁，将累加器水量分配给所有对应滤芯
 * @return true 本次有实际水量提交（>=1L）
 */
static bool commit_accumulators_locked(void)
{
    bool did_commit = false;

    // 逐个滤芯提交独立累加器
    for (int i = 0; i < FILTER_COUNT; i++) {
        if (fctx.filter_accumulator_ml[i] >= ACCUMULATOR_COMMIT_THRESHOLD) {
            did_commit = true;
            uint32_t liters = fctx.filter_accumulator_ml[i] / 1000;
            fctx.filter_accumulator_ml[i] %= 1000;

            // 增加该滤芯已用水量
            if (UINT32_MAX - fctx.filters[i].used_liters >= liters) {
                fctx.filters[i].used_liters += liters;
            }

            // 更新剩余寿命百分比
            if (fctx.filters[i].total_liters > 0) {
                float remaining = 1.0f - (float)fctx.filters[i].used_liters / fctx.filters[i].total_liters;
                if (remaining < 0) remaining = 0;
                if (remaining > 1) remaining = 1;
                fctx.filters[i].percentage = (uint8_t)(remaining * 100);
            }

            // 累加到总用水量/制水量
            if (i <= 2) {  // 前三级计入总用水量
                if (UINT32_MAX - fctx.total_water_used >= liters) {
                    fctx.total_water_used += liters;
                }
            } else {  // 后两级计入总制水量
                if (UINT32_MAX - fctx.total_production_water >= liters) {
                    fctx.total_production_water += liters;
                }
                // 兼容字段（RO膜）
                if (UINT32_MAX - fctx.filter_used_liters >= liters) {
                    fctx.filter_used_liters += liters;
                }
            }
        }
    }

    return did_commit;
}

// ==================== 初始化 ====================

esp_err_t filter_mgr_init(void)
{
    if (fctx.initialized) {
        return ESP_OK;
    }

    // 创建互斥锁
    fctx.mutex = xSemaphoreCreateMutex();
    if (!fctx.mutex) {
        ESP_LOGE(TAG, "创建互斥锁失败");
        return ESP_ERR_NO_MEM;
    }

    // 获取锁，保护后续初始化操作
    if (xSemaphoreTake(fctx.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "初始化时获取互斥锁超时");
        vSemaphoreDelete(fctx.mutex);
        fctx.mutex = NULL;
        return ESP_ERR_TIMEOUT;
    }

    // 初始化滤芯默认状态（NTP未同步时时间戳为0）
    time_t now_sec = time(NULL);
    uint32_t init_time = (now_sec > 1) ? (uint32_t)now_sec : 0;  // NTP同步时用真实时间，否则用0

    for (int i = 0; i < FILTER_COUNT; i++) {
        fctx.filters[i].percentage = 100;
        fctx.filters[i].time_percentage = 100;
        fctx.filters[i].effective_percentage = 100;
        fctx.filters[i].used_liters = 0;
        fctx.filters[i].total_liters = filter_default_capacities[i];
        fctx.filters[i].time_limit_hours = filter_default_time_hours[i];
        fctx.filters[i].install_time = init_time;  // 0表示未同步，等待fixup修正
        fctx.filters[i].last_reset_time = init_time;
        fctx.filters[i].replacement_needed = false;
        strncpy(fctx.filters[i].name, filter_names[i], sizeof(fctx.filters[i].name) - 1);
    }

    // 从NVS恢复滤芯状态
    load_from_nvs();

    // 初始化完成后，重新计算有效寿命
    // 注意：此时可能NTP未同步，时间戳为0，等待后续fixup修正
    update_filter_time_percentage_locked();

    fctx.initialized = true;

    // 如果NTP已同步，修正时间戳（需在initialized=true之后，其他任务可能已开始调用）
    time_t now_after = time(NULL);
    if (now_after > 1) {
        fixup_filter_time_on_ntp_sync_locked();
    }

    LOCK_GIVE();

    return ESP_OK;
}

esp_err_t filter_mgr_deinit(void)
{
    if (!fctx.initialized) {
        return ESP_OK;
    }

    // 保存未写入的数据
    if (fctx.save_needed) {
        LOCK_GET(100);
        save_to_nvs_locked();
        LOCK_GIVE();
    }

    // 删除互斥锁
    if (fctx.mutex) {
        vSemaphoreDelete(fctx.mutex);
        fctx.mutex = NULL;
    }

    fctx.initialized = false;
    return ESP_OK;
}

// ==================== 用水量统计 ====================

esp_err_t filter_mgr_update_water_usage(float liters)
{
    return filter_mgr_update_water_usage_dual(liters, liters);
}

esp_err_t filter_mgr_update_water_usage_dual(float pre_liters, float post_liters)
{
    // 参数校验
    if (!isfinite(pre_liters)) pre_liters = 0.0f;
    if (!isfinite(post_liters)) post_liters = 0.0f;
    if (pre_liters <= 0.0f && post_liters <= 0.0f) {
        return ESP_OK;
    }

    LOCK_GET(100);

    // 如果NTP刚同步，回补滤芯时间戳
    fixup_filter_time_on_ntp_sync_locked();

    // 转换为毫升累加（整数累加避免浮点精度误差）
    uint32_t pre_ml = (uint32_t)(pre_liters * 1000.0f);  // 升转毫升
    uint32_t post_ml = (uint32_t)(post_liters * 1000.0f);

    // 前三级各自累加（串联关系，每级都处理相同水量）
    if (pre_ml > 0) {
        for (int i = 0; i <= 2; i++) {
            if (UINT32_MAX - fctx.filter_accumulator_ml[i] >= pre_ml) {
                fctx.filter_accumulator_ml[i] += pre_ml;
            } else {
                fctx.filter_accumulator_ml[i] = UINT32_MAX;  // 防止溢出
            }
        }
    }
    // 后两级各自累加（串联关系，每级都处理相同水量）
    if (post_ml > 0) {
        for (int i = 3; i <= 4; i++) {
            if (UINT32_MAX - fctx.filter_accumulator_ml[i] >= post_ml) {
                fctx.filter_accumulator_ml[i] += post_ml;
            } else {
                fctx.filter_accumulator_ml[i] = UINT32_MAX;
            }
        }
    }

    // 提交累加器（>=1L时提交），仅在实际提交时设置脏标志
    bool did_commit = commit_accumulators_locked();

    // 更新时间维度寿命
    update_filter_time_percentage_locked();

    // 仅在实际提交水量时标记需要保存（减少无意义脏标志）
    if (did_commit) {
        fctx.save_needed = true;
    }

    LOCK_GIVE();
    return ESP_OK;
}

esp_err_t filter_mgr_set_waste_flow_lph(float lph)
{
    if (!isfinite(lph) || lph < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    LOCK_GET(50);
    fctx.waste_flow_lph = lph;
    LOCK_GIVE();
    return ESP_OK;
}

float filter_mgr_get_waste_flow_lph(void)
{
    float lph;
    LOCK_GET(50);
    lph = fctx.waste_flow_lph;
    LOCK_GIVE();
    return lph;
}

esp_err_t filter_mgr_set_pump_flow_lph(float lph)
{
    if (!isfinite(lph) || lph < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    LOCK_GET(50);
    fctx.pump_flow_lph = lph;
    LOCK_GIVE();
    return ESP_OK;
}

float filter_mgr_get_pump_flow_lph(void)
{
    float lph;
    LOCK_GET(50);
    lph = fctx.pump_flow_lph;
    LOCK_GIVE();
    return lph;
}

uint32_t filter_mgr_get_total_water_usage(void)
{
    uint32_t usage;
    LOCK_GET(50);
    usage = fctx.total_water_used;
    LOCK_GIVE();
    return usage;
}

void filter_mgr_set_total_water_usage(uint32_t liters)
{
    LOCK_GET_VOID(50);
    fctx.total_water_used = liters;
    fctx.save_needed = true;
    LOCK_GIVE();
}

uint32_t filter_mgr_get_total_production_water(void)
{
    uint32_t water;
    LOCK_GET(50);
    water = fctx.total_production_water;
    LOCK_GIVE();
    return water;
}

void filter_mgr_set_total_production_water(uint32_t liters)
{
    LOCK_GET_VOID(50);
    fctx.total_production_water = liters;
    fctx.save_needed = true;
    LOCK_GIVE();
}

esp_err_t filter_mgr_get_filters_status(filters_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    LOCK_GET(100);

    fixup_filter_time_on_ntp_sync_locked();
    update_filter_time_percentage_locked();

    memcpy(status->filters, fctx.filters, sizeof(fctx.filters));
    status->total_water_used = fctx.total_water_used;
    status->any_filter_needs_replacement = filter_mgr_any_filter_needs_replacement();

    LOCK_GIVE();
    return ESP_OK;
}

esp_err_t filter_mgr_reset_filter(filter_type_t filter_type)
{
    if (filter_type >= FILTER_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    LOCK_GET(100);

    // 先提交累加器水量到所有滤芯（包括即将重置的滤芯）
    // 这样重置后，累加器归零，下次累积的水量只属于未重置的滤芯
    (void)commit_accumulators_locked();  // 忽略返回值，重置操作本身会触发保存

    // 只清除被重置滤芯的独立累加器（不影响其他滤芯）
    fctx.filter_accumulator_ml[filter_type] = 0;
    ESP_LOGD(TAG, "清除滤芯%d独立累加器", filter_type);

    // 重置指定滤芯
    time_t now_sec = time(NULL);
    uint32_t reset_time = (now_sec > 1) ? (uint32_t)now_sec : 0;

    fctx.filters[filter_type].percentage = 100;
    fctx.filters[filter_type].time_percentage = 100;
    fctx.filters[filter_type].effective_percentage = 100;
    fctx.filters[filter_type].used_liters = 0;
    fctx.filters[filter_type].install_time = reset_time;
    fctx.filters[filter_type].last_reset_time = reset_time;
    fctx.filters[filter_type].replacement_needed = false;

    // 更新兼容字段
    if (filter_type == FILTER_RO_MEMBRANE) {
        fctx.filter_used_liters = 0;
        fctx.filter_install_time = reset_time;
    }

    fctx.save_needed = true;

    // 立即保存
    esp_err_t ret = save_to_nvs_locked();

    LOCK_GIVE();

    ESP_LOGI(TAG, "%s滤芯已重置", filter_names[filter_type]);
    return ret;
}

esp_err_t filter_mgr_reset_all_filters(void)
{
    LOCK_GET(100);

    // 清除所有独立累加器
    for (int i = 0; i < FILTER_COUNT; i++) {
        fctx.filter_accumulator_ml[i] = 0;
    }

    time_t now_sec = time(NULL);
    uint32_t reset_time = (now_sec > 1) ? (uint32_t)now_sec : 0;

    for (int i = 0; i < FILTER_COUNT; i++) {
        fctx.filters[i].percentage = 100;
        fctx.filters[i].time_percentage = 100;
        fctx.filters[i].effective_percentage = 100;
        fctx.filters[i].used_liters = 0;
        fctx.filters[i].install_time = reset_time;
        fctx.filters[i].last_reset_time = reset_time;
        fctx.filters[i].replacement_needed = false;
    }
    fctx.total_water_used = 0;
    fctx.total_production_water = 0;
    fctx.filter_used_liters = 0;
    fctx.filter_install_time = reset_time;
    fctx.filter_total_liters = fctx.filters[FILTER_RO_MEMBRANE].total_liters;

    fctx.save_needed = true;
    esp_err_t ret = save_to_nvs_locked();

    LOCK_GIVE();

    ESP_LOGI(TAG, "所有滤芯已重置");
    return ret;
}

/**
 * @brief 批量设置所有滤芯容量（一次NVs写入）
 * @param capacities 容量数组，长度必须为FILTER_COUNT
 * @return ESP_OK 成功
 */
esp_err_t filter_mgr_set_all_filter_capacity(const uint32_t capacities[FILTER_COUNT])
{
    if (!capacities) {
        return ESP_ERR_INVALID_ARG;
    }

    LOCK_GET(100);

    for (int i = 0; i < FILTER_COUNT; i++) {
        fctx.filters[i].total_liters = capacities[i];
        if (capacities[i] > 0) {
            float remaining = 1.0f - (float)fctx.filters[i].used_liters / capacities[i];
            if (remaining < 0) remaining = 0;
            if (remaining > 1) remaining = 1;
            fctx.filters[i].percentage = (uint8_t)(remaining * 100);
        }
        fctx.filters[i].effective_percentage =
            (fctx.filters[i].percentage < fctx.filters[i].time_percentage) ?
            fctx.filters[i].percentage : fctx.filters[i].time_percentage;
        fctx.filters[i].replacement_needed = (fctx.filters[i].effective_percentage < 10);
    }

    fctx.filter_total_liters = capacities[FILTER_RO_MEMBRANE];
    fctx.save_needed = true;
    esp_err_t ret = save_to_nvs_locked();

    LOCK_GIVE();

    ESP_LOGI(TAG, "所有滤芯容量已批量更新，RO膜: %lu 升", capacities[FILTER_RO_MEMBRANE]);
    return ret;
}

esp_err_t filter_mgr_set_all_filter_times(const uint32_t time_hours[FILTER_COUNT])
{
    if (!time_hours) {
        return ESP_ERR_INVALID_ARG;
    }

    LOCK_GET(100);

    for (int i = 0; i < FILTER_COUNT; i++) {
        if (time_hours[i] > 0) {
            fctx.filters[i].time_limit_hours = time_hours[i];
        }
    }

    update_filter_time_percentage_locked();
    fctx.save_needed = true;
    esp_err_t ret = save_to_nvs_locked();

    LOCK_GIVE();

    ESP_LOGI(TAG, "所有滤芯时间寿命已批量更新");
    return ret;
}

const char* filter_mgr_get_filter_name(filter_type_t filter_type)
{
    if (filter_type < FILTER_COUNT) {
        return filter_names[filter_type];
    }
    return "未知滤芯";
}

bool filter_mgr_any_filter_needs_replacement(void)
{
    // 内部调用，假定已持有锁或快速检查
    for (int i = 0; i < FILTER_COUNT; i++) {
        if (fctx.filters[i].replacement_needed ||
            fctx.filters[i].percentage < 10) {
            return true;
        }
    }
    return false;
}

// ==================== 制水速率 ====================

esp_err_t filter_mgr_set_production_rate(float lph)
{
    if (!isfinite(lph) || lph <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    LOCK_GET(50);
    fctx.production_rate_lph = lph;
    LOCK_GIVE();
    ESP_LOGI(TAG, "RO制水速率设置: %.1f L/h", lph);
    return ESP_OK;
}

float filter_mgr_get_production_rate(void)
{
    float rate;
    LOCK_GET(50);
    rate = fctx.production_rate_lph;
    LOCK_GIVE();
    return rate;
}

// ==================== 周期保存 ====================

/**
 * @brief 周期性保存滤芯数据到NVS
 * @note 仅在数据有变化时写入，避免频繁写入Flash
 *       保存失败时save_needed保持true，下次继续尝试
 *       连续失败3次后输出告警
 * @return true 执行了保存且成功
 */
bool filter_mgr_periodic_save(void)
{
    if (!fctx.save_needed) {
        return false;
    }

    LOCK_GET(100);

    if (!fctx.save_needed) {
        LOCK_GIVE();
        return false;
    }

    esp_err_t err = save_to_nvs_locked();
    bool success = (err == ESP_OK);

    LOCK_GIVE();

    if (!success) {
        ESP_LOGW(TAG, "周期性保存失败(%d次): %s", fctx.save_fail_count, esp_err_to_name(err));
    }

    return success;
}
