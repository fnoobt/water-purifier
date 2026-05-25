/**
 * @file wifi_manager.c
 * @brief WiFi连接管理模块实现
 * @note 支持AP热点配网方式
 */

#include "wifi_manager.h"
#include "config_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "lwip/apps/sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"  // 任务看门狗
#include <string.h>
#include <time.h>

static const char *TAG = "WIFI";

// ==================== mDNS配置 ====================

#define MDNS_HOSTNAME "waterpurifier"
#define MDNS_INSTANCE "ESP32净水器"

// mDNS是否已初始化标志
static bool s_mdns_initialized = false;

// SNTP是否已初始化标志
static bool s_sntp_initialized = false;

// 系统启动时的墙钟时间（Unix timestamp，在NTP首次同步后计算）
static time_t s_boot_wall_clock_time = 0;

// ==================== 事件位定义 ====================

#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1

// ==================== AP配置常量 ====================

#define AP_SSID_PREFIX        "WaterPurifier-"
#define AP_PASSWORD           "12345678"
#define AP_CHANNEL            1
#define AP_MAX_CONN           4

// ==================== 私有变量 ====================

static struct {
    bool initialized;
    bool started;
    wifi_state_t state;
    char ssid[33];
    char password[65];
    char ap_ssid[33];
    bool is_ap_mode;

    esp_netif_t *sta_netif;
    esp_netif_t *ap_netif;
    EventGroupHandle_t event_group;
    int retry_count;

    wifi_state_callback_t callback;

    // 重连任务句柄和追踪
    TaskHandle_t reconnect_task_handle;
    uint32_t reconnect_sequence;      // 每次断开事件递增的序列号
    uint32_t active_reconnect_seq;    // 当前活跃任务的序列号
    volatile bool reconnect_active;   // 重连任务是否正在运行

    // 状态访问互斥锁（保护state、is_ap_mode等字段）
    SemaphoreHandle_t state_mutex;

    // 事件处理器实例句柄（用于注销）
    esp_event_handler_instance_t wifi_event_handler_instance;
    esp_event_handler_instance_t ip_event_handler_instance;
} ctx = {0};

// ==================== 状态名称 ====================

static const char* state_names[] = {
    "空闲", "连接中", "已连接", "已断开", "重连中", "AP模式", "错误"
};

// ==================== 私有函数 ====================

static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data);
static void set_state(wifi_state_t state);
static void init_mdns(void);
static void init_sntp(void);
static void wifi_reconnect_task(void *pvParameters);

// ==================== mDNS初始化 ====================

static void init_mdns(void)
{
    if (s_mdns_initialized) {
        ESP_LOGD(TAG, "mDNS已初始化，跳过");
        return;
    }

    // 初始化mDNS
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS初始化失败: %s", esp_err_to_name(ret));
        return;
    }
    s_mdns_initialized = true;

    // 设置主机名
    ret = mdns_hostname_set(MDNS_HOSTNAME);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS主机名设置失败");
        return;
    }

    // 设置实例名
    mdns_instance_name_set(MDNS_INSTANCE);

    // 添加HTTP服务
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    ESP_LOGI(TAG, "mDNS已启动: http://%s.local", MDNS_HOSTNAME);
}

// ==================== SNTP初始化 ====================

static void init_sntp(void)
{
    if (s_sntp_initialized) {
        ESP_LOGD(TAG, "SNTP已初始化，跳过");
        return;
    }

    ESP_LOGI(TAG, "启动SNTP时间同步...");

    // 设置时区（中国标准时间 UTC+8）
    setenv("TZ", "CST-8", 1);
    tzset();

    // 配置NTP服务器
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "ntp.aliyun.com");
    sntp_init();
    s_sntp_initialized = true;
}

/**
 * @brief 更新启动墙钟时间（在NTP同步后调用）
 * @note 检查NTP是否已同步，若已同步则计算启动时的墙钟时间
 */
void wifi_manager_update_boot_wall_clock_time(void)
{
    if (s_boot_wall_clock_time > 0) {
        return;  // 已经计算过
    }

    // 检查时间是否有效（年份大于2020）
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    if (tm_now.tm_year + 1900 > 2020) {
        // 时间已同步，计算启动墙钟时间
        uint64_t elapsed_us = esp_timer_get_time();
        s_boot_wall_clock_time = now - (elapsed_us / 1000000ULL);
        ESP_LOGI(TAG, "NTP时间已同步！启动墙钟时间: %ld (当前: %ld, 运行: %llu秒)",
                 (long)s_boot_wall_clock_time, (long)now, elapsed_us / 1000000ULL);
    }
}

