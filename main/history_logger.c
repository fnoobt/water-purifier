/**
 * @file history_logger.c
 * @brief 历史记录日志模块实现
 * @note 记录状态变化、停止、冲洗等事件，支持50条记录和每日统计
 */

#include "history_logger.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "HISTORY";

// ==================== NVS命名空间 ====================

#define NVS_NAMESPACE_HISTORY   "history"
#define NVS_NAMESPACE_DAILY     "daily_stats"

// 最大假日期天数（约3年），超过后循环使用
#define MAX_BOOT_DAYS           1000

// ==================== 私有变量 ====================

static struct {
    bool initialized;
    history_record_t records[HISTORY_MAX_RECORDS];
    uint32_t record_count;
    uint32_t write_index;
    daily_stats_t today;
    SemaphoreHandle_t mutex;
    bool history_dirty;             // 历史记录脏标志
    bool daily_stats_dirty;         // 每日统计脏标志
    uint64_t last_save_time;        // 上次保存时间（esp_timer，微秒）
} ctx = {
    .initialized = false,
    .record_count = 0,
    .write_index = 0,
};

// ==================== 事件名称 ====================

static const char* const event_names[] = {
    [HISTORY_EVENT_STATE_CHANGE] = "状态变化",
    [HISTORY_EVENT_PRODUCTION_START] = "开始制水",
    [HISTORY_EVENT_PRODUCTION_END] = "结束制水",
    [HISTORY_EVENT_FLUSH_START] = "开始冲洗",
    [HISTORY_EVENT_FLUSH_END] = "结束冲洗",
    [HISTORY_EVENT_WATER_SHORTAGE] = "缺水",
    [HISTORY_EVENT_WATER_RESTORE] = "水恢复",
    [HISTORY_EVENT_TANK_FULL] = "水满",
    [HISTORY_EVENT_LEAK_ALARM] = "漏水报警",
    [HISTORY_EVENT_STOP] = "停止",
    [HISTORY_EVENT_STOP_CLEAR] = "停止清除",
    [HISTORY_EVENT_MAINTENANCE] = "维护提醒",
    [HISTORY_EVENT_MANUAL_CONTROL] = "手动控制",
    [HISTORY_EVENT_SYSTEM_START] = "系统启动",
};

// ==================== 私有函数 ====================

/**
 * @brief 生成安全的NVS日期key
 * @param date_val 日期值（YYYYMMDD格式或boot_day偏移）
 * @param buffer 输出缓冲区
 * @param buffer_size 缓冲区大小
 * @note NTP同步时使用真实日期（9字符），未同步时使用boot_day格式（最多5字符）
 */
static void make_date_key(uint32_t date_val, char *buffer, size_t buffer_size)
{
    // 真实日期格式：YYYYMMDD（8位数字），key为"dYYYYMMDD"（9字符）
    // 假日期格式：boot_day偏移（<=MAX_BOOT_DAYS），key为"b%03u"（最多5字符）
    if (date_val >= 20000101 && date_val <= 20991231) {
        // 真实日期范围，使用"d"前缀
        snprintf(buffer, buffer_size, "d%08lu", date_val);
    } else {
        // boot_day格式，使用"b"前缀（循环使用防止溢出）
        uint32_t boot_day = date_val % MAX_BOOT_DAYS;
        snprintf(buffer, buffer_size, "b%03lu", boot_day);
    }
}

/**
 * @brief 获取今日日期值
 * @return NTP同步时返回YYYYMMDD格式；未同步时返回boot_day偏移（循环计数）
 * @note boot_day格式确保NVS key永远不超过5字符，长时间运行安全
 */
