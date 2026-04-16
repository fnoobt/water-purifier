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
#include <string.h>
#include <math.h>
#include <time.h>

static const char *TAG = "FILTER_MGR";

// ==================== NVS命名空间 ====================

#define NVS_FILTERS_NAMESPACE "wp_filters"

// ==================== 私有变量 ====================

// 滤芯默认时间寿命（小时）
static const uint32_t filter_default_time_hours[FILTER_COUNT] = {
    3000,   // PP棉：约3-6个月
    4000,   // 颗粒活性炭：约6个月
    4000,   // 压缩活性炭：约6个月
    8000,   // RO膜：约24个月
    4000,   // 后置活性炭：约12个月
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

    // 五级滤芯管理
    filter_info_t filters[FILTER_COUNT];
    uint32_t total_water_used;

    // 用水量累积器（避免短时间制水被丢弃）
    float water_usage_accumulator;

    // RO制水速率（升/小时）
    float production_rate_lph;

    // 兼容旧接口的单滤芯状态
    uint32_t filter_total_liters;
    uint32_t filter_used_liters;
    uint32_t filter_install_time;

    // NVS写入控制（按周期检查，不每次写入）
    bool save_needed;
} fctx = {
    .initialized = false,
    .total_water_used = 0,
    .production_rate_lph = 12.0f,  // 默认汇通75G
    .filter_total_liters = 3000,
    .filter_used_liters = 0,
    .filter_install_time = 0,
    .save_needed = false,
};

// ==================== 私有函数 ====================

static void load_from_nvs(void);
static void save_to_nvs(void);
static void fixup_filter_time_on_ntp_sync(void);
static void update_filter_time_percentage(void);

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

    // 加载制水速率
    int32_t rate_i32;
    if (nvs_get_i32(handle, "prod_rate", &rate_i32) == ESP_OK) {
        fctx.production_rate_lph = rate_i32 / 100.0f;
    }

    // 加载各级滤芯数据
    time_t now_sec = time(NULL);
    for (int i = 0; i < FILTER_COUNT; i++) {
        char key[16];

        // 已用水量
        snprintf(key, sizeof(key), "f%d_used", i);
        nvs_get_u32(handle, key, &fctx.filters[i].used_liters);

        // 重置时间
        snprintf(key, sizeof(key), "f%d_reset", i);
        uint32_t reset_time = (uint32_t)now_sec;
        if (nvs_get_u32(handle, key, &reset_time) == ESP_OK) {
            fctx.filters[i].last_reset_time = reset_time;
            fctx.filters[i].install_time = reset_time;
        }

        // 自定义容量
        snprintf(key, sizeof(key), "f%d_cap", i);
        uint32_t cap;
        if (nvs_get_u32(handle, key, &cap) == ESP_OK) {
            fctx.filters[i].total_liters = cap;
        }

        // 重新计算水量百分比
        if (fctx.filters[i].total_liters > 0) {
            float remaining = 1.0f - (float)fctx.filters[i].used_liters / fctx.filters[i].total_liters;
            if (remaining < 0) remaining = 0;
            fctx.filters[i].percentage = (uint8_t)(remaining * 100);
        }
    }

    // 兼容旧接口数据
    fctx.filter_used_liters = fctx.filters[FILTER_RO_MEMBRANE].used_liters;
    fctx.filter_install_time = fctx.filters[FILTER_RO_MEMBRANE].last_reset_time;

    nvs_close(handle);
    ESP_LOGI(TAG, "滤芯数据已加载: 总用水%luL, RO已用%luL",
             fctx.total_water_used, fctx.filters[FILTER_RO_MEMBRANE].used_liters);
}

static void save_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_FILTERS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS打开失败: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_u32(handle, "total_water", fctx.total_water_used);
    nvs_set_i32(handle, "prod_rate", (int32_t)(fctx.production_rate_lph * 100));

    for (int i = 0; i < FILTER_COUNT; i++) {
        char key[16];

        snprintf(key, sizeof(key), "f%d_used", i);
        nvs_set_u32(handle, key, fctx.filters[i].used_liters);

        snprintf(key, sizeof(key), "f%d_reset", i);
        nvs_set_u32(handle, key, fctx.filters[i].last_reset_time);

        // 保存滤芯容量（始终保存，确保恢复默认值时不残留旧值）
        snprintf(key, sizeof(key), "f%d_cap", i);
        nvs_set_u32(handle, key, fctx.filters[i].total_liters);
    }

    nvs_commit(handle);
    nvs_close(handle);
    fctx.save_needed = false;
    ESP_LOGD(TAG, "滤芯数据已保存");
}

