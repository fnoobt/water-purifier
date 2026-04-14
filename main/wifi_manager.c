/**
 * @file wifi_manager.c
 * @brief WiFi连接管理模块实现
 * @note 支持AP热点配网方式
 */

#include "wifi_manager.h"
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
#include "esp_timer.h"
#include <string.h>
#include <time.h>

static const char *TAG = "WIFI";

// ==================== mDNS配置 ====================

#define MDNS_HOSTNAME "waterpurifier"
#define MDNS_INSTANCE "ESP32净水器"

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

// ==================== mDNS初始化 ====================

static void init_mdns(void)
{
    // 初始化mDNS
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS初始化失败: %s", esp_err_to_name(ret));
        return;
    }

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
    ESP_LOGI(TAG, "启动SNTP时间同步...");

    // 配置NTP服务器
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "ntp.aliyun.com");
    sntp_init();

    // 设置时区（中国标准时间 UTC+8）
    setenv("TZ", "CST-8", 1);
    tzset();
}

// ==================== 初始化 ====================

esp_err_t wifi_manager_init(void)
{
    if (ctx.initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "初始化WiFi管理器...");

    // 创建事件组
    ctx.event_group = xEventGroupCreate();
    if (!ctx.event_group) {
        return ESP_ERR_NO_MEM;
    }

    // 初始化网络接口
    esp_netif_init();
    esp_event_loop_create_default();
    ctx.sta_netif = esp_netif_create_default_wifi_sta();

    // 初始化WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    // 注册事件处理器
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    // 生成AP SSID
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(ctx.ap_ssid, sizeof(ctx.ap_ssid), "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);

    esp_wifi_set_mode(WIFI_MODE_STA);

    ctx.initialized = true;
    ESP_LOGI(TAG, "WiFi管理器初始化完成");
    return ESP_OK;
}

esp_err_t wifi_manager_deinit(void)
{
    if (!ctx.initialized) return ESP_OK;

    wifi_manager_stop();
    if (ctx.event_group) {
        vEventGroupDelete(ctx.event_group);
        ctx.event_group = NULL;
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
        strncpy((char*)wifi_cfg.sta.ssid, ctx.ssid, sizeof(wifi_cfg.sta.ssid));
        strncpy((char*)wifi_cfg.sta.password, ctx.password, sizeof(wifi_cfg.sta.password));
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
    esp_wifi_start();

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
    return ctx.state;
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
    return ctx.state == WIFI_STATE_CONNECTED;
}

bool wifi_manager_is_ap_mode(void)
{
    return ctx.is_ap_mode;
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

const char* wifi_manager_get_ssid(void)
{
    return ctx.ssid;
}

// ==================== 存储 ====================

// WiFi配置保存节流：避免每次连接成功都写入Flash
static uint64_t s_last_wifi_save_time_us = 0;
#define WIFI_SAVE_THROTTLE_SEC 60  // 最小保存间隔60秒

esp_err_t wifi_manager_save_config(void)
{
    // 节流检查
    uint64_t now_us = esp_timer_get_time();
    uint64_t elapsed = (now_us - s_last_wifi_save_time_us) / 1000000ULL;
    if (elapsed < WIFI_SAVE_THROTTLE_SEC && s_last_wifi_save_time_us > 0) {
        ESP_LOGD(TAG, "WiFi配置保存节流：距上次保存仅%lu秒，跳过", (uint32_t)elapsed);
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_str(handle, "ssid", ctx.ssid);
    nvs_set_str(handle, "pass", ctx.password);
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        s_last_wifi_save_time_us = esp_timer_get_time();
        ESP_LOGI(TAG, "WiFi配置已保存");
    }
    return err;
}

esp_err_t wifi_manager_load_config(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t len = sizeof(ctx.ssid);
    err = nvs_get_str(handle, "ssid", ctx.ssid, &len);
    if (err == ESP_OK) {
        len = sizeof(ctx.password);
        nvs_get_str(handle, "pass", ctx.password, &len);
    }
    nvs_close(handle);
    return err;
}

esp_err_t wifi_manager_clear_config(void)
{
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
    ctx.ssid[0] = '\0';
    ctx.password[0] = '\0';
    return ESP_OK;
}

bool wifi_manager_has_saved_config(void)
{
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    char ssid[33];
    size_t len = sizeof(ssid);
    esp_err_t err = nvs_get_str(handle, "ssid", ssid, &len);
    nvs_close(handle);
    return err == ESP_OK && strlen(ssid) > 0;
}

// ==================== 回调 ====================

esp_err_t wifi_manager_register_callback(wifi_state_callback_t callback)
{
    ctx.callback = callback;
    return ESP_OK;
}

static void set_state(wifi_state_t state)
{
    if (ctx.state != state) {
        ctx.state = state;
        if (ctx.callback) {
            ctx.callback(state);
        }
        ESP_LOGI(TAG, "状态: %s", state_names[state]);
    }
}

// ==================== 调试 ====================

esp_err_t wifi_manager_get_status_string(char *buffer, size_t buffer_size)
{
    if (!buffer) return ESP_ERR_INVALID_ARG;

    char ip[16] = "未连接";
    wifi_manager_get_ip(ip, sizeof(ip));

    snprintf(buffer, buffer_size,
             "状态: %s\nSSID: %s\nIP: %s\nRSSI: %d dBm",
             state_names[ctx.state],
             ctx.ssid,
             ip,
             wifi_manager_get_rssi());
    return ESP_OK;
}

// ==================== 事件处理 ====================

static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA启动");
                if (strlen(ctx.ssid) > 0) {
                    set_state(WIFI_STATE_CONNECTING);
                    esp_wifi_connect();
                }
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "已连接到AP");
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "连接断开");
                if (ctx.retry_count < 10) {
                    ctx.retry_count++;
                    set_state(WIFI_STATE_RECONNECTING);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    esp_wifi_connect();
                } else {
                    set_state(WIFI_STATE_DISCONNECTED);
                    // 连接失败，启动AP模式
                    wifi_manager_start_ap_mode();
                }
                break;

            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "AP已启动");
                break;

            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGI(TAG, "有设备连接到AP");
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
            wifi_manager_save_config();

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