static uint32_t get_today_date(void)
{
    time_t now = time(NULL);
    if (now > 0) {
        // NTP已同步，使用真实日期
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        // 确保年份在有效范围内（2000-2099）
        if (tm_now.tm_year >= 100 && tm_now.tm_year < 200) {
            return (uint32_t)((tm_now.tm_year + 1900) * 10000 +
                              (tm_now.tm_mon + 1) * 100 + tm_now.tm_mday);
        }
    }
    // NTP未同步或时间异常，使用启动天数计数（循环防止溢出）
    uint64_t uptime_us = esp_timer_get_time();
    uint32_t days_since_boot = (uint32_t)(uptime_us / 86400000000ULL);
    // 循环计数，确保key永远有效
    return days_since_boot % MAX_BOOT_DAYS;
}

static void save_to_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE_HISTORY, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    // 保存记录数量和写入索引
    nvs_set_u32(handle, "count", ctx.record_count);
    nvs_set_u32(handle, "index", ctx.write_index);

    // 保存记录数据
    nvs_set_blob(handle, "records", ctx.records, sizeof(ctx.records));

    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "历史记录NVS提交失败: %s", esp_err_to_name(err));
    }
}

static void load_from_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE_HISTORY, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    esp_err_t err;
    err = nvs_get_u32(handle, "count", &ctx.record_count);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS中无历史记录count，使用默认值");
    }
    err = nvs_get_u32(handle, "index", &ctx.write_index);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS中无历史记录index，使用默认值");
    }

    size_t len = sizeof(ctx.records);
    err = nvs_get_blob(handle, "records", ctx.records, &len);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS中无历史记录blob，使用默认值");
    } else if (len != sizeof(ctx.records)) {
        ESP_LOGW(TAG, "历史记录blob大小不匹配: 期望%zu, 实际%zu", sizeof(ctx.records), len);
    }

    nvs_close(handle);
}

static void save_daily_to_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE_DAILY, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    char key[12];  // 足够容纳 "b999" 或 "d20991231"
    make_date_key(ctx.today.date, key, sizeof(key));
    nvs_set_blob(handle, key, &ctx.today, sizeof(daily_stats_t));

    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "每日统计NVS提交失败: %s", esp_err_to_name(err));
    }
}

static void load_today_from_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE_DAILY, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    char key[12];
    make_date_key(ctx.today.date, key, sizeof(key));

    size_t len = sizeof(daily_stats_t);
    daily_stats_t loaded;
    if (nvs_get_blob(handle, key, &loaded, &len) == ESP_OK && len == sizeof(daily_stats_t) && loaded.date == ctx.today.date) {
        memcpy(&ctx.today, &loaded, sizeof(daily_stats_t));
        ESP_LOGI(TAG, "加载今日统计: 制水%lu秒, 冲洗%lu次",
                 ctx.today.production_sec, ctx.today.flush_count);
    } else if (len != sizeof(daily_stats_t)) {
        ESP_LOGW(TAG, "每日统计blob大小不匹配: 期望%zu, 实际%zu", sizeof(daily_stats_t), len);
    }

    nvs_close(handle);
}

// ==================== 公共接口实现 ====================

esp_err_t history_logger_init(void)
{
    if (ctx.initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "初始化历史记录模块...");

    ctx.mutex = xSemaphoreCreateMutex();
    if (!ctx.mutex) {
        ESP_LOGE(TAG, "创建互斥锁失败");
        return ESP_ERR_NO_MEM;
    }

    /* 在锁保护下加载数据（确保与其他任务访问一致） */
    if (xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(ctx.records, 0, sizeof(ctx.records));
        load_from_nvs();

        memset(&ctx.today, 0, sizeof(daily_stats_t));
        ctx.today.date = get_today_date();
        load_today_from_nvs();

        ctx.last_save_time = esp_timer_get_time();
        ctx.initialized = true;
        xSemaphoreGive(ctx.mutex);
    } else {
        ESP_LOGE(TAG, "获取互斥锁超时");
        vSemaphoreDelete(ctx.mutex);
        ctx.mutex = NULL;
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "历史记录模块初始化完成，已有%lu条记录", ctx.record_count);
    return ESP_OK;
}