/**
 * @brief 获取系统启动时的墙钟时间（用于日志时间戳转换）
 * @return 启动时的Unix timestamp，0表示尚未同步
 */
time_t wifi_manager_get_boot_wall_clock_time(void)
{
    // 每次调用时尝试更新（如果尚未同步）
    wifi_manager_update_boot_wall_clock_time();
    return s_boot_wall_clock_time;
}

// ==================== 初始化 ====================

esp_err_t wifi_manager_init(void)
{
    if (ctx.initialized) {
        return ESP_OK;
    }

    // 创建状态访问互斥锁
    ctx.state_mutex = xSemaphoreCreateMutex();
    if (!ctx.state_mutex) {
        return ESP_ERR_NO_MEM;
    }

    // 创建事件组
    ctx.event_group = xEventGroupCreate();
    if (!ctx.event_group) {
        vSemaphoreDelete(ctx.state_mutex);
        ctx.state_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    // 初始化网络接口
    esp_netif_init();
    esp_event_loop_create_default();
    ctx.sta_netif = esp_netif_create_default_wifi_sta();

    // 初始化WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    // 注册事件处理器（保存实例句柄用于注销）
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &ctx.wifi_event_handler_instance);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &ctx.ip_event_handler_instance);

    // 生成AP SSID
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(ctx.ap_ssid, sizeof(ctx.ap_ssid), "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);

    esp_wifi_set_mode(WIFI_MODE_STA);

    ctx.initialized = true;
    return ESP_OK;
}

esp_err_t wifi_manager_deinit(void)
{
    if (!ctx.initialized) return ESP_OK;

    wifi_manager_stop();

    // 注销事件处理器（防止内存泄漏和野指针）
    if (ctx.wifi_event_handler_instance != NULL) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, ctx.wifi_event_handler_instance);
        ctx.wifi_event_handler_instance = NULL;
    }
    if (ctx.ip_event_handler_instance != NULL) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ctx.ip_event_handler_instance);
        ctx.ip_event_handler_instance = NULL;
    }

    // 删除事件组
    if (ctx.event_group) {
        vEventGroupDelete(ctx.event_group);
        ctx.event_group = NULL;
    }

    // 删除互斥锁
    if (ctx.state_mutex) {
        vSemaphoreDelete(ctx.state_mutex);
        ctx.state_mutex = NULL;
    }

    // 销毁网络接口
    if (ctx.sta_netif) {
        esp_netif_destroy(ctx.sta_netif);
        ctx.sta_netif = NULL;
    }
    if (ctx.ap_netif) {
        esp_netif_destroy(ctx.ap_netif);
        ctx.ap_netif = NULL;
    }

    ctx.initialized = false;
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    if (!ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (ctx.started) return ESP_OK;

    ESP_LOGI(TAG, "启动WiFi...");

    // 检查是否有保存的配置
    if (wifi_manager_has_saved_config()) {
        wifi_manager_load_config();
        ESP_LOGI(TAG, "使用保存的配置: %s", ctx.ssid);

        wifi_config_t wifi_cfg = {0};
        strncpy((char*)wifi_cfg.sta.ssid, ctx.ssid, sizeof(wifi_cfg.sta.ssid) - 1);
        strncpy((char*)wifi_cfg.sta.password, ctx.password, sizeof(wifi_cfg.sta.password) - 1);
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        esp_wifi_start();
        // 连接由WIFI_EVENT_STA_START事件处理器触发，避免重复调用
    } else {
        // 无配置，直接启动AP配置模式
        ESP_LOGI(TAG, "无保存配置，启动AP配置模式...");
        wifi_manager_start_ap_mode();
    }

    ctx.started = true;
    return ESP_OK;
}

esp_err_t wifi_manager_stop(void)
{
    if (!ctx.started) return ESP_OK;

    // 取消重连任务
    if (ctx.reconnect_task_handle) {
        vTaskDelete(ctx.reconnect_task_handle);
        ctx.reconnect_task_handle = NULL;
    }

    if (ctx.is_ap_mode) {
        wifi_manager_stop_ap_mode();
    }
    esp_wifi_stop();
    ctx.started = false;
    set_state(WIFI_STATE_IDLE);
    return ESP_OK;
}