// ==================== 辅助函数 ====================

/**
 * @brief 在NTP同步后回补滤芯安装时间戳
 */
static void fixup_filter_time_on_ntp_sync(void)
{
    time_t now_sec = time(NULL);
    if (now_sec <= 1) return;  // NTP仍未同步

    bool fixed = false;
    for (int i = 0; i < FILTER_COUNT; i++) {
        if (fctx.filters[i].last_reset_time <= 1) {
            fctx.filters[i].last_reset_time = (uint32_t)now_sec;
            fctx.filters[i].install_time = (uint32_t)now_sec;
            fixed = true;
        }
    }
    if (fctx.filter_install_time <= 1) {
        fctx.filter_install_time = (uint32_t)now_sec;
        fixed = true;
    }
    if (fixed) {
        ESP_LOGI(TAG, "NTP已同步，修正滤芯日历时间戳");
    }
}

/**
 * @brief 更新所有滤芯的时间维度寿命百分比
 */
static void update_filter_time_percentage(void)
{
    time_t now_sec = time(NULL);
    if (now_sec <= 0) return;  // NTP未同步，无法计算日历时长

    for (int i = 0; i < FILTER_COUNT; i++) {
        // 时间维度（日历时长，NTP墙钟）
        if (fctx.filters[i].time_limit_hours > 0 &&
            now_sec > 0 && fctx.filters[i].last_reset_time > 0 &&
            now_sec >= fctx.filters[i].last_reset_time) {
            uint32_t elapsed_hours = (uint32_t)((now_sec - fctx.filters[i].last_reset_time) / 3600);
            float time_remaining = 1.0f - (float)elapsed_hours / fctx.filters[i].time_limit_hours;
            if (time_remaining < 0) time_remaining = 0;
            fctx.filters[i].time_percentage = (uint8_t)(time_remaining * 100);
        }

        // 有效寿命 = min(水量%, 时间%)
        fctx.filters[i].effective_percentage =
            (fctx.filters[i].percentage < fctx.filters[i].time_percentage) ?
            fctx.filters[i].percentage : fctx.filters[i].time_percentage;

        // 检查是否需要更换（低于10%）
        bool needs_replacement = (fctx.filters[i].effective_percentage < 10);

        // 边沿触发警告：只在首次进入低于10%时输出
        if (needs_replacement && !fctx.filters[i].replacement_needed) {
            ESP_LOGW(TAG, "%s滤芯寿命不足10%%，请及时更换", filter_names[i]);
        }

        fctx.filters[i].replacement_needed = needs_replacement;
    }
}

// ==================== 初始化 ====================

esp_err_t filter_mgr_init(void)
{
    if (fctx.initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "初始化滤芯管理模块...");

    // 初始化滤芯默认状态
    time_t now_sec = time(NULL);
    for (int i = 0; i < FILTER_COUNT; i++) {
        fctx.filters[i].percentage = 100;
        fctx.filters[i].time_percentage = 100;
        fctx.filters[i].effective_percentage = 100;
        fctx.filters[i].used_liters = 0;
        fctx.filters[i].total_liters = filter_default_capacities[i];
        fctx.filters[i].time_limit_hours = filter_default_time_hours[i];
        fctx.filters[i].install_time = (uint32_t)now_sec;
        fctx.filters[i].last_reset_time = (uint32_t)now_sec;
        fctx.filters[i].replacement_needed = false;
        strncpy(fctx.filters[i].name, filter_names[i], sizeof(fctx.filters[i].name) - 1);
    }

    // 从NVS恢复滤芯状态
    load_from_nvs();

    // 重新计算有效寿命（基于加载的水量和时间）
    update_filter_time_percentage();

    fctx.initialized = true;
    ESP_LOGI(TAG, "滤芯管理模块初始化完成");
    return ESP_OK;
}

// ==================== 用水量统计 ====================

esp_err_t filter_mgr_set_filter_capacity(uint32_t total_liters)
{
    fctx.filter_total_liters = total_liters;
    // 同步更新所有滤芯的容量
    for (int i = 0; i < FILTER_COUNT; i++) {
        fctx.filters[i].total_liters = total_liters;
        // 重新计算水量百分比
        if (total_liters > 0) {
            float remaining = 1.0f - (float)fctx.filters[i].used_liters / total_liters;
            if (remaining < 0) remaining = 0;
            fctx.filters[i].percentage = (uint8_t)(remaining * 100);
        }
    }
    ESP_LOGI(TAG, "滤芯容量设置: %lu 升", total_liters);
    return ESP_OK;
}