esp_err_t history_logger_deinit(void)
{
    if (!ctx.initialized) {
        return ESP_OK;
    }

    if (ctx.mutex) {
        vSemaphoreDelete(ctx.mutex);
        ctx.mutex = NULL;
    }

    ctx.initialized = false;
    return ESP_OK;
}

esp_err_t history_add_record(history_event_type_t type, uint8_t state_from,
                              uint8_t state_to, float value1, float value2,
                              const char *description)
{
    if (!ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    history_record_t *record = &ctx.records[ctx.write_index];
    record->timestamp = esp_timer_get_time();
    record->type = type;
    record->state_from = state_from;
    record->state_to = state_to;
    record->value1 = value1;
    record->value2 = value2;

    if (description) {
        strncpy(record->description, description, sizeof(record->description) - 1);
        record->description[sizeof(record->description) - 1] = '\0';
    } else {
        record->description[0] = '\0';
    }

    ctx.write_index = (ctx.write_index + 1) % HISTORY_MAX_RECORDS;
    if (ctx.record_count < HISTORY_MAX_RECORDS) {
        ctx.record_count++;
    }

    xSemaphoreGive(ctx.mutex);

    ESP_LOGI(TAG, "记录事件: %s", event_names[type]);

    // 标记脏数据，由周期任务统一写入
    ctx.history_dirty = true;

    return ESP_OK;
}

esp_err_t history_log_state_change(uint8_t from, uint8_t to)
{
    return history_add_record(HISTORY_EVENT_STATE_CHANGE, from, to, 0, 0, NULL);
}

esp_err_t history_log_production_start(void)
{
    return history_add_record(HISTORY_EVENT_PRODUCTION_START, 0, 0, 0, 0, NULL);
}

esp_err_t history_log_production_end(uint32_t duration_sec)
{
    // 更新每日统计
    history_update_daily_production(duration_sec);
    return history_add_record(HISTORY_EVENT_PRODUCTION_END, 0, 0, (float)duration_sec, 0, NULL);
}

esp_err_t history_log_flush_start(void)
{
    return history_add_record(HISTORY_EVENT_FLUSH_START, 0, 0, 0, 0, NULL);
}

esp_err_t history_log_flush_end(uint32_t duration_sec)
{
    // 更新每日统计
    history_increment_daily_flush();
    return history_add_record(HISTORY_EVENT_FLUSH_END, 0, 0, (float)duration_sec, 0, NULL);
}

esp_err_t history_log_stop(uint8_t stop_type, const char *description)
{
    return history_add_record(HISTORY_EVENT_STOP, stop_type, 0, 0, 0, description);
}

esp_err_t history_log_leak_alarm(void)
{
    return history_add_record(HISTORY_EVENT_LEAK_ALARM, 0, 0, 0, 0, NULL);
}

uint32_t history_get_count(void)
{
    return ctx.record_count;
}

uint32_t history_get_records(history_record_t *records, uint32_t max_count)
{
    if (!records || max_count == 0) {
        return 0;
    }

    if (xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }

    uint32_t count = (ctx.record_count < max_count) ? ctx.record_count : max_count;

    // 从最新的记录开始复制
    uint32_t start_index = (ctx.write_index + HISTORY_MAX_RECORDS - count) % HISTORY_MAX_RECORDS;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (start_index + i) % HISTORY_MAX_RECORDS;
        memcpy(&records[i], &ctx.records[idx], sizeof(history_record_t));
    }

    xSemaphoreGive(ctx.mutex);

    return count;
}

esp_err_t history_clear_all(void)
{
    if (!ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memset(ctx.records, 0, sizeof(ctx.records));
    ctx.record_count = 0;
    ctx.write_index = 0;

    xSemaphoreGive(ctx.mutex);

    // 清除NVS
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE_HISTORY, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }

    ESP_LOGI(TAG, "历史记录已清除");
    return ESP_OK;
}