// ==================== 配置 ====================

esp_err_t wifi_manager_set_config(const char *ssid, const char *password)
{
    if (!ssid || !password) return ESP_ERR_INVALID_ARG;

    strncpy(ctx.ssid, ssid, sizeof(ctx.ssid) - 1);
    strncpy(ctx.password, password, sizeof(ctx.password) - 1);

    wifi_config_t wifi_cfg = {0};
    strncpy((char*)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strncpy((char*)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret == ESP_OK && ctx.started) {
        ctx.retry_count = 0;
        set_state(WIFI_STATE_CONNECTING);
        ret = esp_wifi_connect();
    }

    return ret;
}

// ==================== AP模式 ====================

esp_err_t wifi_manager_start_ap_mode(void)
{
    if (ctx.is_ap_mode) return ESP_OK;

    ESP_LOGI(TAG, "启动AP配置模式...");

    if (!ctx.ap_netif) {
        ctx.ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char*)ap_cfg.ap.ssid, ctx.ap_ssid, sizeof(ap_cfg.ap.ssid));
    strncpy((char*)ap_cfg.ap.password, AP_PASSWORD, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = strlen(ctx.ap_ssid);

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    // WiFi可能已启动（STA模式），esp_wifi_start在ESP-IDF v6.0中如果已启动返回ESP_OK
    esp_err_t start_ret = esp_wifi_start();
    if (start_ret != ESP_OK) {
        ESP_LOGW(TAG, "AP模式启动失败: %s", esp_err_to_name(start_ret));
    }

    ctx.is_ap_mode = true;
    set_state(WIFI_STATE_AP_MODE);

    ESP_LOGI(TAG, "AP模式已启动: SSID=%s, 密码=%s, IP=192.168.4.1", ctx.ap_ssid, AP_PASSWORD);
    return ESP_OK;
}

esp_err_t wifi_manager_stop_ap_mode(void)
{
    if (!ctx.is_ap_mode) return ESP_OK;

    ESP_LOGI(TAG, "停止AP模式");
    esp_wifi_set_mode(WIFI_MODE_STA);

    if (ctx.ap_netif) {
        esp_netif_destroy(ctx.ap_netif);
        ctx.ap_netif = NULL;
    }

    ctx.is_ap_mode = false;
    return ESP_OK;
}

// ==================== 状态查询 ====================

wifi_state_t wifi_manager_get_state(void)
{
    wifi_state_t state = WIFI_STATE_IDLE;
    if (ctx.state_mutex && xSemaphoreTake(ctx.state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        state = ctx.state;
        xSemaphoreGive(ctx.state_mutex);
    } else {
        state = ctx.state;  // 互斥锁获取失败时仍返回当前值（降级处理）
    }
    return state;
}

const char* wifi_manager_get_state_name(wifi_state_t state)
{
    if (state < sizeof(state_names) / sizeof(state_names[0])) {
        return state_names[state];
    }
    return "未知";
}

bool wifi_manager_is_connected(void)
{
    return wifi_manager_get_state() == WIFI_STATE_CONNECTED;
}

bool wifi_manager_is_ap_mode(void)
{
    bool is_ap = false;
    if (ctx.state_mutex && xSemaphoreTake(ctx.state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        is_ap = ctx.is_ap_mode;
        xSemaphoreGive(ctx.state_mutex);
    } else {
        is_ap = ctx.is_ap_mode;  // 降级处理
    }
    return is_ap;
}

esp_err_t wifi_manager_get_ip(char *ip_str, size_t buffer_size)
{
    if (!ip_str) return ESP_ERR_INVALID_ARG;
    if (!ctx.sta_netif) return ESP_ERR_INVALID_STATE;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(ctx.sta_netif, &ip_info) == ESP_OK) {
        snprintf(ip_str, buffer_size, IPSTR, IP2STR(&ip_info.ip));
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

int8_t wifi_manager_get_rssi(void)
{
    if (!wifi_manager_is_connected()) return 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

/**
 * @brief 获取当前WiFi SSID
 * @return SSID字符串指针
 * @warning 返回值指向静态缓冲区，多线程调用会互相覆盖。调用者应立即复制结果，不要长期持有指针。
 */
const char* wifi_manager_get_ssid(void)
{
    // 使用静态缓冲区返回SSID（避免返回可能被修改的内部缓冲区指针）
    // 注意：多线程环境下不安全，调用者应立即复制结果
    static char ssid_copy[33] = {0};
    if (ctx.state_mutex && xSemaphoreTake(ctx.state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        strncpy(ssid_copy, ctx.ssid, sizeof(ssid_copy) - 1);
        ssid_copy[sizeof(ssid_copy) - 1] = '\0';
        xSemaphoreGive(ctx.state_mutex);
    } else {
        strncpy(ssid_copy, ctx.ssid, sizeof(ssid_copy) - 1);
        ssid_copy[sizeof(ssid_copy) - 1] = '\0';
    }
    return ssid_copy;
}

// ==================== 存储 ====================

// WiFi配置保存节流：避免每次连接成功都写入Flash
// 注意：s_last_wifi_save_time_us在事件回调中使用，竞态风险低，无需额外mutex
static uint64_t s_last_wifi_save_time_us = 0;
#define WIFI_SAVE_THROTTLE_SEC 60  // 最小保存间隔60秒

esp_err_t wifi_manager_save_config(void)
{
    // 节流检查（esp_timer_get_time()是原子操作，竞态风险低）
    uint64_t now_us = esp_timer_get_time();
    uint64_t last_save = s_last_wifi_save_time_us;
    uint64_t elapsed = (now_us - last_save) / 1000000ULL;
    if (elapsed < WIFI_SAVE_THROTTLE_SEC && last_save > 0) {
        ESP_LOGD(TAG, "WiFi配置保存节流：距上次保存仅%lu秒，跳过", (uint32_t)elapsed);
        return ESP_OK;
    }

    // 使用mutex保护ssid/password读取
    char ssid[33], password[65];
    if (ctx.state_mutex && xSemaphoreTake(ctx.state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        strncpy(ssid, ctx.ssid, sizeof(ssid) - 1);
        strncpy(password, ctx.password, sizeof(password) - 1);
        xSemaphoreGive(ctx.state_mutex);
    } else {
        strncpy(ssid, ctx.ssid, sizeof(ssid) - 1);
        strncpy(password, ctx.password, sizeof(password) - 1);
    }
    ssid[sizeof(ssid) - 1] = '\0';
    password[sizeof(password) - 1] = '\0';

    // 使用config_manager保存（统一存储到water_purifier命名空间）
    esp_err_t err = config_manager_set_string("wifi_ssid", ssid);
    if (err != ESP_OK) return err;
    err = config_manager_set_string("wifi_pass", password);
    if (err != ESP_OK) return err;
    err = config_manager_save();

    if (err == ESP_OK) {
        s_last_wifi_save_time_us = esp_timer_get_time();
        ESP_LOGI(TAG, "WiFi配置已保存");
    }
    return err;
}

esp_err_t wifi_manager_load_config(void)
{
    // 从config_manager加载（统一存储在water_purifier命名空间）
    esp_err_t err = config_manager_get_string("wifi_ssid", ctx.ssid, sizeof(ctx.ssid));
    if (err != ESP_OK) return err;

    esp_err_t pass_err = config_manager_get_string("wifi_pass", ctx.password, sizeof(ctx.password));
    if (pass_err != ESP_OK && pass_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "加载WiFi密码失败: %s", esp_err_to_name(pass_err));
        ctx.password[0] = '\0';
    }
    return err;
}

esp_err_t wifi_manager_clear_config(void)
{
    // 通过config_manager清除（设置空字符串）
    config_manager_set_string("wifi_ssid", "");
    config_manager_set_string("wifi_pass", "");
    config_manager_save();

    ctx.ssid[0] = '\0';
    ctx.password[0] = '\0';
    return ESP_OK;
}

bool wifi_manager_has_saved_config(void)
{
    return config_manager_has_wifi_config();
}

// ==================== 回调 ====================

esp_err_t wifi_manager_register_callback(wifi_state_callback_t callback)
{
    ctx.callback = callback;
    return ESP_OK;
}

static void set_state(wifi_state_t state)
{
    // 使用mutex保护状态变更
    if (ctx.state_mutex && xSemaphoreTake(ctx.state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (ctx.state != state) {
            ctx.state = state;
            wifi_state_callback_t callback = ctx.callback;  // 在锁内复制回调指针
            xSemaphoreGive(ctx.state_mutex);
            if (callback) {
                callback(state);
            }
            ESP_LOGI(TAG, "状态: %s", state_names[state]);
        } else {
            xSemaphoreGive(ctx.state_mutex);
        }
    } else {
        // 互斥锁获取失败时直接更新（降级处理，避免阻塞事件回调）
        if (ctx.state != state) {
            ctx.state = state;
            if (ctx.callback) {
                ctx.callback(state);
            }
            ESP_LOGW(TAG, "状态变更(无锁): %s", state_names[state]);
        }
    }
}

// ==================== 重连任务 ====================

/**
 * @brief WiFi重连任务（独立FreeRTOS任务，不在事件回调中延时）
 *
 * 策略：指数退避重连，初始1s，最大30s，总计约5分钟
 * 失败后进入AP模式并退出任务，下次断开时自动重建重连任务
 *
 * @param arg 重连序列号（用于检测是否仍是活跃任务）
 */

static void wifi_reconnect_task(void *arg)
{
    uint32_t my_seq = (uint32_t)arg;  // 本任务的序列号
    int retry = 0;
    const int max_retries = 20;
    const TickType_t delays[] = {
        pdMS_TO_TICKS(1000),   // 第1次：1s
        pdMS_TO_TICKS(2000),   // 第2次：2s
        pdMS_TO_TICKS(4000),   // 第3次：4s
        pdMS_TO_TICKS(8000),   // 第4次：8s
        pdMS_TO_TICKS(16000),  // 第5次：16s
        pdMS_TO_TICKS(30000),  // 第6+次：30s（上限）
    };
    const int delay_count = sizeof(delays) / sizeof(delays[0]);

    ESP_LOGI(TAG, "WiFi重连任务启动 seq=%lu", my_seq);

    // 注册看门狗
    esp_task_wdt_add(NULL);

    while (retry < max_retries) {
        TickType_t delay = delays[(retry < delay_count) ? retry : delay_count - 1];
        ESP_LOGD(TAG, "尝试重连 %d/%d，等待 %lums...", retry + 1, max_retries, pdTICKS_TO_MS(delay));
        esp_task_wdt_reset();
        vTaskDelay(delay);

        // 使用mutex保护的状态检查
        bool is_connected = false;
        bool still_active = false;
        if (ctx.state_mutex && xSemaphoreTake(ctx.state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            is_connected = (ctx.state == WIFI_STATE_CONNECTED);
            still_active = (ctx.active_reconnect_seq == my_seq);
            xSemaphoreGive(ctx.state_mutex);
        }

        // 如果已连接或不再是活跃任务，退出
        if (is_connected || !still_active) {
            ESP_LOGD(TAG, "重连任务退出 seq=%lu (connected=%d, active=%d)", my_seq, is_connected, still_active);
            goto exit_task;
        }

        esp_err_t ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "重连发起失败: %s", esp_err_to_name(ret));
        }
        retry++;

        // 等待连接结果（最多等10秒，路由器重启通常需几秒到十几秒）
        for (int i = 0; i < 10; i++) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(1000));

            // mutex保护的状态检查
            if (ctx.state_mutex && xSemaphoreTake(ctx.state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                is_connected = (ctx.state == WIFI_STATE_CONNECTED);
                still_active = (ctx.active_reconnect_seq == my_seq);
                xSemaphoreGive(ctx.state_mutex);
            }

            if (is_connected || !still_active) {
                ESP_LOGD(TAG, "重连任务退出 seq=%lu (connected=%d, active=%d)", my_seq, is_connected, still_active);
                goto exit_task;
            }
        }
    }

    // 在进入AP模式前，再次确认仍是活跃任务且未连接
    bool is_connected = false;
    bool still_active = false;

    if (ctx.state_mutex && xSemaphoreTake(ctx.state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        is_connected = (ctx.state == WIFI_STATE_CONNECTED);
        still_active = (ctx.active_reconnect_seq == my_seq);
        xSemaphoreGive(ctx.state_mutex);
    }

    if (still_active && !is_connected) {
        ESP_LOGW(TAG, "WiFi重连失败%d次，进入AP模式", retry);
        set_state(WIFI_STATE_DISCONNECTED);
        wifi_manager_start_ap_mode();
    }

exit_task:
    // 清除活跃标志（使用mutex保护）
    if (ctx.state_mutex && xSemaphoreTake(ctx.state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // 只有当前序列号仍匹配时才清除标志
        if (ctx.active_reconnect_seq == my_seq) {
            ctx.reconnect_active = false;
            ctx.reconnect_task_handle = NULL;
        }
        xSemaphoreGive(ctx.state_mutex);
    }

    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

// ==================== 调试 ====================

// ==================== 事件处理 ====================

static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGD(TAG, "STA启动");
                if (strlen(ctx.ssid) > 0) {
                    set_state(WIFI_STATE_CONNECTING);
                    esp_wifi_connect();
                }
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGD(TAG, "已连接到AP");
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t* disconn = (wifi_event_sta_disconnected_t*)data;
                ESP_LOGW(TAG, "连接断开 (原因码: %d)", disconn->reason);

                // 使用mutex保护重连任务创建，避免竞态条件
                if (ctx.state_mutex && xSemaphoreTake(ctx.state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    // 递增序列号，用于追踪每次断开事件
                    ctx.reconnect_sequence++;

                    // 如果没有活跃的重连任务，创建新任务
                    if (!ctx.reconnect_active) {
                        ctx.reconnect_active = true;
                        ctx.active_reconnect_seq = ctx.reconnect_sequence;
                        xSemaphoreGive(ctx.state_mutex);

                        set_state(WIFI_STATE_RECONNECTING);
                        // 将序列号作为任务参数传递
                        BaseType_t ret = xTaskCreate(wifi_reconnect_task, "wifi_reconnect",
                                                     4096, (void*)ctx.active_reconnect_seq, 5,
                                                     &ctx.reconnect_task_handle);
                        if (ret != pdPASS) {
                            ESP_LOGE(TAG, "创建重连任务失败");
                            // 创建失败时重置标志
                            if (ctx.state_mutex && xSemaphoreTake(ctx.state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                                ctx.reconnect_active = false;
                                xSemaphoreGive(ctx.state_mutex);
                            }
                        }
                    } else {
                        // 任务已运行 - 更新活跃序列号，让旧任务继续处理新的断开事件
                        ctx.active_reconnect_seq = ctx.reconnect_sequence;
                        ESP_LOGI(TAG, "重连任务已运行，更新序列号=%lu", ctx.reconnect_sequence);
                        xSemaphoreGive(ctx.state_mutex);
                    }
                } else {
                    // mutex获取失败时的降级处理（不应发生，但做防御性编程）
                    ESP_LOGW(TAG, "无法获取mutex，降级创建重连任务");
                    set_state(WIFI_STATE_RECONNECTING);
                    if (!ctx.reconnect_active) {
                        ctx.reconnect_active = true;
                        ctx.reconnect_sequence++;
                        ctx.active_reconnect_seq = ctx.reconnect_sequence;
                        xTaskCreate(wifi_reconnect_task, "wifi_reconnect",
                                   4096, (void*)ctx.active_reconnect_seq, 5,
                                   &ctx.reconnect_task_handle);
                    }
                }
                break;
            }

            case WIFI_EVENT_AP_START:
                ESP_LOGD(TAG, "AP已启动");
                break;

            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGD(TAG, "有设备连接到AP");
                break;

            default:
                break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t*)data;
            ESP_LOGI(TAG, "获取IP: " IPSTR, IP2STR(&event->ip_info.ip));
            ctx.retry_count = 0;
            set_state(WIFI_STATE_CONNECTED);
            xEventGroupSetBits(ctx.event_group, WIFI_CONNECTED_BIT);
            // WiFi配置仅在用户设置时保存，不在每次重连后重复保存（减少NVS磨损）

            // 清除重连活跃标志（连接成功，重连任务将检测到并退出）
            if (ctx.state_mutex && xSemaphoreTake(ctx.state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                ctx.reconnect_active = false;
                ctx.reconnect_task_handle = NULL;
                xSemaphoreGive(ctx.state_mutex);
            }

            // 初始化mDNS，支持域名访问
            init_mdns();

            // 启动SNTP时间同步
            init_sntp();

            // 连接成功，停止AP模式
            if (ctx.is_ap_mode) {
                wifi_manager_stop_ap_mode();
            }
        }
    }
}