esp_err_t filter_mgr_update_water_usage(float liters)
{
    if (liters <= 0.0f) {
        return ESP_OK;
    }

    // 如果NTP刚同步，回补滤芯时间戳
    fixup_filter_time_on_ntp_sync();

    // 累积小数水量
    fctx.water_usage_accumulator += liters;

    // 当累积达到1升时，更新总量
    if (fctx.water_usage_accumulator >= 1.0f) {
        uint32_t committed_liters = (uint32_t)fctx.water_usage_accumulator;
        fctx.water_usage_accumulator -= committed_liters;

        fctx.filter_used_liters += committed_liters;
        fctx.total_water_used += committed_liters;

        // 更新每个滤芯的用水量
        for (int i = 0; i < FILTER_COUNT; i++) {
            fctx.filters[i].used_liters += committed_liters;

            // 水量维度
            if (fctx.filters[i].total_liters > 0) {
                float remaining = 1.0f - (float)fctx.filters[i].used_liters / fctx.filters[i].total_liters;
                if (remaining < 0) remaining = 0;
                fctx.filters[i].percentage = (uint8_t)(remaining * 100);
            }
        }
    }

    // 统一更新所有滤芯的时间维度寿命、有效寿命、更换标志
    update_filter_time_percentage();

    // 标记脏，由周期保存任务写入
    fctx.save_needed = true;

    return ESP_OK;
}

uint32_t filter_mgr_get_total_water_usage(void)
{
    return fctx.total_water_used;
}

void filter_mgr_set_total_water_usage(uint32_t liters)
{
    fctx.total_water_used = liters;
}

// ==================== 滤芯寿命管理 ====================

esp_err_t filter_mgr_get_filter_life(filter_life_t *life)
{
    if (!life) {
        return ESP_ERR_INVALID_ARG;
    }

    update_filter_time_percentage();

    life->used_liters = fctx.filter_used_liters;
    life->total_liters = fctx.filter_total_liters;
    life->install_time = fctx.filter_install_time;

    // 使用RO膜的有效寿命（水量+时间最小值）
    if (fctx.filter_total_liters > 0) {
        uint8_t pct = fctx.filters[FILTER_RO_MEMBRANE].effective_percentage;
        life->percentage = pct;
    } else {
        life->percentage = 100;
    }

    life->replacement_needed = (life->percentage < 10);

    return ESP_OK;
}

esp_err_t filter_mgr_reset_filter_life(void)
{
    // 重置RO膜（兼容旧接口，实际调用reset_filter）
    return filter_mgr_reset_filter(FILTER_RO_MEMBRANE);
}

esp_err_t filter_mgr_get_filters_status(filters_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    // 如果NTP刚同步，回补滤芯时间戳
    fixup_filter_time_on_ntp_sync();

    // 更新所有滤芯的时间维度寿命（确保查询时倒计时实时）
    update_filter_time_percentage();

    memcpy(status->filters, fctx.filters, sizeof(fctx.filters));
    status->total_water_used = fctx.total_water_used;
    status->any_filter_needs_replacement = filter_mgr_any_filter_needs_replacement();

    return ESP_OK;
}

esp_err_t filter_mgr_get_filter_info(filter_type_t filter_type, filter_info_t *info)
{
    if (filter_type >= FILTER_COUNT || !info) {
        return ESP_ERR_INVALID_ARG;
    }

    update_filter_time_percentage();
    memcpy(info, &fctx.filters[filter_type], sizeof(filter_info_t));
    return ESP_OK;
}