// ==================== 每日统计实现 ====================

esp_err_t history_update_daily_production(uint32_t seconds)
{
    if (!ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // 检查日期变化：只重置，不写入（由周期任务统一处理）
    uint32_t today = get_today_date();
    if (ctx.today.date != today) {
        ctx.daily_stats_dirty = true;  // 标记昨天的脏数据待保存
        memset(&ctx.today, 0, sizeof(daily_stats_t));
        ctx.today.date = today;
    }

    ctx.today.production_sec += seconds;
    ctx.daily_stats_dirty = true;

    return ESP_OK;
}

esp_err_t history_increment_daily_flush(void)
{
    if (!ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // 检查日期变化：只重置，不写入
    uint32_t today = get_today_date();
    if (ctx.today.date != today) {
        ctx.daily_stats_dirty = true;
        memset(&ctx.today, 0, sizeof(daily_stats_t));
        ctx.today.date = today;
    }

    ctx.today.flush_count++;
    ctx.daily_stats_dirty = true;

    return ESP_OK;
}

esp_err_t history_update_daily_tds(float tds_in, float tds_out)
{
    if (!ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // 检查日期变化：只重置，不写入
    uint32_t today = get_today_date();
    if (ctx.today.date != today) {
        ctx.daily_stats_dirty = true;
        memset(&ctx.today, 0, sizeof(daily_stats_t));
        ctx.today.date = today;
    }

    // 更新移动平均（指数平滑）
    if (ctx.today.tds_sample_count < UINT16_MAX) {
        ctx.today.tds_sample_count++;
    }
    // 计算alpha：当样本数达到上限时，使用固定alpha防止平均值冻结
    float alpha = 1.0f / (float)ctx.today.tds_sample_count;
    if (alpha < 0.0005f) {
        // 样本数超过2000时，使用固定alpha确保平均值仍能响应新数据
        alpha = 0.0005f;  // 每2000次采样权重为1
    }
    ctx.today.tds_in_avg = ctx.today.tds_in_avg * (1 - alpha) + tds_in * alpha;
    ctx.today.tds_out_avg = ctx.today.tds_out_avg * (1 - alpha) + tds_out * alpha;

    return ESP_OK;
}

/**
 * @brief 周期保存历史记录和每日统计（脏数据 + 节流）
 * @param min_interval_sec 最小保存间隔（秒）
 * @return true 执行了保存
 */
bool history_periodic_save(uint32_t min_interval_sec)
{
    if (!ctx.initialized) {
        return false;
    }

    uint64_t now_us = esp_timer_get_time();
    uint64_t elapsed_sec = (now_us - ctx.last_save_time) / 1000000ULL;

    // 检查日期变化
    uint32_t today = get_today_date();
    bool date_changed = (ctx.today.date != today);

    bool did_save = false;

    // 保存历史记录
    if (ctx.history_dirty && (date_changed || elapsed_sec >= min_interval_sec)) {
        save_to_nvs();
        ctx.history_dirty = false;
        did_save = true;
        ESP_LOGD(TAG, "历史记录已保存（节流间隔%lu秒）", min_interval_sec);
    }

    // 保存每日统计：仅在脏标志为真且有数据时保存
    if (ctx.daily_stats_dirty) {
        // 判断是否有实际数据
        bool has_data = (ctx.today.production_sec > 0 ||
                         ctx.today.flush_count > 0 ||
                         ctx.today.tds_sample_count > 0);

        bool should_save = false;
        if (date_changed) {
            // 跨天：有数据才保存昨天的记录
            should_save = has_data;
        } else if (elapsed_sec >= min_interval_sec) {
            // 正常节流：有数据就保存
            should_save = has_data;
        }

        if (should_save) {
            if (date_changed) {
                ESP_LOGI(TAG, "跨天保存昨日统计: 制水%lu秒, 冲洗%lu次",
                         ctx.today.production_sec, ctx.today.flush_count);
            }
            save_daily_to_nvs();
            // 跨天时重置今日数据
            if (date_changed) {
                // 清除旧格式的假日期key（兼容旧固件）
                // 旧固件使用 d20260101+days 格式，新固件使用 b%03u 格式
                uint32_t yesterday = ctx.today.date;
                char old_key[12];
                // 清除boot_day格式的旧key
                make_date_key(yesterday, old_key, sizeof(old_key));
                nvs_handle_t erase_handle;
                if (nvs_open(NVS_NAMESPACE_DAILY, NVS_READWRITE, &erase_handle) == ESP_OK) {
                    nvs_erase_key(erase_handle, old_key);
                    nvs_commit(erase_handle);
                    nvs_close(erase_handle);
                    ESP_LOGI(TAG, "已清除昨日key: %s", old_key);
                }
                memset(&ctx.today, 0, sizeof(daily_stats_t));
                ctx.today.date = today;
            }
            ctx.daily_stats_dirty = false;
            did_save = true;
        } else if (date_changed && !has_data) {
            // 昨天没有数据，跳过保存但清除旧key
            uint32_t yesterday = ctx.today.date;
            char old_key[12];
            make_date_key(yesterday, old_key, sizeof(old_key));
            nvs_handle_t erase_handle;
            if (nvs_open(NVS_NAMESPACE_DAILY, NVS_READWRITE, &erase_handle) == ESP_OK) {
                nvs_erase_key(erase_handle, old_key);
                nvs_commit(erase_handle);
                nvs_close(erase_handle);
                ESP_LOGD(TAG, "已清除无数据key: %s", old_key);
            }
            memset(&ctx.today, 0, sizeof(daily_stats_t));
            ctx.today.date = today;
            ctx.daily_stats_dirty = false;
        }
    }

    if (did_save) {
        ctx.last_save_time = now_us;
    }

    return did_save;
}

esp_err_t history_get_today_stats(daily_stats_t *stats)
{
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // 检查日期变化
    uint32_t today = get_today_date();
    if (ctx.today.date != today) {
        memset(&ctx.today, 0, sizeof(daily_stats_t));
        ctx.today.date = today;
    }

    memcpy(stats, &ctx.today, sizeof(daily_stats_t));
    xSemaphoreGive(ctx.mutex);
    return ESP_OK;
}

uint32_t history_get_recent_stats(daily_stats_t *stats, uint32_t max_days)
{
    if (!stats || max_days == 0) {
        return 0;
    }

    if (xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE_DAILY, NVS_READONLY, &handle) != ESP_OK) {
        memcpy(&stats[0], &ctx.today, sizeof(daily_stats_t));
        xSemaphoreGive(ctx.mutex);
        return 1;
    }

    uint32_t count = 0;
    uint32_t today = get_today_date();

    /* 先返回今日统计 */
    memcpy(&stats[0], &ctx.today, sizeof(daily_stats_t));
    count = 1;

    // 从NVS读取前几天（仅在NTP同步时有效，boot_day模式下可能不准确）
    for (uint32_t i = 1; i < max_days && count < max_days; i++) {
        char key[12];
        // 生成前一天的key（boot_day模式下按天数递减）
        uint32_t prev_date = today - i;
        make_date_key(prev_date, key, sizeof(key));

        size_t len = sizeof(daily_stats_t);
        if (nvs_get_blob(handle, key, &stats[count], &len) == ESP_OK && len == sizeof(daily_stats_t)) {
            // 验证日期匹配（防止读取到错误的记录）
            if (stats[count].date == prev_date) {
                count++;
            }
        }
    }

    nvs_close(handle);
    xSemaphoreGive(ctx.mutex);
    return count;
}

// ==================== 调试接口 ====================

const char* history_get_event_name(history_event_type_t type)
{
    if (type < sizeof(event_names) / sizeof(event_names[0])) {
        return event_names[type];
    }
    return "未知";
}