esp_err_t filter_mgr_reset_filter(filter_type_t filter_type)
{
    if (filter_type >= FILTER_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t now_sec = time(NULL);
    fctx.filters[filter_type].percentage = 100;
    fctx.filters[filter_type].time_percentage = 100;
    fctx.filters[filter_type].effective_percentage = 100;
    fctx.filters[filter_type].used_liters = 0;
    fctx.filters[filter_type].install_time = (uint32_t)now_sec;
    fctx.filters[filter_type].last_reset_time = (uint32_t)now_sec;
    fctx.filters[filter_type].replacement_needed = false;

    // 更新兼容字段
    if (filter_type == FILTER_RO_MEMBRANE) {
        fctx.filter_used_liters = 0;
        fctx.filter_install_time = (uint32_t)now_sec;
    }

    ESP_LOGI(TAG, "%s滤芯已重置", filter_names[filter_type]);
    fctx.save_needed = true;
    save_to_nvs();  // 关键操作立即写入
    return ESP_OK;
}

esp_err_t filter_mgr_reset_all_filters(void)
{
    time_t now_sec = time(NULL);

    for (int i = 0; i < FILTER_COUNT; i++) {
        fctx.filters[i].percentage = 100;
        fctx.filters[i].time_percentage = 100;
        fctx.filters[i].effective_percentage = 100;
        fctx.filters[i].used_liters = 0;
        fctx.filters[i].install_time = (uint32_t)now_sec;
        fctx.filters[i].last_reset_time = (uint32_t)now_sec;
        fctx.filters[i].replacement_needed = false;
    }
    fctx.total_water_used = 0;
    fctx.filter_used_liters = 0;
    fctx.water_usage_accumulator = 0.0f;

    ESP_LOGI(TAG, "所有滤芯已重置");
    save_to_nvs();
    return ESP_OK;
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

    // 先在内存中更新所有滤芯容量和百分比
    for (int i = 0; i < FILTER_COUNT; i++) {
        fctx.filters[i].total_liters = capacities[i];
        if (capacities[i] > 0) {
            float remaining = 1.0f - (float)fctx.filters[i].used_liters / capacities[i];
            if (remaining < 0) remaining = 0;
            fctx.filters[i].percentage = (uint8_t)(remaining * 100);
            if (fctx.filters[i].percentage > 100) {
                fctx.filters[i].percentage = 100;
            }
        }
        // 更新有效寿命
        fctx.filters[i].effective_percentage =
            (fctx.filters[i].percentage < fctx.filters[i].time_percentage) ?
            fctx.filters[i].percentage : fctx.filters[i].time_percentage;
        fctx.filters[i].replacement_needed = (fctx.filters[i].effective_percentage < 10);
    }

    // 更新兼容字段
    fctx.filter_total_liters = capacities[FILTER_RO_MEMBRANE];

    ESP_LOGI(TAG, "所有滤芯容量已批量更新，RO膜: %lu 升", capacities[FILTER_RO_MEMBRANE]);
    fctx.save_needed = true;
    save_to_nvs();  // 一次性写入所有滤芯
    return ESP_OK;
}

esp_err_t filter_mgr_set_filter_capacity_ex(filter_type_t filter_type, uint32_t total_liters)
{
    if (filter_type >= FILTER_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    fctx.filters[filter_type].total_liters = total_liters;
    // 重新计算水量百分比
    if (total_liters > 0) {
        float remaining = 1.0f - (float)fctx.filters[filter_type].used_liters / total_liters;
        if (remaining < 0) remaining = 0;
        fctx.filters[filter_type].percentage = (uint8_t)(remaining * 100);
        if (fctx.filters[filter_type].percentage > 100) {
            fctx.filters[filter_type].percentage = 100;
        }
    }
    // 更新有效寿命
    fctx.filters[filter_type].effective_percentage =
        (fctx.filters[filter_type].percentage < fctx.filters[filter_type].time_percentage) ?
        fctx.filters[filter_type].percentage : fctx.filters[filter_type].time_percentage;
    fctx.filters[filter_type].replacement_needed = (fctx.filters[filter_type].effective_percentage < 10);

    // 更新兼容字段
    if (filter_type == FILTER_RO_MEMBRANE) {
        fctx.filter_total_liters = total_liters;
    }

    ESP_LOGI(TAG, "%s滤芯容量设置为 %lu 升", filter_names[filter_type], total_liters);
    fctx.save_needed = true;
    save_to_nvs();  // 关键操作立即写入
    return ESP_OK;
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
    if (lph <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    fctx.production_rate_lph = lph;
    ESP_LOGI(TAG, "RO制水速率设置: %.1f L/h", lph);
    return ESP_OK;
}

float filter_mgr_get_production_rate(void)
{
    return fctx.production_rate_lph;
}

// ==================== 周期保存 ====================

/**
 * @brief 周期性保存滤芯数据到NVS
 * @note 仅在数据有变化时写入，避免频繁写入Flash
 * @return true 执行了保存
 */
bool filter_mgr_periodic_save(void)
{
    if (fctx.save_needed) {
        save_to_nvs();
        return true;
    }
    return false;
}
